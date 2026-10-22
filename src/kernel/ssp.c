/* SEC-4 (security-plan.md #4): kernel stack canaries.
 *
 * The kernel is built with -fstack-protector-strong -mstack-protector-guard=global:
 * every function with a local buffer or address-taken local gets a guard
 * value between its frame and the saved return address, checked on return.
 * This file provides the two runtime pieces clang's instrumentation needs
 * (there is no libc in the kernel) plus the boot-time seeding of the guard.
 *
 * The guard is deliberately NOT zero: an overflow that memsets or zeroes a
 * frame would sail through a zero canary untouched.  ssp_init() mixes the
 * physical counter with the guard's own address through splitmix64 so the
 * value varies between boots; it runs right after uart_init() in main().
 */
#include <stdint.h>

extern void uart_puts(const char *s);

/* Referenced by every instrumented function (-mstack-protector-guard=global). */
uint64_t __stack_chk_guard;

static uint64_t splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

void ssp_init(void) {
  uint64_t seed = (uint64_t)(uintptr_t)&__stack_chk_guard;
#ifdef __aarch64__
  uint64_t t;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
#elif defined(__x86_64__)
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  uint64_t t = ((uint64_t)hi << 32) | lo;
#else
  uint64_t t = 0x1234567887654321ULL;
#endif
  for (int i = 0; i < 8; i++) {
    seed += t + 0x9E3779B97F4A7C15ULL;
    seed = splitmix64(seed);
    t ^= seed >> 17;
  }
  if (!seed)
    seed = 0xA5A5A5A53C3C3C3CULL; /* never zero (see header comment) */
  __stack_chk_guard = seed;
}

/* A smashed canary: print and halt.  Returning would resume execution with
 * a corrupted frame — exactly what the check exists to prevent.  Must not
 * itself be instrumented (recursion on failure). */
__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void) {
  uart_puts("KERNEL PANIC: stack smashing detected (SEC-4 canary)\n");
  for (;;)
    ;
}
