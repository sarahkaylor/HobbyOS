/*
 * src/user/linux_probe_fdrace.c — fd numbers handed out concurrently
 * (C1-k6, docs/cp-k6.md §6).  C1-k5 put the pipe/socketpair/dup/SCM_RIGHTS
 * installs under fdtab_lock (fdscale F11); eventfd, timerfd, epoll_create,
 * memfd_create and accept still scanned the fd table and filled the slot
 * with no lock, so two threads could be handed the same fd number (one file
 * orphaned, the other named twice).  Wine creates eventfd/epoll/memfd fds
 * from many threads.
 *
 * ORDINARY musl Linux program (strix host and guest).  Rows "[LP] ...",
 * last "[LP] done rc=<fails>".
 *
 *   D1  16 threads x 300 rounds: each round every thread creates one
 *       eventfd, timerfd, epoll fd and memfd and records the numbers; after
 *       a barrier the main thread checks that no number was handed out
 *       twice, and every fd still answers its own type (an eventfd counts,
 *       a timerfd gettime, an epoll ctl, a memfd ftruncate); then all close
 *   D2  the same with AF_UNIX accept() on a shared listening socket
 *   D3  EPOLL_CLOEXEC / EFD_CLOEXEC / TFD_CLOEXEC / MFD_CLOEXEC set FD_CLOEXEC
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#define NT 16
#define ROUNDS 300
#define PER 4

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

static pthread_barrier_t bar;
static int fds[NT][PER];
static int bad_create, bad_type, dups, rounds_done;
static int mode; /* 0 create, 1 accept */
static int lsock = -1;
static char lpath[64];

static int memfd(const char *n, unsigned f) { return (int)syscall(SYS_memfd_create, n, f); }

static int check_type(int t, int fd) {
  switch (t) {
  case 0: { uint64_t v = 1; return write(fd, &v, 8) == 8 && read(fd, &v, 8) == 8 && v == 1; }
  case 1: { struct itimerspec c; return timerfd_gettime(fd, &c) == 0; }
  case 2: { int e = epoll_ctl(fd, EPOLL_CTL_DEL, 0, 0); return e < 0 && (errno == ENOENT || errno == EBADF || errno == EPERM); }
  case 3: return ftruncate(fd, 4096) == 0;
  }
  return 0;
}

static void *worker(void *a) {
  int me = (int)(intptr_t)a;
  for (int r = 0; r < ROUNDS; r++) {
    if (mode == 0) {
      fds[me][0] = eventfd(0, 0);
      fds[me][1] = timerfd_create(CLOCK_MONOTONIC, 0);
      fds[me][2] = epoll_create1(0);
      fds[me][3] = memfd("fdrace", 0);
      for (int k = 0; k < PER; k++)
        if (fds[me][k] < 0) __atomic_fetch_add(&bad_create, 1, __ATOMIC_RELAXED);
    } else {
      struct sockaddr_un sa;
      memset(&sa, 0, sizeof sa);
      sa.sun_family = AF_UNIX;
      strcpy(sa.sun_path, lpath);
      /* two connects, two accepts per thread: [0] client, [1] accept,
         [2] client, [3] accept4(SOCK_CLOEXEC) */
      for (int k = 0; k < PER; k += 2) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (c < 0 || connect(c, (struct sockaddr *)&sa, sizeof sa) != 0)
          __atomic_fetch_add(&bad_create, 1, __ATOMIC_RELAXED);
        fds[me][k] = c;
        fds[me][k + 1] = k ? accept4(lsock, 0, 0, SOCK_CLOEXEC) : accept(lsock, 0, 0);
        if (fds[me][k + 1] < 0) __atomic_fetch_add(&bad_create, 1, __ATOMIC_RELAXED);
      }
    }
    pthread_barrier_wait(&bar); /* main checks */
    pthread_barrier_wait(&bar);
    for (int k = 0; k < PER; k++)
      if (fds[me][k] >= 0) close(fds[me][k]);
    pthread_barrier_wait(&bar);
  }
  return 0;
}

static void run(const char *id, const char *what) {
  bad_create = bad_type = dups = rounds_done = 0;
  pthread_t th[NT];
  pthread_barrier_init(&bar, 0, NT + 1);
  for (int i = 0; i < NT; i++) pthread_create(&th[i], 0, worker, (void *)(intptr_t)i);
  static unsigned char seen[8192];
  for (int r = 0; r < ROUNDS; r++) {
    pthread_barrier_wait(&bar);
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < NT; i++)
      for (int k = 0; k < PER; k++) {
        int fd = fds[i][k];
        if (fd < 0 || fd >= (int)sizeof seen) continue;
        if (seen[fd]++) dups++;
        else if (mode == 0 && !check_type(k, fd)) bad_type++;
      }
    rounds_done++;
    pthread_barrier_wait(&bar);
    pthread_barrier_wait(&bar);
  }
  for (int i = 0; i < NT; i++) pthread_join(th[i], 0);
  pthread_barrier_destroy(&bar);
  int ok = dups == 0 && bad_type == 0 && bad_create == 0;
  if (!ok) fails++;
  row("%s %-36s rounds=%d fds=%d dup_numbers=%d wrong_type=%d failed_creates=%d -> %s", id,
      what, rounds_done, rounds_done * NT * PER, dups, bad_type, bad_create, ok ? "PASS" : "FAIL");
}

static int cloexec(int fd) {
  int f = fcntl(fd, F_GETFD);
  close(fd);
  return f >= 0 && (f & FD_CLOEXEC);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  mode = 0;
  run("D1", "eventfd/timerfd/epoll/memfd x16 threads");

  snprintf(lpath, sizeof lpath, "/tmp/fdrace.%d", (int)getpid());
  unlink(lpath);
  lsock = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, lpath);
  if (lsock < 0 || bind(lsock, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(lsock, 64) != 0) {
    fails++;
    row("D2 listen on %s failed errno=%d -> FAIL", lpath, errno);
  } else {
    mode = 1;
    run("D2", "AF_UNIX connect/accept x16 threads");
  }
  close(lsock);
  unlink(lpath);

  int e = cloexec(epoll_create1(EPOLL_CLOEXEC)), v = cloexec(eventfd(0, EFD_CLOEXEC));
  int t = cloexec(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)), m = cloexec(memfd("x", MFD_CLOEXEC));
  int ok = e && v && t && m;
  if (!ok) fails++;
  row("D3 *_CLOEXEC sets FD_CLOEXEC                epoll=%d eventfd=%d timerfd=%d memfd=%d -> %s",
      e, v, t, m, ok ? "PASS" : "FAIL");

  row("done rc=%d", fails);
  return fails;
}
