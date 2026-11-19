/*
 * src/user/linux_probe_ipclat.c — the wineserver round trip, timed (C1-k6
 * round 2, docs/cp-k6.md §12).
 *
 * Every Wine synchronisation call without ntsync/esync is a wineserver
 * request: the client writes its request pipe and blocks in read() on its
 * reply pipe; the single-threaded server sleeps in epoll_wait() over every
 * client's request pipe, reads the request and writes the reply.  The game
 * runs ~55 threads, so the server's epoll set holds ~200 fds.  This probe
 * times that round trip with 1, 64 and 256 registered (idle) fds, and the
 * same with poll() for comparison.
 *
 *   I1-I3  epoll server, 1 / 64 / 256 fds in the set: round trip p50/p90 us
 *   I4     poll server, 256 fds
 *   I5     4 clients at once against the 256-fd epoll server (throughput)
 *
 * Grade (guest): every row completes; p50 <= 200 us for I1-I4 (the host
 * does ~10-20 us).  Integers only.  ORDINARY musl program.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

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
static int64_t mono_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static int cmp64(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return x < y ? -1 : x > y;
}

#define MAXC 4
#define NREQ 2000
static int req[MAXC][2], rep[MAXC][2];
static int idle_fds[300][2];
static int nidle, use_poll, nclients;
static volatile int stop;

static void *server(void *a) {
  (void)a;
  int ep = -1;
  struct pollfd pf[MAXC + 300];
  int n = 0;
  if (!use_poll) {
    ep = epoll_create1(0);
    for (int i = 0; i < nclients; i++) {
      struct epoll_event ev = {EPOLLIN, {.u32 = (uint32_t)i}};
      epoll_ctl(ep, EPOLL_CTL_ADD, req[i][0], &ev);
    }
    for (int i = 0; i < nidle; i++) {
      struct epoll_event ev = {EPOLLIN, {.u32 = 1000u + (uint32_t)i}};
      epoll_ctl(ep, EPOLL_CTL_ADD, idle_fds[i][0], &ev);
    }
  } else {
    for (int i = 0; i < nclients; i++) pf[n++] = (struct pollfd){req[i][0], POLLIN, 0};
    for (int i = 0; i < nidle; i++) pf[n++] = (struct pollfd){idle_fds[i][0], POLLIN, 0};
  }
  while (!stop) {
    if (!use_poll) {
      struct epoll_event ev[8];
      int r = epoll_wait(ep, ev, 8, 100);
      for (int k = 0; k < r; k++) {
        uint32_t c = ev[k].data.u32;
        if (c >= MAXC) continue;
        char b[64];
        if (read(req[c][0], b, sizeof b) > 0) (void)!write(rep[c][1], b, 8);
      }
    } else {
      int r = poll(pf, (nfds_t)n, 100);
      for (int k = 0; r > 0 && k < nclients; k++) {
        if (!(pf[k].revents & POLLIN)) continue;
        char b[64];
        if (read(req[k][0], b, sizeof b) > 0) (void)!write(rep[k][1], b, 8);
      }
    }
  }
  if (ep >= 0) close(ep);
  return 0;
}

static int64_t lat[MAXC][NREQ];
static void *client(void *a) {
  int c = (int)(intptr_t)a;
  char b[64] = {0};
  for (int i = 0; i < NREQ; i++) {
    int64_t t0 = mono_us();
    if (write(req[c][1], b, 64) != 64) break;
    if (read(rep[c][0], b, 8) != 8) break;
    lat[c][i] = mono_us() - t0;
  }
  return 0;
}

static void run(const char *id, const char *what, int idle, int poll_mode, int clients) {
  nidle = idle;
  use_poll = poll_mode;
  nclients = clients;
  stop = 0;
  memset(lat, 0, sizeof lat);
  pthread_t s, c[MAXC];
  pthread_create(&s, 0, server, 0);
  struct timespec d = {0, 20000000};
  nanosleep(&d, 0);
  int64_t t0 = mono_us();
  for (int i = 0; i < clients; i++) pthread_create(&c[i], 0, client, (void *)(intptr_t)i);
  for (int i = 0; i < clients; i++) pthread_join(c[i], 0);
  int64_t wall = mono_us() - t0;
  stop = 1;
  pthread_join(s, 0);
  static int64_t all[MAXC * NREQ];
  int n = 0, zero = 0;
  for (int i = 0; i < clients; i++)
    for (int k = 0; k < NREQ; k++) {
      if (lat[i][k] <= 0 && k > 0) zero++;
      all[n++] = lat[i][k];
    }
  qsort(all, (size_t)n, sizeof all[0], cmp64);
  int64_t p50 = all[n / 2], p90 = all[n * 9 / 10], p99 = all[n * 99 / 100];
  int64_t rate = wall > 0 ? (int64_t)n * 1000000 / wall : 0;
  int ok = zero < n / 10 && (clients > 1 || p50 <= 200);
  if (!ok) fails++;
  row("%s %-34s n=%d rt_us p50=%lld p90=%lld p99=%lld max=%lld  %lld round trips/s %s", id, what,
      n, (long long)p50, (long long)p90, (long long)p99, (long long)all[n - 1], (long long)rate,
      ok ? "PASS" : "FAIL");
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  for (int i = 0; i < MAXC; i++)
    if (pipe(req[i]) || pipe(rep[i])) { row("pipe failed errno=%d", errno); return 1; }
  for (int i = 0; i < 256; i++)
    if (pipe(idle_fds[i])) { row("idle pipe %d failed errno=%d", i, errno); return 1; }
  {
    /* I0: the floor -- one trivial syscall, and a pipe write+read on one
       thread (no wake, no switch) */
    int64_t t0 = mono_us();
    for (int i = 0; i < 20000; i++) syscall(SYS_getppid);
    int64_t t1 = mono_us();
    int pp[2];
    char b[8] = {0};
    (void)!pipe(pp);
    for (int i = 0; i < 20000; i++) {
      (void)!write(pp[1], b, 8);
      (void)!read(pp[0], b, 8);
    }
    int64_t t2 = mono_us();
    close(pp[0]);
    close(pp[1]);
    row("I0 syscall floor: getppid %lld ns, pipe write+read (same thread) %lld ns",
        (long long)((t1 - t0) * 1000 / 20000), (long long)((t2 - t1) * 1000 / 20000));
  }
  run("I1", "epoll server, 1 fd", 0, 0, 1);
  run("I2", "epoll server, 64 fds", 63, 0, 1);
  run("I3", "epoll server, 256 fds", 255, 0, 1);
  run("I4", "poll server, 256 fds", 255, 1, 1);
  run("I5", "epoll 256 fds, 4 clients", 252, 0, 4);
  row("done rc=%d", fails);
  return fails;
}
