#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include <stdint.h>

extern uint32_t get_cpuid(void);

static void test_get_cpuid(void) {
  uart_puts("  Running test_get_cpuid...\n");
  tests_run++;
  uint32_t id = get_cpuid();
  // Test runs on main core, so it should be 0
  EXPECT_EQ(id, 0);
}

#ifdef __x86_64__
static void test_cpu_locals_cpu_id(void) {
  uart_puts("  Running test_cpu_locals_cpu_id...\n");
  tests_run++;

  /* Must mirror struct cpu_local in arch/x64/trap.c EXACTLY: the array
     stride is sizeof(this struct), so a short copy makes cpu_locals[1]
     read cpu_locals[0]'s snap_* fields (GB-147 added 8 trailing fields
     after this test was written — that made the old 6-field copy read
     kernel_stack from cpu0.snap_rip and cpu_id from cpu0.snap_ss, i.e.
     "Left 0 / Right 1" forever).  Keep in sync. */
  struct cpu_local {
    uint64_t kernel_stack;
    uint64_t user_rsp;
    uint64_t temp_rax;
    uint64_t user_sp_temp;
    uint64_t cpu_id;
    void *current_proc;
    uint64_t snap_rip;
    uint64_t snap_cs;
    uint64_t snap_rflags;
    uint64_t snap_rsp;
    uint64_t snap_ss;
    uint64_t snap_valid;
    uint64_t snap_count;
    uint64_t fault_count;
  } __attribute__((packed));
  extern struct cpu_local cpu_locals[];

  for (int i = 0; i < 4; i++) {
    /* A core that was never brought up has an all-zero cpu_local (the
       kernel's own liveness probe is kernel_stack != 0, trap.c).  The unit
       tier boots one core, so a live core is asserted to know its own
       index and an unstarted one only has to not claim another index.
       so-vm lane A (CROSS-LANE): the unconditional `== i` form had been
       red at the M0 freeze commit 466c6a84 (Left 0 / Right 1 for core 1). */
    if (cpu_locals[i].kernel_stack == 0) {
      EXPECT_EQ(cpu_locals[i].cpu_id, 0);
      continue;
    }
    EXPECT_EQ(cpu_locals[i].cpu_id, (uint64_t)i);
  }
}
#endif

void smp_test_suite(void) {
  uart_puts("smp_test_suite:\n");
  test_get_cpuid();
#ifdef __x86_64__
  test_cpu_locals_cpu_id();
#endif
}

#endif // KERNEL_MODE_UNIT_TEST
