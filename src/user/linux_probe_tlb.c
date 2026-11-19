/*
 * src/user/linux_probe_tlb.c - C1-k4: a mapping change made by one thread is
 * seen by another thread that is running on another CPU at that moment
 * (cross-CPU TLB shootdown, docs/cp-k4.md §2).
 *
 * ORDINARY musl Linux program; same static-PIE ELF on the reference kernel
 * (strix, Linux) and in the HobbyOS guest (8 vCPUs).  Every row is ONE
 * write(2); the last row is "[LP] done rc=<fails>".
 *
 * Shape of every row: thread A spins on a page (reads, or writes) counting
 * its accesses; once A is hot, thread B changes the mapping and reads A's
 * count the moment the call returns (c0).  Linux flushes every CPU before
 * munmap/mprotect/mremap/madvise return, so A's next access faults (or sees
 * the new contents): A's count at that point (c1) is c0 + 1 or so.  With only
 * the local TLB flushed, A keeps using the stale translation until its CPU
 * happens to reload CR3 -- c1 - c0 is then hundreds of thousands, and A may
 * read a frame the kernel has already handed to B's next mapping.
 *
 *  T1 munmap            A reads; B munmaps; A must SIGSEGV (SEGV_MAPERR)
 *  T2 mprotect NONE     A reads; B mprotects PROT_NONE; A must SIGSEGV
 *  T3 mprotect READ     A writes; B mprotects PROT_READ; A must SIGSEGV
 *                       (SEGV_ACCERR) and no write may land after it
 *  T4 mremap move       A reads the old address; B moves the mapping; A
 *                       must SIGSEGV at the old address
 *  T5 madvise DONTNEED  A reads a private anonymous page; B drops it; A must
 *                       see the zero page
 * In T1/T2/T4 B maps and fills fresh pages right after the call, so a frame
 * reused before A's CPU flushed shows up as a foreign value in A's reads.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ITERS 12
#define SLACK 1000 /* accesses allowed between "the call returned" and A's fault */
#define MAGIC 0x5AFEC0DE5AFEC0DEULL

static int fails;
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

enum { M_READ, M_WRITE, M_ZERO };
static volatile uint64_t *a_page;
static volatile int a_mode;
static volatile unsigned long a_count, a_fault_count;
static volatile int a_faulted, a_code, a_done, a_go;
static volatile uint64_t a_foreign, a_zero_at;
static volatile void *a_addr;
static __thread sigjmp_buf a_jb;
static __thread int a_is_a;

static void segv(int sig, siginfo_t *si, void *uc) {
  (void)sig;
  (void)uc;
  if (!a_is_a) _exit(99); /* B never faults */
  a_fault_count = a_count;
  a_code = si->si_code;
  a_addr = si->si_addr;
  a_faulted = 1;
  siglongjmp(a_jb, 1);
}

static void *thread_a(void *v) {
  (void)v;
  a_is_a = 1;
  if (sigsetjmp(a_jb, 1)) {
    a_done = 1;
    return 0;
  }
  while (!a_go) {}
  volatile uint64_t *p = a_page;
  for (;;) {
    if (a_mode == M_WRITE) {
      p[0] = a_count;
    } else {
      uint64_t v2 = p[0];
      if (a_mode == M_ZERO) {
        if (v2 == 0) {
          a_zero_at = a_count;
          a_done = 1;
          return 0;
        }
        if (v2 != 7 && !a_foreign) a_foreign = v2;
      } else if (v2 != MAGIC) {
        a_foreign = v2;        /* stop: a new mapping (or a reused frame) */
        a_fault_count = a_count;
        a_done = 1;
        return 0;
      }
    }
    a_count++;
    if (a_done) return 0; /* B gave up */
  }
}

static uint64_t *map_page_hint(void *hint) {
  void *m = mmap(hint, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return m == MAP_FAILED ? 0 : (uint64_t *)m;
}
static uint64_t *map_page(void) {
  void *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return m == MAP_FAILED ? 0 : (uint64_t *)m;
}

/* One iteration: returns 0 ok, 1 stale, 2 never faulted, 3 foreign data */
static int one(int row, int it, char *why, size_t whyn) {
  uint64_t *pg = map_page();
  if (!pg) { snprintf(why, whyn, "mmap errno=%d", errno); return 9; }
  pg[0] = row == 5 ? 7 : MAGIC;
  a_page = pg;
  a_mode = row == 3 ? M_WRITE : (row == 5 ? M_ZERO : M_READ);
  a_count = a_fault_count = 0;
  a_faulted = a_done = a_go = 0;
  a_foreign = 0;
  a_zero_at = 0;
  pthread_t t;
  pthread_create(&t, 0, thread_a, 0);
  a_go = 1;
  while (a_count < 200000) {}  /* A is hot: its TLB holds the page */
  int rc = 0;
  void *moved = 0;
  if (row == 1) rc = munmap(pg, 4096);
  else if (row == 2 || row == 3) rc = mprotect(pg, 4096, row == 2 ? PROT_NONE : PROT_READ);
  else if (row == 4) {
    void *dst = mmap(0, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    moved = mremap(pg, 4096, 4096, MREMAP_MAYMOVE | MREMAP_FIXED, (char *)dst + 4096);
    rc = moved == MAP_FAILED ? -1 : 0;
  } else rc = madvise(pg, 4096, MADV_DONTNEED);
  unsigned long c0 = a_count;
  if (rc) { snprintf(why, whyn, "call failed errno=%d", errno); a_done = 1; pthread_join(t, 0); return 9; }
  /* fresh pages filled with a foreign value: a reused frame shows up */
  uint64_t *fresh[16] = {0};
  if (row == 1 || row == 2 || row == 4)
    for (int i = 0; i < 16; i++) {
      fresh[i] = map_page_hint((char *)pg + (1UL << 30) + (unsigned long)i * 4096);
      if (fresh[i]) for (int w = 0; w < 512; w++) fresh[i][w] = 0xBADBADBAD0000000ULL | (uint64_t)(it * 16 + i);
    }
  long t0 = now_ms();
  while (!a_done && now_ms() - t0 < 3000) {}
  int ret = 0;
  if (!a_done) {
    a_done = 1; /* release A; it exits on its next loop */
    snprintf(why, whyn, "A kept going 3 s after the call (count %lu -> %lu)", c0, a_count);
    ret = 2;
  } else if (row == 5) {
    if (a_zero_at > c0 + SLACK) {
      snprintf(why, whyn, "A saw the dropped page for %lu accesses after madvise", a_zero_at - c0);
      ret = 1;
    }
  } else if (!a_faulted) {
    int reused = 0; /* the VA itself went to a fresh mapping: legal reads */
    for (int i = 0; i < 16; i++) if (fresh[i] == pg) reused = 1;
    if (a_foreign && !reused) {
      snprintf(why, whyn, "A read 0x%llx through the old VA: another mapping's frame",
               (unsigned long long)a_foreign);
      ret = 3;
    } else if (!a_foreign) {
      snprintf(why, whyn, "A finished without a fault");
      ret = 2;
    } else if (a_fault_count > c0 + SLACK) {
      snprintf(why, whyn, "A used the stale translation %lu more times (c0=%lu)", a_fault_count - c0, c0);
      ret = 1;
    }
  } else if (a_fault_count > c0 + SLACK) {
    snprintf(why, whyn, "A used the stale translation %lu more times (c0=%lu)", a_fault_count - c0, c0);
    ret = 1;
  } else if (row == 3 && (a_code != SEGV_ACCERR)) {
    snprintf(why, whyn, "si_code %d, want SEGV_ACCERR", a_code);
    ret = 4;
  } else if ((row == 1 || row == 4) && a_code != SEGV_MAPERR) {
    snprintf(why, whyn, "si_code %d, want SEGV_MAPERR", a_code);
    ret = 4;
  } else if (a_addr != (void *)pg) {
    snprintf(why, whyn, "si_addr %p, want %p", (void *)a_addr, (void *)pg);
    ret = 4;
  }

  pthread_join(t, 0);
  if (row == 1 || row == 2 || row == 4)
    for (int i = 0; i < 16; i++) if (fresh[i]) munmap(fresh[i], 4096);
  if (row == 2 || row == 3 || row == 5) munmap(pg, 4096);
  if (row == 4 && moved) munmap((char *)moved - 4096, 8192);
  return ret;
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = segv;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
  static const char *names[] = {"", "T1 munmap", "T2 mprotect(PROT_NONE)", "T3 mprotect(PROT_READ) under writes",
                                "T4 mremap move", "T5 madvise(DONTNEED)"};
  for (int row = 1; row <= 5; row++) {
    int bad = 0;
    char why[200] = "";
    for (int it = 0; it < ITERS && !bad; it++) {
      int r = one(row, it, why, sizeof why);
      if (r) { bad = 1; out("[LP] FAIL %s iter %d: %s\n", names[row], it, why); }
    }
    if (!bad) out("[LP] PASS %s (%d iterations, slack %d)\n", names[row], ITERS, SLACK);
    fails += bad;
  }
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
