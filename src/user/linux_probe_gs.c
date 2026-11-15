/*
 * src/user/linux_probe_gs.c — cp P1 (docs/cyberpunk.md P1): the user GS base.
 *
 * Windows x64 code addresses its TEB through %gs, and Wine installs that base
 * on Linux with arch_prctl(ARCH_SET_GS).  ORDINARY musl Linux program: no
 * HobbyOS header, so the same static-PIE binary runs on a Linux box (the
 * reference) and inside the intel guest.  Rows "[LP] ...", last
 * "[LP] done rc=<fails>".
 *
 *   G0  ARCH_SET_GS / ARCH_GET_GS round-trip on the main thread, and %gs:0 /
 *       %gs:8 read the block it names; a non-canonical base is refused
 *       (EPERM, Linux's answer) and leaves the old base live.
 *   G1  8 threads, each first checks it INHERITED the creator's base (Linux
 *       clone copies it), then installs its own distinct block and keeps
 *       reading %gs:-relative words across many sched_yield()s, futex waits
 *       (timeouts) and busy spins while 4 extra spinner threads oversubscribe
 *       the CPUs (forced preemption).  Each also takes a self-sent signal
 *       whose handler must see the thread's own %gs.
 *   G2  fork(): the child sees the parent's base (%gs:8 and ARCH_GET_GS).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004
#ifndef SYS_futex
#define SYS_futex 202
#endif
#define FUTEX_WAIT_PRIVATE 128

static int fails;

static void row(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

struct gsblk {
  uint64_t self; /* %gs:0 */
  uint64_t id;   /* %gs:8 */
  uint64_t pad[6];
} __attribute__((aligned(64)));

#define NGS 8
#define NSPIN 4
static struct gsblk blk[NGS + 1]; /* [NGS] is the main thread's */

static long set_gs(uint64_t v) { return syscall(SYS_arch_prctl, ARCH_SET_GS, v); }
static long get_gs(uint64_t *v) { return syscall(SYS_arch_prctl, ARCH_GET_GS, v); }

static inline uint64_t gs0(void) {
  uint64_t v;
  __asm__ volatile("mov %%gs:0, %0" : "=r"(v));
  return v;
}
static inline uint64_t gs8(void) {
  uint64_t v;
  __asm__ volatile("mov %%gs:8, %0" : "=r"(v));
  return v;
}

/* n reads of %gs:8 in a tight loop; returns how many were not `id`. */
static uint64_t gs_spin(uint64_t id, uint64_t n) {
  uint64_t bad = 0;
  __asm__ volatile("1: mov %%gs:8, %%rax\n cmp %[id], %%rax\n je 2f\n"
                   " inc %[bad]\n2: pause\n dec %[n]\n jnz 1b\n"
                   : [bad] "+r"(bad), [n] "+r"(n)
                   : [id] "r"(id)
                   : "rax", "cc");
  return bad;
}

static volatile int stop_spinners;
static void *spinner(void *vp) {
  (void)vp;
  volatile uint64_t x = 0;
  while (!stop_spinners) x++;
  return NULL;
}

static __thread uint64_t t_sig_gs; /* fs-based TLS: independent of gs */
static __thread int t_sig_ran;
static void on_usr2(int sig) {
  (void)sig;
  t_sig_gs = gs8();
  t_sig_ran++;
}

struct garg {
  int i;
  uint64_t inherited;
  long set_rc, get_rc;
  uint64_t got;
  uint64_t bad_reads, reads;
  int sig_ok;
  int cpus;
};

static void *gs_thread(void *vp) {
  struct garg *a = vp;
  uint64_t g = 0;
  get_gs(&g);
  a->inherited = g;
  blk[a->i].self = (uint64_t)(uintptr_t)&blk[a->i];
  blk[a->i].id = 1000 + (uint64_t)a->i;
  a->set_rc = set_gs((uint64_t)(uintptr_t)&blk[a->i]);
  a->got = 0;
  a->get_rc = get_gs(&a->got);
  const uint64_t id = blk[a->i].id, self = blk[a->i].self;
  uint64_t cpumask = 0;
  int futex_word = 0;
  for (int it = 0; it < 300; it++) {
    a->reads += 2;
    if (gs8() != id) a->bad_reads++;
    sched_yield();
    if (gs0() != self) a->bad_reads++;
    if ((it & 7) == 0) {
      struct timespec ts = {0, 1000000}; /* 1 ms: ETIMEDOUT */
      syscall(SYS_futex, &futex_word, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0);
      a->reads++;
      if (gs8() != id) a->bad_reads++;
    }
    if ((it & 15) == 0) {
      a->bad_reads += gs_spin(id, 300000);
      a->reads += 300000;
    }
    int c = sched_getcpu();
    if (c >= 0 && c < 64) cpumask |= 1ull << c;
  }
  t_sig_ran = 0;
  syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR2);
  a->sig_ok = (t_sig_ran == 1 && t_sig_gs == id && gs8() == id);
  a->cpus = __builtin_popcountll(cpumask);
  return NULL;
}

int main(void) {
  row("[LP] gs: start pid=%d\n", (int)getpid());
  signal(SIGUSR2, on_usr2);

  /* G0 */
  {
    int bad = 0;
    struct gsblk *m = &blk[NGS];
    m->self = (uint64_t)(uintptr_t)m;
    m->id = 99;
    long rc = set_gs(m->self);
    uint64_t got = 0;
    long grc = get_gs(&got);
    uint64_t r0 = gs0(), r8 = gs8();
    errno = 0;
    long nrc = set_gs(0x8000000000000000ull);
    int nerr = errno;
    uint64_t after = 0;
    get_gs(&after);
    uint64_t r8b = gs8();
    row("[LP] G0 set=%ld get=%ld got=%s gs:0=%s gs:8=%llu noncanon_rc=%ld "
        "errno=%d base_after=%s gs:8_after=%llu\n",
        rc, grc, got == m->self ? "match" : "MISMATCH",
        r0 == m->self ? "self" : "WRONG", (unsigned long long)r8, nrc, nerr,
        after == m->self ? "unchanged" : "CHANGED", (unsigned long long)r8b);
    if (rc != 0 || grc != 0 || got != m->self || r0 != m->self || r8 != 99)
      bad = 1;
    if (nrc != -1 || nerr != EPERM || after != m->self || r8b != 99) bad = 1;
    row("[LP] G0 %s\n", bad ? "FAIL" : "PASS");
    fails += bad;
  }

  /* G1 */
  {
    pthread_t sp[NSPIN], th[NGS];
    struct garg a[NGS];
    stop_spinners = 0;
    for (int i = 0; i < NSPIN; i++) pthread_create(&sp[i], NULL, spinner, NULL);
    for (int i = 0; i < NGS; i++) {
      memset(&a[i], 0, sizeof a[i]);
      a[i].i = i;
      if (pthread_create(&th[i], NULL, gs_thread, &a[i]) != 0) {
        row("[LP] G1 FAIL pthread_create %d\n", i);
        fails++;
        return 1;
      }
    }
    int bad = 0;
    uint64_t reads = 0, badr = 0;
    int maxc = 0;
    for (int i = 0; i < NGS; i++) {
      pthread_join(th[i], NULL);
      reads += a[i].reads;
      badr += a[i].bad_reads;
      if (a[i].cpus > maxc) maxc = a[i].cpus;
      int tb = a[i].inherited != blk[NGS].self || a[i].set_rc != 0 ||
               a[i].get_rc != 0 || a[i].got != (uint64_t)(uintptr_t)&blk[i] ||
               a[i].bad_reads != 0 || !a[i].sig_ok;
      if (tb)
        row("[LP] G1 thread %d FAIL inherited=%s set=%ld get=%ld got=%s "
            "bad_reads=%llu sig_ok=%d\n",
            i, a[i].inherited == blk[NGS].self ? "parent" : "WRONG",
            a[i].set_rc, a[i].get_rc,
            a[i].got == (uint64_t)(uintptr_t)&blk[i] ? "match" : "MISMATCH",
            (unsigned long long)a[i].bad_reads, a[i].sig_ok);
      bad |= tb;
    }
    stop_spinners = 1;
    for (int i = 0; i < NSPIN; i++) pthread_join(sp[i], NULL);
    /* the main thread's base is untouched by all of that */
    if (gs8() != 99) {
      row("[LP] G1 FAIL main thread's %%gs:8 changed to %llu\n",
          (unsigned long long)gs8());
      bad = 1;
    }
    row("[LP] G1 %s threads=%d spinners=%d gs_reads=%llu bad=%llu "
        "max_cpus_per_thread=%d\n",
        bad ? "FAIL" : "PASS", NGS, NSPIN, (unsigned long long)reads,
        (unsigned long long)badr, maxc);
    fails += bad;
  }

  /* G2 */
  {
    pid_t c = fork();
    if (c == 0) {
      uint64_t g = 0;
      get_gs(&g);
      int ok = g == blk[NGS].self && gs8() == 99 && gs0() == blk[NGS].self;
      _exit(ok ? 0 : 3);
    }
    int st = 0;
    pid_t w = waitpid(c, &st, 0);
    int ok = c > 0 && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    row("[LP] G2 %s fork child inherits the base (pid=%d status=0x%x)\n",
        ok ? "PASS" : "FAIL", (int)c, st);
    if (!ok) fails++;
  }

  row("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
