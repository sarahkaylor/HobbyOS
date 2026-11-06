/* src/kernel/arch/x64/fpu_test.c — unit suite for the signal-frame FP image.
 *
 * M6/L63.  The defect this suite guards is not a wrong answer, it is a fault:
 * FXSAVE/FXRSTOR take an implicit 512-byte memory operand and #GP(0) on a
 * misaligned one.  Executed from the kernel against a user address, that #GP
 * is unrecoverable under KVM (the vCPU stops with "internal error, suberror
 * 1"), so the guard in front of the instruction is the thing that has to hold.
 *
 * These tests therefore assert the GUARD, not the hardware: the hardware rule
 * is measured from ring 3 in src/user/linux_probe_fpu_sig.c P1, where a #GP is
 * an ordinary SIGSEGV instead of a dead machine.
 */
#ifdef KERNEL_MODE_UNIT_TEST

#include <stdint.h>

#include "errno.h"
#include "process.h"
#include "../../unit_test.h"
#include "vm.h"

/* A frame base this kernel plants is 8 (mod 16); Linux's fpstate offset is
   568, so 568 is the aligned answer for that base and 576 is the aligned
   answer for a 16-aligned base.  Both cases must yield an aligned ADDRESS,
   which is the only thing FXSAVE cares about. */
static uint32_t test_fp_off(uint64_t base) {
  uint64_t fp = (base + 568u + 15u) & ~(uint64_t)15u;
  return (uint32_t)(fp - base);
}

static void test_fpu_fpstate_offset_arithmetic(void) {
  tests_run++;
  uart_puts("  Running test_fpu_fpstate_offset_arithmetic...\n");

  /* base ≡ 8 (mod 16): the psABI handler-entry alignment.  568 = 8 mod 16, so
     base+568 ≡ 0 (mod 16) -- the kernel's actual case.  576 is the trap. */
  EXPECT_EQ((int)test_fp_off(0x1000u + 8u), 568);
  EXPECT_EQ((int)((0x1008u + test_fp_off(0x1008u)) & 15u), 0);
  EXPECT_EQ((int)((0x1008u + 576u) & 15u), 8);

  /* base ≡ 0 (mod 16): 568 would be misaligned, the derived offset is 576. */
  EXPECT_EQ((int)test_fp_off(0x2000u), 576);
  EXPECT_EQ((int)((0x2000u + test_fp_off(0x2000u)) & 15u), 0);
  EXPECT_EQ((int)((0x2000u + 568u) & 15u), 8);
}

static void test_fpu_user_image_guard(void) {
  tests_run++;
  uart_puts("  Running test_fpu_user_image_guard...\n");

  uint64_t ok8 = 0x1083EB1BB8ull + 568ull; /* base ≡ 8, the measured crash VA */
  EXPECT_EQ((int)(ok8 & 15u), 0);
  EXPECT_EQ(arch_fpu_user_image_ok(ok8), 0);
  EXPECT_EQ(arch_fpu_user_image_ok(0x1083EB1BB8ull + 576ull), -EFAULT);
  EXPECT_EQ(arch_fpu_user_image_ok(ok8 + 8), -EFAULT);
  EXPECT_EQ(arch_fpu_user_image_ok(ok8 + 4), -EFAULT);
  /* Outside the user window: rejected before any translation happens. */
  EXPECT_EQ(arch_fpu_user_image_ok(0), -EFAULT);
  EXPECT_EQ(arch_fpu_user_image_ok(0x1000), -EFAULT);
  EXPECT_EQ(arch_fpu_user_image_ok(USER_VA_TOP - 256), -EFAULT);
}

static void test_fpu_save_user_rejects_without_faulting(void) {
  tests_run++;
  uart_puts("  Running test_fpu_save_user_rejects_without_faulting...\n");

  /* These calls must RETURN -EFAULT.  If the alignment/range guard ever moves
     behind the FXSAVE, the misaligned one executes `fxsave64` in ring 0 and
     the machine dies here instead of failing the test -- which is exactly the
     §48.4 mechanism, kept as a live tripwire. */
  EXPECT_EQ(arch_fpu_save_user(0, (void *)0x1083EB1DF8ull), -EFAULT);
  EXPECT_EQ(arch_fpu_restore_user(0, (const void *)0x1083EB1DF8ull), -EFAULT);
  /* 16-aligned but with no address space to copy into. */
  EXPECT_EQ(arch_fpu_save_user(0, (void *)0x1083EB1DF0ull), -EFAULT);
}

static void test_fpu_pcb_image_alignment(void) {
  tests_run++;
  uart_puts("  Running test_fpu_pcb_image_alignment...\n");

  /* The PCB copy is a plain kernel address; the struct must keep it aligned
     for the same reason (FXSAVE64/FXRSTOR64 #GP on a misaligned operand). */
  EXPECT_EQ((int)FPU_SIG_IMAGE_BYTES, 512);
  EXPECT_EQ((int)__alignof__(((struct process *)0)->fpu_state), 16);
}

void fpu_sig_test_suite(void) {
  test_fpu_fpstate_offset_arithmetic();
  test_fpu_user_image_guard();
  test_fpu_save_user_rejects_without_faulting();
  test_fpu_pcb_image_alignment();
}

#endif /* KERNEL_MODE_UNIT_TEST */
