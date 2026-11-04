/* src/user/linux_probe_vmcommit.c — Linux-ABI probe: the *commit* protocol.
 *
 * Why this probe exists (L24): the [MM] trace of a real RimWorld boot shows
 * that Unity does not use mremap, does not use MADV_DONTNEED, and never calls
 * mmap(MAP_FIXED) for its arenas.  Its whole memory protocol is:
 *
 *     mmap(NULL, big, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS)   reserve
 *     munmap(head); munmap(tail)                              trim to alignment
 *     madvise(NORMAL); mprotect(base, len, RW)                commit
 *
 * and then it writes objects into the committed window.  So the semantics that
 * matter are (a) a reservation is *reserved* and nothing else, (b) a partial
 * munmap splits and keeps the rest, (c) a partial mprotect commits exactly the
 * sub-range and leaves its neighbours as they were, and (d) committing does
 * not disturb a sibling window that was committed earlier.
 *
 * An ordinary musl/glibc x86_64 program: the SAME source runs on the host
 * (reference behaviour) and inside HobbyOS (measured behaviour), which is the
 * whole point — a row that cannot fail is decoration, and a row that fails on
 * both is a bad test.  Rows are "[LP] ..."; the last row is "[LP] done rc=<n>".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x4000
#endif
#ifndef MAP_STACK
#define MAP_STACK 0x20000
#endif

#define PAGE 4096UL
#define MiB  (1024UL * 1024UL)

static int fails;

static void row(const char *fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

static void check(int ok, const char *what, long detail) {
  if (!ok) fails++;
  row("[LP] %-34s %s detail=%ld\n", what, ok ? "ok" : "FAIL", detail);
}

static uint64_t align_up(uint64_t v, uint64_t a) {
  return (v + a - 1) & ~(a - 1);
}
static uint64_t align_dn(uint64_t v, uint64_t a) { return v & ~(a - 1); }

/* Fill every page of [p, p+len) with a value derived from the page index and
   verify it back.  A page that was silently dropped came back zero, so the
   value is never 0. */
static int fill_verify(unsigned char *p, size_t len, unsigned seed) {
  for (size_t off = 0; off < len; off += PAGE)
    p[off] = (unsigned char)(seed + (unsigned)(off >> 12) + 1);
  for (size_t off = 0; off < len; off += PAGE)
    if (p[off] != (unsigned char)(seed + (unsigned)(off >> 12) + 1)) return 0;
  return 1;
}

static int all_zero(unsigned char *p, size_t len) {
  for (size_t off = 0; off < len; off += PAGE)
    if (p[off] != 0) return 0;
  return 1;
}

/* --- A. the game's reservation dance ------------------------------------- */
/* Reserve 512 MiB - 4 KiB PROT_NONE, trim the unaligned head and tail so an
   aligned 256 MiB window remains, then commit two 4 MiB windows at its base
   and read the first one back.  Measured in the guest at [MM] #45..#66. */
static void test_reserve_trim_commit(void) {
  size_t want = 512 * MiB - PAGE;
  unsigned char *p = mmap(0, want, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) {
    row("[LP] reserve 512MiB               FAIL errno=%d\n", errno);
    fails++;
    return;
  }
  uint64_t base = (uint64_t)p;
  uint64_t lo = align_up(base, 256 * MiB);
  uint64_t hi = align_dn(base + want, 256 * MiB);
  if (hi - lo < 8 * MiB) { /* not enough room: ask for a bigger span next time */
    munmap(p, want);
    row("[LP] reserve 512MiB               skip (no aligned window)\n");
    return;
  }
  int head_ok = 1, tail_ok = 1;
  if (lo > base) head_ok = munmap((void *)base, lo - base) == 0;
  if (hi < base + want) tail_ok = munmap((void *)hi, base + want - hi) == 0;

  /* advisory, exactly as the game does it before the commit */
  int adv = madvise((void *)lo, 4 * MiB, MADV_NORMAL);
  int c1 = mprotect((void *)lo, 4 * MiB, PROT_READ | PROT_WRITE);
  int w1 = c1 == 0 ? fill_verify((unsigned char *)lo, 4 * MiB, 0x10) : 0;

  /* commit the NEXT window; the first must not move or lose a byte */
  int c2 = mprotect((void *)(lo + 4 * MiB), 4 * MiB, PROT_READ | PROT_WRITE);
  int w2 = c2 == 0 ? fill_verify((unsigned char *)(lo + 4 * MiB), 4 * MiB, 0x40)
                   : 0;
  int kept = fill_verify((unsigned char *)lo, 4 * MiB, 0x10);
  check(head_ok && tail_ok, "reserve trim (munmap head+tail)",
        (long)((head_ok << 1) | tail_ok));
  check(adv == 0 && c1 == 0 && w1, "commit first 4MiB",
        (long)((adv == 0) | ((c1 == 0) << 1) | (w1 << 2)));
  check(c2 == 0 && w2 && kept, "commit next 4MiB keeps sibling",
        (long)((c2 == 0) | (w2 << 1) | (kept << 2)));

  /* The *middle* of a reservation must also be committable (the game commits
     0x10B0020000 inside a 256 MiB reservation). */
  int c3 = mprotect((void *)(lo + 32 * MiB), 1 * MiB, PROT_READ | PROT_WRITE);
  int w3 = c3 == 0 ? fill_verify((unsigned char *)(lo + 32 * MiB), 1 * MiB, 0x70)
                   : 0;
  check(c3 == 0 && w3, "commit 1MiB in reservation middle",
        (long)((c3 == 0) | (w3 << 1)));
  munmap((void *)lo, hi - lo);
}

/* --- B. the per-thread stack shape --------------------------------------- */
/* mmap(PROT_NONE, 8 MiB + 1 page, MAP_STACK) then mprotect(base+page, 8 MiB,
   RW): the guard page must stay unmapped-for-access and the committed body
   must be writable.  Measured at [MM] #57/#58 (20 threads). */
static void test_stack_guard(void) {
  size_t len = 8 * MiB + PAGE;
  unsigned char *p = mmap(0, len, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (p == MAP_FAILED) {
    row("[LP] stack reserve                 FAIL errno=%d\n", errno);
    fails++;
    return;
  }
  int c = mprotect(p + PAGE, 8 * MiB, PROT_READ | PROT_WRITE);
  int w = c == 0 ? fill_verify(p + PAGE, 8 * MiB, 0x20) : 0;
  /* The guard was never written, so after committing it the page must be zero
     (a reservation hands out zero pages, never a recycled frame). */
  int g = mprotect(p, PAGE, PROT_READ | PROT_WRITE);
  int gz = g == 0 ? all_zero(p, PAGE) : 0;
  check(c == 0 && w, "stack commit (base+page, 8MiB)",
        (long)((c == 0) | (w << 1)));
  check(gz, "reserved guard page reads zero", (long)gz);
  munmap(p, len);
}

/* --- C. PROT_NONE keeps contents (Linux) --------------------------------- */
/* Reported, and counted as a failure: this is the one documented divergence in
   vm.c 4.4, and the game's protocol never depends on it (its PROT_NONE calls
   are guards over never-touched pages), so a FAIL here is a known cost and a
   PASS here is a pleasant surprise. */
static void test_prot_none_preserve(void) {
  size_t len = 64 * 1024;
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    fails++;
    row("[LP] prot_none base               FAIL errno=%d\n", errno);
    return;
  }
  int filled = fill_verify(p, len, 0x30);
  if (filled && mprotect(p, len, PROT_NONE) == 0 &&
      mprotect(p, len, PROT_READ | PROT_WRITE) == 0) {
    int kept = fill_verify(p, len, 0x30);
    row("[LP] prot_none preserves contents  %s (x64 compat: 0 expected)\n",
        kept ? "ok" : "FAIL");
    if (!kept) fails++;
  } else {
    fails++;
    row("[LP] prot_none transitions         FAIL\n");
  }
  munmap(p, len);
}

/* --- D. a hint inside a live mapping is only a hint ---------------------- */
static void test_hint_ignored(void) {
  size_t len = 4 * MiB;
  unsigned char *a = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (a == MAP_FAILED) {
    fails++;
    row("[LP] hint base                   FAIL errno=%d\n", errno);
    return;
  }
  memset(a, 0x5A, len);
  /* NOT MAP_FIXED: the kernel may not put the new mapping on top of a live one,
     whatever the hint says. */
  unsigned char *b = mmap(a + 64 * 1024, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int disjoint = b != MAP_FAILED &&
                 ((uint64_t)b + len <= (uint64_t)a ||
                  (uint64_t)b >= (uint64_t)a + len);
  int intact = ((unsigned char *)a)[0] == 0x5A &&
               ((unsigned char *)a)[len - 1] == 0x5A;
  check(disjoint && intact, "hint inside live map is ignored",
        (long)((disjoint << 1) | intact));
  if (b != MAP_FAILED) munmap(b, len);
  munmap(a, len);
}

/* --- E. partial munmap splits, keeps the rest ---------------------------- */
static void test_munmap_split(void) {
  size_t len = 4 * MiB;
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    fails++;
    row("[LP] munmap split base             FAIL errno=%d\n", errno);
    return;
  }
  int filled = fill_verify(p, len, 0x50);
  int rc = munmap(p + MiB, MiB);
  int left = ((unsigned char *)p)[MiB - PAGE] != 0;
  int right = ((unsigned char *)p)[2 * MiB] != 0;
  /* Re-unmapping the hole and unmapping a range that STARTS in a hole must
     both succeed (Linux). */
  int again = munmap(p + MiB, MiB) == 0;
  int across = munmap(p, MiB) == 0 && munmap(p + 3 * MiB, MiB) == 0;
  check(filled && rc == 0 && left && right && again && across,
        "munmap interior split",
        (long)(filled | ((rc == 0) << 1) | (left << 2) | (right << 3) |
               (again << 4) | (across << 5)));
  munmap(p, len);
}

/* --- F. mremap: move copies, shrink keeps, grow in place ----------------- */
static void test_mremap(void) {
  size_t len = 2 * MiB;
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    fails++;
    row("[LP] mremap base                  FAIL errno=%d\n", errno);
    return;
  }
  int filled = fill_verify(p, len, 0x60);
  unsigned char *g = mremap(p, len, 4 * MiB, MREMAP_MAYMOVE);
  int moved = g != MAP_FAILED;
  int kept = moved && fill_verify(g, len, 0x60);
  int tail0 = moved && all_zero(g + len, 4 * MiB - len);
  check(filled && kept && tail0, "mremap grow(maymove) copies",
        (long)(filled | (kept << 1) | (tail0 << 2)));
  if (!moved) return;

  /* shrink in place: same base, head preserved */
  unsigned char *s = mremap(g, 4 * MiB, 1 * MiB, 0);
  int shrink = s == g && fill_verify(s, 1 * MiB, 0x60);
  check(shrink, "mremap shrink in place", (long)shrink);

  /* grow in place one page: same base, old bytes intact */
  unsigned char *gi = mremap(s, 1 * MiB, 1 * MiB + PAGE, 0);
  int grow = gi == s && fill_verify(gi, 1 * MiB, 0x60);
  check(grow, "mremap grow in place", (long)grow);
  munmap(gi, 1 * MiB + PAGE);
}

/* --- G. madvise DONTNEED: pages go, mapping stays, refill is zero -------- */
static void test_madvise(void) {
  size_t len = 1 * MiB;
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    fails++;
    row("[LP] madvise base                 FAIL errno=%d\n", errno);
    return;
  }
  int filled = fill_verify(p, len, 0x80);
  int rc = filled ? madvise(p, len, MADV_DONTNEED) : -1;
  int zero = rc == 0 ? all_zero(p, len) : 0;
  int writable = rc == 0 ? fill_verify(p, len, 0x90) : 0;
  check(rc == 0 && zero && writable, "madvise DONTNEED zeroes anon",
        (long)((rc == 0) | (zero << 1) | (writable << 2)));
  munmap(p, len);
}

/* --- H. brk: grow across a SPLIT heap ------------------------------------ */
/* Raw SYS_brk, not sbrk(): musl caches its own break, which would measure the
   libc instead of the kernel.  The interesting shape is the one the audit
   found: a partial mprotect splits the heap VMA, and Linux still grows the
   break across it (brk extends the data segment whatever its VMA count).
   The HobbyOS handler used to demand ONE region covering the whole grow and
   then insert the whole span -- which overlapped the surviving fragment, so a
   single mprotect of a single heap page refused every later grow with ENOMEM
   for the life of the process. */
static void test_brk(void) {
  long h0 = syscall(SYS_brk, 0);
  if (h0 <= 0) {
    fails++;
    row("[LP] brk query                    FAIL\n");
    return;
  }
  row("[LP] brk start=0x%lx\n", h0);
  /* Map the first heap page, then make it READ-ONLY: a real protection split
     (an mprotect that lands on the same prot is merged back and proves
     nothing).  Linux grows the break across it -- only a *foreign* VMA is a
     conflict -- and the HobbyOS handler used to insert the whole span, overlap
     the surviving fragment and refuse every later grow for the life of the
     process. */
  long pre = syscall(SYS_brk, h0 + 128 * 1024);
  /* Split INSIDE the span the next grow covers: map past it, make one page
     read-only, then bring the break back below it.  A split under the break
     (or at it, which is not yet mapped) is outside [old_break,new_break) and
     proves nothing. */
  long at = h0 + 64 * 1024;
  errno = 0;
  int mp = mprotect((void *)at, 4096, PROT_READ);
  int mpe = errno;
  (void)syscall(SYS_brk, at);
  long g = syscall(SYS_brk, h0 + 256 * 1024);
  int grew = g == h0 + 256 * 1024;
  (void)mprotect((void *)at, 4096, PROT_READ | PROT_WRITE);
  int w = grew && fill_verify((unsigned char *)h0, 256 * 1024, 0xA0);
  long back = syscall(SYS_brk, h0 + 64 * 1024);
  int shrunk = back == h0 + 64 * 1024;
  /* And a grow that has never been mapped at all must work too (the lazy
     heap-region creation path). */
  long g2 = syscall(SYS_brk, h0 + 512 * 1024);
  int grew2 = g2 == h0 + 512 * 1024;
  int w2 = grew2 && fill_verify((unsigned char *)h0, 512 * 1024, 0xB0);
  (void)syscall(SYS_brk, h0);
  row("[LP] brk pre=0x%lx splitrc=%d/%d grow=0x%lx back=0x%lx\n", pre, mp, mpe,
      g, back);
  check(pre == h0 + 128 * 1024 && mp == 0 && grew && w && shrunk && grew2 && w2,
        "brk grow across split heap",
        (long)((pre == h0 + 128 * 1024) | (mp == 0) << 1 | (grew << 2) |
               (w << 3) | (shrunk << 4) | (grew2 << 5) | (w2 << 6)));
}

int main(void) {
  row("[LP] vmcommit probe pid=%d pagesz=%ld\n", (int)getpid(), (long)sysconf(_SC_PAGESIZE));
  test_reserve_trim_commit();
  test_stack_guard();
  test_prot_none_preserve();
  test_hint_ignored();
  test_munmap_split();
  test_mremap();
  test_madvise();
  test_brk();
  row("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
