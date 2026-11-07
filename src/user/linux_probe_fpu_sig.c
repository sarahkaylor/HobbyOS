/*
 * src/user/linux_probe_fpu_sig.c — Linux-ABI probe: the FP/SSE state a signal
 * frame carries, and the alignment rule that decides whether the kernel can
 * save it at all.
 *
 * Why (M6/L63): the 900 s soak's VM stop was `fxsave (%rdi)` at
 * arch_fpu_save_to+4, with %rdi = 0x1083EB1DF0 — the `fpstate` area of a
 * Linux signal frame, i.e. a USER address.  QEMU answered a KVM emulation
 * failure by stopping the vCPU, so a guest-side #UD took the whole machine
 * down instead of killing one process, and no ladder could continue.
 *
 * The kernel-side fix (arch_fpu_save_user) stages the image through a kernel
 * buffer and refuses a misaligned destination with -EFAULT.  This probe is the
 * user-visible half of the proof, and it runs unchanged on the workstation and
 * in the guest:
 *
 *   P1  the ALIGNMENT RULE, measured from ring 3 where a #GP is a SIGSEGV and
 *       not a dead machine.  Four cases pin the arithmetic that makes 568
 *       correct and 576 wrong:
 *            base16 + 568  -> #GP       (FXSAVE needs 16-byte alignment)
 *            base16 + 576  -> ok
 *            base8  + 568  -> ok        <- the layout this kernel plants
 *            base8  + 576  -> #GP
 *       The handler skips the faulting instruction (4 bytes: 48 0f ae /r).
 *   P2  a real SIGUSR1 with SA_SIGINFO on the NORMAL stack: xmm0..15, MXCSR
 *       and the x87 register file round-trip BIT-IDENTICALLY through the
 *       frame, while the handler deliberately clobbers every one of them.
 *   P3  the same with SA_ONSTACK and a deliberately TIGHT altstack (4 KiB),
 *       plus the two properties userspace needs from the frame:
 *       uc_mcontext.fpregs != NULL, (fpregs & 15) == 0, and a userspace
 *       `fxrstor` FROM that pointer must load the interrupted state (so the
 *       pointer the kernel publishes is one the guest may legally use).
 *   P4  the shape that killed the VM: a fault inside a signal handler.  In a
 *       FORKED child (so the parent can report), the handler faults again;
 *       whatever the kernel does with the second delivery (nest it, or kill
 *       the child), the machine must survive and the parent must be able to
 *       say what happened.  A VM stop here shows up as the probe never
 *       printing its done marker.
 *
 * Every row is "[LP] ..."; the last is "[LP] done rc=<fails>".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static int fails;

static void row(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

/* ---- FPU primitives (x86-64; this probe is ARCH=intel only) ------------- */

#define IMG 512

static unsigned char snap_before[IMG] __attribute__((aligned(16)));
static unsigned char snap_after[IMG] __attribute__((aligned(16)));
static unsigned char pat_a[16 * 16] __attribute__((aligned(16)));
static unsigned char pat_b[16 * 16] __attribute__((aligned(16)));
static unsigned char xmm_out[16 * 16] __attribute__((aligned(16)));
static unsigned short fcw_in, fcw_out;
static unsigned int mxcsr_a, mxcsr_b;

static void fpu_snapshot(void *dst) {
  memset(dst, 0, IMG); /* reserved bytes are undefined; zero them for memcmp */
  __asm__ volatile("fxsave64 (%0)" : : "r"(dst) : "memory");
}

/* xmm0..15 from a 256-byte table, MXCSR from *mx, then four x87 values and a
   non-default x87 control word. */
static void fpu_load(const void *tab, const unsigned int *mx) {
  __asm__ volatile(
      "movdqu   0(%0), %%xmm0\n\t"
      "movdqu  16(%0), %%xmm1\n\t"
      "movdqu  32(%0), %%xmm2\n\t"
      "movdqu  48(%0), %%xmm3\n\t"
      "movdqu  64(%0), %%xmm4\n\t"
      "movdqu  80(%0), %%xmm5\n\t"
      "movdqu  96(%0), %%xmm6\n\t"
      "movdqu 112(%0), %%xmm7\n\t"
      "movdqu 128(%0), %%xmm8\n\t"
      "movdqu 144(%0), %%xmm9\n\t"
      "movdqu 160(%0), %%xmm10\n\t"
      "movdqu 176(%0), %%xmm11\n\t"
      "movdqu 192(%0), %%xmm12\n\t"
      "movdqu 208(%0), %%xmm13\n\t"
      "movdqu 224(%0), %%xmm14\n\t"
      "movdqu 240(%0), %%xmm15\n\t"
      "ldmxcsr (%1)\n\t"
      :
      : "r"(tab), "r"(mx)
      : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
        "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
        "memory");
  /* x87: four live registers + a distinctive control word (single precision,
     all exceptions masked). */
  __asm__ volatile(
      "fldpi\n\t"
      "fld1\n\t"
      "fldz\n\t"
      "fldl2e\n\t"
      "fstcw %0\n\t"
      "fldcw %1\n\t"
      : "=m"(fcw_in)
      : "m"(fcw_out)
      : "memory");
}

/* Read xmm0..3 into a buffer (used to check what an fxrstor loaded). */
static void fpu_read_xmm0123(void *dst) {
  __asm__ volatile(
      "movdqu %%xmm0,   0(%0)\n\t"
      "movdqu %%xmm1,  16(%0)\n\t"
      "movdqu %%xmm2,  32(%0)\n\t"
      "movdqu %%xmm3,  48(%0)\n\t"
      :
      : "r"(dst)
      : "memory");
}

static int img_diff(const unsigned char *a, const unsigned char *b, int *off) {
  /* [0,432) is architecturally defined by FXSAVE: x87 area, ST0..ST7, xmm0..15,
     MXCSR + MXCSR_MASK.  [432,512) is reserved/undefined. */
  for (int i = 0; i < 432; i++)
    if (a[i] != b[i]) {
      if (off) *off = i;
      return 1;
    }
  return 0;
}

static void build_patterns(void) {
  for (int i = 0; i < 16; i++)
    for (int j = 0; j < 16; j++) {
      pat_a[i * 16 + j] = (unsigned char)(0xA0 + i * 16 + j);
      pat_b[i * 16 + j] = (unsigned char)(0x50 + i * 16 + j);
    }
  mxcsr_a = 0x1F80u; /* all masked, round-nearest: the architectural default */
  mxcsr_b = 0x3FC0u; /* round-down + DAZ + FTZ: legal and visibly different  */
  fcw_out = 0x027Fu; /* single precision, all x87 exceptions masked          */
}

/* ---- P1: the FXSAVE alignment rule ------------------------------------- */

static volatile int p1_faulted;
static volatile int p1_rows;

static void p1_handler(int sig, siginfo_t *si, void *ucp) {
  (void)sig;
  (void)si;
  ucontext_t *uc = (ucontext_t *)ucp;
  p1_faulted = 1;
  /* Skip `fxsave64 (%reg)`: 48 0f ae /r = 4 bytes. */
  uc->uc_mcontext.gregs[REG_RIP] += 4;
}

static void p1_case(unsigned char *base, unsigned off, int want_fault) {
  unsigned char *dst = base + off;
  int ok;

  p1_faulted = 0;
  __asm__ volatile("fxsave64 (%0)" : : "r"(dst) : "memory");
  ok = (p1_faulted == want_fault);
  p1_rows++;
  if (!ok)
    fails++;
  row("[LP] P1 base=0x%lx b16=%lu off=%u dst=0x%lx d16=%lu want=%s got=%s %s\n",
      (unsigned long)(uintptr_t)base, (unsigned long)((uintptr_t)base & 15u),
      off, (unsigned long)(uintptr_t)dst,
      (unsigned long)((uintptr_t)dst & 15u), want_fault ? "GP" : "ok",
      p1_faulted ? "GP" : "ok", ok ? "PASS" : "FAIL");
}

static void p1_alignment_rule(void) {
  static unsigned char area[2048] __attribute__((aligned(16)));
  unsigned char *b16 = area;           /* 16-aligned frame base              */
  unsigned char *b8 = area + 1024 + 8; /* 8 (mod 16): what the kernel plants */

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = p1_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, 0) != 0) {
    row("[LP] P1 FAIL sigaction errno=%d\n", errno);
    fails++;
    return;
  }
  p1_case(b16, 568, 1);
  p1_case(b16, 576, 0);
  p1_case(b8, 568, 0);
  p1_case(b8, 576, 1);
  row("[LP] P1 cases=%d (568 is legal only for base%%16==8)\n", p1_rows);
}

/* ---- P2/P3: FPU state across a signal ---------------------------------- */

static volatile int roundtrip_calls;
static volatile int rt_fp_null, rt_fp_align, rt_fxrstor_ok;
static volatile unsigned long rt_entry_rsp;
static volatile unsigned long alt_lo, alt_hi;
static volatile int rt_on_alt;

static void rt_handler(int sig, siginfo_t *si, void *ucp) {
  (void)sig;
  (void)si;
  ucontext_t *uc = (ucontext_t *)ucp;
  unsigned char *fp;

  __asm__ volatile("movq %%rsp, %0" : "=r"(rt_entry_rsp));
  roundtrip_calls++;
  rt_on_alt = (rt_entry_rsp >= alt_lo && rt_entry_rsp < alt_hi) ? 1 : 0;

  fp = (unsigned char *)uc->uc_mcontext.fpregs;
  rt_fp_null = (fp == 0);
  rt_fp_align = (int)((uintptr_t)fp & 15u);

  /* The frame's image must be the interrupted state and must be a legal
     operand for the GUEST's own fxrstor. */
  rt_fxrstor_ok = 0;
  if (fp && rt_fp_align == 0) {
    __asm__ volatile("fxrstor64 (%0)" : : "r"(fp) : "memory");
    fpu_read_xmm0123(xmm_out);
    rt_fxrstor_ok = (memcmp(xmm_out, pat_a, 64) == 0);
  }

  /* Now clobber everything on purpose: the handler's own FP use must not leak
     back to the interrupted code (that is what the frame is for). */
  fpu_load(pat_b, &mxcsr_b);
  __asm__ volatile("fld1\n\tfldz\n\tfaddp\n\tfldpi\n\tfmulp\n\t" : : : "memory");

  if (rt_on_alt) {
    static const char m[] = "[LP] P3 in-handler on altstack\n";
    (void)!write(1, m, sizeof m - 1);
  }
}

static int rt_sigaction(int signo, int flags) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = rt_handler;
  sa.sa_flags = SA_SIGINFO | flags;
  sigemptyset(&sa.sa_mask);
  return sigaction(signo, &sa, 0);
}

/* Returns 0 on PASS.  `use_alt` installs a tight 4 KiB altstack. */
static int roundtrip_case(int use_alt, const char *tag) {
  void *stk = MAP_FAILED;
  stack_t ss;
  int rc = 0, off = -1;

  if (use_alt) {
    stk = mmap(0, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED) {
      row("[LP] %s FAIL mmap altstack errno=%d\n", tag, errno);
      return 1;
    }
    alt_lo = (unsigned long)(uintptr_t)stk;
    alt_hi = alt_lo + 4096;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = stk;
    ss.ss_size = 4096; /* deliberately tight: the frame alone is 1088 */
    ss.ss_flags = 0;
    if (sigaltstack(&ss, 0) != 0) {
      row("[LP] %s FAIL sigaltstack errno=%d (MINSIGSTKSZ=%ld)\n", tag, errno,
          (long)MINSIGSTKSZ);
      return 1;
    }
    if (rt_sigaction(SIGUSR1, SA_ONSTACK) != 0) {
      row("[LP] %s FAIL sigaction errno=%d\n", tag, errno);
      return 1;
    }
  } else {
    alt_lo = alt_hi = 0;
    if (rt_sigaction(SIGUSR1, 0) != 0) {
      row("[LP] %s FAIL sigaction errno=%d\n", tag, errno);
      return 1;
    }
  }

  roundtrip_calls = 0;
  rt_fp_null = rt_fp_align = -1;
  rt_fxrstor_ok = -1;
  rt_on_alt = 0;

  fpu_load(pat_a, &mxcsr_a);
  fpu_snapshot(snap_before);
  if (raise(SIGUSR1) != 0) {
    row("[LP] %s FAIL raise errno=%d\n", tag, errno);
    rc = 1;
  }
  fpu_snapshot(snap_after);

  if (roundtrip_calls != 1) {
    row("[LP] %s FAIL handler ran %d times\n", tag, roundtrip_calls);
    rc = 1;
  }
  if (rt_fp_null != 0) {
    row("[LP] %s FAIL uc_mcontext.fpregs is NULL\n", tag);
    rc = 1;
  }
  if (rt_fp_align != 0) {
    row("[LP] %s FAIL fpregs %% 16 = %d (FXSAVE/FXRSTOR would #GP)\n", tag,
        rt_fp_align);
    rc = 1;
  }
  if (rt_fxrstor_ok != 1) {
    row("[LP] %s FAIL guest fxrstor from fpregs did not load the interrupted "
        "xmm0..3\n",
        tag);
    rc = 1;
  }
  if (img_diff(snap_before, snap_after, &off) != 0) {
    row("[LP] %s FAIL FPU state did not round-trip: first diff at byte %d\n",
        tag, off);
    rc = 1;
  }
  row("[LP] %s roundtrip calls=%d alt=%d entry_rsp=0x%lx fp_align=%d "
      "fxrstor=%d %s\n",
      tag, roundtrip_calls, rt_on_alt, rt_entry_rsp, rt_fp_align, rt_fxrstor_ok,
      rc ? "FAIL" : "PASS");

  if (use_alt) {
    memset(&ss, 0, sizeof ss);
    ss.ss_flags = SS_DISABLE;
    (void)sigaltstack(&ss, 0);
    (void)munmap(stk, 4096);
  } else {
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    (void)sigaction(SIGUSR1, &dfl, 0);
  }
  return rc;
}

/* ---- P4: a fault inside a signal handler (the shape that killed the VM) --
 *
 * This kernel has no fork() (clone refuses anything without
 * CLONE_VM|CLONE_THREAD: "[LCOMPAT] clone refused ... no fork/COW"), so the
 * sacrificial instance is a SECOND WAVE ENTRY of the same ELF:
 *
 *     PROBE.ELF          the graded run: P1..P3 + its done marker
 *     PROBE.ELF nest     arms a SIGSEGV handler on a 4 KiB altstack, then
 *                        faults INSIDE that handler
 *
 * The nest instance is allowed to die (that is the point: a guest fault must
 * kill a process, not the machine).  It cannot print a done marker, so the
 * verdict for it is structural: the graded instance still reaches its done
 * marker and the tier still completes, i.e. the console KEEPS RUNNING after
 * the nested fault.  Under the §48.4 defect the first delivery's `fxsave`
 * stopped the vCPU, so nothing after it was ever printed.
 */

static volatile int nest_depth;
static volatile int nest_fp_align = -1;
static volatile int nest_fp_null = -1;
static volatile unsigned long nest_entry_rsp;
static volatile int nest_recovered;
static void *nest_stack; /* dedicated 16 KiB stack for the recovery point */

__attribute__((noreturn)) static void nest_recover_fn(void) {
  row("[LP] P4 child: nested delivery handled, depth=%d fp_align=%d "
      "fp_null=%d entry_rsp=0x%lx PASS\n",
      nest_depth, nest_fp_align, nest_fp_null, nest_entry_rsp);
  _exit(nest_recovered == 2 ? 0 : 93);
}

static void nest_handler(int sig, siginfo_t *si, void *ucp) {
  ucontext_t *uc = (ucontext_t *)ucp;
  (void)sig;
  (void)si;
  __asm__ volatile("movq %%rsp, %0" : "=r"(nest_entry_rsp));
  nest_depth++;
  nest_fp_null = (uc->uc_mcontext.fpregs == 0);
  nest_fp_align = (int)((uintptr_t)uc->uc_mcontext.fpregs & 15u);

  if (nest_depth == 1) {
    /* Fault again, INSIDE the handler, on the altstack: the nested delivery.
       If the kernel swallows/routes it, execution resumes here and we leave
       for the recovery point instead of retrying the faulting store. */
    *(volatile int *)8 = 1;
    nest_recovered = 1;
  } else {
    nest_recovered = 2;
  }
  uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)nest_recover_fn;
  uc->uc_mcontext.gregs[REG_RSP] =
      (greg_t)((uintptr_t)nest_stack + 16384 - 32);
}

/* argv[1] == "nest": this instance exists to fault inside its own handler. */
static int nest_instance(void) {
  void *stk = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  stack_t ss;
  struct sigaction sa;

  if (stk == MAP_FAILED) {
    row("[LP] P4 FAIL mmap altstack errno=%d\n", errno);
    return 1;
  }
  nest_stack = mmap(0, 16384, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (nest_stack == MAP_FAILED) {
    row("[LP] P4 FAIL mmap recovery stack errno=%d\n", errno);
    return 1;
  }
  memset(&ss, 0, sizeof ss);
  ss.ss_sp = stk;
  ss.ss_size = 4096;
  if (sigaltstack(&ss, 0) != 0) {
    row("[LP] P4 FAIL sigaltstack errno=%d\n", errno);
    return 1;
  }
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = nest_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, 0) != 0) {
    row("[LP] P4 FAIL sigaction errno=%d\n", errno);
    return 1;
  }
  row("[LP] P4 armed: faulting inside the SIGSEGV handler (altstack 4 KiB); "
      "this instance is expected to die, the MACHINE is not\n");
  fflush(0);
  *(volatile int *)8 = 1; /* fault 1 -> handler -> fault 2 */
  row("[LP] P4 unexpected: the faulting store returned\n");
  return 1;
}

int main(int argc, char **argv) {
  build_patterns();
  if (argc > 1 && strcmp(argv[1], "nest") == 0) return nest_instance();
  row("[LP] fpu_sig: fp image=%d bytes, MINSIGSTKSZ=%ld\n", IMG,
      (long)MINSIGSTKSZ);

  p1_alignment_rule();
  fails += roundtrip_case(0, "P2 normal-stack");
  fails += roundtrip_case(1, "P3 altstack-4k");
  row("[LP] P4: nested-fault case runs as the second wave entry "
      "(\"PROBE.ELF nest\"); this instance must still reach done rc=0\n");

  row("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
