/*
 * src/user/linux_probe_fdscale.c - C1-k5: the fd/process scale Wine needs for
 * Cyberpunk (docs/cp-k5.md).  The game runs 173-190 threads; Wine keeps about
 * three pipes per thread plus eventfds and sockets, and raises RLIMIT_NOFILE's
 * soft limit to the hard one at startup (dlls/ntdll/unix/loader.c
 * set_max_limit, server/main.c init_limits).  NtYieldExecution compares
 * getrusage(RUSAGE_THREAD)'s nvcsw/nivcsw around sched_yield().
 *
 * ORDINARY musl Linux program (no HobbyOS header): the same static-PIE ELF runs
 * on the reference kernel (strix, Linux) and in the HobbyOS guest
 * (tools/gate/t3_linux_probe.sh fdscale).  Every row runs in its own forked
 * child with a deadline, so a hang is a FAIL row, not a wedged probe.
 * The last row is "[LP] done rc=<fails>".
 *
 * Rows:
 *  F1 rlimit      getrlimit/setrlimit/prlimit64(RLIMIT_NOFILE): hard >= 4096,
 *                 soft <= hard, cur > max is EINVAL, soft=hard round-trips,
 *                 prlimit64(0 / getpid()) old+new round-trip
 *  F2 soft_limit  soft = 64: open() stops at fd 63 with EMFILE, pipe() EMFILE,
 *                 dup2(fd, 64) EBADF, F_DUPFD 64 EINVAL; soft = hard reopens
 *  F3 pipes3000   3000 pipes held at once: a child holds 1000, this process
 *                 2000 (4000+ fds in one table, 6000 open files system-wide);
 *                 every pipe carries a byte both ways
 *  F4 poll2000    poll()/ppoll() over 2000 fds: exact ready count, a blocking
 *                 poll woken by another thread, nfds > RLIMIT_NOFILE EINVAL
 *  F5 epoll2000   one epoll set with 2000 fds: exact ready set, a blocking
 *                 epoll_wait woken by another thread
 *  F6 select_big  select()/pselect6() with nfds ~3000 (a 4096-bit set): exact
 *                 ready count, bits >= nfds ignored, blocking wake
 *  F7 fd_kinds    600 eventfds, 100 timerfds, 40 epoll instances, 64
 *                 socketpairs and 100 memfds open at once
 *  F8 dup_high    dup2 to 4000, F_DUPFD(_CLOEXEC) above 3000, close_range
 *                 over them, dup2 to RLIMIT_NOFILE is EBADF
 *  F9 rusage      RUSAGE_THREAD / SELF / CHILDREN: CPU time of a spinning
 *                 thread and a reaped child, nvcsw counting, bad who EINVAL
 *  F10 wine_shape 300 threads, each parked in read() on its own pipe with two
 *                 more pipes open (900 pipes): every thread answers a ping
 *  F11 pipe_race  16 threads create, use and close pipes/socketpairs/dups at
 *                 once (what every new Wine thread does): every fd a thread
 *                 gets carries that thread's own bytes (no two threads are
 *                 ever handed the same fd number)
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
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROW_DEADLINE_MS 90000
#define WANT_HARD 4096

static void out(const char *fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void msleep(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static volatile int row_bad;
static char row_why[200];
static void bad(const char *fmt, ...) {
  if (__sync_fetch_and_add(&row_bad, 1) == 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(row_why, sizeof row_why, fmt, ap);
    va_end(ap);
    out("[LP] info %d: %s\n", (int)getpid(), row_why);
  }
}
#define CHECK(c, ...)        \
  do {                       \
    if (!(c)) {              \
      bad(__VA_ARGS__);      \
      return 1;              \
    }                        \
  } while (0)

struct lrl {
  uint64_t cur, max;
};
static long prl(int pid, const struct lrl *nw, struct lrl *old) {
  return syscall(SYS_prlimit64, pid, RLIMIT_NOFILE, nw, old);
}

/* soft = min(hard, want): every row that needs many fds starts here. */
static int raise_soft(uint64_t want) {
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return -1;
  uint64_t c = want;
  if (rl.rlim_max != RLIM_INFINITY && c > rl.rlim_max) c = rl.rlim_max;
  rl.rlim_cur = c;
  return setrlimit(RLIMIT_NOFILE, &rl);
}

/* ---- F1 -------------------------------------------------------------- */
static int f1(void) {
  struct rlimit rl;
  CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0, "getrlimit errno=%d", errno);
  out("[LP] info F1 initial soft=%llu hard=%llu\n", (unsigned long long)rl.rlim_cur,
      (unsigned long long)rl.rlim_max);
  CHECK(rl.rlim_cur <= rl.rlim_max, "soft %llu > hard %llu", (unsigned long long)rl.rlim_cur,
        (unsigned long long)rl.rlim_max);
  CHECK(rl.rlim_max >= WANT_HARD, "hard %llu < %d", (unsigned long long)rl.rlim_max, WANT_HARD);
  uint64_t hard = rl.rlim_max;
  if (hard != RLIM_INFINITY) {
    struct rlimit b = {hard + 1, hard};
    errno = 0;
    CHECK(setrlimit(RLIMIT_NOFILE, &b) == -1 && errno == EINVAL,
          "setrlimit cur>max: errno=%d (want EINVAL)", errno);
  }
  /* Wine's set_max_limit: soft = hard. */
  struct rlimit w = {hard, hard};
  CHECK(setrlimit(RLIMIT_NOFILE, &w) == 0, "setrlimit soft=hard errno=%d", errno);
  CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == hard && rl.rlim_max == hard,
        "soft=hard did not stick: soft=%llu", (unsigned long long)rl.rlim_cur);
  struct lrl nw = {256, hard}, old = {0, 0};
  CHECK(prl(0, &nw, &old) == 0, "prlimit64(0,new,old) errno=%d", errno);
  CHECK(old.cur == hard && old.max == hard, "prlimit64 old=%llu/%llu want %llu",
        (unsigned long long)old.cur, (unsigned long long)old.max, (unsigned long long)hard);
  CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == 256,
        "prlimit64 new soft not visible: %llu", (unsigned long long)rl.rlim_cur);
  struct lrl o2 = {0, 0};
  CHECK(prl(getpid(), 0, &o2) == 0 && o2.cur == 256 && o2.max == hard,
        "prlimit64(getpid) read %llu/%llu errno=%d", (unsigned long long)o2.cur,
        (unsigned long long)o2.max, errno);
  struct lrl inv = {hard, 16};
  errno = 0;
  CHECK(prl(0, &inv, 0) == -1 && errno == EINVAL, "prlimit64 cur>max errno=%d", errno);
  /* Other resources still answer (unchanged contract). */
  struct rlimit st;
  CHECK(getrlimit(RLIMIT_STACK, &st) == 0 && st.rlim_cur > 0, "RLIMIT_STACK errno=%d", errno);
  CHECK(setrlimit(RLIMIT_NOFILE, &w) == 0, "restore errno=%d", errno);
  return 0;
}

/* ---- F2 -------------------------------------------------------------- */
static int f2(void) {
  struct rlimit rl;
  CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0, "getrlimit errno=%d", errno);
  struct rlimit lo = {64, rl.rlim_max};
  CHECK(setrlimit(RLIMIT_NOFILE, &lo) == 0, "setrlimit 64 errno=%d", errno);
  int maxfd = -1, n = 0, err = 0;
  for (;;) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) {
      err = errno;
      break;
    }
    if (fd > maxfd) maxfd = fd;
    if (++n > 200) break;
  }
  CHECK(err == EMFILE, "open past soft: errno=%d after %d opens (want EMFILE)", err, n);
  CHECK(maxfd == 63, "highest fd %d with soft 64 (want 63)", maxfd);
  int p[2];
  errno = 0;
  CHECK(pipe(p) == -1 && errno == EMFILE, "pipe at soft: errno=%d", errno);
  errno = 0;
  CHECK(dup2(maxfd, 64) == -1 && errno == EBADF, "dup2 to 64: errno=%d (want EBADF)", errno);
  errno = 0;
  CHECK(fcntl(maxfd, F_DUPFD, 64) == -1 && errno == EINVAL, "F_DUPFD 64: errno=%d (want EINVAL)",
        errno);
  close(maxfd - 1);
  int d = fcntl(maxfd, F_DUPFD, 10);
  CHECK(d == maxfd - 1, "F_DUPFD 10 -> %d (want %d, the only free slot)", d, maxfd - 1);
  CHECK(raise_soft(rl.rlim_max) == 0, "raise soft errno=%d", errno);
  int fd = open("/dev/null", O_RDONLY);
  CHECK(fd >= 64, "open after raising soft: fd=%d errno=%d", fd, errno);
  int d2 = dup2(fd, 1000);
  CHECK(d2 == 1000, "dup2 to 1000 after raise: %d errno=%d", d2, errno);
  return 0;
}

/* ---- pipe helpers ---------------------------------------------------- */
static int open_pipes(int (*pp)[2], int n, const char *who) {
  for (int i = 0; i < n; i++) {
    if (pipe(pp[i]) != 0) {
      bad("%s: pipe #%d failed errno=%d", who, i, errno);
      return -1;
    }
  }
  return 0;
}
static int check_pipes(int (*pp)[2], int n, const char *who) {
  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char)(i * 7 + 3), r = 0;
    if (write(pp[i][1], &c, 1) != 1) {
      bad("%s: write pipe #%d errno=%d", who, i, errno);
      return -1;
    }
    if (read(pp[i][0], &r, 1) != 1 || r != c) {
      bad("%s: read pipe #%d got %u want %u errno=%d", who, i, r, c, errno);
      return -1;
    }
  }
  return 0;
}

/* ---- F3 -------------------------------------------------------------- */
static int f3(void) {
  CHECK(raise_soft(WANT_HARD) == 0, "raise soft errno=%d", errno);
  int sy[2], go[2];
  CHECK(pipe(sy) == 0 && pipe(go) == 0, "sync pipes errno=%d", errno);
  pid_t k = fork();
  CHECK(k >= 0, "fork errno=%d", errno);
  if (k == 0) {
    static int cp[1000][2];
    close(sy[0]);
    close(go[1]);
    char st = open_pipes(cp, 1000, "child") == 0 ? 'Y' : 'N';
    (void)!write(sy[1], &st, 1);
    char g;
    if (read(go[0], &g, 1) != 1) _exit(3);
    if (st != 'Y') _exit(2);
    _exit(check_pipes(cp, 1000, "child") == 0 ? 0 : 4);
  }
  close(sy[1]);
  close(go[0]);
  static int pp[2000][2];
  int rc = open_pipes(pp, 2000, "parent");
  char st = 0;
  if (read(sy[0], &st, 1) != 1) st = '?';
  int maxfd = 0;
  for (int i = 0; rc == 0 && i < 2000; i++)
    if (pp[i][1] > maxfd) maxfd = pp[i][1];
  if (rc == 0) rc = check_pipes(pp, 2000, "parent");
  (void)!write(go[1], "g", 1);
  int ws = 0;
  waitpid(k, &ws, 0);
  CHECK(rc == 0, "parent side failed");
  CHECK(st == 'Y', "child could not open its 1000 pipes (status %c)", st);
  CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "child status 0x%x", ws);
  CHECK(maxfd >= 4000, "parent's highest fd %d (want >= 4000)", maxfd);
  out("[LP] info F3 3000 pipes live, parent maxfd=%d\n", maxfd);
  return 0;
}

/* ---- F4 -------------------------------------------------------------- */
#define NPF 2000
static int (*g_pp)[2];
static void *late_writer(void *a) {
  int fd = (int)(intptr_t)a;
  msleep(150);
  (void)!write(fd, "x", 1);
  return 0;
}
static int f4(void) {
  CHECK(raise_soft(WANT_HARD) == 0, "raise soft errno=%d", errno);
  static int pp[NPF][2];
  g_pp = pp;
  if (open_pipes(pp, NPF, "poll") != 0) return 1;
  static struct pollfd pf[WANT_HARD + 1];
  int want = 0;
  for (int i = 0; i < NPF; i++) {
    pf[i].fd = pp[i][0];
    pf[i].events = POLLIN;
    pf[i].revents = 0;
    if (i % 7 == 0) {
      (void)!write(pp[i][1], "a", 1);
      want++;
    }
  }
  int r = poll(pf, NPF, 0);
  CHECK(r == want, "poll(2000,0) = %d errno=%d (want %d)", r, errno, want);
  for (int i = 0; i < NPF; i++) {
    int exp = (i % 7 == 0) ? POLLIN : 0;
    CHECK(pf[i].revents == exp, "fd #%d revents=0x%x want 0x%x", i, pf[i].revents, exp);
  }
  char c;
  for (int i = 0; i < NPF; i += 7) (void)!read(pp[i][0], &c, 1);
  pthread_t t;
  pthread_create(&t, 0, late_writer, (void *)(intptr_t)pp[NPF - 1][1]);
  long t0 = now_ms();
  r = poll(pf, NPF, 10000);
  long dt = now_ms() - t0;
  pthread_join(t, 0);
  CHECK(r == 1 && pf[NPF - 1].revents == POLLIN, "blocking poll = %d last=0x%x errno=%d", r,
        pf[NPF - 1].revents, errno);
  CHECK(dt < 5000, "blocking poll took %ld ms", dt);
  (void)!read(pp[NPF - 1][0], &c, 1);
  /* ppoll with a timespec */
  (void)!write(pp[1000][1], "b", 1);
  struct timespec ts = {1, 0};
  r = ppoll(pf, NPF, &ts, 0);
  CHECK(r == 1 && pf[1000].revents == POLLIN, "ppoll = %d errno=%d", r, errno);
  (void)!read(pp[1000][0], &c, 1);
  /* nfds > RLIMIT_NOFILE is EINVAL (Linux do_sys_poll) */
  for (int i = NPF; i <= WANT_HARD; i++) {
    pf[i].fd = -1;
    pf[i].events = 0;
  }
  struct rlimit rl = {WANT_HARD, WANT_HARD};
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = WANT_HARD;
  CHECK(setrlimit(RLIMIT_NOFILE, &rl) == 0, "soft=%d errno=%d", WANT_HARD, errno);
  errno = 0;
  r = poll(pf, WANT_HARD + 1, 0);
  CHECK(r == -1 && errno == EINVAL, "poll nfds=%d > rlimit: %d errno=%d (want EINVAL)",
        WANT_HARD + 1, r, errno);
  r = poll(pf, WANT_HARD, 0);
  CHECK(r == 0, "poll nfds=%d == rlimit: %d errno=%d", WANT_HARD, r, errno);
  return 0;
}

/* ---- F5 -------------------------------------------------------------- */
static int f5(void) {
  CHECK(raise_soft(WANT_HARD) == 0, "raise soft errno=%d", errno);
  static int pp[NPF][2];
  if (open_pipes(pp, NPF, "epoll") != 0) return 1;
  int ep = epoll_create1(EPOLL_CLOEXEC);
  CHECK(ep >= 0, "epoll_create1 errno=%d", errno);
  for (int i = 0; i < NPF; i++) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1000000ull + (uint64_t)i;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pp[i][0], &ev) == 0, "epoll_ctl ADD #%d errno=%d", i, errno);
  }
  int want = 0;
  for (int i = 0; i < NPF; i += 5) {
    (void)!write(pp[i][1], "a", 1);
    want++;
  }
  static struct epoll_event evs[4096];
  static unsigned char seen[NPF];
  int r = epoll_wait(ep, evs, 4096, 0);
  CHECK(r == want, "epoll_wait = %d errno=%d (want %d)", r, errno, want);
  for (int k = 0; k < r; k++) {
    uint64_t d = evs[k].data.u64 - 0x1000000ull;
    CHECK(d < NPF && d % 5 == 0 && !seen[d], "bad event data %llu", (unsigned long long)d);
    seen[d] = 1;
    CHECK(evs[k].events & EPOLLIN, "event %d events=0x%x", k, evs[k].events);
  }
  char c;
  for (int i = 0; i < NPF; i += 5) (void)!read(pp[i][0], &c, 1);
  pthread_t t;
  pthread_create(&t, 0, late_writer, (void *)(intptr_t)pp[NPF - 3][1]);
  long t0 = now_ms();
  r = epoll_wait(ep, evs, 4096, 10000);
  long dt = now_ms() - t0;
  pthread_join(t, 0);
  CHECK(r == 1 && evs[0].data.u64 == 0x1000000ull + NPF - 3, "blocking epoll_wait = %d errno=%d",
        r, errno);
  CHECK(dt < 5000, "blocking epoll_wait took %ld ms", dt);
  return 0;
}

/* ---- F6 -------------------------------------------------------------- */
#define NSP 1500
struct bigset {
  unsigned long w[WANT_HARD / (8 * sizeof(unsigned long))];
};
static void bs_set(struct bigset *s, int fd) { s->w[fd / 64] |= 1ul << (fd % 64); }
static int bs_isset(const struct bigset *s, int fd) { return (int)((s->w[fd / 64] >> (fd % 64)) & 1); }
static int f6(void) {
  CHECK(raise_soft(WANT_HARD) == 0, "raise soft errno=%d", errno);
  static int pp[NSP][2];
  if (open_pipes(pp, NSP, "select") != 0) return 1;
  int maxfd = 0;
  for (int i = 0; i < NSP; i++) {
    if (pp[i][0] > maxfd) maxfd = pp[i][0];
    if (pp[i][1] > maxfd) maxfd = pp[i][1];
  }
  CHECK(maxfd > 2900, "maxfd %d", maxfd);
  /* The ready pipe with the highest read fd stays OUT of the examined range. */
  int top = 0;
  for (int i = 1; i < NSP; i++)
    if (pp[i][0] > pp[top][0]) top = i;
  int nfds = pp[top][0]; /* fd == nfds is not examined */
  static struct bigset rd, keep;
  memset(&rd, 0, sizeof rd);
  int want = 0;
  for (int i = 0; i < NSP; i++) {
    bs_set(&rd, pp[i][0]);
    if (i % 3 == 0 || i == top) {
      (void)!write(pp[i][1], "a", 1);
      if (i != top) want++;
    }
  }
  keep = rd;
  struct timeval tv = {0, 0};
  int r = select(nfds, (fd_set *)&rd, 0, 0, &tv);
  CHECK(r == want, "select(nfds=%d) = %d errno=%d (want %d)", nfds, r, errno, want);
  for (int i = 0; i < NSP; i++) {
    if (i == top) continue;
    int exp = (i % 3 == 0);
    CHECK(bs_isset(&rd, pp[i][0]) == exp, "fd %d bit=%d want %d", pp[i][0], bs_isset(&rd, pp[i][0]),
          exp);
  }
  /* glibc/musl FD_SETSIZE (1024) sets still work for low fds */
  fd_set small;
  FD_ZERO(&small);
  int lowi = -1;
  for (int i = 0; i < NSP; i += 3)
    if (pp[i][0] < 1000) {
      lowi = i;
      FD_SET(pp[i][0], &small);
    }
  CHECK(lowi >= 0, "no low fd");
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  r = select(FD_SETSIZE, &small, 0, 0, &tv);
  CHECK(r > 0 && FD_ISSET(pp[lowi][0], &small), "select(FD_SETSIZE) = %d errno=%d", r, errno);
  char c;
  for (int i = 0; i < NSP; i += 3) (void)!read(pp[i][0], &c, 1);
  /* blocking pselect woken by a thread */
  int wi = (top == NSP - 2) ? NSP - 3 : NSP - 2;
  rd = keep;
  pthread_t t;
  pthread_create(&t, 0, late_writer, (void *)(intptr_t)pp[wi][1]);
  struct timespec ts = {10, 0};
  long t0 = now_ms();
  r = pselect(nfds, (fd_set *)&rd, 0, 0, &ts, 0);
  long dt = now_ms() - t0;
  pthread_join(t, 0);
  CHECK(r == 1 && bs_isset(&rd, pp[wi][0]), "blocking pselect = %d errno=%d", r, errno);
  CHECK(dt < 5000, "blocking pselect took %ld ms", dt);
  return 0;
}

/* ---- F7 -------------------------------------------------------------- */
static int f7(void) {
  CHECK(raise_soft(WANT_HARD) == 0, "raise soft errno=%d", errno);
  for (int i = 0; i < 600; i++) {
    int e = eventfd(0, EFD_CLOEXEC);
    CHECK(e >= 0, "eventfd #%d errno=%d", i, errno);
    if (i == 599) {
      uint64_t v = 5;
      CHECK(write(e, &v, 8) == 8 && read(e, &v, 8) == 8 && v == 5, "eventfd #599 io");
    }
  }
  for (int i = 0; i < 100; i++) {
    int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    CHECK(t >= 0, "timerfd #%d errno=%d", i, errno);
  }
  for (int i = 0; i < 40; i++) {
    int e = epoll_create1(0);
    CHECK(e >= 0, "epoll instance #%d errno=%d", i, errno);
  }
  for (int i = 0; i < 64; i++) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair #%d errno=%d", i, errno);
  }
  for (int i = 0; i < 100; i++) {
    int m = (int)syscall(SYS_memfd_create, "k5", 0);
    CHECK(m >= 0, "memfd #%d errno=%d", i, errno);
  }
  return 0;
}

/* ---- F8 -------------------------------------------------------------- */
static int f8(void) {
  CHECK(raise_soft(WANT_HARD) == 0, "raise soft errno=%d", errno);
  struct rlimit rl;
  CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0, "getrlimit");
  int lim = (int)rl.rlim_cur;
  int p[2];
  CHECK(pipe(p) == 0, "pipe errno=%d", errno);
  CHECK(dup2(p[1], 4000) == 4000, "dup2 -> 4000 errno=%d", errno);
  int a = fcntl(p[1], F_DUPFD, 3000);
  CHECK(a >= 3000, "F_DUPFD 3000 -> %d errno=%d", a, errno);
  int b = fcntl(p[1], F_DUPFD_CLOEXEC, 3500);
  CHECK(b >= 3500 && (fcntl(b, F_GETFD) & FD_CLOEXEC), "F_DUPFD_CLOEXEC 3500 -> %d", b);
  CHECK(write(4000, "z", 1) == 1, "write via fd 4000 errno=%d", errno);
  char c = 0;
  CHECK(read(p[0], &c, 1) == 1 && c == 'z', "read back");
  errno = 0;
  CHECK(dup2(p[1], lim) == -1 && errno == EBADF, "dup2 to rlimit %d: errno=%d (want EBADF)", lim,
        errno);
  CHECK(syscall(SYS_close_range, 2999u, ~0u, 0) == 0, "close_range errno=%d", errno);
  errno = 0;
  CHECK(fcntl(4000, F_GETFD) == -1 && errno == EBADF, "fd 4000 survived close_range");
  CHECK(fcntl(a, F_GETFD) == -1, "fd %d survived close_range", a);
  CHECK(fcntl(p[1], F_GETFD) >= 0, "low fd closed by close_range");
  return 0;
}

/* ---- F9 -------------------------------------------------------------- */
static long tv_ms(const struct timeval *t) { return t->tv_sec * 1000L + t->tv_usec / 1000; }
static long ru_cpu_ms(const struct rusage *r) { return tv_ms(&r->ru_utime) + tv_ms(&r->ru_stime); }
static void spin_ms(int ms) {
  long t0 = now_ms();
  volatile unsigned long x = 0;
  while (now_ms() - t0 < ms) x++;
}
static struct rusage spin_ru;
static long spin_rc;
static void *spinner(void *a) {
  (void)a;
  spin_ms(400);
  spin_rc = syscall(SYS_getrusage, 1 /* RUSAGE_THREAD */, &spin_ru);
  return 0;
}
static struct rusage vs0, vs1;
static long vs_rc;
static void *sleeper(void *a) {
  (void)a;
  vs_rc = syscall(SYS_getrusage, 1, &vs0);
  for (int i = 0; i < 5; i++) msleep(10);
  if (vs_rc == 0) vs_rc = syscall(SYS_getrusage, 1, &vs1);
  return 0;
}
static int f9(void) {
  struct rusage me0, self, ch0, ch1;
  CHECK(syscall(SYS_getrusage, 1, &me0) == 0, "RUSAGE_THREAD errno=%d", errno);
  pthread_t t;
  pthread_create(&t, 0, spinner, 0);
  pthread_join(t, 0);
  CHECK(spin_rc == 0, "spinner RUSAGE_THREAD errno=%d", errno);
  long spin = ru_cpu_ms(&spin_ru);
  CHECK(spin >= 200, "spinning thread's RUSAGE_THREAD cpu=%ld ms after 400 ms spin", spin);
  struct rusage me1;
  CHECK(syscall(SYS_getrusage, 1, &me1) == 0, "RUSAGE_THREAD errno=%d", errno);
  long mine = ru_cpu_ms(&me1) - ru_cpu_ms(&me0);
  CHECK(mine < spin, "joining thread charged %ld ms (spinner %ld)", mine, spin);
  CHECK(getrusage(RUSAGE_SELF, &self) == 0, "RUSAGE_SELF errno=%d", errno);
  CHECK(ru_cpu_ms(&self) >= spin, "RUSAGE_SELF %ld < spinner %ld (exited threads count)",
        ru_cpu_ms(&self), spin);
  pthread_create(&t, 0, sleeper, 0);
  pthread_join(t, 0);
  CHECK(vs_rc == 0, "sleeper getrusage errno=%d", errno);
  CHECK(vs1.ru_nvcsw > vs0.ru_nvcsw, "nvcsw %ld -> %ld across 5 sleeps", vs0.ru_nvcsw,
        vs1.ru_nvcsw);
  CHECK(getrusage(RUSAGE_CHILDREN, &ch0) == 0, "RUSAGE_CHILDREN errno=%d", errno);
  pid_t k = fork();
  CHECK(k >= 0, "fork errno=%d", errno);
  if (k == 0) {
    spin_ms(400);
    _exit(0);
  }
  int ws = 0;
  struct rusage kr;
  memset(&kr, 0, sizeof kr);
  CHECK(wait4(k, &ws, 0, &kr) == k, "wait4 errno=%d", errno);
  CHECK(ru_cpu_ms(&kr) >= 200, "wait4 rusage cpu=%ld ms for a 400 ms child", ru_cpu_ms(&kr));
  CHECK(getrusage(RUSAGE_CHILDREN, &ch1) == 0, "RUSAGE_CHILDREN errno=%d", errno);
  long dch = ru_cpu_ms(&ch1) - ru_cpu_ms(&ch0);
  CHECK(dch >= 200, "RUSAGE_CHILDREN grew %ld ms after reaping a 400 ms child", dch);
  struct rusage x;
  errno = 0;
  CHECK(syscall(SYS_getrusage, 7, &x) == -1 && errno == EINVAL, "who=7 errno=%d (want EINVAL)",
        errno);
  out("[LP] info F9 spinner=%ldms self=%ldms child=%ldms nvcsw=%ld->%ld\n", spin,
      ru_cpu_ms(&self), dch, vs0.ru_nvcsw, vs1.ru_nvcsw);
  return 0;
}

/* ---- F10 ------------------------------------------------------------- */
#define NWT 300
struct wt {
  int req[2], rep[2], wait[2];
  int ok;
};
static struct wt wts[NWT];
static void *wt_main(void *a) {
  struct wt *w = a;
  unsigned char c;
  ssize_t r;
  do r = read(w->req[0], &c, 1); while (r < 0 && errno == EINTR);
  if (r != 1) return 0;
  c ^= 0x5a;
  if (write(w->rep[1], &c, 1) == 1) w->ok = 1;
  return 0;
}
static int f10(void) {
  CHECK(raise_soft(WANT_HARD) == 0, "raise soft errno=%d", errno);
  static pthread_t th[NWT];
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 64 * 1024);
  for (int i = 0; i < NWT; i++) {
    CHECK(pipe(wts[i].req) == 0 && pipe(wts[i].rep) == 0 && pipe(wts[i].wait) == 0,
          "thread %d pipes errno=%d", i, errno);
    CHECK(pthread_create(&th[i], &at, wt_main, &wts[i]) == 0, "pthread_create #%d", i);
  }
  msleep(200);
  for (int i = 0; i < NWT; i++) {
    unsigned char c = (unsigned char)i;
    CHECK(write(wts[i].req[1], &c, 1) == 1, "ping %d errno=%d", i, errno);
  }
  for (int i = 0; i < NWT; i++) {
    unsigned char c = 0;
    CHECK(read(wts[i].rep[0], &c, 1) == 1 && c == (unsigned char)(i ^ 0x5a), "pong %d = %u", i, c);
  }
  for (int i = 0; i < NWT; i++) pthread_join(th[i], 0);
  for (int i = 0; i < NWT; i++) CHECK(wts[i].ok, "thread %d did not answer", i);
  return 0;
}

/* ---- F11 ------------------------------------------------------------- */
#define NRT 16
#define RT_ITERS 1500
static volatile int rt_go;
static volatile int rt_bad;
static char rt_why[160];
static void *rt_main(void *a) {
  int id = (int)(intptr_t)a;
  while (!rt_go) {}
  for (int it = 0; it < RT_ITERS && !rt_bad; it++) {
    int p[2], q[2];
    unsigned char tok = (unsigned char)(id * 16 + (it & 15)), got = 0;
    int kind = it % 3;
    if (kind == 0) {
      if (pipe(p) != 0) { snprintf(rt_why, sizeof rt_why, "t%d pipe errno=%d", id, errno); rt_bad = 1; break; }
    } else if (kind == 1) {
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, p) != 0) { snprintf(rt_why, sizeof rt_why, "t%d socketpair errno=%d", id, errno); rt_bad = 1; break; }
    } else {
      if (pipe(q) != 0) { snprintf(rt_why, sizeof rt_why, "t%d pipe errno=%d", id, errno); rt_bad = 1; break; }
      p[0] = dup(q[0]);
      p[1] = fcntl(q[1], F_DUPFD, 3);
      close(q[0]);
      close(q[1]);
      if (p[0] < 0 || p[1] < 0) { snprintf(rt_why, sizeof rt_why, "t%d dup errno=%d", id, errno); rt_bad = 1; break; }
    }
    if (write(p[1], &tok, 1) != 1 || read(p[0], &got, 1) != 1 || got != tok) {
      snprintf(rt_why, sizeof rt_why, "t%d iter %d kind %d fds %d/%d: wrote %u read %u errno=%d", id, it, kind,
               p[0], p[1], tok, got, errno);
      rt_bad = 1;
    }
    close(p[0]);
    close(p[1]);
  }
  return 0;
}
static int f11(void) {
  CHECK(raise_soft(WANT_HARD) == 0, "raise soft errno=%d", errno);
  pthread_t th[NRT];
  for (int i = 0; i < NRT; i++) pthread_create(&th[i], 0, rt_main, (void *)(intptr_t)i);
  msleep(50);
  rt_go = 1;
  for (int i = 0; i < NRT; i++) pthread_join(th[i], 0);
  CHECK(!rt_bad, "%s", rt_why);
  return 0;
}

static int fails;
static void run_row(const char *name, int (*fn)(void)) {
  long t0 = now_ms();
  int pr[2];
  if (pipe(pr)) {
    out("[LP] FAIL %s pipe errno=%d\n", name, errno);
    fails++;
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    out("[LP] FAIL %s fork errno=%d\n", name, errno);
    fails++;
    return;
  }
  if (pid == 0) {
    close(pr[0]);
    setpgid(0, 0);
    row_bad = 0;
    int rc = fn();
    char msg[240];
    int n = snprintf(msg, sizeof msg, "%s", (rc || row_bad) ? row_why : "ok");
    (void)!write(pr[1], msg, (size_t)n);
    _exit((rc || row_bad) ? 1 : 0);
  }
  close(pr[1]);
  int st = 0, done = 0;
  while (now_ms() - t0 < ROW_DEADLINE_MS) {
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w == pid) {
      done = 1;
      break;
    }
    msleep(20);
  }
  long ms = now_ms() - t0;
  if (!done) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    out("[LP] FAIL %s HANG: no finish in %d ms\n", name, ROW_DEADLINE_MS);
    fails++;
    waitpid(pid, &st, 0);
    close(pr[0]);
    return;
  }
  char msg[240] = {0};
  fcntl(pr[0], F_SETFL, O_NONBLOCK);
  ssize_t n = read(pr[0], msg, sizeof msg - 1);
  if (n < 0) n = 0;
  msg[n] = 0;
  close(pr[0]);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
    out("[LP] PASS %s (%ld ms)\n", name, ms);
  } else {
    out("[LP] FAIL %s status=0x%x %s (%ld ms)\n", name, st, n ? msg : "(no report)", ms);
    fails++;
  }
}

int main(void) {
  run_row("F1 rlimit", f1);
  run_row("F2 soft_limit", f2);
  run_row("F3 pipes3000", f3);
  run_row("F4 poll2000", f4);
  run_row("F5 epoll2000", f5);
  run_row("F6 select_big", f6);
  run_row("F7 fd_kinds", f7);
  run_row("F8 dup_high", f8);
  run_row("F9 rusage", f9);
  run_row("F10 wine_shape", f10);
  run_row("F11 pipe_race", f11);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
