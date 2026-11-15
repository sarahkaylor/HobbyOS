/* src/kernel/arch/x64/fpu.c — x87 + SSE (+ AVX via XSAVE) state management for
 * the x86_64 port.
 *
 * HobbyOS F1.5 (browser.md): userland may use the FPU/SSE, so the kernel has
 * to (a) enable the hardware on every core before user code can run there and
 * (b) carry each process's register file across context switches.
 *
 * cp P1 / AD-4 (docs/cyberpunk.md): when the CPU has XSAVE the kernel sets
 * CR4.OSXSAVE and programs XCR0 = (x87 | SSE | AVX) & CPUID.(EAX=0xD,ECX=0).EAX
 * -- deliberately never AVX-512, PKRU, AMX or any other component, even when
 * the CPU offers them -- and the per-thread image becomes an XSAVE
 * standard-format area (XSAVE/XRSTOR; XSAVEOPT/XSAVES are later
 * optimisations).  Without XSAVE (e.g. QEMU's default qemu64 CPU) everything
 * stays exactly the FXSAVE64/FXRSTOR64 path it always was, and AVX
 * instructions #UD for user code, as they would on such a CPU under Linux.
 *
 * This file is the ONLY x64 translation unit that intentionally touches the
 * FPU, and it is deliberately self-contained: the kernel CFLAGS keep
 * -mno-sse -mno-sse2 -mno-mmx -mno-avx, so no kernel C code can emit SIMD,
 * MMX or x87 instructions.  FXSAVE/XSAVE and friends do not require SSE
 * codegen -- they are plain instructions executed from inline asm.  The
 * XSAVE-family ones are emitted as .byte sequences with a fixed %rdi operand
 * so the integrated assembler's feature checks under the kernel target flags
 * can never refuse them.
 *
 * Where the pieces hook in:
 *   - arch_fpu_enable_core()  — CR0/CR4(/XCR0) per core (trap_init_core_with_id,
 *     which runs on the boot core during mmu_init() and on every AP from
 *     secondary_main() before start_scheduler()).  The boot core's call also
 *     makes the XSAVE decision for the machine; APs apply the same one.
 *   - arch_fpu_reset()        — process_create()/thread create, so a PCB slot
 *     reused from a dead process starts from a clean architectural default
 *     instead of the previous occupant's register file.
 *   - arch_fpu_save()         — save_context() (process.c): XSAVE/FXSAVE64 the
 *     LIVE register file into the outgoing PCB.  The kernel never executes
 *     x87/SSE/AVX, so at that point the registers still hold exactly the state
 *     the preempted process had.  For fork this is also the inheritable FP
 *     context: process_fork() calls save_context(child, tf) while the
 *     parent is executing, so the child's image is the parent's state.
 *   - arch_fpu_restore()      — restore_context() (process.c): XRSTOR/FXRSTOR64
 *     the incoming PCB on every resume (schedule() and the scheduler's idle
 *     loop), so state follows a process across preemption and across CPUs.
 *   - signal frames: src/kernel/fpu_user.c (staged, never a user operand).
 *
 * The image lives in struct process (src/include/process.h) in
 * `uint64_t fpu_state[128]` (FPU_PCB_IMAGE_BYTES = 1024) with 64-byte
 * alignment -- XSAVE/XRSTOR #GP on a memory operand that is not 64-byte
 * aligned, FXSAVE64/FXRSTOR64 on one that is not 16-byte aligned.
 */
#include <stdint.h>

#include "kline.h"
#include "process.h"

#define FPU_IMAGE_BYTES 512

/* FXSAVE image offsets used for the architectural default state. */
#define FPU_IMG_FCW 0         /* x87 control word (2 bytes)              */
#define FPU_IMG_FTW 4         /* abridged x87 tag word (1 byte)          */
#define FPU_IMG_MXCSR 24      /* SSE control/status word (4 bytes)       */
#define FPU_IMG_XSTATE_BV 512 /* XSAVE header: XSTATE_BV (8 bytes)       */

#define CR4_OSFXSR (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_OSXSAVE (1ull << 18)

#define XFEAT_X87 0x1ull
#define XFEAT_SSE 0x2ull
#define XFEAT_AVX 0x4ull

/* The machine-wide decision, made once by the boot core's first
   arch_fpu_enable_core() (before any AP runs and before any user code). */
static int fpu_probed;
static int fpu_xsave_on;  /* 1 = XSAVE/XRSTOR are the save/restore ops */
static uint64_t fpu_xcr0; /* XCR0 programmed on every core              */
static uint32_t fpu_xsave_bytes = FPU_IMAGE_BYTES;
static uint32_t fpu_mxcsr_mask_v = 0xFFBFu;
static uint32_t fpu_cpuid1_edx;
static uint8_t fpu_probe_buf[FPU_IMAGE_BYTES] __attribute__((aligned(16)));

static void cpuid2(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b,
                   uint32_t *c, uint32_t *d) {
  uint32_t ea = leaf, eb = 0, ec = sub, ed = 0;
  __asm__ volatile("cpuid" : "+a"(ea), "=b"(eb), "+c"(ec), "=d"(ed));
  *a = ea;
  *b = eb;
  *c = ec;
  *d = ed;
}

/* XSETBV/XGETBV: 0F 01 D1 / 0F 01 D0, ECX = register index (0 = XCR0). */
static void xsetbv0(uint64_t v) {
  __asm__ volatile(".byte 0x0f, 0x01, 0xd1"
                   :
                   : "c"(0u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))
                   : "memory");
}

static uint64_t xgetbv0(void) {
  uint32_t lo, hi;
  __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0u));
  return ((uint64_t)hi << 32) | lo;
}

/* The machine decision: XSAVE on iff CPUID.1:ECX.XSAVE[26], leaf 0xD exists,
   and the standard-format area for the XCR0 we would program fits the PCB. */
static void fpu_probe(void) {
  uint32_t a, b, c, d;
  fpu_probed = 1;

  cpuid2(1, 0, &a, &b, &c, &d);
  fpu_cpuid1_edx = d;
  int has_xsave = (int)((c >> 26) & 1u);

  /* MXCSR_MASK (FXSAVE image offset 28; 0 means the architectural 0xFFBF).
     Needed to sanitize MXCSR from a signal frame: FXRSTOR/XRSTOR #GP on a
     reserved MXCSR bit, and that #GP would be in ring 0. */
  for (int i = 0; i < FPU_IMAGE_BYTES; i++)
    fpu_probe_buf[i] = 0;
  __asm__ volatile("fxsave64 (%0)" : : "r"(fpu_probe_buf) : "memory");
  {
    uint32_t m = (uint32_t)fpu_probe_buf[28] |
                 ((uint32_t)fpu_probe_buf[29] << 8) |
                 ((uint32_t)fpu_probe_buf[30] << 16) |
                 ((uint32_t)fpu_probe_buf[31] << 24);
    fpu_mxcsr_mask_v = m ? m : 0xFFBFu;
  }

  uint32_t max_leaf;
  cpuid2(0, 0, &max_leaf, &b, &c, &d);
  if (!has_xsave || max_leaf < 0xD)
    return;

  uint32_t sup_lo, sup_hi, ecx_max;
  cpuid2(0xD, 0, &sup_lo, &b, &ecx_max, &sup_hi);
  uint64_t want = (XFEAT_X87 | XFEAT_SSE | XFEAT_AVX) & (uint64_t)sup_lo;
  if ((want & (XFEAT_X87 | XFEAT_SSE)) != (XFEAT_X87 | XFEAT_SSE))
    return; /* XCR0 must have x87; SSE is a precondition of AVX */

  /* Standard format: legacy 512 + header 64, then each component at its
     CPUID.(0xD,i).EBX offset with size EAX. */
  uint32_t need = 576;
  if (want & XFEAT_AVX) {
    uint32_t sz, off;
    cpuid2(0xD, 2, &sz, &off, &c, &d);
    if (off + sz > need)
      need = off + sz;
  }
  if (need > FPU_PCB_IMAGE_BYTES)
    return;

  fpu_xcr0 = want;
  fpu_xsave_bytes = need;
  fpu_xsave_on = 1;
}

/**
 * Enables x87/SSE (and, when the machine decision says so, XSAVE + AVX
 * state) on the CURRENT core.  CR0, CR4 and XCR0 are all per-core registers,
 * so this must run once on every core before user code executes there (see
 * trap_init_core_with_id); the early boot paths set the CR0/CR4 SSE bits in
 * boot.s long before this.
 *
 *   CR0.EM = 0  no x87 emulation trap (EM=1 makes every x87/SSE op #UD)
 *   CR0.MP = 1  fwait/finit honors pending FP operations
 *   CR0.TS = 0  no lazy FPU switching: this kernel saves/restores the whole
 *               image on every context switch, so the #NM "task switched"
 *               trap is never part of the design.
 *   CR4.OSFXSR     = 1  lets FXSAVE/FXRSTOR run and marks SSE as OS-managed
 *   CR4.OSXMMEXCPT = 1  unmasked SIMD FP exceptions arrive as #XM (vector
 *                       19, which the IDT already routes) instead of #UD
 *   CR4.OSXSAVE    = 1  (XSAVE machines) XSAVE/XRSTOR/XGETBV usable, and the
 *                       CPUID.1:ECX.OSXSAVE bit user code tests reads 1
 *   XCR0 = x87|SSE|AVX  (masked by what the CPU supports)
 */
void arch_fpu_enable_core(void) {
  uint64_t cr0;
  uint64_t cr4;

  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(uint64_t)((1u << 2) | (1u << 3)); /* EM, TS = 0 */
  cr0 |= (uint64_t)(1u << 1);                /* MP = 1     */
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

  /* The machine decision needs FXSAVE (for MXCSR_MASK), so it runs after
     OSFXSR is on; only the boot core's first call makes it. */
  if (!fpu_probed)
    fpu_probe();

  if (fpu_xsave_on) {
    cr4 |= CR4_OSXSAVE;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
  }

  if (fpu_xsave_on) {
    xsetbv0(fpu_xcr0);
    /* After XCR0 is set, CPUID.(0xD,0).EBX is the exact standard-format size
       for it (832 on every x87|SSE|AVX CPU).  Take it when it fits. */
    uint32_t a, b, c, d;
    cpuid2(0xD, 0, &a, &b, &c, &d);
    if (b >= 576 && b <= FPU_PCB_IMAGE_BYTES)
      fpu_xsave_bytes = b;
  }

  {
    static int reported;
    if (!reported) {
      reported = 1;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[FPU] xsave=");
      kl_i(&kl, fpu_xsave_on);
      kl_s(&kl, " xcr0=");
      kl_hex(&kl, (unsigned long)(fpu_xsave_on ? xgetbv0() : 0));
      kl_s(&kl, " size=");
      kl_i(&kl, (long)fpu_xsave_bytes);
      kl_s(&kl, " mxcsr_mask=");
      kl_hex(&kl, (unsigned long)fpu_mxcsr_mask_v);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
}

int arch_fpu_xsave_enabled(void) { return fpu_xsave_on; }
uint64_t arch_fpu_xcr0(void) { return fpu_xsave_on ? fpu_xcr0 : 0; }
uint32_t arch_fpu_xsave_size(void) { return fpu_xsave_bytes; }
uint32_t arch_fpu_mxcsr_mask(void) { return fpu_mxcsr_mask_v; }

uint64_t arch_linux_hwcap(void) {
  if (!fpu_probed)
    fpu_probe();
  return (uint64_t)fpu_cpuid1_edx;
}

/**
 * Initializes a fresh process's FP image: zeroed, then stamped with the
 * architectural default control words.  The SAME bytes are a valid FXSAVE64
 * image and a valid XSAVE standard-format image, so the reset does not depend
 * on the machine decision:
 *
 *   FCW   = 0x037F   all x87 exceptions masked, extended precision
 *   FTW   = 0x00     abridged tag word: every x87 register EMPTY.  In the
 *                    FXSAVE format a 1 bit means VALID, so the 0xFF this
 *                    used to store declared all eight registers full and the
 *                    first x87 push of a fresh process overflowed the stack
 *                    (Linux's fpstate_init_fxstate leaves it 0).
 *   MXCSR = 0x1F80   all SIMD exceptions masked, round-to-nearest
 *   XSTATE_BV = x87|SSE  XRSTOR loads those two from the legacy region; AVX
 *                    (bit 2 clear) comes up in its init state (all zero)
 *
 * An all-zero image is NOT a usable default: MXCSR=0 leaves all six SSE
 * exception masks clear, so the first inexact result (e.g. 2.0/7.0) would
 * raise #XM and kill the process, and FCW=0 leaves the x87 exceptions
 * unmasked the same way.  FXRSTOR ignores bytes 512+; XRSTOR requires the
 * rest of the header (XCOMP_BV and the reserved bytes) to be zero.
 */
void arch_fpu_reset(struct process *p) {
  uint8_t *img = (uint8_t *)p->fpu_state;

  for (int i = 0; i < FPU_PCB_IMAGE_BYTES; i++)
    img[i] = 0;

  img[FPU_IMG_FCW] = 0x7F; /* FCW = 0x037F                              */
  img[FPU_IMG_FCW + 1] = 0x03;
  img[FPU_IMG_FTW] = 0x00; /* FTW = all x87 registers empty (abridged)  */
  img[FPU_IMG_MXCSR] = 0x80; /* MXCSR = 0x1F80                          */
  img[FPU_IMG_MXCSR + 1] = 0x1F;
  img[FPU_IMG_XSTATE_BV] = (uint8_t)(XFEAT_X87 | XFEAT_SSE);
}

/**
 * Saves the live x87+SSE register file into an arbitrary 16-byte-aligned
 * destination (always the 512-byte FXSAVE64 image, in both modes).  P5 S3
 * (D8.7): the native signal frame carries this image.
 */
void arch_fpu_save_to(void *dst) {
  __asm__ volatile("fxsave64 (%0)" : : "r"(dst) : "memory");
}

/**
 * Loads an x87+SSE register file from an arbitrary 16-byte-aligned source
 * (P5 S3: SIGRETURN restores from the native signal frame).  With XSAVE on,
 * FXRSTOR touches x87/SSE only and leaves the AVX upper halves as they are.
 */
void arch_fpu_restore_from(const void *src) {
  __asm__ volatile("fxrstor64 (%0)" : : "r"(src) : "memory");
}

/* XSAVE64 (%rdi) = 48 0F AE 27; XRSTOR64 (%rdi) = 48 0F AE 2F.
   EDX:EAX = requested-feature bitmap; all ones means "everything in XCR0". */
void arch_fpu_xsave_to(void *dst64) {
  __asm__ volatile(".byte 0x48, 0x0f, 0xae, 0x27"
                   :
                   : "D"(dst64), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu)
                   : "memory");
}

void arch_fpu_xrstor_from(const void *src64) {
  __asm__ volatile(".byte 0x48, 0x0f, 0xae, 0x2f"
                   :
                   : "D"(src64), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu)
                   : "memory");
}

/**
 * Saves the live register file into a PCB: XSAVE when the machine has it
 * (x87, SSE and the AVX upper halves), FXSAVE64 otherwise.
 */
void arch_fpu_save(struct process *p) {
  if (fpu_xsave_on)
    arch_fpu_xsave_to(p->fpu_state);
  else
    arch_fpu_save_to(p->fpu_state);
}

/**
 * Loads a process's register file from its PCB image.
 */
void arch_fpu_restore(struct process *p) {
  if (fpu_xsave_on)
    arch_fpu_xrstor_from(p->fpu_state);
  else
    arch_fpu_restore_from(p->fpu_state);
}
