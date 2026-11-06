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

int arch_fpu_save_user(struct process *p, void *user_dst) {
  uint8_t stage[FPU_SIG_IMAGE_BYTES] __attribute__((aligned(16)));
  uint64_t dst = (uint64_t)(uintptr_t)user_dst;

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
    arch_fpu_restore_from(stage);
    return 0;
  }
  if (arch_fpu_user_image_ok(src) != 0)
    return -EFAULT;

  if (vm_user_read(p, src, stage, FPU_SIG_IMAGE_BYTES) != 0)
    return -EFAULT;
  arch_fpu_restore_from(stage);
  return 0;
}
