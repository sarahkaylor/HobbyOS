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
 *
 * GB-246: the panic used to name nothing — five of ten intel waves died at
 * this line with no function, no canary delta, no stack range, so every wave
 * was another guess.  The hook now captures the return address INSIDE the
 * faulting function (resolve it with `llvm-addr2line -e hobbyos.elf <pc>`),
 * %rsp, the caller's %rbp, the guard, and the canary slot decoded from the
 * function's prologue (see ssp_report.c), and prints expected-vs-found with
 * the arithmetic delta before halting.
 */
#include <stdint.h>

#include <ssp.h>

extern void panic_puts(const char *s);
#if defined(__x86_64__)
extern int panic_claim(void);
extern void panic_leave(void);
#endif

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

/* Shared body of the panic: `fp` is the faulting function's frame base and
 * `sp` points at the return address the call pushed.  Must not itself be
 * instrumented (a canary failure inside the canary reporter would recurse). */
__attribute__((used, noreturn, noinline, no_stack_protector))
void ssp_fail_c(uint64_t fp, uint64_t sp) {
  uint64_t pc = *(const uint64_t *)sp; /* return address inside the smasher */
  uint64_t guard = __stack_chk_guard;
  uint64_t found = 0;
  int have_found = 0;
  uint64_t off = ssp_pc_canary_offset(pc);

  if (off && fp) {
    found = *(const volatile uint64_t *)(fp - off);
    have_found = 1;
  }

  char buf[384];
  ssp_format_report(buf, (int)sizeof buf, pc, sp, fp, guard, found, have_found);
#if defined(__x86_64__)
  /* Enter panic mode and claim the record: the sink is lock-free for exactly
   * this path (a smasher may hold uart_lock, and a reporter that deadlocks on
   * the console loses the fact it exists to record). */
  if (panic_claim()) {
    panic_puts(buf);
    panic_leave();
  }
#else
  panic_puts(buf);
#endif
  for (;;)
    ;
}

/* A smashed canary: name it, print expected-vs-found, and halt.  Returning
 * would resume execution with a corrupted frame — exactly what the check
 * exists to prevent.  Naked so %rbp still holds the caller's frame base and
 * %rsp points at the return address: a compiler prologue here would replace
 * both before we could read them. */
__attribute__((naked, noreturn, no_stack_protector)) void __stack_chk_fail(void) {
#if defined(__x86_64__)
  __asm__ volatile("movq %rbp, %rdi\n\t"
                   "movq %rsp, %rsi\n\t"
                   "callq ssp_fail_c\n\t");
#elif defined(__aarch64__)
  __asm__ volatile("mov x0, x29\n\t"
                   "mov x1, sp\n\t"
                   "bl ssp_fail_c\n\t");
#else
  for (;;)
    ;
#endif
}
