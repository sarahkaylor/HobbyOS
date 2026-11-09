/* src/kernel/arch/x64/trap_user_class_test.c — GB-243.
 *
 * Kernel unit proof for GB-101's user-CS classifier: a user-mode trap frame
 * with no current process must select the user-mode path, not the kernel-mode
 * fatal, and the classifier must leave the record fields the dump prints
 * untouched.  Precedent: fpu_sig_test.c (KERNEL_MODE_UNIT_TEST).
 *
 * The case fabricates the frame GB-147 measured (#PF, cs=0x1B, pid=-1); it
 * calls the same helper the trap path calls, so it cannot drift from the
 * shipped decision without failing here.
 */
#ifdef KERNEL_MODE_UNIT_TEST

#include <stdint.h>

#include "process.h"
#include "arch/trap.h"
#include "../../unit_test.h"

/* Defined in trap.c; declared here because the suite is the second caller. */
int trap_user_cs_no_process(struct trap_frame *tf, struct process *cur);

static void test_trap_user_cs_classifier(void) {
  tests_run++;
  uart_puts("  Running test_trap_user_cs_classifier...\n");

  struct trap_frame tf;
  uint8_t *p = (uint8_t *)&tf;
  for (unsigned i = 0; i < sizeof tf; i++)
    p[i] = 0;

  tf.vector = 14;           /* #PF */
  tf.cs = 0x1B;             /* user selector */
  tf.elr = 0x17FFFFF000ull; /* rip */
  tf.error_code = 0x4;      /* user, not-present */
  tf.lr = 0x17FFFFF100ull;  /* rsp */

  /* The classifier selects the user path with no current process (the
     GB-147 shape) and does not touch the fields the record prints. */
  EXPECT_EQ(trap_user_cs_no_process(&tf, 0), 1);
  EXPECT_EQ((int)tf.vector, 14);
  EXPECT_EQ((int)tf.cs, 0x1B);
  EXPECT_EQ((int)tf.error_code, 4);
  EXPECT_EQ((int)(tf.elr & 0xFFFFFFFFu), (int)(0x17FFFFF000ull & 0xFFFFFFFFu));
  EXPECT_EQ((int)(tf.lr & 0xFFFFFFFFu), (int)(0x17FFFFF100ull & 0xFFFFFFFFu));

  /* A kernel-CS frame is not a user fault even with no process. */
  tf.cs = 0x08;
  EXPECT_EQ(trap_user_cs_no_process(&tf, 0), 0);

  /* A different user selector (0x23) still classifies as user mode. */
  tf.cs = 0x23;
  EXPECT_EQ(trap_user_cs_no_process(&tf, 0), 1);
}

void trap_user_class_test_suite(void) {
  test_trap_user_cs_classifier();
}

#endif /* KERNEL_MODE_UNIT_TEST */
