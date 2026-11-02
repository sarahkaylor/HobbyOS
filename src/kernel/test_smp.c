#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "process.h"   /* GB-152: the one struct cpu_local + smp_init_count */
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

  extern void print_int(int val);
  /* GB-152: use the kernel's ONE declaration (process.h), not a local copy,
     and measure only the cores that have actually run
     trap_init_core_with_id.  The T2 suite runs before the scheduler, so on a
     1-CPU run this is 1 and the test says so instead of asserting cores that
     do not exist yet.  AP bring-up is sequential, so ids 0..n-1 are the
     initialized ones. */
  uint32_t n_init = smp_init_count();
  uart_puts("  cpu_locals initialized: ");
  print_int((int)n_init);
  uart_puts("\n");

  for (uint32_t i = 0; i < n_init; i++) {
    if (cpu_locals[i].cpu_id != (uint64_t)i) {
      /* Name the index: EXPECT_EQ prints the expression, not i, and a plain
         "cpu_id == i" failure cannot tell a reader which core was wrong. */
      uart_puts("  cpu_locals index ");
      print_int((int)i);
      uart_puts(" has cpu_id ");
      print_int((int)cpu_locals[i].cpu_id);
      uart_puts("\n");
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
