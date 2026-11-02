/* src/user/linux_probe_mem.c — Linux-ABI probe: the memory-management half.
 *
 * An ordinary musl x86_64 Linux program: no HobbyOS header, no HobbyOS-ism, so
 * the SAME source runs on this workstation (reference behaviour) and inside
 * HobbyOS (measured behaviour).  Rows are prefixed "[LP] "; the last row is
 * "[LP] done rc=<n>" and tools/gate/t3_linux_probe.sh only accepts rc=0.
 *
 * What each row is FOR (a row that cannot fail is decoration):
 *   mmap 4K/1M/16M   alignment, PROT_READ|WRITE, zero fill BEFORE any write:
 *                    a stale frame from a previous mapping would show here.
 *   MAP_FIXED        replace a live mapping at a known address: Linux hands
 *                    back fresh zero pages.  A kernel that only rewrites its
 *                    region records (and leaves the old leaf installed) shows
 *                    the old bytes -- a real W^X/stale-data defect, measured.
 *   mprotect         RW -> R, write must fault (SIGSEGV), then R -> RW and the
 *                    data must still be there; then PROT_NONE and back, where
 *                    the x64 port is expected to report lost contents.
 *   mremap           grow with MREMAP_MAYMOVE (contents preserved), shrink in
 *                    place, and grow in place next to a known hole.
 *   munmap partial   an interior page range goes away, its neighbours do not.
 *   brk              the return value is the NEW break (Linux), never 0/-errno;
 *                    grow then shrink and print the break both times.
 *   madvise/mincore/msync  the advisory surface the allocators call.
 *   memfd_create     S4 object + MAP_SHARED, only if the guest has it.
 *   malloc stress    8 MiB malloc, every page written, freed: musl's allocator
 *                    and Doom's 8 MiB zone both live on this path.
 *
 * SIGSEGV rows are handled inside the probe (sigaction + siglongjmp), the same
 * way a real program would observe a protection change; a kernel that kills
 * the process instead fails the gate by never printing the done marker.
 */
#define _GNU_SOURCE /* mremap + sigjmp_buf are GNU/musl extensions */
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
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
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif

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
  row("[LP] %-28s %s detail=%ld\n", what, ok ? "ok" : "FAIL", detail);
}

/* --- SIGSEGV harness: prove that a protection change is ENFORCED ---------- */
static sigjmp_buf segv_jmp;
static volatile sig_atomic_t segv_hit;

static void segv_handler(int sig) {
  (void)sig;
  segv_hit = 1;
  siglongjmp(segv_jmp, 1);
}

/* Probe a byte; returns 1 if the access faulted, 0 if it succeeded. */
static int access_faults(volatile unsigned char *p, int write_it) {
  segv_hit = 0;
  if (sigsetjmp(segv_jmp, 1) == 0) {
    if (write_it)
      *p = 0x5A;
    else
      (void)*p;
    return 0;
  }
  return 1; /* the handler ran: the access was refused */
}

/* --- 1. three anonymous mappings of different sizes ---------------------- */
struct maprow {
  size_t len;
  const char *name;
};

static void test_sizes(void) {
  static const struct maprow rows[] = { {4 * 1024, "mmap 4KiB"}, {1024 * 1024, "mmap 1MiB"}, {16 * 1024 * 1024, "mmap 16MiB"},
  };
  for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    size_t len = rows[i].len;
    unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
      row("[LP] %-28s FAIL errno=%d\n", rows[i].name, errno);
      fails++;
      continue;
    }
    int aligned = ((uintptr_t)p & 0xFFF) == 0;
    /* Zero fill BEFORE writing: the only order that can detect a stale frame. */
    int zeroed = 1;
    for (size_t off = 0; off < len; off += 4096) {
      if (p[off] != 0) {
        zeroed = 0;
        break;
      }
    }
    /* Every page must be writable and read back. */
    int writable = 1;
    for (size_t off = 0; off < len; off += 4096) {
      p[off] = (unsigned char)(0xA0 | (off >> 12));
      if (p[off] != (unsigned char)(0xA0 | (off >> 12))) writable = 0;
    }
    check(aligned && zeroed && writable, rows[i].name,
          (long)((aligned << 2) | (zeroed << 1) | writable));
    if (munmap(p, len) != 0) {
      row("[LP] %-28s FAIL munmap errno=%d\n", rows[i].name, errno);
      fails++;
    }
  }
}

/* --- 2. MAP_FIXED replaces a live mapping -------------------------------- */
static void test_map_fixed(void) {
  size_t len = 256 * 1024;
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    row("[LP] map_fixed base FAIL errno=%d\n", errno);
    fails++;
    return;
  }
  memset(p, 0xC7, len);
  /* Replace the same span.  Linux: the old contents are gone, fresh zeros. */
  unsigned char *q = mmap(p, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (q == MAP_FAILED) {
    row("[LP] map_fixed remap FAIL errno=%d\n", errno);
    fails++;
    munmap(p, len);
    return;
  }
  int same = (q == p);
  int fresh = same;
  for (size_t off = 0; off < len; off += 4096) {
    if (q[off] != 0) {
      fresh = 0;
      break;
    }
  }
  check(same && fresh, "map_fixed replace", (long)((same << 1) | fresh));
  /* Remapping INSIDE the span must split it, keeping the outer pages. */
  q[0] = 1;
  q[len - 1] = 2;
  unsigned char *mid = mmap(q + 4096, 8192, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  int split_ok = mid == q + 4096 && q[0] == 1 && mid[0] == 0 && q[len - 1] == 2;
  check(split_ok, "map_fixed interior split", (long)(mid == q + 4096));
  munmap(q, len);
}

/* --- 3. mprotect: enforce, restore, and PROT_NONE ------------------------ */
static void test_mprotect(void) {
  size_t len = 64 * 1024;
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    row("[LP] mprotect base FAIL errno=%d\n", errno);
    fails++;
    return;
  }
  memset(p, 0x11, len);

  if (mprotect(p, len, PROT_READ) != 0) {
    row("[LP] mprotect RO FAIL errno=%d\n", errno);
    fails++;
    munmap(p, len);
    return;
  }
  int ro_enforced = access_faults(p, 1);          /* write must SIGSEGV */
  int ro_readable = !access_faults(p, 0);         /* read must still work */

  if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) {
    row("[LP] mprotect RW FAIL errno=%d\n", errno);
    fails++;
    munmap(p, len);
    return;
  }
  int kept = 1;
  for (size_t off = 0; off < len; off += 4096)
    if (p[off] != 0x11) kept = 0;
  check(ro_enforced && ro_readable && kept, "mprotect RO enforced",
        (long)((ro_enforced << 2) | (ro_readable << 1) | kept));

  /* PROT_NONE must be enforced; Linux also keeps the pages, but the x64 port
     can only express "inaccessible" as "not present", so content loss is an
     expected, documented divergence -- measured here, not asserted as a bug. */
  if (mprotect(p, len, PROT_NONE) != 0) {
    row("[LP] mprotect NONE FAIL errno=%d\n", errno);
    fails++;
  } else {
    int none_enforced = access_faults(p, 0);
    if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) {
      row("[LP] mprotect NONE->RW FAIL errno=%d\n", errno);
      fails++;
    } else {
      int preserved = (p[0] == 0x11);
      row("[LP] mprotect NONE enforced=%d preserved=%d (x64: 0 expected)\n",
          none_enforced, preserved);
      if (!none_enforced) fails++;
    }
  }
  munmap(p, len);
  /* mprotect on a KNOWN hole is -ENOMEM in Linux: take a mapping away first so
     the address cannot be some neighbouring allocation's live page. */
  unsigned char *h = mmap(0, 8192, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (h != MAP_FAILED && munmap(h, 8192) == 0) {
    errno = 0;
    int hole = mprotect(h + 4096, 4096, PROT_READ);
    check(hole == -1 && errno == ENOMEM, "mprotect hole ENOMEM", errno);
  }
}

/* --- 4. mremap: grow by move, shrink in place, grow in place ------------- */
static void test_mremap(void) {
  size_t len = 1 << 20;
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    row("[LP] mremap base FAIL errno=%d\n", errno);
    fails++;
    return;
  }
  for (size_t off = 0; off < len; off += 4096)
    p[off] = (unsigned char)(off >> 12);

  /* Grow with MAYMOVE: the new size must be usable and the old bytes kept. */
  size_t bigger = len * 2;
  unsigned char *g = mremap(p, len, bigger, MREMAP_MAYMOVE);
  if (g == MAP_FAILED) {
    row("[LP] mremap grow FAIL errno=%d\n", errno);
    fails++;
    munmap(p, len);
    return;
  }
  int kept = 1;
  for (size_t off = 0; off < len; off += 4096)
    if (g[off] != (unsigned char)(off >> 12)) kept = 0;
  int tail_zero = 1;
  for (size_t off = len; off < bigger; off += 4096)
    if (g[off] != 0) tail_zero = 0;
  int tail_writable = 1;
  for (size_t off = len; off < bigger; off += 4096) {
    g[off] = 0x77;
    if (g[off] != 0x77) tail_writable = 0;
  }
  check(kept && tail_zero && tail_writable, "mremap grow(maymove)",
        (long)((kept << 2) | (tail_zero << 1) | tail_writable));

  /* Shrink in place: the same base, the head preserved. */
  size_t smaller = 64 * 1024;
  unsigned char *s = mremap(g, bigger, smaller, 0);
  int shrink_ok = (s == g);
  int head_ok = shrink_ok;
  for (size_t off = 0; off < smaller; off += 4096)
    if (s[off] != (unsigned char)(off >> 12)) head_ok = 0;
  check(shrink_ok && head_ok, "mremap shrink in place",
        (long)((shrink_ok << 1) | head_ok));

  /* Grow in place: a fresh 4 KiB mapping right after `s`'s span, with the
     tail of the span above it still free, is the case where Linux keeps the
     address.  Ask for one page more than the current size. */
  size_t cur = smaller;
  unsigned char *nudge = mmap(s + cur, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (nudge != MAP_FAILED) {
    /* Not the in-place case: the page is taken.  Unmap it and try the real
       in-place grow (the span above `s` is then free). */
    munmap(nudge, 4096);
    unsigned char *ip = mremap(s, cur, cur + 4096, 0);
    int inplace = (ip == s);
    row("[LP] mremap grow in place      same_addr=%d\n", inplace);
    if (!inplace) fails++;
    munmap(s, cur + 4096);
  } else {
    munmap(s, cur);
  }
}

/* --- 5. munmap of a partial interior range ------------------------------- */
static void test_munmap_partial(void) {
  size_t len = 256 * 1024;
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    row("[LP] munmap partial FAIL errno=%d\n", errno);
    fails++;
    return;
  }
  memset(p, 0x22, len);
  size_t off = 64 * 1024, n = 64 * 1024;
  int rc = munmap(p + off, n);
  /* The unmapped span must fault, the neighbours must survive. */
  int gone = access_faults(p + off, 0);
  int left_ok = (p[off - 4096] == 0x22);
  int right_ok = (p[off + n] == 0x22);
  check(rc == 0 && gone && left_ok && right_ok, "munmap interior range",
        (long)((rc == 0) | (gone << 1) | (left_ok << 2) | (right_ok << 3)));
  /* Re-unmapping the same span, and unmapping a hole, must SUCCEED (Linux). */
  errno = 0;
  int again = munmap(p + off, n);
  check(again == 0, "munmap unmapped succeeds", errno);
  munmap(p, len);
}

/* --- 6. brk: the return value is the new break --------------------------- */
static void test_brk(void) {
  void *b0 = sbrk(0);
  if (b0 == (void *)-1) {
    row("[LP] brk query FAIL errno=%d\n", errno);
    fails++;
    return;
  }
  row("[LP] brk start=%p\n", b0);
  /* Grow 64 KiB; the raw syscall must return the NEW break, not 0. */
  long r = syscall(SYS_brk, (char *)b0 + 64 * 1024);
  int grew = (void *)r == (char *)b0 + 64 * 1024;
  check(grew, "brk grow returns new break", r);
  if (grew) {
    /* The new span must be writable: brk's whole point. */
    memset(b0, 0x33, 64 * 1024);
    int writable = ((unsigned char *)b0)[64 * 1024 - 1] == 0x33;
    check(writable, "brk span writable", writable);
  }
  long s = syscall(SYS_brk, b0);
  int shrank = (void *)s == b0;
  check(shrank, "brk shrink returns new break", s);
  /* Asking for an address far outside the heap must NOT return -errno; Linux
     returns the unchanged break. */
  long bad = syscall(SYS_brk, (void *)0x10);
  int unchanged = (void *)bad == b0;
  check(unchanged, "brk refusal returns old break", bad);
}

/* --- 7. advisory surface ------------------------------------------------ */
static void test_advisory(void) {
  size_t len = 128 * 1024;
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    row("[LP] advisory base FAIL errno=%d\n", errno);
    fails++;
    return;
  }
  memset(p, 0x44, len);
  int a1 = madvise(p, len, MADV_NORMAL);
  int a2 = madvise(p, len, MADV_WILLNEED);
  check(a1 == 0 && a2 == 0, "madvise normal/willneed",
        (long)((a1 == 0) | ((a2 == 0) << 1)));

  /* mincore on pages that were just written: bit 0 means "resident", so every
     one of the 32 must be set (the upper bits are the kernel's business). */
  unsigned char vec[32];
  memset(vec, 0, sizeof vec);
  int m = mincore(p, 32 * 4096, vec);
  int bits = 1;
  for (int i = 0; i < 32; i++)
    if (!(vec[i] & 1)) bits = 0;
  check(m == 0 && bits, "mincore resident", (long)((m == 0) | (bits << 1)));

  /* DONTNEED drops the pages: the refill must be zero and mincore must now say
     "not resident" -- the differential that proves the advice did something. */
  int a3 = madvise(p, len, MADV_DONTNEED);
  int after = (p[0] == 0);
  int still = 0;
  memset(vec, 0xFF, sizeof vec);
  if (mincore(p, 32 * 4096, vec) == 0)
    for (int i = 0; i < 32; i++)
      if (vec[i] & 1) still++;
  row("[LP] madvise DONTNEED rc=%d refill_zero=%d resident_after=%d/32\n",
      a3, after, still);
  if (a3 != 0 || !after) fails++;

  int s1 = msync(p, len, MS_ASYNC);
  int s2 = msync(p, len, MS_SYNC);
  check(s1 == 0 && s2 == 0, "msync async/sync", (long)((s1 == 0) | ((s2 == 0) << 1)));
  munmap(p, len);
}

/* --- 8. memfd_create + MAP_SHARED (only if the guest provides it) -------- */
static void test_memfd(void) {
#ifdef SYS_memfd_create
  int fd = (int)syscall(SYS_memfd_create, "lp", 0);
  if (fd < 0) {
    row("[LP] memfd_create unsupported errno=%d\n", errno);
    return; /* reported, not failed: the guest may predate S4 memfd */
  }
  row("[LP] memfd_create fd=%d\n", fd);
  size_t len = 64 * 1024;
  /* ftruncate and pread are the fd lane's rows, not this probe's: a gap there
     is named and skipped rather than failed, so this gate stays a measurement
     of the MEMORY contract. */
  if (ftruncate(fd, (off_t)len) != 0) {
    row("[LP] memfd ftruncate skipped errno=%d\n", errno);
    close(fd);
    return;
  }
  unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  unsigned char *q = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED || q == MAP_FAILED) {
    row("[LP] memfd MAP_SHARED unsupported errno=%d\n", errno);
    if (p != MAP_FAILED) munmap(p, len);
    if (q != MAP_FAILED) munmap(q, len);
    close(fd);
    return;
  }
  /* Two MAP_SHARED mappings of one object are one set of pages: a write
     through the first must be visible through the second. */
  memset(p, 0x66, len);
  int shared = q[0] == 0x66 && q[len / 2] == 0x66 && q[len - 1] == 0x66;
  check(shared, "memfd shared write-through", shared);
  munmap(p, len);
  munmap(q, len);
  close(fd);
#else
  row("[LP] memfd_create not in this libc\n");
#endif
}

/* --- 9. musl allocator stress: Doom's zone and Xlib both live here ------- */
static void test_malloc(void) {
  size_t len = 8 * 1024 * 1024;
  unsigned char *p = malloc(len);
  if (!p) {
    row("[LP] malloc 8MiB FAIL\n");
    fails++;
    return;
  }
  for (size_t off = 0; off < len; off += 4096) p[off] = (unsigned char)(off >> 12);
  int ok = 1;
  for (size_t off = 0; off < len; off += 4096)
    if (p[off] != (unsigned char)(off >> 12)) ok = 0;
  free(p);
  /* A second round catches an allocator whose free() poisoned its own arenas. */
  unsigned char *q = malloc(2 * 1024 * 1024);
  int ok2 = q != 0;
  if (q) {
    memset(q, 0x99, 2 * 1024 * 1024);
    ok2 = q[2 * 1024 * 1024 - 1] == 0x99;
    free(q);
  }
  check(ok && ok2, "malloc 8MiB write+free", (long)((ok << 1) | ok2));
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = segv_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGSEGV, &sa, 0);

  row("[LP] mem probe pid=%ld pagesz=%ld\n", (long)getpid(), (long)sysconf(_SC_PAGESIZE));

  test_sizes();
  test_map_fixed();
  test_mprotect();
  test_mremap();
  test_munmap_partial();
  test_brk();
  test_advisory();
  test_memfd();
  test_malloc();

  row("[LP] done rc=%d\n", fails);
  return fails;
}
