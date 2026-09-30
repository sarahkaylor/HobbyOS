/* P2.2 (S2) + P2.3 (S3): MMTEST.BIN — the v2 (AS_V2) userland acceptance.
 *
 * Runs as a normal wave process, but loaded by loader v2
 * (load_and_run_program_v2): its image lives in fresh 4 KiB frames mapped
 * at USER_IMG_BASE (64 GiB) with no 32 MiB v1 block, and its stack is the
 * v2 main-stack demand reserve.  Everything it exercises is therefore on
 * the new address-space path end to end:
 *
 *   S2 surface:
 *   1. image fetch/execute at USER_IMG_BASE (loader-mapped leaves),
 *   2. writable .data/.bss in the image region,
 *   3. stack read/write,
 *   4. SYS_WRITE_CONSOLE / SYS_GETPROGNAME with user pointers resolved
 *      under the process's own AS (the kernel-side range check now walks
 *      the v2 regions instead of the legacy 32 MiB window),
 *   5. clean exit — the last P2.2-visible path (AS teardown + frame
 *      accounting on the way out).
 *
 *   S3 surface (docs/browser/p2-vm-design.md sections 3-5):
 *   6. demand-zero stack growth: the loader no longer pre-commits any
 *      stack page, so everything below ran on demand; step 6 reads a
 *      word 128 KiB below sp and requires it to be ZERO (fresh frame),
 *      then writes + reads it back,
 *   7. SYS_MMAP (62, 6-arg): anonymous RW mapping, first touch demand,
 *      write/read-back, then MUNMAP and re-MAP of the same span,
 *   8. SYS_MPROTECT (81): RW -> PROT_NONE (zaps the resident leaves) ->
 *      RW: the re-armed page must fault, re-demand and be zero again,
 *   9. SYS_MADVISE (82, DONTNEED): zap + re-demand-zero of a dirtied
 *      mapping,
 *  10. SYS_BRK (61) v2 heap: extend the break and touch the new heap
 *      page (demand-zero, writable).
 *
 * Prints exactly one "MMTEST PASS" (or "MMTEST FAIL n") line so the wave
 * log scan is unambiguous.
 *
 * SELF-CONTAINED ON PURPOSE: this file makes its own syscalls and links
 * with NO libc objects (bare _start, linker_v2.ld).  The classic
 * crt0/libc.a path pulls in the TLS bootstrap, whose `__tls_align`
 * absolute-symbol idiom cannot be reached from a 64 GiB image by
 * PC-relative relocations -- that TLS rework is not part of the P2
 * milestone.  Everything here is plain svc/syscall inline asm.
 */

#include <stdint.h>

#define SYS_WRITE_CONSOLE 1
#define SYS_EXIT 2
#define SYS_BRK 61
#define SYS_MMAP 62
#define SYS_MUNMAP 63
#define SYS_MPROTECT 81
#define SYS_MADVISE 82
#define SYS_GETPROGNAME 60

#define PROT_R 1
#define PROT_W 2
#define PROT_NONE 0
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MADV_DONTNEED 4

#ifdef __x86_64__
static long syscall6(long num, long a0, long a1, long a2, long a3, long a4,
                     long a5) {
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  register long r8 __asm__("r8") = a4;
  register long r9 __asm__("r9") = a5;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
}
static long syscall4(long num, long a0, long a1, long a2, long a3) {
  return syscall6(num, a0, a1, a2, a3, 0, 0);
}
#else
static long syscall6(long num, long a0, long a1, long a2, long a3, long a4,
                     long a5) {
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x5 __asm__("x5") = a5;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory");
  return x0;
}
static long syscall4(long num, long a0, long a1, long a2, long a3) {
  return syscall6(num, a0, a1, a2, a3, 0, 0);
}
#endif

static void putstr(const char *s) {
  long len = 0;
  while (s[len])
    len++;
  syscall4(SYS_WRITE_CONSOLE, (long)s, len, 0, 0);
}

static void putdigit(int d) {
  char c[2];
  c[0] = (char)('0' + (d % 10));
  c[1] = '\0';
  putstr(c);
}

/* Exit with a diagnostic; n is the failing step number. */
static void fail(int n) {
  putstr("MMTEST FAIL ");
  putdigit(n);
  putstr("\n");
  syscall4(SYS_EXIT, (long)n, 0, 0, 0);
  for (;;)
    ;
}

/* Writable globals land in .data (image region must be RW). */
static uint64_t g_data_a = 0x1122334455667788ULL;
static uint64_t g_bss_b;

static long mmap_anon(unsigned long len, long prot) {
  return syscall6(SYS_MMAP, 0, (long)len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                  0);
}

__attribute__((section(".text._start")))
void _start(void) {
  putstr("\n==============================\n"
         "HobbyOS MMTEST - v2 address space acceptance\n"
         "==============================\n");

  /* 1. Executing here at all proves the v2 image mapping.  The string
     above also proves the .rodata fetch. */
  putstr("MMTEST: image at USER_IMG_BASE, text+rodata ok\n");

  /* 2. .data / .bss writability (image region is RW). */
  g_bss_b = 0xDEADBEEFULL;
  if (g_bss_b != 0xDEADBEEFULL)
    fail(2);
  if (g_data_a != 0x1122334455667788ULL)
    fail(2);
  g_data_a = 0x8877665544332211ULL;
  if (g_data_a != 0x8877665544332211ULL)
    fail(2);

  /* 3. Stack read/write (the loader maps no stack page eagerly any more;
     these locals themselves already demanded their pages). */
  {
    char buf[256];
    for (int i = 0; i < 256; i++)
      buf[i] = (char)(i ^ 0x5A);
    for (int i = 0; i < 256; i++) {
      if (buf[i] != (char)(i ^ 0x5A))
        fail(3);
    }
  }

  /* 4. Kernel writes into a USER pointer under this process's AS:
     SYS_GETPROGNAME copies the binary name into a stack buffer. */
  {
    char name[64];
    name[0] = '\0';
    long r = syscall4(SYS_GETPROGNAME, (long)name, (long)sizeof name, 0, 0);
    if (r != 0)
      fail(4);
    if (name[0] == '\0')
      fail(4);
    putstr("MMTEST: kernel wrote into user buffer via v2 AS, name=");
    putstr(name);
    putstr("\n");
  }

  /* 6. Demand-zero stack growth: a fresh frame 128 KiB below sp must be
     zero and must become writable. */
  {
    char probe_local[8];
    uint64_t deep = ((uint64_t)&probe_local - (128 * 1024)) & ~(uint64_t)7;
    volatile uint64_t *p = (volatile uint64_t *)deep;
    if (*p != 0)
      fail(6); /* demand must deliver a zeroed page */
    *p = 0xC0FFEE123456789AULL;
    if (*p != 0xC0FFEE123456789AULL)
      fail(6);
    putstr("MMTEST: stack demand-zero growth at -128KiB ok\n");
  }

  /* 7. mmap: 64 KiB anonymous RW; demand on first touch; munmap; the same
     span maps again (vacated). */
  {
    long base = mmap_anon(64 * 1024, PROT_R | PROT_W);
    if (base <= 0)
      fail(7);
    volatile uint64_t *m = (volatile uint64_t *)base;
    if (*m != 0)
      fail(7); /* demand-zero */
    m[0] = 0xABCDEF01ULL;
    m[1024] = 0x12345678ULL; /* a second page in the mapping */
    if (m[0] != 0xABCDEF01ULL || m[1024] != 0x12345678ULL)
      fail(7);
    if (syscall4(SYS_MUNMAP, base, 64 * 1024, 0, 0) != 0)
      fail(7);
    long again = mmap_anon(64 * 1024, PROT_R | PROT_W);
    if (again <= 0)
      fail(7);
    volatile uint64_t *m2 = (volatile uint64_t *)again;
    if (*m2 != 0)
      fail(7); /* the reused span must be zero again */
    putstr("MMTEST: mmap/munmap demand-zero ok\n");
  }

  /* 8. mprotect: RW -> PROT_NONE (zaps the resident leaves; the design's
     accepted divergence) -> RW.  The re-armed page faults again and must
     come back zero. */
  {
    long base = mmap_anon(16 * 1024, PROT_R | PROT_W);
    if (base <= 0)
      fail(8);
    volatile uint64_t *m = (volatile uint64_t *)base;
    m[0] = 0xFEEDFACEULL;
    if (syscall4(SYS_MPROTECT, base, 16 * 1024, PROT_NONE, 0) != 0)
      fail(8);
    if (syscall4(SYS_MPROTECT, base, 16 * 1024, PROT_R | PROT_W, 0) != 0)
      fail(8);
    if (*m != 0)
      fail(8); /* zap + re-demand = zero again */
    m[0] = 0x5A5A5A5AULL;
    if (*m != 0x5A5A5A5AULL)
      fail(8);
    putstr("MMTEST: mprotect PROT_NONE zap + re-demand ok\n");
  }

  /* 9. madvise(DONTNEED): dirtied mapping comes back zeroed after the
     zap; DONTNEED keeps the mapping (unlike munmap). */
  {
    long base = mmap_anon(16 * 1024, PROT_R | PROT_W);
    if (base <= 0)
      fail(9);
    volatile uint64_t *m = (volatile uint64_t *)base;
    m[0] = 0x1111222233334444ULL;
    m[1024] = 0x5555666677778888ULL;
    if (syscall4(SYS_MADVISE, base, 16 * 1024, MADV_DONTNEED, 0) != 0)
      fail(9);
    if (*m != 0 || m[1024] != 0)
      fail(9); /* re-demand delivers fresh zero pages */
    m[0] = 0x9999AAAAULL;
    if (*m != 0x9999AAAAULL)
      fail(9);
    putstr("MMTEST: madvise(DONTNEED) zap + re-demand-zero ok\n");
  }

  /* 10. brk v2: extend the break by 64 KiB and touch the new heap page. */
  {
    long cur = syscall4(SYS_BRK, 0, 0, 0, 0);
    if (cur <= 0)
      fail(10);
    long want = cur + 64 * 1024;
    if (syscall4(SYS_BRK, want, 0, 0, 0) != 0)
      fail(10);
    volatile uint64_t *h = (volatile uint64_t *)cur;
    if (*h != 0)
      fail(10); /* demand-zero heap page */
    *h = 0xBEEFCAFEULL;
    if (*h != 0xBEEFCAFEULL)
      fail(10);
    if (syscall4(SYS_BRK, 0, 0, 0, 0) != want)
      fail(10);
    putstr("MMTEST: brk heap demand-zero ok\n");
  }

  /* 5/11. Final: the PASS token the wave scans for. */
  {
    const char *tail = "MMTEST PASS\n";
    char msg[32];
    int i = 0;
    while (tail[i] && i < 31) {
      msg[i] = tail[i];
      i++;
    }
    msg[i] = '\0';
    putstr(msg);
  }

  syscall4(SYS_EXIT, 0, 0, 0, 0);
  for (;;)
    ;
}
