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
 * PROTECTION ENFORCEMENT IS REPORTED, NOT ASSERTED.  Observing that a
 * PROT_READ page really refuses a write needs the kernel to turn a HARDWARE
 * fault into a caught signal.  The compat layer does not do that yet: explicit
 * raise()/kill() delivery works, but a bad access still terminates the process.
 * A faulting row here would therefore kill the probe before mremap/munmap/brk/
 * malloc ever ran, and would be asserting a capability the layer does not
 * promise.  So the default build prints those rows as "unmeasurable" with the
 * reason and never faults; build with -DLP_FAULT_PROBE on a tree that delivers
 * fault signals to turn them into real assertions.
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

/* --- fault entrapment: opt-in only ---------------------------------------
 *
 * 1 = the faulting rows run (needs a kernel that delivers hardware faults as
 * signals).  0 = they print "unmeasurable" and the access is NEVER performed.
 * The default is 0; see the header comment for why. */
#ifdef LP_FAULT_PROBE
#define FAULTS_MEASURABLE 1
#else
#define FAULTS_MEASURABLE 0
#endif

#if FAULTS_MEASURABLE

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

#else /* !LP_FAULT_PROBE */

/* Never called: every call site is guarded by FAULTS_MEASURABLE, which is 0
   here.  Defined so those guarded branches still compile (and so an -O0 build
   cannot leave an undefined symbol behind). */
static int access_faults(volatile unsigned char *p, int write_it) {
  (void)p;
  (void)write_it;
  return -1;
}

#endif /* LP_FAULT_PROBE */

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

/* --- 1b. MAP_ANONYMOUS ignores the descriptor ---------------------------- */
static void test_anon_fd(void) {
  /* Linux ignores fd (and offset) when MAP_ANONYMOUS is set; a caller that
     passes a real descriptor -- not musl, but plenty of ports do -- must still
     get anonymous memory, not that file's contents or EBADF. */
  unsigned char *p = mmap(0, 8192, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, 0 /* stdin, a real fd */,
                          0);
  int ok = p != MAP_FAILED && p[0] == 0 && p[4096] == 0 && p[8191] == 0;
  if (p != MAP_FAILED) {
    memset(p, 0x5C, 8192);
    ok = ok && p[8191] == 0x5C;
    munmap(p, 8192);
  }
  check(ok, "mmap anon ignores fd", ok);
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
  /* The write would have to be caught as SIGSEGV to prove enforcement; see
     FAULTS_MEASURABLE.  The read is safe either way (the region allows it). */
  int ro_enforced = FAULTS_MEASURABLE ? access_faults(p, 1) : -1;
  int ro_readable = FAULTS_MEASURABLE ? !access_faults(p, 0) : 1;

  if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) {
    row("[LP] mprotect RW FAIL errno=%d\n", errno);
    fails++;
    munmap(p, len);
    return;
  }
  int kept = 1;
  for (size_t off = 0; off < len; off += 4096)
    if (p[off] != 0x11) kept = 0;
  if (FAULTS_MEASURABLE) {
    check(ro_enforced && ro_readable && kept, "mprotect RO enforced",
          (long)((ro_enforced << 2) | (ro_readable << 1) | kept));
  } else {
    /* Explicit gap row: PROT_READ was applied (the region reports it and the
       kernel refuses the write, measured as an in=PROT fault), but the refusal
       is a process kill, not a catchable signal -- so it is not asserted. */
    row("[LP] mprotect RO enforced=unmeasurable kept=%d "
        "(no fault-signal delivery in compat; raise() path only)\n",
        kept);
    if (!kept) fails++;
  }

  /* PROT_NONE must be enforced; Linux also keeps the pages, but the x64 port
     can only express "inaccessible" as "not present", so content loss is an
     expected, documented divergence -- measured here, not asserted as a bug. */
  if (mprotect(p, len, PROT_NONE) != 0) {
    row("[LP] mprotect NONE FAIL errno=%d\n", errno);
    fails++;
  } else {
    int none_enforced = FAULTS_MEASURABLE ? access_faults(p, 0) : -1;
    if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) {
      row("[LP] mprotect NONE->RW FAIL errno=%d\n", errno);
      fails++;
    } else {
      /* preserved=0 is the DOCUMENTED x64 divergence (vm.c 4.4: a PROT_NONE
         leaf cannot exist, so mprotect drops the pages); it is reported, not
         failed, and it is not the enforcement question above. */
      int preserved = (p[0] == 0x11);
      row("[LP] mprotect NONE enforced=%s preserved=%d (x64: 0 expected)\n",
          none_enforced < 0 ? "unmeasurable" : (none_enforced ? "yes" : "no"),
          preserved);
      if (FAULTS_MEASURABLE && !none_enforced) fails++;
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

  /* A moved READ-ONLY mapping: Linux copies in kernel mode, so this succeeds
     and the bytes survive; a compat layer that copies through its own
     user-write path returns -EFAULT here. */
  unsigned char *ro = mmap(0, 64 * 1024, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ro != MAP_FAILED) {
    memset(ro, 0x3C, 64 * 1024);
    if (mprotect(ro, 64 * 1024, PROT_READ) == 0) {
      unsigned char *rm = mremap(ro, 64 * 1024, 128 * 1024, MREMAP_MAYMOVE);
      int ro_ok =
          rm != MAP_FAILED && rm[0] == 0x3C && rm[64 * 1024 - 1] == 0x3C;
      row("[LP] mremap read-only move       %s\n", ro_ok ? "ok" : "FAIL");
      if (!ro_ok) fails++;
      if (rm != MAP_FAILED) munmap(rm, 128 * 1024);
    } else {
      munmap(ro, 64 * 1024);
    }
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
  int gone = FAULTS_MEASURABLE ? access_faults(p + off, 0) : -1;
  int left_ok = (p[off - 4096] == 0x22);
  int right_ok = (p[off + n] == 0x22);
  if (FAULTS_MEASURABLE) {
    check(rc == 0 && gone && left_ok && right_ok, "munmap interior range",
          (long)((rc == 0) | (gone << 1) | (left_ok << 2) | (right_ok << 3)));
  } else {
    /* The split is measured by the surviving neighbours on both sides; that
       the middle is UNMAPPED (not merely zeroed) needs a fault to observe, so
       it gets its own explicit gap row instead of a silent pass. */
    check(rc == 0 && left_ok && right_ok, "munmap interior range",
          (long)((rc == 0) | (left_ok << 2) | (right_ok << 3)));
    row("[LP] munmap interior gone=unmeasurable (no fault-signal delivery "
        "in compat)\n");
  }
  /* Re-unmapping the same span, and unmapping a hole, must SUCCEED (Linux). */
  errno = 0;
  int again = munmap(p + off, n);
  check(again == 0, "munmap unmapped succeeds", errno);
  munmap(p, len);

  /* A range whose FIRST page is a hole but whose later pages are mapped must
     also succeed -- the shape a caller gets after freeing the head of a block
     and then releasing the rest.  Linux unmaps what is there, ignores the
     hole; a "nothing mapped at addr -> EINVAL" rule breaks this. */
  unsigned char *h = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (h != MAP_FAILED) {
    memset(h, 0x55, 3 * 4096);
    munmap(h, 4096);
    errno = 0;
    int across = munmap(h, 8192);
    int survivor = h[8192] == 0x55;
    check(across == 0 && survivor, "munmap across leading hole",
          (long)((across == 0) | (survivor << 1)));
    munmap(h + 8192, 4096);
  }
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

/* --- 8b. mapping churn: the allocator's real access pattern --------------
 *
 * musl's mallocng and libX11 both keep SEVERAL small mappings alive at once and
 * recycle them; Doom's zone does the same in bursts.  Two failure modes are
 * invisible in the single-mapping rows above: a region list that only grows
 * (64 live mappings exhaust a 16-entry array), and a region record that is not
 * removed on munmap (the 200-cycle loop below would run out of arena). */
static void test_churn(void) {
  enum { LIVE = 64 };
  static void *live[LIVE];
  int ok = 1;
  int mapped = 0;
  for (int i = 0; i < LIVE; i++) {
    live[i] = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (live[i] == MAP_FAILED) {
      ok = 0;
      break;
    }
    mapped++;
    ((unsigned char *)live[i])[0] = (unsigned char)i;
    ((unsigned char *)live[i])[4095] = (unsigned char)(i ^ 0xFF);
  }
  for (int i = 0; i < mapped; i++) {
    unsigned char *q = live[i];
    if (q[0] != (unsigned char)i || q[4095] != (unsigned char)(i ^ 0xFF)) ok = 0;
    if (munmap(q, 4096) != 0) ok = 0;
  }
  check(ok, "64 live 4KiB mappings", mapped);

  /* Recycle one address 200 times: a leaked region record shows up as an
     -ENOMEM long before the arena is actually full. */
  int cycles = 0;
  for (int i = 0; i < 200; i++) {
    unsigned char *q = mmap(0, 8192, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED) break;
    q[0] = 1;
    q[8191] = 2;
    if (munmap(q, 8192) != 0) break;
    cycles++;
  }
  check(cycles == 200, "200 mmap/munmap cycles", cycles);
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
#if FAULTS_MEASURABLE
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = segv_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER;
  if (sigaction(SIGSEGV, &sa, 0) != 0) {
    row("[LP] sigaction FAIL errno=%d\n", errno);
    return 1; /* LP_FAULT_PROBE asked for a measurement it cannot make */
  }
#endif

  row("[LP] mem probe pid=%ld pagesz=%ld\n", (long)getpid(), (long)sysconf(_SC_PAGESIZE));

  test_sizes();
  test_anon_fd();
  test_map_fixed();
  test_mprotect();
  test_mremap();
  test_munmap_partial();
  test_brk();
  test_advisory();
  test_memfd();
  test_churn();
  test_malloc();

  row("[LP] done rc=%d\n", fails);
  return fails;
}
