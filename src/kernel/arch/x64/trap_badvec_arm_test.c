/* src/kernel/arch/x64/trap_badvec_arm_test.c — GB-244.
 *
 * Kernel unit proof for the [BADVEC] DR0 arming decision.  Falsifier for the
 * card: without the pid-1 kernel-CS allowance the watch is blind to exactly
 * the GB-101-V7 frame (pid 1, cs=0x08, v=13, vector slot 13 -> 1879777880 at
 * 9.3 s), and without the guards it re-arms on the boot frames GB-146-A
 * excluded.  Both directions are asserted here, so the decision cannot drift
 * from the shipped condition without failing this suite.
 */
#ifdef KERNEL_MODE_UNIT_TEST

#include <stdint.h>

#include "process.h"
#include "arch/trap.h"
#include "../../unit_test.h"

/* Defined in trap.c; declared here because the suite is the second caller. */
int badvec_should_arm(uint64_t frame, uint64_t cs, uint64_t vector, int64_t pid,
                      uint64_t now_ms, int armed, int prints, int pid1_prints);
void badvec_print_writer_row(uint64_t rip_after, uint64_t slot, uint64_t frame,
                             int frame_pid, int vect, int pid, int cpu);
int badvec_frame_window(uint64_t p);
void badvec_print_not_a_frame(uint64_t tfu);

static void test_badvec_should_arm_pid1(void) {
  tests_run++;
  uart_puts("  Running test_badvec_should_arm_pid1...\n");

  /* The GB-101-V7 shape: pid 1, kernel CS, v=13, census window, 9.3 s. */
  EXPECT_EQ(badvec_should_arm(0x23FFE280ull, 0x08, 13, 1, 9333, 0, 0, 0), 1);
  /* Boot-phase pid-1 frame: excluded (wave7's boot flood). */
  EXPECT_EQ(badvec_should_arm(0x23FFE280ull, 0x08, 13, 1, 1000, 0, 0, 0), 0);
  /* Not kernel CS: the user-CS classifier owns that path. */
  EXPECT_EQ(badvec_should_arm(0x23FFE280ull, 0x1B, 13, 1, 9333, 0, 0, 0), 0);
  /* IRQ frames stay out: their entry would overwrite the stamp being watched. */
  EXPECT_EQ(badvec_should_arm(0x23FFE280ull, 0x08, 32, 1, 9333, 0, 0, 0), 0);
  /* Kernel image address is outside the census window. */
  EXPECT_EQ(badvec_should_arm(0x7008A3FEull, 0x08, 13, 1, 9333, 0, 0, 0), 0);
  /* One shot per CPU, and both budgets hold. */
  EXPECT_EQ(badvec_should_arm(0x23FFE280ull, 0x08, 13, 1, 9333, 1, 0, 0), 0);
  EXPECT_EQ(badvec_should_arm(0x23FFE280ull, 0x08, 13, 1, 9333, 0, 64, 0), 0);
  EXPECT_EQ(badvec_should_arm(0x23FFE280ull, 0x08, 13, 1, 9333, 0, 0, 8), 0);
  /* A test process keeps the pre-GB-244 behaviour. */
  EXPECT_EQ(badvec_should_arm(0x25FFB640ull, 0x1B, 13, 7, 500, 0, 0, 0), 1);
}

/* GB-244 clause 3: force the GB-101-V7 kernel-CS frame through the same row
   the #DB handler prints.  The row appears in the unit serial; the assert is
   that the same frame arms. */
static void test_badvec_writer_row_kernel_cs(void) {
  tests_run++;
  uart_puts("  Running test_badvec_writer_row_kernel_cs...\n");
  EXPECT_EQ(badvec_should_arm(0x23FFE280ull, 0x08, 13, 1, 55247, 0, 0, 0), 1);
  badvec_print_writer_row(0x7008A3FEull, 0x23FFE388ull, 0x23FFE280ull, 1, 13,
                          1, 7);
}

/* GB-247: the fatal [BADVEC] dump must never read a slot through a pointer
   that is not a frame.  The GB-146-V2 record read 0x1F36 (unaligned, so not a
   uint64 frame on any CPU) and then printed a slot VA from a different
   pointer; this case proves the window rejects it and that the dump's
   not-a-frame line is the one row that comes out. */
static void test_badvec_fatal_frame_window(void) {
  tests_run++;
  uart_puts("  Running test_badvec_fatal_frame_window...\n");

  /* The 0x1F36 pointer from GB-146-V2's run 1: not 8-byte aligned, so it
     cannot be a trap frame on any CPU's stack. */
  EXPECT_EQ(badvec_frame_window(0x1F36ull), 0);
  /* The entry stamps from the same record (0x23FFE2A0) and the earlier
     census (0x25FFB640) are aligned frames and still pass. */
  EXPECT_EQ(badvec_frame_window(0x23FFE2A0ull), 1);
  EXPECT_EQ(badvec_frame_window(0x25FFB640ull), 1);
  /* Kernel text is never a frame. */
  EXPECT_EQ(badvec_frame_window(0x7008A3FEull), 0);
  /* The exact line the fatal dump prints for the 0x1F36 pointer; no
     vector_slot=/now_v=/now_cs= row is emitted through it. */
  badvec_print_not_a_frame(0x1F36ull);
}

void trap_badvec_arm_test_suite(void) {
  test_badvec_should_arm_pid1();
  test_badvec_writer_row_kernel_cs();
  test_badvec_fatal_frame_window();
}
#endif /* KERNEL_MODE_UNIT_TEST */
