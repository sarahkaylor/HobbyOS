/* src/user/linux_probe_trapsig.c - Linux-ABI probe: which signal a CPU
 * exception raises (lane C2-wine2; Wine's SEH depends on it).
 *
 * Linux (arch/x86/kernel/traps.c): #DE -> SIGFPE/FPE_INTDIV, #UD -> SIGILL,
 * #BP (int3) -> SIGTRAP, #PF -> SIGSEGV; the ucontext carries the vector in
 * REG_TRAPNO.  Wine's fpe_handler turns SIGFPE+trapno 0 into
 * EXCEPTION_INT_DIVIDE_BY_ZERO; a SIGSEGV instead is "Got unexpected trap 0".
 * Rows "[LP] ..."; last row "[LP] done rc=<failures>".
 */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static int fails;
static sigjmp_buf jb;
static volatile int got_sig, got_code;
static volatile long got_trapno;

static void out(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}
static void ck(int ok, const char *what, long v) {
  out("[LP] %s %s (%ld)\n", ok ? "PASS" : "FAIL", what, v);
  if (!ok) fails++;
}
static void h(int sig, siginfo_t *si, void *ucv) {
  ucontext_t *uc = ucv;
  got_sig = sig;
  got_code = si->si_code;
  got_trapno = (long)uc->uc_mcontext.gregs[REG_TRAPNO];
  siglongjmp(jb, 1);
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = h;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  int sigs[] = {SIGFPE, SIGILL, SIGTRAP, SIGSEGV, SIGBUS};
  for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, 0);

  got_sig = 0;
  if (!sigsetjmp(jb, 1)) {
    __asm__ volatile("xorl %%edx,%%edx\n\tmovl $7,%%eax\n\txorl %%ecx,%%ecx\n\tdivl %%ecx" ::: "eax", "ecx", "edx");
  }
  ck(got_sig == SIGFPE, "#DE (div by 0) -> SIGFPE", got_sig);
  ck(got_code == FPE_INTDIV, "#DE si_code FPE_INTDIV", got_code);
  ck(got_trapno == 0, "#DE REG_TRAPNO 0", got_trapno);

  got_sig = 0;
  if (!sigsetjmp(jb, 1)) __asm__ volatile("ud2");
  ck(got_sig == SIGILL, "#UD (ud2) -> SIGILL", got_sig);
  ck(got_trapno == 6, "#UD REG_TRAPNO 6", got_trapno);

  got_sig = 0;
  if (!sigsetjmp(jb, 1)) __asm__ volatile("int3");
  ck(got_sig == SIGTRAP, "#BP (int3) -> SIGTRAP", got_sig);
  ck(got_trapno == 3, "#BP REG_TRAPNO 3", got_trapno);

  got_sig = 0;
  if (!sigsetjmp(jb, 1)) { volatile int *z = 0; (void)*z; }
  ck(got_sig == SIGSEGV, "#PF (NULL read) -> SIGSEGV", got_sig);
  ck(got_trapno == 14, "#PF REG_TRAPNO 14", got_trapno);

  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
