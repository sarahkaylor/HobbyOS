/*
 * src/user/linux_probe_tramp_sig.c — Linux-ABI probe: the STACK ALIGNMENT a
 * signal frame, an SA_RESTART syscall restart and an altstack frame hand back
 * to user code.
 *
 * Why: Mono's JIT keeps %rsp 16-byte aligned inside a compiled method body and
 * enters every callee through a `call` (entry %rsp % 16 == 8).  Its generic
 * amd64 trampoline asserts exactly that:
 *
 *     mov %rsp,%r11 ; and $15,%r11 ; cmp $0,%r11 ; je ok
 *     ... call stack_unaligned(tramp_type) ...      (tramp-amd64.c:641)
 *
 * so a trampoline reached with %rsp % 16 == 8 instead of 0 aborts the runtime.
 * A signal-save/restore or a syscall restart that moves %rsp by 8 bytes
 * therefore looks like a Mono trampoline bug.  This probe measures, on the
 * reference kernel (workstation) and in the HobbyOS guest, the four paths the
 * compat layer owns:
 *
 *   T1  handler entry alignment; a function CALLED from the handler; and the
 *       alignment a `call` gets IMMEDIATELY after the handler returns.
 *   T2  the same across a FUTEX_WAIT that is interrupted and RESTARTED
 *       (SA_RESTART): the exact %rsp before and after the syscall wrapper must
 *       be identical, and the first `call` after it must be entered at 8.
 *   T3  the same with sigaltstack + SA_ONSTACK (frame planted on the alt
 *       stack): handler entry alignment and the alignment back on the normal
 *       stack afterwards.
 *   T4  two signals queued so the second is delivered at the first handler's
 *       rt_sigreturn boundary (that is where the kernel re-enters user mode
 *       with a NEW frame): both handler entries must be at 8.
 *   T5  a SIGSEGV handler that emulates Mono's null check (rewrites
 *       uc_mcontext.gregs[REG_RIP] and returns): the resumed code must see the
 *       same alignment it faulted with.
 *
 * Every row is "[LP] Tn ..."; the last is "[LP] done rc=<fails>".  Ordinary
 * musl Linux program: the same static-PIE ELF runs on the host and in the
 * guest (tools/gate/t3_linux_probe.sh tramp_sig), so the two logs can be
 * diffed row by row.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef SYS_futex
#define SYS_futex 202
#endif
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static int fails;

static void row(const char *fmt, ...) {
  char b[400];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

/* ---- measurement primitives -------------------------------------------- */

static volatile unsigned long g_sig_entry_rsp, g_sig_entry_mod;
static volatile unsigned long g_call_entry_mod, g_call_entry_rsp;
static volatile unsigned long g_last_sig;
static volatile unsigned long g_alt_lo, g_alt_hi;

/* The real signal handler: a naked entry that records %rsp BEFORE any
   prologue (this is exactly the value the kernel planted), then jumps to the
   C body with the frame untouched (same as a `call` would have entered it). */
extern void sig_entry(void);
extern void sig_body(int, siginfo_t *, void *);

__asm__(
    ".text\n"
    ".global sig_entry\n"
    ".type sig_entry,@function\n"
    "sig_entry:\n"
    "  movq %rsp, g_sig_entry_rsp(%rip)\n"
    "  movq %rsp, %rax\n"
    "  andq $15, %rax\n"
    "  movq %rax, g_sig_entry_mod(%rip)\n"
    "  jmp sig_body\n"
    ".size sig_entry, .-sig_entry\n"
    /* A call target that only records its ENTRY %rsp (no prologue).  A
       function entered by `call` from a compiled body must see 8 here. */
    ".global call_entry\n"
    ".type call_entry,@function\n"
    "call_entry:\n"
    "  movq %rsp, g_call_entry_rsp(%rip)\n"
    "  movq %rsp, %rax\n"
    "  andq $15, %rax\n"
    "  movq %rax, g_call_entry_mod(%rip)\n"
    "  ret\n"
    ".size call_entry, .-call_entry\n"
    ".att_syntax prefix\n");
extern void call_entry(void);

/* %rsp of the CALLER at the call site (entry rsp + 8 = the value the caller
   had before the `call`).  Two of these on either side of a syscall must be
   bit-identical: nothing between them may change the frame pointer. */
extern unsigned long naked_rsp(void);
__asm__(
    ".text\n"
    ".global naked_rsp\n"
    ".type naked_rsp,@function\n"
    "naked_rsp:\n"
    "  leaq 8(%rsp), %rax\n"
    "  ret\n"
    ".size naked_rsp, .-naked_rsp\n"
    ".att_syntax prefix\n");

/* A futex wait issued with our own `syscall` instruction, so the compiler
   cannot insert stack realignment (it does: `sub $8,%rsp; push $0` around a
   libc call) between the two rsp measurements.  Returns the raw rax. */
__attribute__((noinline)) static long raw_futex_wait(volatile int *uaddr) {
  register long rax __asm__("rax") = SYS_futex;
  register long rdi __asm__("rdi") = (long)(uintptr_t)uaddr;
  register long rsi __asm__("rsi") = FUTEX_WAIT;
  register long rdx __asm__("rdx") = 0;
  register long r10 __asm__("r10") = 0;
  __asm__ volatile("syscall"
                   : "+r"(rax)
                   : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
  return rax;
}

/* The alignment-sensitive sequence: GCC guarantees `buf` is 16-byte aligned
   only if the function was entered with %rsp % 16 == 8 (the psABI function
   entry invariant).  If it was not, the address is 8 mod 16 and movdqa faults
   exactly as the kernel's `movaps %xmm,0x30(%rsp)` spill does.  We measure
   first, and only execute the aligned move when the compiler's assumption
   actually holds, so a misaligned guest reports a row instead of dying. */
__attribute__((noinline)) static unsigned long sensitive16(int *aligned) {
  char buf[64] __attribute__((aligned(16)));
  unsigned long mod;
  buf[0] = 1;
  buf[32] = 2;
  __asm__ volatile("movq %%rsp, %0\n\tandq $15, %0" : "=&r"(mod));
  int ok = (((uintptr_t)buf & 15u) == 0);
  *aligned = ok;
  if (ok) {
    __asm__ volatile("movdqa (%0), %%xmm0\n\t"
                     "movdqa %%xmm0, 16(%0)\n\t"
                     "pxor %%xmm0, %%xmm0"
                     :
                     : "r"(buf)
                     : "xmm0", "memory");
  }
  return mod;
}

/* ---- handler bodies ---------------------------------------------------- */

struct hit {
  unsigned long mod, rsp, uc_mod, on_alt;
  int calls;
};
static volatile struct hit g_hit[3]; /* [0]=USR1, [1]=USR2, [2]=SEGV */
static volatile int g_usr1_calls, g_usr2_calls;
static volatile uintptr_t g_resume_pc;
static volatile int g_segv_calls;

void sig_body(int sig, siginfo_t *si, void *ucp) {
  (void)si;
  unsigned long rsp = g_sig_entry_rsp;
  int idx = (sig == SIGUSR2) ? 1 : (sig == SIGSEGV) ? 2 : 0;
  g_hit[idx].mod = g_sig_entry_mod;
  g_hit[idx].rsp = rsp;
  g_hit[idx].uc_mod = ((unsigned long)ucp) & 15;
  g_hit[idx].on_alt =
      (g_alt_lo && rsp >= g_alt_lo && rsp < g_alt_hi) ? 1 : 0;
  g_hit[idx].calls = (idx == 0) ? ++g_usr1_calls
                    : (idx == 1) ? ++g_usr2_calls
                                 : ++g_segv_calls;
  g_last_sig = (unsigned long)sig;
  /* (a) call a function FROM the handler: its entry must be at 8 too. */
  call_entry();
  if (idx == 0) {
    int a = 0;
    unsigned long m = sensitive16(&a); /* a handler may use SSE too */
    (void)m;
  }
  if (idx == 2) {
    /* Mono's null check: skip the faulting instruction and return. */
    ucontext_t *uc = (ucontext_t *)ucp;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)g_resume_pc;
  }
}

static int install(int sig, int extra_flags) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = (void (*)(int, siginfo_t *, void *))sig_entry;
  sa.sa_flags = SA_SIGINFO | SA_RESTART | extra_flags;
  sigemptyset(&sa.sa_mask);
  return sigaction(sig, &sa, NULL);
}

/* ---- T1: handler -> call -> return -> call ----------------------------- */

static void case_t1(void) {
  memset((void *)g_hit, 0, sizeof g_hit);
  g_usr1_calls = 0;
  if (install(SIGUSR1, 0) != 0) {
    row("[LP] T1 FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  raise(SIGUSR1); /* handler runs and returns before the next statement */
  int aligned = -1;
  unsigned long body_mod = sensitive16(&aligned);
  call_entry();
  unsigned long after_mod = g_call_entry_mod;
  unsigned long before_rsp = naked_rsp();
  call_entry();
  unsigned long after_rsp = g_call_entry_rsp;

  int ok = g_hit[0].mod == 8 && g_hit[0].uc_mod == 0 && g_call_entry_mod == 8 &&
           body_mod == 0 && aligned == 1 && after_mod == 8 &&
           after_rsp == before_rsp - 8;
  row("[LP] T1 handler_rsp16=%lu uc16=%lu call_from_handler=%lu "
      "body_rsp16=%lu buf_aligned=%d call_after_handler=%lu "
      "callsite_delta=%ld ok=%d\n",
      g_hit[0].mod, g_hit[0].uc_mod, g_call_entry_mod, body_mod, aligned,
      after_mod, (long)(after_rsp - (before_rsp - 8)), ok);
  if (!ok) fails++;
}

/* ---- T2: futex interrupted + SA_RESTART restart ------------------------ */

struct fut {
  volatile int word;
  volatile int done, ret, err;
  volatile uint64_t before, after, after_call_mod, call_mod;
  volatile unsigned long hmod;
};
static struct fut g_fut;

static void *futex_thread(void *arg) {
  struct fut *f = (struct fut *)arg;
  int v;
  f->before = naked_rsp();
  v = (int)raw_futex_wait(&f->word);
  f->err = (v < 0) ? (int)-v : 0;
  f->ret = (v < 0) ? -1 : v;
  f->after = naked_rsp();
  int aligned = -1;
  (void)sensitive16(&aligned);
  call_entry();
  f->after_call_mod = g_call_entry_mod;
  f->done = 1;
  return NULL;
}

static void case_t2(void) {
  pthread_t th;
  memset(&g_fut, 0, sizeof g_fut);
  g_usr1_calls = 0;
  if (install(SIGUSR1, 0) != 0) {
    row("[LP] T2 FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  if (pthread_create(&th, NULL, futex_thread, &g_fut) != 0) {
    row("[LP] T2 FAIL pthread_create\n");
    fails++;
    return;
  }
  struct timespec ts = {0, 100 * 1000 * 1000};
  nanosleep(&ts, NULL); /* let the thread park */
  pthread_kill(th, SIGUSR1);
  nanosleep(&ts, NULL);
  int calls_after_signal = g_usr1_calls;
  int still_parked = !g_fut.done; /* SA_RESTART: the wait must NOT return */
  g_fut.word = 1;
  (void)syscall(SYS_futex, (int *)&g_fut.word, FUTEX_WAKE, 1, NULL, NULL, 0);
  int waited = 0;
  for (int i = 0; i < 2000 && !g_fut.done; i++) {
    nanosleep(&ts, NULL);
    waited++;
  }
  pthread_join(th, NULL);
  long delta = (long)(g_fut.after - g_fut.before);
  int ok = still_parked && calls_after_signal >= 1 && g_fut.done &&
           g_fut.ret == 0 && delta == 0 && g_fut.after_call_mod == 8;
  row("[LP] T2 still_parked=%d calls=%d ret=%d errno=%d rsp_delta=%ld "
      "call_after_restart=%lu ok=%d\n",
      still_parked, calls_after_signal, g_fut.ret, g_fut.err, delta,
      g_fut.after_call_mod, ok);
  if (!ok) fails++;
}

/* ---- T3: sigaltstack + SA_ONSTACK ------------------------------------- */

static void case_t3(void) {
  size_t sz = 64 * 1024;
  void *stk = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stk == MAP_FAILED) {
    row("[LP] T3 FAIL mmap errno=%d\n", errno);
    fails++;
    return;
  }
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_sp = stk;
  ss.ss_size = sz;
  if (sigaltstack(&ss, NULL) != 0) {
    row("[LP] T3 FAIL sigaltstack errno=%d\n", errno);
    fails++;
    return;
  }
  g_alt_lo = (unsigned long)stk;
  g_alt_hi = (unsigned long)stk + sz;
  memset((void *)&g_hit[0], 0, sizeof g_hit[0]);
  g_usr1_calls = 0;
  if (install(SIGUSR1, SA_ONSTACK) != 0) {
    row("[LP] T3 FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  raise(SIGUSR1);
  int aligned = -1;
  unsigned long body_mod = sensitive16(&aligned);
  call_entry();
  unsigned long after_mod = g_call_entry_mod;
  int ok = g_hit[0].mod == 8 && g_hit[0].on_alt == 1 && g_hit[0].uc_mod == 0 &&
           body_mod == 0 && aligned == 1 && after_mod == 8;
  row("[LP] T3 alt_handler_rsp16=%lu on_altstack=%lu uc16=%lu body_rsp16=%lu "
      "call_after=%lu ok=%d\n",
      g_hit[0].mod, g_hit[0].on_alt, g_hit[0].uc_mod, body_mod, after_mod, ok);
  if (!ok) fails++;
  /* back to the normal stack, SA_ONSTACK off, so later cases are unaffected */
  ss.ss_flags = SS_DISABLE;
  ss.ss_sp = NULL;
  ss.ss_size = 0;
  (void)sigaltstack(&ss, NULL);
  g_alt_lo = g_alt_hi = 0;
}

/* ---- T4: second signal delivered at the first handler's sigreturn ------ */

static void case_t4(void) {
  memset((void *)&g_hit[0], 0, sizeof g_hit[0] * 2);
  g_usr1_calls = g_usr2_calls = 0;
  if (install(SIGUSR1, 0) != 0 || install(SIGUSR2, 0) != 0) {
    row("[LP] T4 FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  sigset_t all, old;
  sigemptyset(&all);
  sigaddset(&all, SIGUSR1);
  sigaddset(&all, SIGUSR2);
  pthread_sigmask(SIG_BLOCK, &all, &old);
  raise(SIGUSR1);
  raise(SIGUSR2); /* both pending; unblocking delivers one, then the other */
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  int aligned = -1;
  unsigned long body_mod = sensitive16(&aligned);
  call_entry();
  unsigned long after_mod = g_call_entry_mod;
  int ok = g_usr1_calls == 1 && g_usr2_calls == 1 &&
           g_hit[0].mod == 8 && g_hit[1].mod == 8 && body_mod == 0 &&
           aligned == 1 && after_mod == 8;
  row("[LP] T4 usr1_entry=%lu usr2_entry=%lu calls=%d/%d body_rsp16=%lu "
      "call_after=%lu ok=%d\n",
      g_hit[0].mod, g_hit[1].mod, g_usr1_calls, g_usr2_calls, body_mod,
      after_mod, ok);
  if (!ok) fails++;
}

/* ---- T5: SIGSEGV handler rewrites RIP and returns (Mono null check) ---- */

static void case_t5(void) {
  memset((void *)&g_hit[2], 0, sizeof g_hit[2]);
  g_segv_calls = 0;
  if (install(SIGSEGV, 0) != 0) {
    row("[LP] T5 FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  uint64_t before = naked_rsp();
  int aligned = -1;
  unsigned long body_mod = 0;
  g_resume_pc = 0;
  /* The faulting load; the handler skips it and returns here. */
  __asm__ volatile(
      "leaq 1f(%%rip), %%rax\n\t"
      "movq %%rax, g_resume_pc(%%rip)\n\t"
      "movq $0, %%rax\n\t"
      "movq (%%rax), %%rax\n\t" /* NULL read -> SIGSEGV */
      "1:\n\t"
      ::: "rax", "memory");
  body_mod = sensitive16(&aligned);
  call_entry();
  unsigned long after_mod = g_call_entry_mod;
  uint64_t after = naked_rsp();
  int ok = g_segv_calls == 1 && g_hit[2].mod == 8 && body_mod == 0 &&
           aligned == 1 && after_mod == 8 && after == before;
  row("[LP] T5 segv_entry=%lu body_rsp16=%lu call_after=%lu rsp_delta=%ld "
      "ok=%d\n",
      g_hit[2].mod, body_mod, after_mod, (long)(after - before), ok);
  if (!ok) fails++;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  case_t1();
  case_t2();
  case_t3();
  case_t4();
  case_t5();
  row("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
