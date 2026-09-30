// F1.5 (browser.md): AArch64 FPSIMD context save/restore + per-core enable.
//
// This is the ONE kernel translation unit permitted to touch the FP/SIMD
// register file (q0-q31) and the FP control registers (FPCR/FPSR) — the
// deliberate exception to the kernel's -mgeneral-regs-only rule:
//   * The rule exists so the COMPILER never emits SIMD in kernel C (no
//     compiler-emitted SIMD at EL1 — see hobbyos-kernel-constraints).  A
//     process's register file cannot be kept coherent across a context
//     switch without moving q0-q31, so the move is HAND-WRITTEN assembly
//     in a file of its own: no C codegen is involved and nothing else can
//     accidentally gain FP/SIMD access.
//   * clang's assembler REJECTS these instructions when the restriction is
//     in effect ("instruction requires: fp-armv8"), so the Makefile gives
//     obj/$(ARCH)/fpu.o its own rule that drops the flag for THIS FILE
//     ONLY; every kernel C TU keeps -mgeneral-regs-only.
//   * CPACR_EL1.FPEN=0b11 is set per core (fpu_enable_core below, called
//     from both boot.s entry paths) before any context switch or user FP
//     instruction runs on that core.
//
// Register-file layout — must match struct process.fpu_state (528 bytes):
//   [   0, 512 )  q0-q31  (32 x 128-bit registers, 16 stp/ldp pairs)
//   [ 512, 520 )  FPCR
//   [ 520, 528 )  FPSR
// The area must be 16-byte aligned (stp/ldp q only do aligned accesses).

.section .text
.global fpu_save
.global fpu_restore
.global fpu_enable_core

// void fpu_enable_core(void)
// Set CPACR_EL1.FPEN (bits 21:20) to 0b11: no trap on FP/SIMD at EL0 or
// EL1 on this core.  cpacr_el1 is PER-CORE, so every core must run this
// before its first context switch / user program.
fpu_enable_core:
    mrs x0, cpacr_el1
    mov x1, #(3 << 20)
    orr x0, x0, x1
    msr cpacr_el1, x0
    isb
    ret

// void fpu_save(uint64_t *area)   — area = &process->fpu_state
fpu_save:
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x1, fpcr
    str x1, [x0, #512]
    mrs x2, fpsr
    str x2, [x0, #520]
    ret

// void fpu_restore(const uint64_t *area)
fpu_restore:
    ldp q0, q1, [x0, #0]
    ldp q2, q3, [x0, #32]
    ldp q4, q5, [x0, #64]
    ldp q6, q7, [x0, #96]
    ldp q8, q9, [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    ldr x1, [x0, #512]
    msr fpcr, x1
    ldr x2, [x0, #520]
    msr fpsr, x2
    ret
