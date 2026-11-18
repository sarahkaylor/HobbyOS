/*
 * src/user/linux_probe_sigint.c — a handled SIGINT must interrupt a thread
 * parked in epoll_wait(-1) at once (H4-kglitch, docs/cp-kglitch.md §6; H2-F2:
 * `wineserver -k` ignored for 10.5 s in 7 of 12 runs, then SIGKILLed).
 *
 * ORDINARY musl Linux program (runs on the strix host and in the guest).
 * Rows "[LP] ...", last "[LP] done rc=<fails>".
 *
 * The child copies wineserver's signal setup (server/signal.c): SIGCHLD,
 * SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1 and SIGIO handlers, every one with
 * sa_mask = all of those and sa_flags = 0, each writing one byte to a pipe
 * that the main loop's epoll set watches.  The main loop is a raw
 * SYS_epoll_wait(ep, ev, 8, -1) (glibc 2.31's epoll_wait, server/fd.c:611);
 * on every return it drains the pipe and acks the parent with what it saw.
 *
 *   I1  kill(child, SIGINT) x ROUNDS: each acked, worst latency < 200 ms
 *   I2  SIGUSR1 then SIGINT back to back: the SIGINT arrives while the
 *       SIGUSR1 handler runs (SIGINT masked by its sa_mask) and must be
 *       delivered after its rt_sigreturn
 *   I3  the child's mask at the top of every loop has no SIGINT
 *       (rt_sigprocmask readback: a handler's sa_mask not restored by
 *       rt_sigreturn would leave it there)
 *   I4  the pid from F_GETLK (wineserver -k's kill_lock_owner): the child
 *       holds an F_SETLK write lock, the parent reads l_pid and SIGINTs it
 *   I7  as I1 with a second, busy fd in the set (a "client" write before each
 *       SIGINT), a 50 ms epoll timeout (server/fd.c's main loop) and SIGCONT
 *       right after the SIGINT (kill_lock_owner sends both)
 *   I8  as I7 with a second thread parked in FUTEX_WAIT for ever
 *   I9  as I7 with 96 more idle pipes in the epoll set (wineserver had 64+)
 *   I6  nested handlers: SIGUSR2's handler raise()s SIGWINCH (handled, not in
 *       its sa_mask), so two frames are planted before either returns; both
 *       return with the interrupted values intact.  The console must carry
 *       no "[SFX] MISMATCH" row for it (tools/gate/lp_check_sigint.py): the
 *       kernel's frame check compared the outer return with the inner plant.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROUNDS 60

static int hpipe[2];   /* handlers -> child main loop */
static int ackpipe[2]; /* child -> parent */
static volatile int in_usr1;

static void h_any(int sig) {
  unsigned char b = (unsigned char)sig;
  if (sig == SIGUSR1) {
    /* stay in the handler a while, so a SIGINT sent now finds it masked */
    in_usr1 = 1;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do
      clock_gettime(CLOCK_MONOTONIC, &t1);
    while ((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec) < 5000000L);
    in_usr1 = 0;
  }
  (void)!write(hpipe[1], &b, 1);
}

static long now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

static int cmode;     /* 0 plain; 1 busy fd + 50 ms timeout; 2 + a futex-parked thread */
static int busypipe[2];
static volatile int futex_word;
static void *parked(void *a) {
  (void)a;
  for (;;)
    syscall(SYS_futex, &futex_word, 0 /* FUTEX_WAIT */, 0, 0, 0, 0);
  return 0;
}

static void child_main(const char *lockpath) {
  int sigs[] = {SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGIO};
  sigset_t m;
  sigemptyset(&m);
  for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
    sigaddset(&m, sigs[i]);
  for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_any;
    sa.sa_mask = m;
    sa.sa_flags = 0;
    sigaction(sigs[i], &sa, 0);
  }
  if (lockpath) {
    int fd = open(lockpath, O_RDWR | O_CREAT, 0600);
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fd < 0 || fcntl(fd, F_SETLK, &fl) != 0)
      _exit(5);
  }
  int ep = epoll_create1(0);
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN;
  ev.data.fd = hpipe[0];
  epoll_ctl(ep, EPOLL_CTL_ADD, hpipe[0], &ev);
  fcntl(hpipe[0], F_SETFL, O_NONBLOCK);
  if (cmode >= 1) {
    ev.data.fd = busypipe[0];
    epoll_ctl(ep, EPOLL_CTL_ADD, busypipe[0], &ev);
    fcntl(busypipe[0], F_SETFL, O_NONBLOCK);
  }
  if (cmode == 2) {
    pthread_t t;
    pthread_create(&t, 0, parked, 0);
  }
  if (cmode == 3) {
    /* wineserver's set held 64+ client pipes ([EPOLLHW] n=64): past the
       kernel's partial-scan threshold, so only notified keys are rechecked */
    for (int i = 0; i < 96; i++) {
      int pp[2];
      if (pipe(pp))
        break;
      ev.data.fd = pp[0];
      epoll_ctl(ep, EPOLL_CTL_ADD, pp[0], &ev);
    }
  }
  unsigned char ready = 0xff;
  (void)!write(ackpipe[1], &ready, 1);
  for (;;) {
    sigset_t cur;
    sigemptyset(&cur);
    syscall(SYS_rt_sigprocmask, SIG_BLOCK, 0, &cur, 8);
    unsigned char masked = sigismember(&cur, SIGINT) ? 0x80 : 0;
    struct epoll_event out[8];
    long r = syscall(SYS_epoll_wait, ep, out, 8, cmode ? 50 : -1);
    int eintr = (r < 0 && errno == EINTR);
    if (cmode >= 1) {
      char junk[256]; /* serve the "clients" (wineserver's request pipes) */
      while (read(busypipe[0], junk, sizeof junk) > 0)
        ;
    }
    unsigned char got[32];
    ssize_t n = read(hpipe[0], got, sizeof got);
    for (ssize_t i = 0; i < n; i++) {
      if (got[i] == SIGTERM)
        _exit(0);
      unsigned char a = (unsigned char)(got[i] | masked | (eintr ? 0x40 : 0));
      (void)!write(ackpipe[1], &a, 1);
    }
  }
}

/* wait for an ack byte naming `sig` (other acks are consumed); -1 on timeout */
static long wait_ack(int sig, int ms, int *maskflag) {
  long t0 = now_us();
  for (;;) {
    long left = ms - (now_us() - t0) / 1000;
    if (left <= 0)
      return -1;
    struct pollfd pf = {ackpipe[0], POLLIN, 0};
    if (poll(&pf, 1, (int)left) <= 0)
      continue;
    unsigned char a;
    if (read(ackpipe[0], &a, 1) != 1)
      continue;
    if (a & 0x80)
      *maskflag = 1;
    if ((a & 0x3f) == sig)
      return now_us() - t0;
  }
}

static int fails;

static volatile int depth, maxdepth, inner_ok;
static void h_inner(int sig) {
  (void)sig;
  if (++depth > maxdepth)
    maxdepth = depth;
  depth--;
  inner_ok++;
}
static void h_outer(int sig) {
  (void)sig;
  if (++depth > maxdepth)
    maxdepth = depth;
  volatile long canary = 0x1234567;
  raise(SIGWINCH); /* delivered at once: a second frame on top of this one */
  if (canary != 0x1234567)
    inner_ok = -1000;
  depth--;
}

static pid_t start(const char *lockpath) {
  if (pipe(hpipe) || pipe(ackpipe) || pipe(busypipe))
    return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(ackpipe[0]);
    child_main(lockpath);
  }
  close(hpipe[0]);
  close(hpipe[1]);
  close(ackpipe[1]);
  close(busypipe[0]);
  unsigned char b = 0;
  struct pollfd pf = {ackpipe[0], POLLIN, 0};
  if (poll(&pf, 1, 5000) != 1 || read(ackpipe[0], &b, 1) != 1 || b != 0xff)
    return -1;
  return pid;
}

static void stop(pid_t pid) {
  kill(pid, SIGTERM);
  int st;
  for (int i = 0; i < 100 && waitpid(pid, &st, WNOHANG) == 0; i++)
    usleep(20000);
  kill(pid, SIGKILL);
  waitpid(pid, &st, 0);
  close(ackpipe[0]);
  close(busypipe[1]);
}

static void run(const char *id, const char *what, pid_t pid, int with_usr1) {
  int lost = 0, masked = 0;
  long worst = 0;
  for (int i = 0; i < ROUNDS; i++) {
    usleep(2000); /* let it park in epoll_wait */
    if (with_usr1) {
      kill(pid, SIGUSR1);
      usleep(1000); /* now inside the 5 ms handler */
    }
    if (cmode >= 1)
      (void)!write(busypipe[1], "req", 3); /* a client request in flight */
    kill(pid, SIGINT);
    kill(pid, SIGCONT); /* kill_lock_owner sends SIGCONT right after */
    long us = wait_ack(SIGINT, 2000, &masked);
    if (us < 0)
      lost++;
    else if (us > worst)
      worst = us;
  }
  int ok = lost == 0 && worst < 200000 && !masked;
  if (!ok)
    fails++;
  printf("[LP] %s %-46s rounds=%d lost=%d worst_us=%ld mask_has_sigint=%d -> %s\n", id, what, ROUNDS,
         lost, worst, masked, ok ? "PASS" : "FAIL");
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  pid_t pid = start(0);
  if (pid < 0) {
    printf("[LP] I1 child setup failed -> FAIL\n[LP] done rc=1\n");
    return 1;
  }
  run("I1", "SIGINT wakes epoll_wait(-1)", pid, 0);
  run("I2", "SIGINT sent during a SIGUSR1 handler", pid, 1);
  stop(pid);

  cmode = 1;
  pid = start(0);
  if (pid > 0) {
    run("I7", "busy set, 50 ms timeout, SIGINT+SIGCONT", pid, 0);
    stop(pid);
  } else {
    printf("[LP] I7 child setup failed -> FAIL\n");
    fails++;
  }
  cmode = 2;
  pid = start(0);
  if (pid > 0) {
    run("I8", "I7 + a second thread parked in FUTEX_WAIT", pid, 0);
    stop(pid);
  } else {
    printf("[LP] I8 child setup failed -> FAIL\n");
    fails++;
  }
  cmode = 3;
  pid = start(0);
  if (pid > 0) {
    run("I9", "I7 with 96 more idle pipes in the set", pid, 0);
    stop(pid);
  } else {
    printf("[LP] I9 child setup failed -> FAIL\n");
    fails++;
  }
  cmode = 0;

  /* I3 is checked in every round above (mask_has_sigint); say so once */
  printf("[LP] I3 %-46s (see mask_has_sigint in I1/I2)\n", "handler sa_mask restored by rt_sigreturn");

  char path[] = "/tmp/lp_sigint_lock";
  pid = start(path);
  if (pid < 0) {
    printf("[LP] I4 child setup failed -> FAIL\n");
    fails++;
  } else {
    int fd = open(path, O_RDWR);
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    int r = fd >= 0 ? fcntl(fd, F_GETLK, &fl) : -1;
    int okpid = r == 0 && fl.l_type == F_WRLCK && fl.l_pid == pid;
    printf("[LP] I4 %-46s rc=%d l_type=%d l_pid=%d child=%d -> %s\n", "F_GETLK names the lock owner", r,
           fl.l_type, (int)fl.l_pid, (int)pid, okpid ? "PASS" : "FAIL");
    if (!okpid)
      fails++;
    if (r == 0 && fl.l_pid > 0)
      run("I5", "SIGINT to F_GETLK's pid wakes epoll_wait", fl.l_pid, 0);
    if (fd >= 0)
      close(fd);
    stop(pid);
    unlink(path);
  }
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_inner;
    sigaction(SIGWINCH, &sa, 0);
    sa.sa_handler = h_outer;
    sigaction(SIGUSR2, &sa, 0);
    volatile long before = 0x55aa;
    for (int i = 0; i < 20; i++)
      raise(SIGUSR2);
    int ok = maxdepth == 2 && inner_ok == 20 && before == 0x55aa;
    if (!ok)
      fails++;
    printf("[LP] I6 %-46s rounds=20 maxdepth=%d inner=%d -> %s (+ console: no [SFX] MISMATCH)\n",
           "nested handler frames return intact", maxdepth, inner_ok, ok ? "PASS" : "FAIL");
  }
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
