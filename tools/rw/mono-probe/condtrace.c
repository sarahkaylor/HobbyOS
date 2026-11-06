/* tools/rw/mono-probe/condtrace.c — condvar flight recorder for Mono's
 * Monitor.Wait / w32handle layer, linked INTO the launcher executable.
 *
 * Why the executable: Mono's monitor does not wait on a monitor condvar at
 * all.  mono_monitor_wait (mono/metadata/monitor.c, 6.12) creates a private
 * auto-reset MonoW32Handle event per wait, appends it to mon->wait_list and
 * blocks in mono_w32handle_wait_one(event, ms, alertable=TRUE), which is a
 * pthread_cond_wait on handle->signal_cond under handle->signal_mutex.  Pulse
 * walks mon->wait_list and calls mono_w32event_set(handle), i.e.
 * pthread_cond_signal on the *same* cond.  So the lost wake, if there is one,
 * is visible as: a wait on cond C that is still parked after a signal/
 * broadcast to C, or a wait on C with no signal to C at all.
 *
 * A launcher built -rdynamic wins symbol resolution for the DSOs it dlopens
 * (rule 28), so these wrappers see every Mono pthread_cond_* call.  Output is
 * a single write(2) per event: printf would take libc locks from inside the
 * wait path.
 *
 * Enable: /CONDTRACE in the image root, or RW_CONDTRACE in the environment.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int ct_on;
static pthread_t ct_self_of_main;

static int (*real_wait)(pthread_cond_t *, pthread_mutex_t *);
static int (*real_timedwait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
static int (*real_signal)(pthread_cond_t *);
static int (*real_broadcast)(pthread_cond_t *);

static char *puthex(char *p, uint64_t v) {
  static const char d[] = "0123456789abcdef";
  *p++ = '0'; *p++ = 'x';
  for (int i = 60; i >= 0; i -= 4) *p++ = d[(v >> i) & 15];
  return p;
}
static char *putdec(char *p, long v) {
  char t[24]; int n = 0;
  if (v < 0) { *p++ = '-'; v = -v; }
  do { t[n++] = '0' + (v % 10); v /= 10; } while (v);
  while (n) *p++ = t[--n];
  return p;
}

static void ct_resolve(void) {
  if (real_wait) return;
  /* dlvsym, not dlsym: from the executable RTLD_NEXT reaches libc.so.6 before
     libpthread, and libc's pthread_cond_timedwait@@GLIBC_2.0 compat symbol
     uses the pre-2.3.2 condvar layout -- with it every timed wait returns
     ETIMEDOUT at once and Mono's Task.Wait(timeout) never sleeps (measured:
     taskwait done=False ms=3 on a 100 ms task).  GLIBC_2.3.2 is the ABI
     libpthread exports to a pinned 2.31 process. */
  real_wait = dlvsym(RTLD_NEXT, "pthread_cond_wait", "GLIBC_2.3.2");
  real_timedwait = dlvsym(RTLD_NEXT, "pthread_cond_timedwait", "GLIBC_2.3.2");
  real_signal = dlvsym(RTLD_NEXT, "pthread_cond_signal", "GLIBC_2.3.2");
  real_broadcast = dlvsym(RTLD_NEXT, "pthread_cond_broadcast", "GLIBC_2.3.2");
  if (!real_wait) real_wait = dlsym(RTLD_NEXT, "pthread_cond_wait");
  if (!real_timedwait) real_timedwait = dlsym(RTLD_NEXT, "pthread_cond_timedwait");
  if (!real_signal) real_signal = dlsym(RTLD_NEXT, "pthread_cond_signal");
  if (!real_broadcast) real_broadcast = dlsym(RTLD_NEXT, "pthread_cond_broadcast");
}

void ct_init(void) {
  ct_resolve();
  if (getenv("RW_CONDTRACE") || access("/CONDTRACE", F_OK) == 0) ct_on = 1;
  ct_self_of_main = pthread_self();
  if (ct_on) write(2, "[CT] on=1\n", 10);
}

__attribute__((constructor)) static void ct_ctor(void) { ct_init(); }

/* kind: 'W' wait, 'T' timedwait, 'w' wait-return, 'S' signal, 'B' broadcast */
static void ct_row(char kind, pthread_cond_t *c, pthread_mutex_t *m, long a, int r) {
  char b[256], *p = b;
  long tid = syscall(SYS_gettid);
  int main_t = pthread_equal(pthread_self(), ct_self_of_main);
  memcpy(p, "[CT] ", 5); p += 5;
  *p++ = kind;
  memcpy(p, " tid=", 5); p += 5; p = putdec(p, tid);
  memcpy(p, " cond=", 6); p += 6; p = puthex(p, (uint64_t)(uintptr_t)c);
  if (kind == 'W' || kind == 'T' || kind == 'w' || kind == 't') {
    memcpy(p, " mutex=", 7); p += 7; p = puthex(p, (uint64_t)(uintptr_t)m);
  }
  if (kind == 'T') { memcpy(p, " to=", 4); p += 4; p = putdec(p, a); }
  if (kind == 'w' || kind == 't') { memcpy(p, " ret=", 5); p += 5; p = putdec(p, r); }
  if (main_t) { memcpy(p, " main=1", 7); p += 7; }
  *p++ = '\n';
  write(2, b, p - b);
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  ct_resolve();
  if (!ct_on) return real_wait(c, m);
  ct_row('W', c, m, 0, 0);
  int r = real_wait(c, m);
  ct_row('w', c, m, 0, r);
  return r;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *ts) {
  ct_resolve();
  if (!ct_on) return real_timedwait(c, m, ts);
  long ms = -2;
  if (ts) {
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    ms = (long)(ts->tv_sec - now.tv_sec) * 1000 + (ts->tv_nsec - now.tv_nsec) / 1000000;
  }
  ct_row('T', c, m, ms, 0);
  int r = real_timedwait(c, m, ts);
  ct_row('t', c, m, ms, r);
  return r;
}

int pthread_cond_signal(pthread_cond_t *c) {
  ct_resolve();
  if (!ct_on) return real_signal(c);
  ct_row('S', c, 0, 0, 0);
  return real_signal(c);
}

int pthread_cond_broadcast(pthread_cond_t *c) {
  ct_resolve();
  if (!ct_on) return real_broadcast(c);
  ct_row('B', c, 0, 0, 0);
  return real_broadcast(c);
}
