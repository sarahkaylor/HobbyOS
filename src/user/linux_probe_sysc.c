/*
 * src/user/linux_probe_sysc.c — what one trap costs (C1-k7, docs/cp-k7.md).
 *
 * The game's hot threads spend 11-25 % of the wall clock in the kernel,
 * spread over many syscalls (C6-present): the per-trap cost, not one call.
 * This probe times the cheapest calls one by one, on one thread and then on
 * N threads at once (the game runs 2-3 busy threads plus the wineserver), so
 * a fixed cost that grows with concurrency -- a shared cache line every trap
 * writes, a global lock -- shows up as the multi-thread rows rising above
 * the single-thread ones.
 *
 *   S0  getpid                      (raw syscall)
 *   S1  gettid
 *   S2  clock_gettime(MONOTONIC)    (raw syscall: never the vDSO)
 *   S3  clock_gettime via libc      (the vDSO where there is one)
 *   S4  futex(FUTEX_WAKE_PRIVATE) on a word nobody waits on
 *   S5  read() of an empty O_NONBLOCK pipe (EAGAIN)
 *   S6  write 8 bytes + read 8 bytes, one pipe, one thread (two calls)
 *   S7  rt_sigprocmask(SIG_BLOCK, {}, &old)
 *   S8  epoll_wait(1 idle fd, timeout 0)
 *   S9  poll(NULL, 0, 0)
 *   S10 an unknown syscall number (ENOSYS)
 *   S11 S6 again while another thread sleeps in poll() on an unrelated pipe
 *       (every pipe write notifies pollers: does it pay for one that does
 *       not care?)
 *   M2/M4/M8  getpid on 2 / 4 / 8 threads at once: ns per call per thread
 *   M4f       futex wake-nobody on 4 threads (each its own word)
 *   M4c       clock_gettime (raw) on 4 threads
 *   T0  (test kernels) getpid, cycles per segment of the kernel's syscall
 *       path (arch/x64/trapprof.c, test syscall 0x4B37)
 *   T1  (test kernels) the profile's own null call, plus the two legs the
 *       kernel cannot see alone: user rdtsc -> handler entry (the `syscall`
 *       instruction and the entry asm) and handler exit -> user rdtsc (the
 *       exit asm, the frame check and the iretq)
 *
 * Each row: 15 batches of 4000 calls, the batch's ns per call; min / p50 /
 * p90 of the batches.  Grade: every row completes; S0 p50 <= SYSC_S0_MAX
 * (default 2000 ns -- generous: a sanity bound, not the A/B, which is the
 * table); M4 p50 <= 4 x S0 p50 + 1000 (a trap path whose cost scales with
 * the number of CPUs trapping fails this).  Integers only.  ORDINARY musl
 * program, also run unchanged on the strix host for the Linux column.
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
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SYSC_S0_MAX
#define SYSC_S0_MAX 2000
#endif
#define NB 15
#define NC 4000

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
static int64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static int cmp64(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return x < y ? -1 : x > y;
}

static int pfd[2], ep, efd[2], ufd[2];
static void *s11_poller(void *a) {
  (void)a;
  struct pollfd q = {ufd[0], POLLIN, 0};
  poll(&q, 1, -1);
  return 0;
}
static volatile int fwords[16 * 16];

/* one call of kind k by thread t */
static inline void call(int k, int t) {
  switch (k) {
  case 0: syscall(SYS_getpid); break;
  case 1: syscall(SYS_gettid); break;
  case 2: {
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    break;
  }
  case 3: {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    break;
  }
  case 4: syscall(SYS_futex, &fwords[t * 16], 1 | 128 /* WAKE|PRIVATE */, 1, 0, 0, 0); break;
  case 5: {
    char b[8];
    (void)!read(pfd[0], b, 8);
    break;
  }
  case 6: {
    char b[8] = {0};
    (void)!write(efd[1], b, 8);
    (void)!read(efd[0], b, 8);
    break;
  }
  case 7: {
    sigset_t e, o;
    sigemptyset(&e);
    sigprocmask(SIG_BLOCK, &e, &o);
    break;
  }
  case 8: {
    struct epoll_event ev[2];
    epoll_wait(ep, ev, 2, 0);
    break;
  }
  case 9: poll(NULL, 0, 0); break;
  case 10: syscall(4000); break;
  case 11: {
    char b[8] = {0};
    (void)!write(efd[1], b, 8);
    (void)!read(efd[0], b, 8);
    break;
  }
  }
}

static int64_t batch(int k, int t) {
  int64_t t0 = mono_ns();
  for (int i = 0; i < NC; i++) call(k, t);
  return (mono_ns() - t0) / NC;
}

struct res { int64_t mn, p50, p90; };
static struct res summarize(int64_t *v, int n) {
  qsort(v, (size_t)n, sizeof v[0], cmp64);
  struct res r = {v[0], v[n / 2], v[n * 9 / 10]};
  return r;
}

static struct res single(int k) {
  int64_t v[NB];
  for (int i = 0; i < 2; i++) (void)batch(k, 0); /* warm */
  for (int i = 0; i < NB; i++) v[i] = batch(k, 0);
  return summarize(v, NB);
}

/* N threads at once: each runs NB batches; every thread's batches pooled */
static volatile int go, ready_n;
static int mt_kind;
static int64_t mt_v[16][NB];
static void *mt_thread(void *a) {
  int t = (int)(intptr_t)a;
  __atomic_add_fetch(&ready_n, 1, __ATOMIC_SEQ_CST);
  while (!go) { }
  (void)batch(mt_kind, t);
  for (int i = 0; i < NB; i++) mt_v[t][i] = batch(mt_kind, t);
  return 0;
}
static struct res multi(int k, int n) {
  pthread_t th[16];
  mt_kind = k;
  go = 0;
  ready_n = 0;
  for (int i = 0; i < n; i++) pthread_create(&th[i], 0, mt_thread, (void *)(intptr_t)i);
  while (ready_n < n) { }
  go = 1;
  for (int i = 0; i < n; i++) pthread_join(th[i], 0);
  static int64_t all[16 * NB];
  int m = 0;
  for (int i = 0; i < n; i++)
    for (int b = 0; b < NB; b++) all[m++] = mt_v[i][b];
  return summarize(all, m);
}

static void show(const char *id, const char *what, struct res r, int ok) {
  row("%-4s %-40s ns/call min=%lld p50=%lld p90=%lld %s", id, what, (long long)r.mn,
      (long long)r.p50, (long long)r.p90, ok ? "PASS" : "FAIL");
  if (!ok) fails++;
}

static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
static inline long tp_call(long op, uint64_t a1, uint64_t a2) {
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(0x4B37L), "D"(op), "S"(a1), "d"(a2)
                   : "rcx", "r11", "memory");
  return r;
}
#define TPN 11
static const char *tp_names[TPN] = {"-", "trapenter", "diag", "route", "syspre", "disppre",
                                    "handler", "disppost", "sigexit", "unwind", "ruexit"};
/* returns 0 when the kernel has no profile (release kernel, Linux host) */
static int tprof(const char *id, int kind) {
  uint64_t w[6 + TPN];
  if (tp_call(0, 0, 0) != 0) return 0;
  if (kind == 0) {
    for (int i = 0; i < 100000; i++) syscall(SYS_getpid);
  } else {
    uint64_t after = 0;
    for (int i = 0; i < 100000; i++) {
      uint64_t t0 = rdtsc();
      tp_call(3, t0, after);
      after = rdtsc();
    }
  }
  tp_call(1, 0, 0);
  memset(w, 0, sizeof w);
  if (tp_call(2, (uint64_t)(uintptr_t)w, sizeof w) != (long)sizeof w) return 0;
  uint64_t n = w[1] ? w[1] : 1, khz = w[0] ? w[0] : 1;
  char b[512];
  int o = snprintf(b, sizeof b, "%s %s n=%llu cyc/call", id, kind ? "nullop" : "getpid",
                   (unsigned long long)w[1]);
  uint64_t tot = 0;
  for (int i = 1; i < TPN && o < (int)sizeof b; i++) {
    o += snprintf(b + o, sizeof b - (size_t)o, " %s=%llu", tp_names[i],
                  (unsigned long long)(w[2 + i] / n));
    tot += w[2 + i];
  }
  row("%s", b);
  uint64_t en = w[3 + TPN] ? w[3 + TPN] : 1, xn = w[5 + TPN] ? w[5 + TPN] : 1;
  uint64_t ent = w[2 + TPN] / en, ext = w[4 + TPN] / xn;
  row("%s %s C-path=%llu cyc (%llu ns)  entry-leg=%llu cyc (%llu ns, n=%llu)  exit-leg=%llu cyc "
      "(%llu ns, n=%llu)  tsc_khz=%llu",
      id, kind ? "nullop" : "getpid", (unsigned long long)(tot / n),
      (unsigned long long)(tot / n * 1000000 / khz), (unsigned long long)ent,
      (unsigned long long)(ent * 1000000 / khz), (unsigned long long)w[3 + TPN],
      (unsigned long long)ext, (unsigned long long)(ext * 1000000 / khz),
      (unsigned long long)w[5 + TPN], (unsigned long long)khz);
  return 1;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (pipe2(pfd, O_NONBLOCK) || pipe(efd)) { row("pipe failed errno=%d", errno); return 1; }
  ep = epoll_create1(0);
  {
    struct epoll_event ev = {EPOLLIN, {.u32 = 1}};
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev)) { row("epoll failed errno=%d", errno); return 1; }
  }
  static const char *names[] = {
    "getpid", "gettid", "clock_gettime raw", "clock_gettime libc (vDSO if any)",
    "futex wake, nobody waiting", "read empty nonblocking pipe (EAGAIN)",
    "pipe write 8 + read 8 (2 calls)", "rt_sigprocmask(SIG_BLOCK, {})",
    "epoll_wait(1 idle fd, 0)", "poll(NULL, 0, 0)", "unknown nr (ENOSYS)",
    "S6 with an unrelated poller parked"};
  struct res s0 = {0, 0, 0};
  if (pipe(ufd)) { row("pipe failed errno=%d", errno); return 1; }
  pthread_t s11;
  for (int k = 0; k <= 11; k++) {
    if (k == 11) {
      pthread_create(&s11, 0, s11_poller, 0);
      struct timespec d = {0, 20000000};
      nanosleep(&d, 0); /* let it park */
    }
    struct res r = single(k);
    if (k == 0) s0 = r;
    char id[8];
    snprintf(id, sizeof id, "S%d", k);
    show(id, names[k], r, k != 0 || r.p50 <= SYSC_S0_MAX);
  }
  { char c = 'x'; (void)!write(ufd[1], &c, 1); pthread_join(s11, 0); }
  {
    struct res r;
    r = multi(0, 2); show("M2", "getpid, 2 threads at once", r, 1);
    r = multi(0, 4); show("M4", "getpid, 4 threads at once", r, r.p50 <= 4 * s0.p50 + 1000);
    r = multi(0, 8); show("M8", "getpid, 8 threads at once", r, 1);
    r = multi(4, 4); show("M4f", "futex wake-nobody, 4 threads", r, 1);
    r = multi(2, 4); show("M4c", "clock_gettime raw, 4 threads", r, 1);
  }
  if (tprof("T0", 0)) (void)tprof("T1", 1);
  else row("T0 no trap profile here (release kernel or Linux host)");
  row("done rc=%d", fails);
  return fails;
}
