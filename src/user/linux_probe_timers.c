/*
 * src/user/linux_probe_timers.c — the timer family a managed runtime's
 * scheduler may need, row by row, host vs guest.
 *
 * ORDINARY musl Linux program: the same static-PIE binary runs on the
 * workstation (reference kernel) and inside the intel guest (HobbyOS compat).
 * Rows are "[LP] ...", the last is "[LP] done rc=<fails>".
 *
 * WHY (docs/rimworld-launch-log.md §35): the game's main thread waits on a
 * Task completion while all seven pool items have completed.  A Task whose
 * continuation needs a *timer* (Task.Delay, a Timer, a timed WaitHandle) would
 * hang exactly like that if the guest's timer syscalls answered ENOSYS or
 * ignored their timeout, so every timer primitive the runtime can reach is
 * measured here, with the ELAPSED MILLISECONDS printed next to the expected
 * value: a wrong answer and a missing syscall are different defects and both
 * are visible in one row.
 *
 * Covered:
 *   timerfd_create/settime/gettime (+ read blocking until expiry, + poll)
 *   setitimer(ITIMER_REAL) + SIGALRM, and alarm(2) (which glibc builds on it)
 *   timer_create(CLOCK_MONOTONIC, SIGEV_SIGNAL) + settime/gettime/delete
 *   epoll_wait with a timeout on an empty set
 *   poll(NULL,0,ms) and select(0,...,&tv) timeouts
 *   nanosleep, as the baseline every one of the above is built on
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int fails;
static volatile sig_atomic_t sig_hits;

static void row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[LP] ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(ap);
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A timeout contract: the call must return in [want, want+slack] ms. */
static void timed(const char *what, long got, long want, long slack, int rc) {
  int ok = (rc == 0 && got >= want - 2 && got <= want + slack);
  if (!ok) fails++;
  row("%-28s rc=%d elapsed=%ldms want=%ld..%ld %s", what, rc, got, want,
      want + slack, ok ? "OK" : "MISMATCH");
}

static void on_alarm(int sig) { (void)sig; sig_hits++; }

static int wait_signal(long ms) {
  /* sigsuspend over an empty mask with SIGALRM unblocked: the handler runs and
     the call returns EINTR.  A timeout path that never delivers the signal
     leaves this spinning, so the probe bounds it with its own deadline by
     arming alarm() as a backstop only after the primary test. */
  sigset_t empty;
  sigemptyset(&empty);
  long t0 = now_ms();
  while (!sig_hits && now_ms() - t0 < ms + 2000) sigsuspend(&empty);
  return sig_hits ? 0 : -ETIMEDOUT;
}

/* ---- 1. timerfd -------------------------------------------------------- */
static void probe_timerfd(void) {
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  if (fd < 0) {
    fails++;
    row("timerfd_create              rc=-1 errno=%d(%s) UNSUPPORTED", errno,
        strerror(errno));
    return;
  }
  row("timerfd_create              fd=%d OK", fd);

  struct itimerspec its;
  memset(&its, 0, sizeof its);
  its.it_value.tv_nsec = 50 * 1000000L; /* 50 ms one-shot */
  long t0 = now_ms();
  int rc = timerfd_settime(fd, 0, &its, NULL);
  row("timerfd_settime             rc=%d errno=%d", rc, rc ? errno : 0);
  if (rc != 0) fails++;

  struct itimerspec cur;
  memset(&cur, 0, sizeof cur);
  if (timerfd_gettime(fd, &cur) == 0)
    row("timerfd_gettime             left=%ldms", (long)cur.it_value.tv_sec * 1000 +
                                                     cur.it_value.tv_nsec / 1000000);
  else { fails++; row("timerfd_gettime             errno=%d UNSUPPORTED", errno); }

  uint64_t exp = 0;
  ssize_t n = read(fd, &exp, sizeof exp);
  long got = now_ms() - t0;
  timed("timerfd read (50ms)", got, 50, 250, n == (ssize_t)sizeof exp ? 0 : -1);
  row("timerfd read count          n=%ld expirations=%llu", (long)n,
      (unsigned long long)exp);
  if (exp < 1) fails++;

  /* periodic + poll: 20 ms period, poll must report readable. */
  memset(&its, 0, sizeof its);
  its.it_value.tv_nsec = 20 * 1000000L;
  its.it_interval.tv_nsec = 20 * 1000000L;
  if (timerfd_settime(fd, 0, &its, NULL) != 0) { fails++; row("timerfd periodic settime FAILED errno=%d", errno); }
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  t0 = now_ms();
  int pr = poll(&pfd, 1, 500);
  timed("poll(timerfd) 20ms", now_ms() - t0, 20, 250, pr == 1 ? 0 : -1);
  if (pr == 1) { exp = 0; (void)!read(fd, &exp, sizeof exp); row("timerfd poll revents=0x%x exp=%llu", pfd.revents, (unsigned long long)exp); }
  close(fd);
}

/* ---- 2. setitimer / alarm --------------------------------------------- */
static void probe_itimer(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_alarm;
  sigaction(SIGALRM, &sa, NULL);

  struct itimerval it;
  memset(&it, 0, sizeof it);
  it.it_value.tv_usec = 50 * 1000; /* 50 ms */
  sig_hits = 0;
  long t0 = now_ms();
  int rc = setitimer(ITIMER_REAL, &it, NULL);
  if (rc != 0) {
    fails++;
    row("setitimer(50ms)             rc=-1 errno=%d(%s) UNSUPPORTED", errno,
        strerror(errno));
    return;
  }
  struct itimerval cur;
  memset(&cur, 0, sizeof cur);
  getitimer(ITIMER_REAL, &cur);
  row("setitimer                   rc=0 left=%ldms", (long)cur.it_value.tv_sec * 1000 +
                                                        cur.it_value.tv_usec / 1000);
  int w = wait_signal(50);
  timed("setitimer SIGALRM (50ms)", now_ms() - t0, 50, 250, w);
  row("setitimer handler hits      %d", (int)sig_hits);
  memset(&it, 0, sizeof it);
  setitimer(ITIMER_REAL, &it, NULL); /* disarm */

  sig_hits = 0;
  t0 = now_ms();
  unsigned left = alarm(1);
  row("alarm(1)                    prev=%u", left);
  w = wait_signal(1000);
  timed("alarm(1) SIGALRM (1000ms)", now_ms() - t0, 1000, 400, w);
  alarm(0);
}

/* ---- 3. POSIX timer_create -------------------------------------------- */
static void probe_posix_timer(void) {
  struct sigevent sev;
  memset(&sev, 0, sizeof sev);
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGALRM;
  timer_t tid;
  int rc = timer_create(CLOCK_MONOTONIC, &sev, &tid);
  if (rc != 0) {
    fails++;
    row("timer_create                rc=-1 errno=%d(%s) UNSUPPORTED", errno,
        strerror(errno));
    return;
  }
  row("timer_create                OK");

  struct itimerspec its;
  memset(&its, 0, sizeof its);
  its.it_value.tv_nsec = 60 * 1000000L;
  sig_hits = 0;
  long t0 = now_ms();
  rc = timer_settime(tid, 0, &its, NULL);
  row("timer_settime               rc=%d errno=%d", rc, rc ? errno : 0);
  if (rc != 0) { fails++; return; }

  struct itimerspec cur;
  memset(&cur, 0, sizeof cur);
  if (timer_gettime(tid, &cur) == 0)
    row("timer_gettime               left=%ldms", (long)cur.it_value.tv_sec * 1000 +
                                                     cur.it_value.tv_nsec / 1000000);
  else { fails++; row("timer_gettime               errno=%d UNSUPPORTED", errno); }

  int w = wait_signal(60);
  timed("timer_create SIGALRM (60ms)", now_ms() - t0, 60, 250, w);
  row("timer overrun               %d", timer_getoverrun(tid));
  rc = timer_delete(tid);
  row("timer_delete                rc=%d errno=%d", rc, rc ? errno : 0);
  if (rc != 0) fails++;
}

/* ---- 4. epoll / poll / select / nanosleep ----------------------------- */
static void probe_waits(void) {
  int ep = epoll_create1(EPOLL_CLOEXEC);
  if (ep < 0) {
    fails++;
    row("epoll_create1               errno=%d(%s) UNSUPPORTED", errno,
        strerror(errno));
  } else {
    struct epoll_event ev;
    long t0 = now_ms();
    int n = epoll_wait(ep, &ev, 1, 60);
    timed("epoll_wait empty (60ms)", now_ms() - t0, 60, 250, n == 0 ? 0 : -1);
    close(ep);
  }

  long t0 = now_ms();
  int r = poll(NULL, 0, 60);
  timed("poll(NULL,0,60)", now_ms() - t0, 60, 250, r == 0 ? 0 : -1);

  struct timeval tv = {0, 60 * 1000};
  t0 = now_ms();
  r = select(0, NULL, NULL, NULL, &tv);
  timed("select(0,...,60ms)", now_ms() - t0, 60, 250, r == 0 ? 0 : -1);

  struct timespec ts = {0, 60 * 1000000L};
  t0 = now_ms();
  r = nanosleep(&ts, NULL);
  timed("nanosleep(60ms)", now_ms() - t0, 60, 250, r == 0 ? 0 : -1);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  row("timers probe start");
  probe_timerfd();
  probe_itimer();
  probe_posix_timer();
  probe_waits();
  row("done rc=%d", fails);
  return fails ? 1 : 0;
}
