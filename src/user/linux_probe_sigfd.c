/*
 * src/user/linux_probe_sigfd.c — signalfd/signalfd4 and blocked SIG_DFL
 * signals (C1-k6, docs/cp-k6.md §4).
 *
 * ORDINARY musl Linux program (runs on the strix host and in the guest).
 * Rows "[LP] ...", last "[LP] done rc=<fails>".
 *
 *   S1  signalfd4(-1, {USR1,USR2,CHLD,TERM}, 8, SFD_NONBLOCK|SFD_CLOEXEC) -> fd
 *   S2  nothing pending: read -> EAGAIN; poll -> not readable
 *   S3  raise(SIGUSR1) (tgkill): poll readable, read = one record,
 *       ssi_signo 10, ssi_code SI_TKILL; then nothing pending
 *   S4  kill(getpid(), SIGUSR1): ssi_code SI_USER
 *   S5  two pending (USR1, USR2) and a 3-record buffer: 2 records, in order
 *   S6  a blocking signalfd read on a worker thread is ended by a kill
 *   S7  epoll_wait on the signalfd wakes for a kill
 *   S8  SIGCHLD (blocked, SIG_DFL) of an exiting child: CLD_EXITED, the
 *       child's pid and exit status 5
 *   S9  a BLOCKED SIG_DFL SIGTERM does not kill; the signalfd reads it
 *   S10 signalfd(fd, {USR2}): the mask is replaced (USR1 no longer read)
 *   S11 the errors: sizemask 4, bad flags, a non-signalfd fd -> EINVAL
 *   S12 ssi_pid is the sending process (a forked child's kill)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_signalfd4
#define SYS_signalfd4 289
#endif
#define SFD_CLOEXEC_ 02000000
#define SFD_NONBLOCK_ 04000

struct ssi {
  uint32_t signo;
  int32_t err;
  int32_t code;
  uint32_t pid;
  uint32_t uid;
  int32_t fd;
  uint32_t tid;
  uint32_t band;
  uint32_t overrun;
  uint32_t trapno;
  int32_t status;
  int32_t sint;
  uint64_t ptr, utime, stime, addr;
  uint16_t addr_lsb;
  uint8_t pad[46];
};

static int fails;
static void row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[LP] ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(ap);
}
static void verdict(const char *id, const char *what, int ok, const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (!ok) fails++;
  row("%s %-44s %s -> %s", id, what, b, ok ? "PASS" : "FAIL");
}
static int64_t mono_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static long sfd(int fd, const sigset_t *m, size_t sz, int flags) {
  long r = syscall(SYS_signalfd4, fd, m, sz, flags);
  return r < 0 ? -errno : r;
}

static int bfd = -1; /* a blocking signalfd for S6 */
static volatile int s6_sig;
static volatile int64_t s6_t;
static void *s6_reader(void *a) {
  (void)a;
  struct ssi s;
  ssize_t n = read(bfd, &s, sizeof s);
  s6_t = mono_us();
  s6_sig = n == (ssize_t)sizeof s ? (int)s.signo : -(int)errno;
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  sigset_t m;
  sigemptyset(&m);
  sigaddset(&m, SIGUSR1);
  sigaddset(&m, SIGUSR2);
  sigaddset(&m, SIGCHLD);
  sigaddset(&m, SIGTERM);
  sigprocmask(SIG_BLOCK, &m, NULL); /* the signalfd pattern: block, then read */

  long fd = sfd(-1, &m, 8, SFD_NONBLOCK_ | SFD_CLOEXEC_);
  verdict("S1", "signalfd4(-1, mask, 8, NONBLOCK|CLOEXEC)", fd >= 0, "fd=%ld", fd);
  if (fd < 0) {
    row("done rc=%d", fails + 10);
    return 1;
  }
  struct ssi s[3];
  ssize_t n = read((int)fd, s, sizeof s[0]);
  struct pollfd p = {(int)fd, POLLIN, 0};
  int pr = poll(&p, 1, 0);
  verdict("S2", "nothing pending: EAGAIN, not readable", n < 0 && errno == EAGAIN && pr == 0,
          "read=%zd errno=%d poll=%d", n, n < 0 ? errno : 0, pr);

  raise(SIGUSR1);
  pr = poll(&p, 1, 0);
  memset(s, 0, sizeof s);
  n = read((int)fd, s, sizeof s[0]);
  sigset_t pend;
  sigpending(&pend);
  verdict("S3", "raise(USR1): one record, SI_TKILL, consumed",
          pr == 1 && n == 128 && s[0].signo == SIGUSR1 && s[0].code == SI_TKILL &&
              !sigismember(&pend, SIGUSR1),
          "poll=%d read=%zd signo=%u code=%d still_pending=%d", pr, n, s[0].signo, s[0].code,
          sigismember(&pend, SIGUSR1));

  kill(getpid(), SIGUSR1);
  memset(s, 0, sizeof s);
  n = read((int)fd, s, sizeof s[0]);
  verdict("S4", "kill(self, USR1): SI_USER", n == 128 && s[0].signo == SIGUSR1 && s[0].code == SI_USER,
          "read=%zd signo=%u code=%d", n, s[0].signo, s[0].code);

  kill(getpid(), SIGUSR2);
  kill(getpid(), SIGUSR1);
  memset(s, 0, sizeof s);
  n = read((int)fd, s, sizeof s);
  verdict("S5", "two pending, 3-record buffer: 2 in order",
          n == 256 && s[0].signo == SIGUSR1 && s[1].signo == SIGUSR2,
          "read=%zd signos=%u,%u", n, s[0].signo, s[1].signo);

  /* S6: blocking read on a worker, ended by a kill from main */
  bfd = (int)sfd(-1, &m, 8, 0);
  pthread_t th;
  pthread_create(&th, 0, s6_reader, 0);
  struct timespec d = {0, 50000000};
  nanosleep(&d, 0);
  int64_t t0 = mono_us();
  kill(getpid(), SIGUSR2);
  pthread_join(th, 0);
  verdict("S6", "a blocking read is ended by the signal", s6_sig == SIGUSR2 && s6_t - t0 < 300000,
          "sig=%d latency=%lldus", s6_sig, (long long)(s6_t - t0));

  /* S7: epoll on the signalfd */
  int ep = epoll_create1(0);
  struct epoll_event ev = {EPOLLIN, {.u64 = 7}};
  epoll_ctl(ep, EPOLL_CTL_ADD, (int)fd, &ev);
  t0 = mono_us();
  if (fork() == 0) { /* a sender process: kill the parent after 50 ms */
    nanosleep(&d, 0);
    kill(getppid(), SIGUSR1);
    _exit(0);
  }
  struct epoll_event out;
  int er = epoll_wait(ep, &out, 1, 2000);
  int64_t lat = mono_us() - t0;
  memset(s, 0, sizeof s);
  n = read((int)fd, s, sizeof s[0]);
  verdict("S7", "epoll_wait(signalfd) wakes for a kill", er == 1 && out.data.u64 == 7 && n == 128 &&
          s[0].signo == SIGUSR1 && lat < 1000000, "epoll=%d read=%zd signo=%u after=%lldus",
          er, n, s[0].signo, (long long)lat);
  /* the sender's SIGCHLD */
  int st;
  while (waitpid(-1, &st, 0) > 0) {}
  memset(s, 0, sizeof s);
  while (read((int)fd, s, sizeof s[0]) == 128) {}

  /* S8: SIGCHLD of an exiting child */
  pid_t c = fork();
  if (c == 0) _exit(5);
  p.revents = 0;
  pr = poll(&p, 1, 5000);
  memset(s, 0, sizeof s);
  n = read((int)fd, s, sizeof s[0]);
  waitpid(c, &st, 0);
  verdict("S8", "SIGCHLD: CLD_EXITED, child pid, status 5",
          pr == 1 && n == 128 && s[0].signo == SIGCHLD && s[0].code == CLD_EXITED &&
              (pid_t)s[0].pid == c && s[0].status == 5,
          "poll=%d read=%zd signo=%u code=%d pid=%u/%d status=%d", pr, n, s[0].signo, s[0].code,
          s[0].pid, (int)c, s[0].status);

  /* S9: blocked SIG_DFL SIGTERM: queued, not fatal */
  kill(getpid(), SIGTERM);
  memset(s, 0, sizeof s);
  n = read((int)fd, s, sizeof s[0]);
  verdict("S9", "blocked SIG_DFL SIGTERM: alive, read it", n == 128 && s[0].signo == SIGTERM,
          "read=%zd signo=%u", n, s[0].signo);

  /* S10: replace the mask */
  sigset_t m2;
  sigemptyset(&m2);
  sigaddset(&m2, SIGUSR2);
  long r10 = sfd((int)fd, &m2, 8, 0);
  kill(getpid(), SIGUSR1);
  n = read((int)fd, s, sizeof s[0]);
  int e1 = n < 0 ? errno : 0;
  kill(getpid(), SIGUSR2);
  memset(s, 0, sizeof s);
  ssize_t n2 = read((int)fd, s, sizeof s[0]);
  verdict("S10", "signalfd(fd, {USR2}) replaces the mask",
          r10 == fd && n < 0 && e1 == EAGAIN && n2 == 128 && s[0].signo == SIGUSR2,
          "rc=%ld usr1_read=%zd/%d usr2_read=%zd signo=%u", r10, n, e1, n2, s[0].signo);
  /* drop the pending USR1 before the next rows */
  {
    struct timespec z = {0, 0};
    siginfo_t si;
    sigset_t u1;
    sigemptyset(&u1);
    sigaddset(&u1, SIGUSR1);
    sigtimedwait(&u1, &si, &z);
  }

  long e_size = sfd(-1, &m, 4, 0);
  long e_flag = sfd(-1, &m, 8, 0x1);
  int pfd[2];
  (void)!pipe(pfd);
  long e_fd = sfd(pfd[0], &m, 8, 0);
  verdict("S11", "EINVAL: sizemask 4, bad flags, a pipe fd",
          e_size == -EINVAL && e_flag == -EINVAL && e_fd == -EINVAL,
          "size=%ld flags=%ld pipe=%ld", e_size, e_flag, e_fd);

  /* S12 (C1-k6 round 2): ssi_pid names the sender */
  {
    sigset_t mu;
    sigemptyset(&mu);
    sigaddset(&mu, SIGUSR1);
    long f12 = sfd(-1, &mu, 8, 0);
    pid_t sender = fork();
    if (sender == 0) {
      kill(getppid(), SIGUSR1);
      _exit(0);
    }
    memset(s, 0, sizeof s);
    ssize_t n12 = f12 >= 0 ? read((int)f12, s, sizeof s[0]) : -1;
    int st12;
    waitpid(sender, &st12, 0);
    while (read((int)fd, s + 1, sizeof s[0]) == 128) {} /* the child's SIGCHLD */
    verdict("S12", "ssi_pid is the sender", n12 == 128 && s[0].signo == SIGUSR1 &&
            (pid_t)s[0].pid == sender, "read=%zd signo=%u pid=%u sender=%d", n12, s[0].signo,
            s[0].pid, (int)sender);
  }

  row("done rc=%d", fails);
  return fails;
}
