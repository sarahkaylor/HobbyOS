/*
 * src/user/linux_probe_siginfo.c - C1-k4: the siginfo a CPU fault delivers
 * (si_signo, si_code, si_addr), the detail Wine's exception translation and
 * runtimes with their own fault handlers read (docs/cp-k4.md §5).
 *
 * ORDINARY musl Linux program; the same static-PIE ELF runs on the reference
 * kernel (strix, Linux) and in the HobbyOS guest.  Every row is ONE write(2);
 * the last row is "[LP] done rc=<fails>".  Reference values: Linux
 * arch/x86/kernel/traps.c + fpu/core.c fpu__exception_code().
 *
 *  I1 #UD (ud2)                    SIGILL  ILL_ILLOPN   si_addr = the ud2
 *  I2 #DE (div by 0)               SIGFPE  FPE_INTDIV   si_addr = the div
 *  I3 #MF x87 1/0, ZM unmasked     SIGFPE  FPE_FLTDIV
 *  I4 #MF x87 0/0, IM unmasked     SIGFPE  FPE_FLTINV
 *  I5 #XM divss 1/0, ZM unmasked   SIGFPE  FPE_FLTDIV
 *  I6 #XM sqrtss(-1), IM unmasked  SIGFPE  FPE_FLTINV
 *  I7 #XM overflow, OM unmasked    SIGFPE  FPE_FLTOVF
 *  P1 read of an unmapped page     SIGSEGV SEGV_MAPERR  si_addr = the address
 *  P2 write to a PROT_READ page    SIGSEGV SEGV_ACCERR  si_addr = the address
 *  P3 read of a PROT_NONE page     SIGSEGV SEGV_ACCERR
 *  B1 memfd mapping past EOF       SIGBUS  BUS_ADRERR   si_addr = the address
 *  B2 /tmp file mapping past EOF   SIGBUS  BUS_ADRERR
 *  B3 the page holding EOF reads   no signal (zero tail), as Linux
 *  G1 #GP (hlt at CPL3)            SIGSEGV SI_KERNEL    si_addr = 0
 *  T1 #BP (int3)                   SIGTRAP SI_KERNEL
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif

static int fails;
static sigjmp_buf jb;
static volatile int got_sig, got_code;
static volatile uintptr_t got_addr, got_ip;

static void out(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}
static void h(int sig, siginfo_t *si, void *ucv) {
  ucontext_t *uc = ucv;
  got_sig = sig;
  got_code = si->si_code;
  got_addr = (uintptr_t)si->si_addr;
  got_ip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
  siglongjmp(jb, 1);
}
static void row(const char *name, int sig, int code, int want_addr, uintptr_t addr) {
  int ok = got_sig == sig && got_code == code && (!want_addr || got_addr == addr);
  out("[LP] %s %s sig=%d code=%d addr=%#lx (want sig=%d code=%d%s%#lx)\n", ok ? "PASS" : "FAIL",
      name, got_sig, got_code, (unsigned long)got_addr, sig, code, want_addr ? " addr=" : " any-addr ",
      (unsigned long)addr);
  if (!ok) fails++;
}
static void reset_fpu(void) {
  __asm__ volatile("fninit");
  unsigned m = 0x1f80;
  __asm__ volatile("ldmxcsr %0" ::"m"(m));
}

static void fpu_mf(int invalid) {
  unsigned short cw = invalid ? (0x37f & ~0x01) : (0x37f & ~0x04); /* unmask IM / ZM */
  __asm__ volatile("fninit\n\tfldcw %0" ::"m"(cw));
  if (invalid)
    __asm__ volatile("fldz\n\tfldz\n\tfdivrp\n\tfwait" ::: "st", "st(1)");
  else
    __asm__ volatile("fld1\n\tfldz\n\tfdivrp\n\tfwait" ::: "st", "st(1)");
}
static void sse_xm(int kind) {
  unsigned m = 0x1f80 & ~(kind == 0 ? 0x200u : kind == 1 ? 0x080u : 0x400u); /* ZM / IM / OM */
  __asm__ volatile("ldmxcsr %0" ::"m"(m));
  volatile float one = 1.0f, zero = 0.0f, neg = -1.0f, big = 3e38f;
  float r;
  if (kind == 0) __asm__ volatile("movss %1,%0\n\tdivss %2,%0" : "=&x"(r) : "m"(one), "m"(zero));
  else if (kind == 1) __asm__ volatile("sqrtss %1,%0" : "=x"(r) : "m"(neg));
  else __asm__ volatile("movss %1,%0\n\tmulss %1,%0" : "=&x"(r) : "m"(big));
  (void)r;
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = h;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  int sigs[] = {SIGFPE, SIGILL, SIGTRAP, SIGSEGV, SIGBUS};
  for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, 0);

  /* I1 */
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) __asm__ volatile("ud2");
  row("I1 #UD", SIGILL, ILL_ILLOPN, 1, got_ip);
  /* I2 */
  got_sig = 0;
  if (!sigsetjmp(jb, 1))
    __asm__ volatile("xorl %%edx,%%edx\n\tmovl $7,%%eax\n\txorl %%ecx,%%ecx\n\tdivl %%ecx" ::: "eax", "ecx", "edx");
  row("I2 #DE", SIGFPE, FPE_INTDIV, 1, got_ip);
  /* I3/I4 */
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) fpu_mf(0);
  reset_fpu();
  row("I3 #MF x87 1/0", SIGFPE, FPE_FLTDIV, 0, 0);
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) fpu_mf(1);
  reset_fpu();
  row("I4 #MF x87 0/0", SIGFPE, FPE_FLTINV, 0, 0);
  /* I5..I7 */
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) sse_xm(0);
  reset_fpu();
  row("I5 #XM divss 1/0", SIGFPE, FPE_FLTDIV, 1, got_ip);
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) sse_xm(1);
  reset_fpu();
  row("I6 #XM sqrtss -1", SIGFPE, FPE_FLTINV, 1, got_ip);
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) sse_xm(2);
  reset_fpu();
  row("I7 #XM overflow", SIGFPE, FPE_FLTOVF, 1, got_ip);

  /* P1..P3 */
  char *m = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  munmap(m + 4096, 4096);
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) (void)*(volatile char *)(m + 4096 + 8);
  row("P1 unmapped read", SIGSEGV, SEGV_MAPERR, 1, (uintptr_t)(m + 4096 + 8));
  mprotect(m, 4096, PROT_READ);
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) *(volatile char *)(m + 16) = 1;
  row("P2 write to PROT_READ", SIGSEGV, SEGV_ACCERR, 1, (uintptr_t)(m + 16));
  mprotect(m + 8192, 4096, PROT_NONE);
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) (void)*(volatile char *)(m + 8192 + 24);
  row("P3 read of PROT_NONE", SIGSEGV, SEGV_ACCERR, 1, (uintptr_t)(m + 8192 + 24));

  /* B1: memfd of 100 bytes, two pages mapped */
  int mfd = (int)syscall(SYS_memfd_create, "k4bus", 0);
  if (mfd >= 0 && ftruncate(mfd, 100) == 0) {
    char *f = mmap(0, 8192, PROT_READ, MAP_SHARED, mfd, 0);
    if (f != MAP_FAILED) {
      got_sig = 0;
      if (!sigsetjmp(jb, 1)) (void)*(volatile char *)(f + 4096 + 4);
      row("B1 memfd past EOF", SIGBUS, BUS_ADRERR, 1, (uintptr_t)(f + 4096 + 4));
      got_sig = 0;
      volatile char tail = 1;
      if (!sigsetjmp(jb, 1)) tail = *(volatile char *)(f + 200);
      int ok = got_sig == 0 && tail == 0;
      out("[LP] %s B3 EOF page tail reads 0 (sig=%d byte=%d)\n", ok ? "PASS" : "FAIL", got_sig, tail);
      if (!ok) fails++;
    } else {
      out("[LP] FAIL B1 mmap memfd\n");
      fails++;
    }
  } else {
    out("[LP] FAIL B1 memfd_create/ftruncate\n");
    fails++;
  }
  /* B2: a /tmp file */
  int tfd = open("/tmp/k4bus.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (tfd >= 0 && write(tfd, "0123456789", 10) == 10) {
    char *f = mmap(0, 8192, PROT_READ, MAP_PRIVATE, tfd, 0);
    if (f != MAP_FAILED) {
      got_sig = 0;
      if (!sigsetjmp(jb, 1)) (void)*(volatile char *)(f + 4096 + 12);
      row("B2 /tmp file past EOF", SIGBUS, BUS_ADRERR, 1, (uintptr_t)(f + 4096 + 12));
    } else {
      out("[LP] FAIL B2 mmap /tmp file\n");
      fails++;
    }
    close(tfd);
    unlink("/tmp/k4bus.bin");
  } else {
    out("[LP] FAIL B2 /tmp file\n");
    fails++;
  }

  /* G1 / T1 */
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) __asm__ volatile("hlt");
  row("G1 #GP (hlt)", SIGSEGV, SI_KERNEL, 1, 0);
  got_sig = 0;
  if (!sigsetjmp(jb, 1)) __asm__ volatile("int3");
  row("T1 #BP (int3)", SIGTRAP, SI_KERNEL, 0, 0);

  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
