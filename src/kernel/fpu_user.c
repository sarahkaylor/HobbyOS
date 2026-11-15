/* src/kernel/fpu_user.c — signal-frame FPU images, staged through kernel RAM.
 *
 * M6/L63.  Why this file exists
 * ----------------------------
 * A Linux signal frame carries a 512-byte FXSAVE64 image (528 bytes of FPSIMD
 * on ARM64) that the kernel fills at delivery and reloads at sigreturn.  The
 * frame lives in USER memory, and until this lane both plants handed the user
 * address straight to the arch primitive:
 *
 *     arch_fpu_save_to((void *)l_sf_addr(base, L_SF_FP));   <- user VA
 *
 * That is unsafe twice over:
 *
 *  1. FXSAVE/FXRSTOR take an implicit 512-byte memory operand and #GP(0) when
 *     it is not 16-byte aligned.  A signal frame base is 8 (mod 16) by ABI
 *     (the handler must enter with %rsp % 16 == 8), so the fpstate offset has
 *     to be 8 (mod 16) as well -- 568, not 576.  A bare constant cannot carry
 *     that invariant: any change to the frame arithmetic silently turns the
 *     FXSAVE into a kernel-mode #GP.  (This lane measured both halves in the
 *     guest: see src/user/linux_probe_fpu_sig.c P1.)
 *
 *  2. Worse, a #GP (or any translation failure) on that instruction is not a
 *     recoverable fault.  The CPU raises it in RING 0 against a user address,
 *     KVM cannot emulate the instruction, and QEMU stops the whole vCPU:
 *     "KVM internal error. Suberror: 1 ... emulation failure".  A single
 *     guest-side #UD then takes the machine down instead of killing one
 *     process -- exactly the M6/L62 §48.4 death (`fxsave (%rdi)` with
 *     RDI=0x1083EB1DF0 in arch_fpu_save_to+4).
 *
 * The fix is structural, not a constant nudge: the kernel NEVER executes
 * FXSAVE/FXRSTOR with a user operand.  Both helpers below
 *
 *   - reject a misaligned or absent destination/source with -EFAULT before
 *     the instruction can run,
 *   - stage the image in a 16-byte-aligned KERNEL buffer (always legal, always
 *     backed by a KVM memslot, never emulated), and
 *   - move the bytes with vm_kwrite/vm_user_read, which materialize the page
 *     (vm_touch) and copy through the kernel direct map under vm_lock.
 *
 * So a bad user address is an ordinary -EFAULT the caller can act on (deliver
 * the signal's default action / report a corrupt frame) instead of a fault in
 * the kernel.  The staging buffer is a stack local: two CPUs taking signals
 * at the same time must not share it, and 512 bytes inside a 64 KiB kernel
 * stack is cheap.
 *
 * Kernel constraints: this TU is plain C (no FP/SIMD codegen — the FPU is
 * touched only by arch_fpu_save_to/arch_fpu_restore_from in the arch's own
 * fpu.c/fpu.s), so it builds unchanged for both ports.
 */
#include <stdint.h>

#include "errno.h"
#include "linux_sigframe.h"
#include "process.h"
#include "vm.h"

/* The single rule both helpers below obey, exposed so a unit test can assert
 * it without needing a process (M6/L63): the address must be inside the v2
 * user window and 16-byte aligned.  Anything else is -EFAULT -- and it is
 * checked BEFORE the arch instruction runs, which is the whole point: FXSAVE
 * takes an implicit 512-byte operand and #GP(0)s on a misaligned one, in ring
 * 0, where KVM turns the fault into "emulation failure" and stops the vCPU. */
int arch_fpu_user_image_ok(uint64_t va) {
  if (va < USER_VA_BASE || va + FPU_SIG_IMAGE_BYTES - 1 >= USER_VA_TOP)
    return -EFAULT;
  if ((va & 15u) != 0)
    return -EFAULT;
  return 0;
}

/* v1 processes (p->as == NULL) address one statically mapped 32 MiB window at
   USER_VIRT_BASE: there is no v2 tree to walk and the window is always
   resident, so the legacy validate-then-byte-copy is their equivalent of
   vm_kwrite (this mirrors vm_user_read's own v1 arm).  Without this arm every
   native signal to a v1 program would fail its FP image and fall back to the
   default action. */
static int fpu_user_v1_ok(uint64_t va) {
  if (va < (uint64_t)USER_VIRT_BASE)
    return 0;
  if (va - (uint64_t)USER_VIRT_BASE > (uint64_t)USER_REGION_SIZE - FPU_SIG_IMAGE_BYTES)
    return 0;
  return 1;
}

#ifdef __x86_64__
/* FXRSTOR/XRSTOR #GP(0) when MXCSR has a bit set outside MXCSR_MASK.  The
   image comes from user memory (a handler may scribble on its frame), and
   the #GP would be in ring 0, so clear those bits first -- Linux does the
   same (`mxcsr &= mxcsr_feature_mask`). */
static void fpu_sanitize_mxcsr(uint8_t *img) {
  uint32_t m = (uint32_t)img[24] | ((uint32_t)img[25] << 8) |
               ((uint32_t)img[26] << 16) | ((uint32_t)img[27] << 24);
  m &= arch_fpu_mxcsr_mask();
  img[24] = (uint8_t)m;
  img[25] = (uint8_t)(m >> 8);
  img[26] = (uint8_t)(m >> 16);
  img[27] = (uint8_t)(m >> 24);
}
#endif

int arch_fpu_save_user(struct process *p, void *user_dst) {
  uint8_t stage[FPU_SIG_IMAGE_BYTES] __attribute__((aligned(16)));
  uint64_t dst = (uint64_t)(uintptr_t)user_dst;

  /* FXSAVE does not write bytes 416..511 (reserved / software-available), so
     an uninitialised stage would hand the user 96 bytes of kernel stack --
     and could forge FP_XSTATE_MAGIC1 at 464 by accident. */
  for (int i = 0; i < FPU_SIG_IMAGE_BYTES; i++)
    stage[i] = 0;

  /* Alignment first: a misaligned destination must never reach FXSAVE.  This
     is the check that turns a would-be kernel #GP into -EFAULT. */
  if (!p || (dst & 15u) != 0)
    return -EFAULT;

  if (!p->as) { /* v1 legacy window */
    if (!fpu_user_v1_ok(dst))
      return -EFAULT;
    arch_fpu_save_to(stage);
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)dst;
    for (int i = 0; i < FPU_SIG_IMAGE_BYTES; i++)
      d[i] = stage[i];
    return 0;
  }
  if (arch_fpu_user_image_ok(dst) != 0)
    return -EFAULT;

  arch_fpu_save_to(stage); /* kernel memory: aligned, backed, never emulated */
  return vm_kwrite(p, dst, stage, FPU_SIG_IMAGE_BYTES) == 0 ? 0 : -EFAULT;
}

int arch_fpu_restore_user(struct process *p, const void *user_src) {
  uint8_t stage[FPU_SIG_IMAGE_BYTES] __attribute__((aligned(16)));
  uint64_t src = (uint64_t)(uintptr_t)user_src;

  if (!p || (src & 15u) != 0)
    return -EFAULT;

  if (!p->as) { /* v1 legacy window */
    if (!fpu_user_v1_ok(src))
      return -EFAULT;
    const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)src;
    for (int i = 0; i < FPU_SIG_IMAGE_BYTES; i++)
      stage[i] = s[i];
#ifdef __x86_64__
    fpu_sanitize_mxcsr(stage);
#endif
    arch_fpu_restore_from(stage);
    return 0;
  }
  if (arch_fpu_user_image_ok(src) != 0)
    return -EFAULT;

  if (vm_user_read(p, src, stage, FPU_SIG_IMAGE_BYTES) != 0)
    return -EFAULT;
#ifdef __x86_64__
  fpu_sanitize_mxcsr(stage);
#endif
  arch_fpu_restore_from(stage);
  return 0;
}

#ifdef __x86_64__
/* ---- cp P1 / F2: the Linux-shaped XSAVE signal image (compat threads) ----
 *
 * Layout and magic words: src/include/linux_sigframe.h (golden-locked).  The
 * same staging rule as above: XSAVE/XRSTOR only ever see a 64-byte-aligned
 * KERNEL buffer; user memory is reached with vm_kwrite/vm_user_read, so a bad
 * frame is -EFAULT, never a ring-0 #GP.  Compat processes are always v2
 * (p->as != NULL: the compat loader only accepts static-PIE/ld.so images in
 * the v2 window), so there is no v1 arm here. */

static void put32(uint8_t *b, uint32_t off, uint32_t v) {
  b[off] = (uint8_t)v;
  b[off + 1] = (uint8_t)(v >> 8);
  b[off + 2] = (uint8_t)(v >> 16);
  b[off + 3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *b, uint32_t off) {
  return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
         ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}
static void put64(uint8_t *b, uint32_t off, uint64_t v) {
  put32(b, off, (uint32_t)v);
  put32(b, off + 4, (uint32_t)(v >> 32));
}
static uint64_t get64(const uint8_t *b, uint32_t off) {
  return (uint64_t)get32(b, off) | ((uint64_t)get32(b, off + 4) << 32);
}

uint32_t arch_fpu_sig_image_bytes(void) {
  return lsf_image_bytes(arch_fpu_xsave_enabled() ? arch_fpu_xsave_size() : 0);
}

int arch_fpu_save_user_sig(struct process *p, uint64_t dst) {
  if (!arch_fpu_xsave_enabled())
    return arch_fpu_save_user(p, (void *)(uintptr_t)dst);

  uint8_t stage[FPU_PCB_IMAGE_BYTES + LSF_FP_XSTATE_MAGIC2_SIZE]
      __attribute__((aligned(64)));
  uint32_t xsz = arch_fpu_xsave_size();
  uint32_t img = lsf_image_bytes(xsz);

  if (!p || !p->as || (dst & 63u) != 0)
    return -EFAULT;
  if (dst < USER_VA_BASE || dst + img - 1 >= USER_VA_TOP)
    return -EFAULT;

  /* Zero first: XSAVE writes neither the software-available bytes 464..511
     nor the header past XSTATE_BV, and both must read as defined values (the
     sw bytes below, zero for XCOMP_BV/reserved -- XRSTOR #GPs otherwise). */
  for (uint32_t i = 0; i < img; i++)
    stage[i] = 0;
  arch_fpu_xsave_to(stage);

  /* struct _fpx_sw_bytes, exactly as Linux's save_sw_bytes() fills it. */
  put32(stage, LSF_FP_SW_RESERVED_OFF + LSF_SW_MAGIC1_OFF, LSF_FP_XSTATE_MAGIC1);
  put32(stage, LSF_FP_SW_RESERVED_OFF + LSF_SW_EXTENDED_SIZE_OFF, img);
  put64(stage, LSF_FP_SW_RESERVED_OFF + LSF_SW_XFEATURES_OFF, arch_fpu_xcr0());
  put32(stage, LSF_FP_SW_RESERVED_OFF + LSF_SW_XSTATE_SIZE_OFF, xsz);
  put32(stage, xsz, LSF_FP_XSTATE_MAGIC2);

  return vm_kwrite(p, dst, stage, (int)img) == 0 ? 0 : -EFAULT;
}

int arch_fpu_restore_user_sig(struct process *p, uint64_t src, int *xstate_used) {
  if (xstate_used)
    *xstate_used = 0;
  if (!arch_fpu_xsave_enabled())
    return arch_fpu_restore_user(p, (const void *)(uintptr_t)src);

  uint8_t stage[FPU_PCB_IMAGE_BYTES + LSF_FP_XSTATE_MAGIC2_SIZE]
      __attribute__((aligned(64)));
  uint32_t xsz = arch_fpu_xsave_size();
  uint64_t xcr0 = arch_fpu_xcr0();

  if (!p || !p->as || (src & 15u) != 0)
    return -EFAULT;
  if (arch_fpu_user_image_ok(src) != 0)
    return -EFAULT;
  for (uint32_t i = 0; i < xsz; i++)
    stage[i] = 0;
  if (vm_user_read(p, src, stage, FPU_SIG_IMAGE_BYTES) != 0)
    return -EFAULT;
  fpu_sanitize_mxcsr(stage);

  /* Linux check_xstate_in_sigframe(): magic1, a plausible xstate_size no
     larger than ours and no larger than extended_size, then magic2 at
     +xstate_size.  The extended state is only trusted on a 64-byte-aligned
     fpstate (this kernel always plants one). */
  uint32_t sw = LSF_FP_SW_RESERVED_OFF;
  uint32_t m1 = get32(stage, sw + LSF_SW_MAGIC1_OFF);
  uint32_t ext = get32(stage, sw + LSF_SW_EXTENDED_SIZE_OFF);
  uint64_t xf = get64(stage, sw + LSF_SW_XFEATURES_OFF);
  uint32_t fsz = get32(stage, sw + LSF_SW_XSTATE_SIZE_OFF);
  int extended = 0;
  if ((src & 63u) == 0 && m1 == LSF_FP_XSTATE_MAGIC1 &&
      fsz >= LSF_XSTATE_HDR_OFF + LSF_XSTATE_HDR_BYTES && fsz <= xsz &&
      fsz + LSF_FP_XSTATE_MAGIC2_SIZE <= ext) {
    uint32_t m2 = 0;
    uint8_t m2b[4];
    if (vm_user_read(p, src + fsz, m2b, 4) == 0)
      m2 = get32(m2b, 0);
    if (m2 == LSF_FP_XSTATE_MAGIC2 &&
        vm_user_read(p, src + FPU_SIG_IMAGE_BYTES, stage + FPU_SIG_IMAGE_BYTES,
                     (int)(fsz - FPU_SIG_IMAGE_BYTES)) == 0)
      extended = 1;
  }

  uint32_t hdr = LSF_XSTATE_HDR_OFF;
  if (extended) {
    /* XRSTOR #GPs on a header it does not like: XSTATE_BV outside XCR0, a
       non-zero XCOMP_BV (compacted form) or a non-zero reserved byte.  Clamp
       it instead of faulting: features the frame does not claim (sw
       xfeatures) or does not fully contain (xstate_size) are re-initialised,
       which is what Linux's restore does for them too. */
    uint64_t bv = get64(stage, hdr) & xf & xcr0;
    if (fsz < LSF_XSTATE_YMMH_OFF + LSF_XSTATE_YMMH_BYTES)
      bv &= ~LSF_XFEATURE_AVX;
    put64(stage, hdr, bv);
    for (uint32_t i = 8; i < LSF_XSTATE_HDR_BYTES; i++)
      stage[hdr + i] = 0;
  } else {
    /* Linux's fx_only frame: the 512-byte legacy image restores x87/SSE and
       every other enabled component (the AVX upper halves) is initialised. */
    for (uint32_t i = 0; i < LSF_XSTATE_HDR_BYTES; i++)
      stage[hdr + i] = 0;
    put64(stage, hdr, LSF_XFEATURE_X87 | LSF_XFEATURE_SSE);
  }
  arch_fpu_xrstor_from(stage);
  if (xstate_used)
    *xstate_used = extended;
  return 0;
}
#endif /* __x86_64__ */
