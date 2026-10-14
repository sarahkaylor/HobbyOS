/*
 * HobbyOS selfhost W2-2: setjmp/longjmp for the AArch64 and x86-64 ABIs.
 *
 * Saves/restores the callee-saved register file, the stack pointer and the
 * return address so a non-local jump round-trips correctly through the
 * normal return path (LLVM's CrashRecoveryContext compiles every in-process
 * cc1 via this).  Crash unwinding / signal-frame recovery is deliberately
 * out of scope.
 *
 * Buffer offsets (bytes), matching <setjmp.h>:
 *   aarch64: x19..x28 @0|16|32|48|64, x29@80, x30@88, SP@96,
 *            d8..d15 @104..160  (21 words = 168 B)
 *   x86_64 : rbx@0 rbp@8 r12@16 r13@24 r14@32 r15@40, SP@48, RA@56
 *
 * All four handlers are __attribute__((naked)) with the buffer and value
 * taken straight from the ABI registers (env in x0/rdi, val in w1/esi), so
 * no operand can be allocated in a register the code clobbers.  NOTE the
 * sigsetjmp/siglongjmp handlers are their OWN asm, not wrappers calling
 * setjmp: a longjmp lands inside the setjmp function, and a C wrapper's
 * epilogue would pop a stack slot that a later call (e.g. siglongjmp's own
 * call) has since overwritten — real libcs implement sigsetjmp in assembly
 * for exactly this reason.
 *
 * Under HOST_TEST the symbols are renamed hb_* and operate on the host's
 * <setjmp.h> jmp_buf (200 B on x86-64 — our 64 B x86-64 footprint fits), so
 * src/host/libc_setjmp_test.c exercises the real code paths natively.
 */
/*
 * (header note) src/include has a KERNEL setjmp.h (16-word context buffer,
 * used only by kernel TUs via "setjmp.h"); the in-tree -I order would pull
 * it in for <setjmp.h> too.  This implementation must pair with OUR 24-word
 * layout, so the header is reached by an explicit relative path — the
 * toolchain sysroot build (libc-include first) never hits this, and under
 * HOST_TEST include_next still lands on the real host header.
 */
#include "../libc/include/setjmp.h"
#include <stddef.h>

#ifdef HOST_TEST
#define HB_NAME(name) hb_##name
#else
#define HB_NAME(name) name
#endif

#if defined(__aarch64__)
#define HB_SETJMP_ASM \
  "stp x19, x20, [x0, #0]\n" \
  "stp x21, x22, [x0, #16]\n" \
  "stp x23, x24, [x0, #32]\n" \
  "stp x25, x26, [x0, #48]\n" \
  "stp x27, x28, [x0, #64]\n" \
  "stp x29, x30, [x0, #80]\n" \
  "mov x1, sp\n" \
  "str x1, [x0, #96]\n" \
  "stp d8, d9, [x0, #104]\n" \
  "stp d10, d11, [x0, #120]\n" \
  "stp d12, d13, [x0, #136]\n" \
  "stp d14, d15, [x0, #152]\n" \
  "mov x0, #0\n" \
  "ret\n"
#define HB_LONGJMP_ASM \
  "ldp x19, x20, [x0, #0]\n" \
  "ldp x21, x22, [x0, #16]\n" \
  "ldp x23, x24, [x0, #32]\n" \
  "ldp x25, x26, [x0, #48]\n" \
  "ldp x27, x28, [x0, #64]\n" \
  "ldp x29, x30, [x0, #80]\n" \
  "ldr x2, [x0, #96]\n" \
  "ldp d8, d9, [x0, #104]\n" \
  "ldp d10, d11, [x0, #120]\n" \
  "ldp d12, d13, [x0, #136]\n" \
  "ldp d14, d15, [x0, #152]\n" \
  "mov sp, x2\n" \
  "mov w0, w1\n" \
  "cbnz w0, 1f\n" \
  "mov w0, #1\n" \
  "1:\n" \
  "ret\n"
#elif defined(__x86_64__)
#define HB_SETJMP_ASM \
  "movq %rbx, 0(%rdi)\n" \
  "movq %rbp, 8(%rdi)\n" \
  "movq %r12, 16(%rdi)\n" \
  "movq %r13, 24(%rdi)\n" \
  "movq %r14, 32(%rdi)\n" \
  "movq %r15, 40(%rdi)\n" \
  "leaq 8(%rsp), %rax\n" \
  "movq %rax, 48(%rdi)\n" \
  "movq (%rsp), %rax\n" \
  "movq %rax, 56(%rdi)\n" \
  "xorl %eax, %eax\n" \
  "ret\n"
#define HB_LONGJMP_ASM \
  "movq 0(%rdi), %rbx\n" \
  "movq 8(%rdi), %rbp\n" \
  "movq 16(%rdi), %r12\n" \
  "movq 24(%rdi), %r13\n" \
  "movq 32(%rdi), %r14\n" \
  "movq 40(%rdi), %r15\n" \
  "movq 56(%rdi), %rdx\n" \
  "movq 48(%rdi), %rsp\n" \
  "movl %esi, %eax\n" \
  "testl %eax, %eax\n" \
  "jnz 1f\n" \
  "movl $1, %eax\n" \
  "1:\n" \
  "jmp *%rdx\n"
#else
#define HB_SETJMP_ASM /* none */
#define HB_LONGJMP_ASM /* none */
#endif

__attribute__((naked)) int HB_NAME(setjmp)(jmp_buf env) {
#if defined(__aarch64__) || defined(__x86_64__)
  __asm__ __volatile__(HB_SETJMP_ASM);
#else
  (void)env;
  return 0;
#endif
}

__attribute__((naked)) void HB_NAME(longjmp)(jmp_buf env, int val) {
#if defined(__aarch64__) || defined(__x86_64__)
  __asm__ __volatile__(HB_LONGJMP_ASM);
#else
  (void)env;
  (void)val;
#endif
}

/* sigsetjmp/siglongjmp: no signal masking exists on HobbyOS (sigprocmask is
 * a no-op), so these are the same machinery — the savesigs flag is accepted
 * and ignored.  Own naked bodies, per the wrapper caveat above. */
__attribute__((naked)) int HB_NAME(sigsetjmp)(sigjmp_buf env, int savesigs) {
#if defined(__aarch64__) || defined(__x86_64__)
  /* savesigs accepted and ignored (no signal masking exists) */
  __asm__ __volatile__(HB_SETJMP_ASM);
#else
  (void)env;
  (void)savesigs;
  return 0;
#endif
}

__attribute__((naked)) void HB_NAME(siglongjmp)(sigjmp_buf env, int val) {
#if defined(__aarch64__) || defined(__x86_64__)
  __asm__ __volatile__(HB_LONGJMP_ASM);
#else
  (void)env;
  (void)val;
#endif
}
