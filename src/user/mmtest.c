/* P2.2 (S2): MMTEST.BIN — the v2 (AS_V2) userland acceptance.
 *
 * Runs as a normal wave process, but loaded by loader v2
 * (load_and_run_program_v2): its image lives in fresh 4 KiB frames mapped
 * at USER_IMG_BASE (64 GiB) with no 32 MiB v1 block, and its stack is the
 * committed top of the v2 main-stack reserve.  Everything it exercises is
 * therefore on the new address-space path end to end:
 *
 *   1. image fetch/execute at USER_IMG_BASE (loader-mapped leaves),
 *   2. writable .data/.bss in the image region,
 *   3. stack read/write in the committed main-stack pages,
 *   4. SYS_WRITE_CONSOLE / SYS_GETPROGNAME with user pointers resolved
 *      under the process's own AS (the kernel-side range check now walks
 *      the v2 regions instead of the legacy 32 MiB window),
 *   5. clean exit — the last P2.2-visible path (AS teardown + block-layer
 *      accounting on the way out).
 *
 * Prints exactly one "MMTEST PASS" (or "MMTEST FAIL n") line so the wave
 * log scan is unambiguous.
 *
 * SELF-CONTAINED ON PURPOSE: this file makes its own syscalls and links
 * with NO libc objects (bare _start, linker_v2.ld).  The classic
 * crt0/libc.a path pulls in the TLS bootstrap, whose `__tls_align`
 * absolute-symbol idiom cannot be reached from a 64 GiB image by
 * PC-relative relocations -- and the S2 milestone deliberately does not
 * include the TLS-rework that would fix that (S3+).  Everything here is
 * plain svc/syscall inline asm.
 */

#include <stdint.h>

#define SYS_WRITE_CONSOLE 1
#define SYS_EXIT 2
#define SYS_GETPROGNAME 60

#ifdef __x86_64__
static long syscall4(long num, long a0, long a1, long a2, long a3) {
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
}
#else
static long syscall4(long num, long a0, long a1, long a2, long a3) {
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                   : "memory");
  return x0;
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

  /* 3. Stack read/write in the committed top of the reserve. */
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

  /* 5. SYS_WRITE_CONSOLE with a string built on the stack (the earlier
     prints already covered rodata). */
  {
    char msg[32];
    const char *tail = "MMTEST PASS\n";
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
