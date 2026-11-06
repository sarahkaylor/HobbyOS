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
 * L54 changes the output contract from "one row per event" to per-cond
 * counters plus an on-demand report, because the question is now exact:
 *
 *   [CT2] tag=... cond=0x... waits=.. rets=.. inwait=.. sig=.. bc=..
 *         sigw=.. bcw=..      <-- signals/broadcasts issued while a waiter
 *                                 was INSIDE this cond's wait (sigw/bcw)
 *
 *   waits/rets      total entries/returns for this cond
 *   inwait          current entrants minus returns (1 while the thread parks)
 *   sig/bc          total pthread_cond_signal/broadcast calls on this cond
 *   sigw/bcw        calls issued while inwait > 0, i.e. AFTER the wait began
 *
 * A launcher-side caller (rwmain.c) passes the cond address it read out of the
 * parked thread's own futex key (glibc 2.31: key = cond + 0x28 + 4*g) and the
 * cond of every MonoW32Handle it finds on that stack (handle + 0x38).
 *
 * A launcher built -rdynamic wins symbol resolution for the DSOs it dlopens
 * (rule 28), so these wrappers see every Mono pthread_cond_* call.  Output is
 * a single write(2) per printed row: printf would take libc locks from inside
 * the wait path.  Counting takes no lock.
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
#include <time.h>
#include <unistd.h>

/* This file is compiled -nostdinc against the pinned glibc 2.31 dev headers,
 * whose <sys/syscall.h> pulls <asm/unistd.h> that the set does not carry.
 * Declare the one entry point directly, as src/user/rwmain.c does. */
extern long syscall(long nr, ...);
#define CT_SYS_gettid 186

#define CT_MAX 512
#define CT_PRINT_MAX 256          /* bounded [CT] S/B rows */
#define CT_WAIT_ROWS 8            /* bounded per-wait rows for main */

static int ct_on;
static pthread_t ct_self_of_main;

static int (*real_wait)(pthread_cond_t *, pthread_mutex_t *);
static int (*real_timedwait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
static int (*real_signal)(pthread_cond_t *);
static int (*real_broadcast)(pthread_cond_t *);

struct ct_ent {
  const void *cond;
  unsigned waits, rets, sig, bc, sigw, bcw;
};
static struct ct_ent ct_tab[CT_MAX];
static volatile int ct_used;
static volatile unsigned ct_seq;
static volatile int ct_print_n, ct_wait_n, ct_ret_n;

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

static struct ct_ent *ct_ent(const void *c, int create) {
  if (!c) return 0;
  unsigned h = (unsigned)(((uintptr_t)c >> 4) % CT_MAX);
  for (unsigned i = 0; i < 16; i++) {
    struct ct_ent *e = &ct_tab[(h + i) % CT_MAX];
    if (e->cond == c) return e;
    if (!e->cond && create) {
      uintptr_t expect = 0;
      if (__atomic_compare_exchange_n((uintptr_t *)&e->cond, &expect,
                                      (uintptr_t)c, 0, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED))
        return e;
      if (e->cond == c) return e;
    }
  }
  return 0;
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
#ifdef CT_OLD_WAIT
  /* Diagnostic-only rebuild knob (never set in a shipped build): reproduce the
   * L53 stall by resolving the GLIBC_2.2.5 compat entry, which builds a
   * private 0x30-byte "2.0" cond for a zeroed MonoW32Handle.  Used once to
   * print the handle→key identity row from a live stall. */
  real_wait = dlvsym(RTLD_NEXT, "pthread_cond_wait", "GLIBC_2.2.5");
#endif
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

/* One row per event, only for the cases that carry the question:
 *  - a signal/broadcast issued while this cond has a waiter (the pulse),
 *  - the main thread's own wait entry/return (bounded).
 * Everything else only bumps counters. */
static void ct_row(char kind, pthread_cond_t *c, pthread_mutex_t *m, long a, int r) {
  int main_t = pthread_equal(pthread_self(), ct_self_of_main);
  struct ct_ent *e = ct_ent(c, 0);
  unsigned inwait = e ? (e->waits - e->rets) : 0;
  int want = 0;
  if (kind == 'S' || kind == 'B') want = (inwait > 0) && (ct_print_n < CT_PRINT_MAX);
  if (kind == 'W' || kind == 'T') want = main_t && (ct_wait_n < CT_WAIT_ROWS);
  if (kind == 'w' || kind == 't') want = main_t && (ct_ret_n < CT_WAIT_ROWS);
  if (!want) return;
  if (kind == 'S' || kind == 'B') ct_print_n++;
  else if (kind == 'W' || kind == 'T') ct_wait_n++;
  else ct_ret_n++;

  char b[320], *p = b;
  long tid = syscall(CT_SYS_gettid);
  memcpy(p, "[CT] ", 5); p += 5;
  *p++ = kind;
  memcpy(p, " tid=", 5); p += 5; p = putdec(p, tid);
  memcpy(p, " cond=", 6); p += 6; p = puthex(p, (uint64_t)(uintptr_t)c);
  if (kind == 'W' || kind == 'T' || kind == 'w' || kind == 't') {
    memcpy(p, " mutex=", 7); p += 7; p = puthex(p, (uint64_t)(uintptr_t)m);
  }
  if (kind == 'T') { memcpy(p, " to=", 4); p += 4; p = putdec(p, a); }
  if (kind == 'w' || kind == 't') { memcpy(p, " ret=", 5); p += 5; p = putdec(p, r); }
  /* glibc 2.31 picks the wake futex from the waiter's group:
     key = cond + 0x28 + 4*g.  Print both candidates so the kernel's
     [FUTEX] rows can be matched byte for byte. */
  memcpy(p, " inwait=", 8); p += 8; p = putdec(p, (long)inwait);
  memcpy(p, " k0=", 4); p += 4; p = puthex(p, (uint64_t)(uintptr_t)c + 0x28);
  memcpy(p, " k1=", 4); p += 4; p = puthex(p, (uint64_t)(uintptr_t)c + 0x2c);
  if (main_t) { memcpy(p, " main=1", 7); p += 7; }
  *p++ = '\n';
  write(2, b, p - b);
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  ct_resolve();
  if (!ct_on) return real_wait(c, m);
  struct ct_ent *e = ct_ent(c, 1);
  if (e) __atomic_fetch_add(&e->waits, 1u, __ATOMIC_RELAXED);
  ct_row('W', c, m, 0, 0);
  int r = real_wait(c, m);
  if (e) __atomic_fetch_add(&e->rets, 1u, __ATOMIC_RELAXED);
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
  struct ct_ent *e = ct_ent(c, 1);
  if (e) __atomic_fetch_add(&e->waits, 1u, __ATOMIC_RELAXED);
  ct_row('T', c, m, ms, 0);
  int r = real_timedwait(c, m, ts);
  if (e) __atomic_fetch_add(&e->rets, 1u, __ATOMIC_RELAXED);
  ct_row('t', c, m, ms, r);
  return r;
}

int pthread_cond_signal(pthread_cond_t *c) {
  ct_resolve();
  if (!ct_on) return real_signal(c);
  struct ct_ent *e = ct_ent(c, 1);
  if (e) {
    __atomic_fetch_add(&e->sig, 1u, __ATOMIC_RELAXED);
    if (e->waits > e->rets) __atomic_fetch_add(&e->sigw, 1u, __ATOMIC_RELAXED);
  }
  ct_row('S', c, 0, 0, 0);
  return real_signal(c);
}

int pthread_cond_broadcast(pthread_cond_t *c) {
  ct_resolve();
  if (!ct_on) return real_broadcast(c);
  struct ct_ent *e = ct_ent(c, 1);
  if (e) {
    __atomic_fetch_add(&e->bc, 1u, __ATOMIC_RELAXED);
    if (e->waits > e->rets) __atomic_fetch_add(&e->bcw, 1u, __ATOMIC_RELAXED);
  }
  ct_row('B', c, 0, 0, 0);
  return real_broadcast(c);
}

/* Called by the launcher's stall sampler with a cond address taken from the
 * parked thread's own futex key (key-0x28) or from a MonoW32Handle it found on
 * that stack (handle+0x38).  Prints the counters that decide the layer. */
void ct_report(const void *cond, const char *tag) {
  ct_resolve();
  if (!ct_on || !cond) return;
  struct ct_ent *e = ct_ent(cond, 0);
  unsigned w = e ? e->waits : 0, r = e ? e->rets : 0;
  char b[320], *p = b;
  memcpy(p, "[CT2] tag=", 10); p += 10;
  { size_t n = 0; while (tag && tag[n] && n < 24) n++;
    memcpy(p, tag ? tag : "?", n); p += n; }
  memcpy(p, " cond=", 6); p += 6; p = puthex(p, (uint64_t)(uintptr_t)cond);
  memcpy(p, " waits=", 7); p += 7; p = putdec(p, (long)w);
  memcpy(p, " rets=", 6); p += 6; p = putdec(p, (long)r);
  memcpy(p, " inwait=", 8); p += 8; p = putdec(p, (long)(w - r));
  memcpy(p, " sig=", 5); p += 5; p = putdec(p, e ? (long)e->sig : 0);
  memcpy(p, " bc=", 4); p += 4; p = putdec(p, e ? (long)e->bc : 0);
  memcpy(p, " sigw=", 6); p += 6; p = putdec(p, e ? (long)e->sigw : 0);
  memcpy(p, " bcw=", 5); p += 5; p = putdec(p, e ? (long)e->bcw : 0);
  /* The cond's own glibc state: wseq/wrefs say whether it has a waiter group
     at all; gsig0/gsig1 are the two wake futex words. */
  if ((uintptr_t)cond > 0x1000000000ULL) {
    const volatile uint64_t *q = (const volatile uint64_t *)cond;
    const volatile uint32_t *d = (const volatile uint32_t *)cond;
    memcpy(p, " wseq=", 6); p += 6; p = puthex(p, q[0]);
    memcpy(p, " g1s=", 5); p += 5; p = puthex(p, q[1]);
    memcpy(p, " wrefs=", 7); p += 7; p = puthex(p, d[9]);
    memcpy(p, " gsig0=", 7); p += 7; p = puthex(p, d[10]);
    memcpy(p, " gsig1=", 7); p += 7; p = puthex(p, d[11]);
  }
  *p++ = '\n';
  write(2, b, p - b);
}
