/* src/include/linux_sigframe.h — F2 (docs/cyberpunk.md §7.3): the x86_64
 * Linux signal frame's FP/XSAVE state, and the arithmetic this kernel uses to
 * place it.
 *
 * Two halves, both host-tested by src/host/linux_sigframe_test.c:
 *
 *  1. Linux UAPI constants and offsets (arch/x86/include/uapi/asm/sigcontext.h,
 *     uapi/asm/ucontext.h, uapi/linux/auxvec.h).  They are asserted against
 *     tests/fixtures/abi/linux_sigframe.golden, which is MEASURED on a real
 *     Linux x86_64 box from those headers (tools/linux-abi/gen_sigframe_golden.py),
 *     so a number typed from memory cannot survive.  Wine's ntdll parses this
 *     shape (it finds the XSAVE header via fpregs + 512 once sw_reserved carries
 *     FP_XSTATE_MAGIC1), so it is a frozen interface: change it only with the
 *     golden regenerated and C2 (Wine) sign-off.
 *
 *         fpstate (= uc_mcontext.fpregs, 64-byte aligned)
 *         +0    struct _fpstate_64 (512)  -- the FXSAVE legacy region
 *               +464 sw_reserved = struct _fpx_sw_bytes (48):
 *                    +0  magic1        FP_XSTATE_MAGIC1 0x46505853
 *                    +4  extended_size xstate_size + FP_XSTATE_MAGIC2_SIZE
 *                    +8  xfeatures     the XCR0 bits present (x87|SSE|AVX)
 *                    +16 xstate_size   bytes of the XSAVE image
 *                    +20 padding[7]    zero
 *         +512  struct _header (64)       -- XSAVE header (XSTATE_BV @+0)
 *         +576  struct _ymmh_state (256)  -- AVX: upper 128 bits of ymm0..15
 *         +xstate_size  FP_XSTATE_MAGIC2 0x46505845 (4 bytes)
 *
 *     uc_flags carries UC_FP_XSTATE when the extended state is present.  A CPU
 *     without XSAVE gets the bare 512-byte image and no magic (Linux's
 *     fx_only frame), and rt_sigreturn treats any frame whose magic does not
 *     validate the same way.
 *
 *  2. This kernel's placement arithmetic (lsf_*), pure functions so the host
 *     test can sweep bases and image sizes.  The rt_sigframe header (pretcode,
 *     the glibc-sized ucontext, siginfo) occupies [base, base+568); the
 *     fpstate starts at the first 64-byte boundary at or above base+568 (XSAVE
 *     #GPs on anything less than 64-byte alignment, and Linux aligns the
 *     fpstate to 64 in every mode); base is 8 (mod 16), the psABI handler-entry
 *     alignment (see linux_compat_thr.c's layout note).
 */
#ifndef HOBBYOS_LINUX_SIGFRAME_H
#define HOBBYOS_LINUX_SIGFRAME_H

#include <stdint.h>

/* ---- Linux UAPI (golden-locked) ---------------------------------------- */
#define LSF_FPSTATE_BYTES 512u        /* sizeof(struct _fpstate_64)          */
#define LSF_FP_MXCSR_OFF 24u          /* _fpstate_64.mxcsr                   */
#define LSF_FP_MXCSR_MASK_OFF 28u     /* _fpstate_64.mxcsr_mask              */
#define LSF_FP_SW_RESERVED_OFF 464u   /* _fpstate_64.sw_reserved             */
#define LSF_SW_BYTES 48u              /* sizeof(struct _fpx_sw_bytes)        */
#define LSF_SW_MAGIC1_OFF 0u          /* _fpx_sw_bytes.magic1                */
#define LSF_SW_EXTENDED_SIZE_OFF 4u   /* _fpx_sw_bytes.extended_size         */
#define LSF_SW_XFEATURES_OFF 8u       /* _fpx_sw_bytes.xfeatures             */
#define LSF_SW_XSTATE_SIZE_OFF 16u    /* _fpx_sw_bytes.xstate_size           */
#define LSF_SW_PADDING_OFF 20u        /* _fpx_sw_bytes.padding               */
#define LSF_XSTATE_HDR_OFF 512u       /* _xstate.xstate_hdr                  */
#define LSF_XSTATE_HDR_BYTES 64u      /* sizeof(struct _header)              */
#define LSF_XSTATE_YMMH_OFF 576u      /* _xstate.ymmh                        */
#define LSF_XSTATE_YMMH_BYTES 256u    /* sizeof(struct _ymmh_state)          */
#define LSF_XSTATE_BYTES 832u         /* sizeof(struct _xstate): x87|SSE|AVX */
#define LSF_FP_XSTATE_MAGIC1 0x46505853u
#define LSF_FP_XSTATE_MAGIC2 0x46505845u
#define LSF_FP_XSTATE_MAGIC2_SIZE 4u
#define LSF_UC_FP_XSTATE 0x1u         /* uc_flags bit                        */
#define LSF_AT_HWCAP2 26u
#define LSF_AT_MINSIGSTKSZ 51u

/* XSAVE state-component bits this kernel may enable (AD-4).  Anything else
   (AVX-512 opmask/ZMM, PKRU, AMX, ...) is never set in XCR0. */
#define LSF_XFEATURE_X87 0x1ull
#define LSF_XFEATURE_SSE 0x2ull
#define LSF_XFEATURE_AVX 0x4ull
#define LSF_XFEATURES_ALLOWED (LSF_XFEATURE_X87 | LSF_XFEATURE_SSE | LSF_XFEATURE_AVX)

/* ---- this kernel's frame arithmetic ------------------------------------ */

/* pretcode (8) + glibc-sized ucontext header (424) + siginfo (128) = 560,
   rounded so that base (8 mod 16) + 568 is 16-aligned. */
#define LSF_HDR_BYTES 568u

/* Bytes the FP image occupies at the fpstate pointer, given the XSAVE area
   size (0 = no XSAVE: the bare FXSAVE image). */
static inline uint32_t lsf_image_bytes(uint32_t xsave_size) {
  return xsave_size ? xsave_size + LSF_FP_XSTATE_MAGIC2_SIZE : LSF_FPSTATE_BYTES;
}

/* Whole-frame reservation for an image of `img` bytes: the header, the worst
   case 63 bytes of 64-byte alignment padding, the image; a multiple of 16. */
static inline uint32_t lsf_frame_bytes(uint32_t img) {
  return (LSF_HDR_BYTES + 63u + img + 15u) & ~15u;
}

/* The frame base for a delivery whose usable stack top is `sp` (already below
   the red zone, or the altstack top): 8 (mod 16), and base+frame <= sp-8. */
static inline uint64_t lsf_frame_base(uint64_t sp, uint32_t img) {
  return ((sp - (uint64_t)lsf_frame_bytes(img)) & ~(uint64_t)15) - 8;
}

/* Offset of the fpstate from the frame base: the first 64-byte-aligned
   ADDRESS at or above base+LSF_HDR_BYTES. */
static inline uint32_t lsf_fp_off(uint64_t base) {
  uint64_t fp = (base + (uint64_t)LSF_HDR_BYTES + 63u) & ~(uint64_t)63u;
  return (uint32_t)(fp - base);
}

/* AT_MINSIGSTKSZ, and the size an altstack must have before the kernel will
   deliver on it: the frame plus the 8-24 bytes of base alignment, with margin. */
static inline uint32_t lsf_min_sigstksz(uint32_t img) {
  return lsf_frame_bytes(img) + 64u;
}

#endif /* HOBBYOS_LINUX_SIGFRAME_H */
