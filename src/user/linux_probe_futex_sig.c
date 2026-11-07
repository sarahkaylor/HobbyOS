/*
 * src/user/linux_probe_futex_sig.c — Linux-ABI probe: a pending signal must
 * interrupt a blocking compat wait the way Linux does.
 *
 * ORDINARY musl Linux program: no HobbyOS header, so the same static-PIE
 * binary runs on the workstation (reference kernel) and inside the intel
 * guest (HobbyOS's compat layer).  Every row is "[LP] ..." and the last is
 * "[LP] done rc=<fails>"; tools/gate/t3_linux_probe.sh futex_sig only
 * accepts rc=0.
 *
 * WHAT EACH CASE PROVES (and why the game needs it)
 * ------------------------------------------------
 * Mono's Boehm GC stops the world with pthread_kill(tid, SIGPWR=30) and
 * resumes it with pthread_kill(tid, SIGXCPU=24).  A worker parked in a
 * futex/sigsuspend/poll must (a) stop sleeping, (b) run the handler, and
 * only THEN (c) see the syscall return -EINTR.  If the handler is deferred
 * until some later trap, glibc re-enters the wait and the handshake wedges
 * (measured: RimWorld's 8th suspend, [SIGD] with no [RT]).
 *
 *   A  FUTEX_WAIT, infinite timeout, handler WITHOUT SA_RESTART: the handler
 *      runs before the wait's return is visible and futex returns -1/EINTR.
 *   B  FUTEX_WAIT with SA_RESTART: the handler runs once, the wait RESTARTS
 *      (worker still blocked), and only an explicit FUTEX_WAKE releases it
 *      with 0.  This is the discriminating pair: an implementation that
 *      always returns EINTR cannot produce B, and one that always restarts
 *      cannot produce A.
 *   C  rt_sigsuspend(130): blocks with a temporary mask, a signal allowed by
 *      that mask wakes it, the handler runs first, the syscall returns
 *      -EINTR and the pre-suspend mask is restored.  (Mono's suspend
 *      handler is exactly `ack; do { sigsuspend(mask); } while (stopped)`.)
 *   D  clock_nanosleep: a signal interrupts the sleep with -EINTR, handler
 *      first.  Linux never restarts clock_nanosleep across a handler.
 *   E  FUTEX_WAIT with a *masked* pending signal: the mask wins, so the wait
 *      is NOT interrupted (it times out).  Proves the interrupt consults the
 *      thread's signal mask instead of waking on any pending bit.
 *   F  poll(2) over an empty pipe: a signal interrupts it with -EINTR.
 *   G  read(2) on an empty pipe: a signal interrupts it with -EINTR.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_futex
#define SYS_futex 202
#endif
#ifndef SYS_rt_sigsuspend
#define SYS_rt_sigsuspend 130
#endif
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static int fails;

static void row(const char *fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

/* ---- shared fixtures ---------------------------------------------------- */

static volatile int word;
static volatile int handler_calls;

static void usr1_handler(int sig) { (void)sig; handler_calls++; }

static int install_usr1(int with_restart) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = usr1_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = with_restart ? SA_RESTART : 0;
  return sigaction(SIGUSR1, &sa, NULL);
}

static long futex_wait(int *uaddr, int expect, const struct timespec *to) {
  return syscall(SYS_futex, uaddr, FUTEX_WAIT, expect, to, NULL, 0);
}

static int futex_wake(int *uaddr) {
  return (int)syscall(SYS_futex, uaddr, FUTEX_WAKE, 1, NULL, NULL, 0);
}

/* The futex worker: block, record what the syscall returned and whether the
 * handler had already run at that instant. */
struct wait_result {
  int ret;      /* 0 = woken, -1 = error */
  int err;      /* errno when ret == -1 */
  int handler_before_return;
  int done;
};

static void *futex_thread(void *arg) {
  struct wait_result *r = (struct wait_result *)arg;
  errno = 0;
  long v = futex_wait((int *)&word, 0, NULL); /* infinite timeout */
  r->ret = (v == 0) ? 0 : -1;
  r->err = errno;
  r->handler_before_return = handler_calls;
  r->done = 1;
  return NULL;
}

/* Wait up to ms for *flag to become nonzero; 1 = it did. */
static int wait_flag(volatile int *flag, int ms) {
  for (int i = 0; i < ms / 10 + 1; i++) {
    if (*flag) return 1;
    struct timespec ts = {0, 10 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }
  return *flag != 0;
}

/* Give the (single) other thread time to reach its blocking syscall. */
static void settle(void) {
  struct timespec ts = {0, 120 * 1000 * 1000};
  nanosleep(&ts, NULL);
}

/* ---- A: FUTEX_WAIT without SA_RESTART must return EINTR ---------------- */

static void case_a(void) {
  struct wait_result r;
  pthread_t th;
  memset(&r, 0, sizeof r);
  handler_calls = 0;
  word = 0;
  if (install_usr1(0) != 0) {
    row("[LP] A futex-norestart FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  if (pthread_create(&th, NULL, futex_thread, &r) != 0) {
    row("[LP] A futex-norestart FAIL pthread_create\n");
    fails++;
    return;
  }
  settle();
  pthread_kill(th, SIGUSR1);
  int returned = wait_flag(&r.done, 1000);
  if (!returned) { /* unblock it so the probe cannot hang; the case fails */
    word = 1;
    (void)futex_wake((int *)&word);
    (void)wait_flag(&r.done, 1000);
  }
  pthread_join(th, NULL);
  int ok = returned && r.ret == -1 && r.err == EINTR &&
           r.handler_before_return >= 1;
  row("[LP] A futex-norestart returned=%d ret=%d errno=%d(%s) "
      "handler_before_return=%d calls=%d ok=%d\n",
      returned, r.ret, r.err, r.err == EINTR ? "EINTR" : strerror(r.err),
      r.handler_before_return, handler_calls, ok);
  if (!ok) fails++;
}

/* ---- B: FUTEX_WAIT with SA_RESTART must restart the wait --------------- */

static void case_b(void) {
  struct wait_result r;
  pthread_t th;
  memset(&r, 0, sizeof r);
  handler_calls = 0;
  word = 0;
  if (install_usr1(SA_RESTART) != 0) {
    row("[LP] B futex-restart FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  if (pthread_create(&th, NULL, futex_thread, &r) != 0) {
    row("[LP] B futex-restart FAIL pthread_create\n");
    fails++;
    return;
  }
  settle();
  pthread_kill(th, SIGUSR1);
  /* The handler must run, but the wait must NOT return: SA_RESTART. */
  struct timespec ts = {0, 200 * 1000 * 1000};
  nanosleep(&ts, NULL);
  int still_blocked = !r.done;
  int calls_after_signal = handler_calls;
  word = 1;
  (void)futex_wake((int *)&word);
  int returned = wait_flag(&r.done, 2000);
  pthread_join(th, NULL);
  int ok = still_blocked && calls_after_signal >= 1 && returned &&
           r.ret == 0 && r.err == 0;
  row("[LP] B futex-restart still_blocked=%d calls_after_signal=%d "
      "returned=%d ret=%d errno=%d(%s) ok=%d\n",
      still_blocked, calls_after_signal, returned, r.ret, r.err,
      r.err == EINTR ? "EINTR" : strerror(r.err), ok);
  if (!ok) fails++;
}

/* ---- C: rt_sigsuspend -------------------------------------------------- */

struct sus_result {
  int ret, err, handler_before_return, done, mask_still_blocked;
};

static void *sigsuspend_thread(void *arg) {
  struct sus_result *r = (struct sus_result *)arg;
  sigset_t block, empty;
  sigemptyset(&block);
  sigaddset(&block, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &block, NULL);
  sigemptyset(&empty); /* allow everything while suspended */
  errno = 0;
  int v = sigsuspend(&empty);
  r->ret = v;
  r->err = errno;
  r->handler_before_return = handler_calls;
  /* After sigsuspend the pre-suspend mask must be back: SIGUSR1 blocked. */
  sigset_t cur;
  pthread_sigmask(SIG_SETMASK, NULL, &cur);
  r->mask_still_blocked = sigismember(&cur, SIGUSR1) == 1;
  r->done = 1;
  return NULL;
}

static void case_c(void) {
  struct sus_result r;
  pthread_t th;
  memset(&r, 0, sizeof r);
  handler_calls = 0;
  if (install_usr1(0) != 0) {
    row("[LP] C rt-sigsuspend FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  if (pthread_create(&th, NULL, sigsuspend_thread, &r) != 0) {
    row("[LP] C rt-sigsuspend FAIL pthread_create\n");
    fails++;
    return;
  }
  settle();
  pthread_kill(th, SIGUSR1);
  int returned = wait_flag(&r.done, 1500);
  pthread_join(th, NULL);
  int ok = returned && r.ret == -1 && r.err == EINTR &&
           r.handler_before_return >= 1 && r.mask_still_blocked;
  row("[LP] C rt-sigsuspend returned=%d ret=%d errno=%d(%s) "
      "handler_before_return=%d mask_restored=%d ok=%d\n",
      returned, r.ret, r.err, r.err == EINTR ? "EINTR" : strerror(r.err),
      r.handler_before_return, r.mask_still_blocked, ok);
  if (!ok) fails++;
}

/* ---- D: clock_nanosleep interrupted ------------------------------------ */

struct sleep_result {
  int ret, err, handler_before_return, done;
};

static void *sleep_thread(void *arg) {
  struct sleep_result *r = (struct sleep_result *)arg;
  struct timespec req = {5, 0};
  errno = 0;
  int v = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
  r->ret = v;
  r->err = errno;
  r->handler_before_return = handler_calls;
  r->done = 1;
  return NULL;
}

static void case_d(void) {
  struct sleep_result r;
  pthread_t th;
  memset(&r, 0, sizeof r);
  handler_calls = 0;
  if (install_usr1(0) != 0) {
    row("[LP] D clock_nanosleep FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  if (pthread_create(&th, NULL, sleep_thread, &r) != 0) {
    row("[LP] D clock_nanosleep FAIL pthread_create\n");
    fails++;
    return;
  }
  settle();
  pthread_kill(th, SIGUSR1);
  int returned = wait_flag(&r.done, 1500);
  pthread_join(th, NULL);
  /* clock_nanosleep returns the error NUMBER (not -1/errno) on Linux. */
  int ok = returned && (r.ret == EINTR || (r.ret == -1 && r.err == EINTR)) &&
           r.handler_before_return >= 1;
  row("[LP] D clock_nanosleep returned=%d ret=%d errno=%d(%s) "
      "handler_before_return=%d ok=%d\n",
      returned, r.ret, r.err, r.err == EINTR ? "EINTR" : strerror(r.err),
      r.handler_before_return, ok);
  if (!ok) fails++;
}

/* ---- E: a MASKED pending signal must not interrupt the wait ------------ */

static void case_e(void) {
  sigset_t block;
  struct timespec to = {0, 300 * 1000 * 1000}; /* 300 ms */
  handler_calls = 0;
  if (install_usr1(0) != 0) {
    row("[LP] E futex-masked FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  sigemptyset(&block);
  sigaddset(&block, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &block, NULL);
  pthread_kill(pthread_self(), SIGUSR1); /* pending, but blocked */
  word = 0;
  errno = 0;
  long v = futex_wait((int *)&word, 0, &to);
  int err = errno;
  int calls_during = handler_calls;
  sigset_t cur;
  pthread_sigmask(SIG_SETMASK, NULL, &cur);
  int still_pending = sigpending(&cur) == 0 && sigismember(&cur, SIGUSR1) == 1;
  pthread_sigmask(SIG_UNBLOCK, &block, NULL); /* now the handler must run */
  int calls_after = handler_calls;
  int ok = v == -1 && err == ETIMEDOUT && calls_during == 0 &&
           still_pending && calls_after == 1;
  row("[LP] E futex-masked ret=%ld errno=%d(%s) calls_during=%d "
      "still_pending=%d calls_after_unblock=%d ok=%d\n",
      v, err, err == ETIMEDOUT ? "ETIMEDOUT" : strerror(err), calls_during,
      still_pending, calls_after, ok);
  if (!ok) fails++;
}

/* ---- F: poll interrupted ----------------------------------------------- */

struct poll_arg { int fd; int ret, err, calls, done; };

static void *poll_thread(void *arg) {
  struct poll_arg *q = (struct poll_arg *)arg;
  struct pollfd qp = {q->fd, POLLIN, 0};
  errno = 0;
  q->ret = poll(&qp, 1, 5000);
  q->err = errno;
  q->calls = handler_calls;
  q->done = 1;
  return NULL;
}

static void case_f(void) {
  int pfd[2];
  struct poll_arg a;
  pthread_t th;
  memset(&a, 0, sizeof a);
  handler_calls = 0;
  if (pipe(pfd) != 0) {
    row("[LP] F poll FAIL pipe errno=%d\n", errno);
    fails++;
    return;
  }
  if (install_usr1(0) != 0) {
    row("[LP] F poll FAIL sigaction errno=%d\n", errno);
    fails++;
    close(pfd[0]);
    close(pfd[1]);
    return;
  }
  a.fd = pfd[0];
  if (pthread_create(&th, NULL, poll_thread, &a) != 0) {
    row("[LP] F poll FAIL pthread_create\n");
    fails++;
    close(pfd[0]);
    close(pfd[1]);
    return;
  }
  settle();
  pthread_kill(th, SIGUSR1);
  int returned = wait_flag(&a.done, 1500);
  pthread_join(th, NULL);
  int ok = returned && a.ret == -1 && a.err == EINTR && a.calls >= 1;
  row("[LP] F poll returned=%d ret=%d errno=%d(%s) handler_before_return=%d ok=%d\n",
      returned, a.ret, a.err, a.err == EINTR ? "EINTR" : strerror(a.err),
      a.calls, ok);
  if (!ok) fails++;
  close(pfd[0]);
  close(pfd[1]);
}

/* ---- G: read on an empty pipe interrupted ------------------------------ */

struct read_arg { int fd; int ret, err, calls, done; };

static void *read_thread(void *arg) {
  struct read_arg *q = (struct read_arg *)arg;
  char c;
  errno = 0;
  q->ret = (int)read(q->fd, &c, 1);
  q->err = errno;
  q->calls = handler_calls;
  q->done = 1;
  return NULL;
}

static void case_g(void) {
  int pfd[2];
  struct read_arg a;
  pthread_t th;
  memset(&a, 0, sizeof a);
  handler_calls = 0;
  if (pipe(pfd) != 0) {
    row("[LP] G read-pipe FAIL pipe errno=%d\n", errno);
    fails++;
    return;
  }
  if (install_usr1(0) != 0) {
    row("[LP] G read-pipe FAIL sigaction errno=%d\n", errno);
    fails++;
    close(pfd[0]);
    close(pfd[1]);
    return;
  }
  a.fd = pfd[0];
  if (pthread_create(&th, NULL, read_thread, &a) != 0) {
    row("[LP] G read-pipe FAIL pthread_create\n");
    fails++;
    close(pfd[0]);
    close(pfd[1]);
    return;
  }
  settle();
  pthread_kill(th, SIGUSR1);
  int returned = wait_flag(&a.done, 1500);
  pthread_join(th, NULL);
  int ok = returned && a.ret == -1 && a.err == EINTR && a.calls >= 1;
  row("[LP] G read-pipe returned=%d ret=%d errno=%d(%s) handler_before_return=%d ok=%d\n",
      returned, a.ret, a.err, a.err == EINTR ? "EINTR" : strerror(a.err),
      a.calls, ok);
  if (!ok) fails++;
  close(pfd[0]);
  close(pfd[1]);
}

int main(void) {
  row("[LP] futex-sig probe start pid=%d\n", (int)getpid());
  case_a();
  case_b();
  case_c();
  case_d();
  case_e();
  case_f();
  case_g();
  row("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
