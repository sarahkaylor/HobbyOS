/*
 * src/user/linux_probe_forkcow.c - C1-fork: fork() shares private pages
 * copy-on-write (docs/cp-fork.md).  Whatever the kernel does underneath, the
 * Linux contract is: after fork, parent and child each see the memory as it
 * was at the fork, and neither ever sees the other's later stores.
 *
 * ORDINARY musl Linux program (same ELF on the Linux host and in the guest).
 * Rows "[LP] ...", last row "[LP] done rc=<fails>".
 *
 *  K1 child sees the pre-fork bytes, its stores stay its own
 *  K2 parent stores after the fork never reach the child
 *  K3 a kernel write (read(2) from a pipe) into a shared page in the child
 *  K4 fork of a fork: three generations, each isolated
 *  K5 a parent thread storing nonstop across the fork: the child's view is
 *     ONE frozen snapshot (a writable translation left on the thread's CPU
 *     would let later stores reach the child)
 *  K6 mprotect R / RW in the child, then a store: isolated
 *  K7 MADV_DONTNEED in the child zeroes the child's page only
 *  K8 the parent unmaps while the child still reads the pages
 *  K9 200 fork/exit cycles of a 64 MiB resident parent leak no memory (free
 *     memory may drop by at most 64 MiB: one leaked frame per shared page
 *     would be 12.5 GiB)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PG 4096
#define NP 64

static int fails;
static void out(const char *fmt, ...) {
  char b[300];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}
static void row(const char *tag, int bad, const char *what) {
  out("[LP] %s %s: %s\n", bad ? "FAIL" : "PASS", tag, what);
  fails += bad != 0;
}

static uint64_t *area(void) {
  uint64_t *m = mmap(0, NP * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return m == MAP_FAILED ? 0 : m;
}
static void fill(uint64_t *m, uint64_t tag) {
  for (int p = 0; p < NP; p++)
    for (int w = 0; w < PG / 8; w += 64) m[p * (PG / 8) + w] = tag ^ ((uint64_t)p << 20) ^ (uint64_t)w;
}
static int check(const uint64_t *m, uint64_t tag) {
  int bad = 0;
  for (int p = 0; p < NP; p++)
    for (int w = 0; w < PG / 8; w += 64)
      if (m[p * (PG / 8) + w] != (tag ^ ((uint64_t)p << 20) ^ (uint64_t)w)) bad++;
  return bad;
}
/* fork, run fn in the child (its exit status = number of bad words, capped) */
static int in_child(int (*fn)(void *), void *arg) {
  pid_t pid = fork();
  if (pid == 0) {
    int b = fn(arg);
    _exit(b > 250 ? 250 : b);
  }
  if (pid < 0) return 255;
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 254;
}

static uint64_t *g;
static int k1_child(void *v) {
  (void)v;
  int b = check(g, 0xA);
  fill(g, 0xB);
  return b + check(g, 0xB);
}

static int k2_pipe[2], k2_back[2];
static int k2_child(void *v) {
  (void)v;
  char c;
  (void)!read(k2_pipe[0], &c, 1); /* the parent has stored 0xC by now */
  int b = check(g, 0xA);
  (void)!write(k2_back[1], "k", 1);
  return b;
}

static int k3_child(void *v) {
  (void)v;
  int p[2];
  if (pipe(p) != 0) return 200;
  uint64_t want = 0x5151515151515151ULL;
  if (write(p[1], &want, 8) != 8) return 201;
  if (read(p[0], &g[5 * (PG / 8)], 8) != 8) return 202; /* kernel store into page 5 */
  int b = g[5 * (PG / 8)] != want;
  g[5 * (PG / 8)] = 0xAULL ^ (5ULL << 20); /* restore the pattern word for the check */
  return b + check(g, 0xA);
}

static int k4_grand(void *v) {
  (void)v;
  int b = check(g, 0xE);
  fill(g, 0xF);
  return b + check(g, 0xF);
}
static int k4_child(void *v) {
  (void)v;
  int b = check(g, 0xA);
  fill(g, 0xE);
  b += in_child(k4_grand, 0);
  return b + check(g, 0xE);
}

static volatile uint64_t *k5_page;
static volatile int k5_stop, k5_go;
static void *k5_writer(void *v) {
  (void)v;
  uint64_t n = 0;
  k5_go = 1;
  while (!k5_stop) k5_page[0] = ++n;
  return 0;
}
static int k5_child(void *v) {
  (void)v;
  uint64_t a = k5_page[0];
  struct timespec ts = {0, 100 * 1000 * 1000};
  nanosleep(&ts, 0);
  uint64_t b = k5_page[0];
  return a != b; /* the child's copy must not move */
}

static int k6_child(void *v) {
  (void)v;
  int b = check(g, 0xA);
  if (mprotect(g, NP * PG, PROT_READ) != 0) return 200;
  b += check(g, 0xA);
  if (mprotect(g, NP * PG, PROT_READ | PROT_WRITE) != 0) return 201;
  fill(g, 0x6);
  return b + check(g, 0x6);
}

static int k7_child(void *v) {
  (void)v;
  if (madvise(g, PG, MADV_DONTNEED) != 0) return 200;
  int b = g[0] != 0;
  return b;
}

static int k8_pipe[2];
static int k8_child(void *v) {
  (void)v;
  char c;
  (void)!read(k8_pipe[0], &c, 1); /* the parent has unmapped by now */
  return check(g, 0xA);
}

static int k9_child(void *v) {
  (void)v;
  g[0] ^= 1; /* one copy-on-write */
  return 0;
}

static long free_kib(void) {
  struct sysinfo si;
  if (sysinfo(&si) != 0) return -1;
  return (long)((uint64_t)si.freeram * si.mem_unit / 1024);
}

int main(void) {
  char why[160];
  g = area();
  if (!g) { out("[LP] FAIL setup: mmap errno=%d\n[LP] done rc=1\n", errno); return 1; }

  fill(g, 0xA);
  int r = in_child(k1_child, 0);
  int pb = check(g, 0xA);
  snprintf(why, sizeof why, "child bad=%d, parent after the child's stores bad=%d", r, pb);
  row("K1 fork snapshot + child stores", r || pb, why);

  if (pipe(k2_pipe) == 0 && pipe(k2_back) == 0) {
    pid_t pid = fork();
    if (pid == 0) { int b = k2_child(0); _exit(b > 250 ? 250 : b); }
    fill(g, 0xC);
    (void)!write(k2_pipe[1], "g", 1);
    char c;
    (void)!read(k2_back[0], &c, 1);
    int st = 0;
    waitpid(pid, &st, 0);
    int cb = WIFEXITED(st) ? WEXITSTATUS(st) : 254;
    snprintf(why, sizeof why, "child saw %d words of the parent's later stores", cb);
    row("K2 parent stores after fork stay the parent's", cb != 0, why);
  }
  fill(g, 0xA);

  r = in_child(k3_child, 0);
  pb = check(g, 0xA);
  snprintf(why, sizeof why, "child bad=%d, parent bad=%d", r, pb);
  row("K3 read(2) into a shared page in the child", r || pb, why);

  r = in_child(k4_child, 0);
  pb = check(g, 0xA);
  snprintf(why, sizeof why, "child+grandchild bad=%d, parent bad=%d", r, pb);
  row("K4 three generations", r || pb, why);

  {
    uint64_t *pg5 = area();
    k5_page = pg5;
    pthread_t t;
    int bad5 = 0;
    if (pg5 && pthread_create(&t, 0, k5_writer, 0) == 0) {
      while (!k5_go) {}
      for (int i = 0; i < 10; i++) bad5 += in_child(k5_child, 0) != 0;
      k5_stop = 1;
      pthread_join(t, 0);
    } else bad5 = 99;
    snprintf(why, sizeof why, "%d of 10 children saw their snapshot move", bad5);
    row("K5 fork while a thread stores", bad5, why);
  }

  r = in_child(k6_child, 0);
  pb = check(g, 0xA);
  snprintf(why, sizeof why, "child bad=%d, parent bad=%d", r, pb);
  row("K6 mprotect R/RW in the child", r || pb, why);

  r = in_child(k7_child, 0);
  pb = check(g, 0xA);
  snprintf(why, sizeof why, "child page not zero=%d, parent bad=%d", r, pb);
  row("K7 MADV_DONTNEED in the child", r || pb, why);

  if (pipe(k8_pipe) == 0) {
    pid_t pid = fork();
    if (pid == 0) { int b = k8_child(0); _exit(b > 250 ? 250 : b); }
    munmap(g, NP * PG);
    (void)!write(k8_pipe[1], "u", 1);
    int st = 0;
    waitpid(pid, &st, 0);
    int cb = WIFEXITED(st) ? WEXITSTATUS(st) : 254;
    snprintf(why, sizeof why, "child bad=%d after the parent's munmap", cb);
    row("K8 parent munmap under a live child", cb != 0, why);
    g = area();
    if (g) fill(g, 0xA);
  }

  {
    uint64_t *big = mmap(0, 64 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long f0 = 0, f1 = 0;
    int bad9 = 0;
    if (big != MAP_FAILED) {
      for (size_t o = 0; o < (64u << 20) / 8; o += PG / 8) big[o] = o;
      g = big;
      for (int i = 0; i < 5; i++) bad9 += in_child(k9_child, 0) != 0; /* warm */
      f0 = free_kib();
      for (int i = 0; i < 200; i++) bad9 += in_child(k9_child, 0) != 0;
      f1 = free_kib();
      for (size_t o = 0; o < (64u << 20) / 8; o += PG / 8)
        if (big[o] != o) { bad9++; break; }
    } else bad9 = 99;
    long drop = f0 - f1;
    snprintf(why, sizeof why, "free %ld KiB -> %ld KiB (drop %ld KiB), bad=%d", f0, f1, drop, bad9);
    row("K9 200 fork/exit cycles of a 64 MiB parent", bad9 || drop > 65536, why);
  }
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
