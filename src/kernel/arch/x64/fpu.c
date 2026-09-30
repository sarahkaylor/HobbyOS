/* src/kernel/arch/x64/fpu.c — x87 + SSE state management for the x86_64 port.
 *
 * HobbyOS F1.5 (browser.md): userland may use the FPU/SSE, so the kernel has
 * to (a) enable the hardware on every core before user code can run there and
 * (b) carry each process's x87+SSE register file across context switches.
 *
 * This file is the ONLY x64 translation unit that intentionally touches the
 * FPU, and it is deliberately self-contained: the kernel CFLAGS keep
 * -mno-sse -mno-sse2 -mno-mmx -mno-avx, so no kernel C code can emit SIMD,
 * MMX or x87 instructions.  fxsave64/fxrstor64 do not require SSE codegen —
 * they are plain instructions executed from inline asm, which keeps the
 * FPU-touching surface to exactly four functions below.
 *
 * Where the pieces hook in:
 *   - arch_fpu_enable_core()  — CR0/CR4 per core (trap_init_core_with_id,
 *     which runs on the boot core during mmu_init() and on every AP from
 *     secondary_main() before start_scheduler()).
 *   - arch_fpu_reset()        — process_create(), so a PCB slot reused from
 *     a dead process starts from a clean architectural default instead of
 *     the previous occupant's register file.
 *   - arch_fpu_save()         — save_context() (process.c): FXSAVE64 the
 *     LIVE register file into the outgoing PCB.  The kernel never executes
 *     x87/SSE, so at that point the registers still hold exactly the state
 *     the preempted process had.  For fork this is also the inheritable FP
 *     context: process_fork() calls save_context(child, tf) while the
 *     parent is executing, so the child's image is the parent's state.
 *   - arch_fpu_restore()      — restore_context() (process.c): FXRSTOR64
 *     the incoming PCB on every resume (schedule() and the scheduler's idle
 *     loop), so state follows a process across preemption and across CPUs.
 *
 * The 512-byte image lives in struct process (src/include/process.h) as
 * `uint8_t fpu_state[512]` with 16-byte alignment — FXSAVE64/FXRSTOR64
 * fault (#GP) on a misaligned memory operand.
 */
#include <stdint.h>

#include "process.h"

#define FPU_IMAGE_BYTES 512

/* FXSAVE image offsets used for the architectural default state. */
#define FPU_IMG_FCW 0     /* x87 control word (2 bytes)                  */
#define FPU_IMG_FTW 4     /* abridged x87 tag word (1 byte)              */
#define FPU_IMG_MXCSR 24  /* SSE control/status word (4 bytes)           */

/**
 * Enables x87/SSE execution on the CURRENT core.  CR0 and CR4 are both
 * per-core registers, so this must run once on every core before user code
 * executes there (see trap_init_core_with_id); the early boot paths set the
 * same bits in boot.s long before this.
 *
 *   CR0.EM = 0  no x87 emulation trap (EM=1 makes every x87/SSE op #UD)
 *   CR0.MP = 1  fwait/finit honors pending FP operations
 *   CR0.TS = 0  no lazy FPU switching: this kernel saves/restores the whole
 *               FXSAVE64 image on every context switch, so the #NM "task
 *               switched" trap is never part of the design (TS=1 would make
 *               the first FP op after every switch trap into the kernel).
 *   CR4.OSFXSR     = 1  lets FXSAVE/FXRSTOR run and marks SSE as OS-managed
 *   CR4.OSXMMEXCPT = 1  unmasked SIMD FP exceptions arrive as #XM (vector
 *                       19, which the IDT already routes) instead of #UD
 */
void arch_fpu_enable_core(void) {
  uint64_t cr0;
  uint64_t cr4;

  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(uint64_t)((1u << 2) | (1u << 3)); /* EM, TS = 0 */
  cr0 |= (uint64_t)(1u << 1);                /* MP = 1     */
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (uint64_t)((1u << 9) | (1u << 10)); /* OSFXSR, OSXMMEXCPT */
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

/**
 * Initializes a fresh process's FXSAVE64 image: zeroed, then stamped with
 * the architectural default control words.
 *
 * An all-zero image is NOT a usable default: MXCSR=0 leaves all six SSE
 * exception masks clear, so the first inexact result (e.g. 2.0/7.0) would
 * raise #XM and kill the process, and FCW=0 leaves the x87 exceptions
 * unmasked the same way.  The values below are what a just-reset FPU looks
 * like in an FXSAVE image.
 */
void arch_fpu_reset(struct process *p) {
  uint8_t *img = p->fpu_state;

  for (int i = 0; i < FPU_IMAGE_BYTES; i++)
    img[i] = 0;

  img[FPU_IMG_FCW] = 0x7F;     /* FCW = 0x037F  (all x87 exceptions masked) */
  img[FPU_IMG_FCW + 1] = 0x03;
  img[FPU_IMG_FTW] = 0xFF;     /* FTW = all x87 registers empty             */
  img[FPU_IMG_MXCSR] = 0x80;   /* MXCSR = 0x1F80 (all SIMD exceptions       */
  img[FPU_IMG_MXCSR + 1] = 0x1F; /*        masked, round-to-nearest)        */
}

/**
 * Saves the live x87+SSE register file into a PCB.  FXSAVE64 stores the
 * 64-bit-pointer image form, pairing with FXRSTOR64 below.
 */
void arch_fpu_save(struct process *p) {
  __asm__ volatile("fxsave64 (%0)" : : "r"((void *)p->fpu_state) : "memory");
}

/**
 * Loads a process's x87+SSE register file from its PCB image.
 */
void arch_fpu_restore(struct process *p) {
  __asm__ volatile("fxrstor64 (%0)" : : "r"((const void *)p->fpu_state) : "memory");
}
