#ifndef NV_FMC_ARGS_H
#define NV_FMC_ARGS_H

#include <stdint.h>

/* ============================================================================
 *  nv_fmc_args -- the GSP-FMC boot-parameters page, in the vendor's offsets.
 *
 *  A6 of docs/gb202-vendor-diff.md section 5 (D4): the two DMA-target words the
 *  vendor sends are not the same number.
 *
 *    +8   bootGspRmParams.target  -- the WPR-meta descriptor page
 *    +40  gspRmParams.target      -- the libos init-args page
 *
 *  The vendor derives BOTH from the buffer's cache attribute, not from a mode:
 *  _kgspMemdescToDmaTarget (kernel_gsp_gh100.c:390-405) answers
 *      ADDR_SYSMEM + NV_MEMORY_CACHED   -> GSP_DMA_TARGET_COHERENT_SYSTEM   (1)
 *      ADDR_SYSMEM + anything else      -> GSP_DMA_TARGET_NONCOHERENT_SYSTEM (2)
 *  and the two pages are allocated with different attributes:
 *      pWprMetaDescriptor            NV_MEMORY_CACHED    kernel_gsp_tu102.c:121
 *      pLibosInitArgumentsDescriptor NV_MEMORY_UNCACHED  kernel_gsp_tu102.c:148
 *  so the vendor's own pair is +8 = 1 and +40 = 2 (kernel_gsp_gh100.c:453-461
 *  writes exactly these two fields from those two calls).  Ours sent 1 for both
 *  except in the mode-7/67/50 probes, which is the divergence A6 closes.
 *
 *  "Make the buffer's cache attribute match" is therefore code, not prose: the
 *  two target values below are a function of the attribute, so a page that
 *  claims a different pairing cannot be produced by this file.  On this
 *  platform the host's own mapping stays coherent-by-construction (snooped
 *  sysmem, src/include/nv_dma.h "COHERENCE"); the attribute here is the one the
 *  FMC is told to use when it dereferences the page.
 *
 *  Offsets are the vendor's compiled struct, not re-derived: GSP_FMC_BOOT_PARAMS
 *  (gspifpub.h:113-120) with GSP_ACR_BOOT_GSP_RM_PARAMS (:59-75),
 *  GSP_RM_PARAMS (:79-87), GSP_SPDM_PARAMS (:90-102) and GSP_RM_MEM_PARAMS
 *  (:104-109); measured sizeof = 88 at this ABI.
 * ========================================================================== */

#define NV_FMC_ARGS_BYTES              88u
#define NV_FMC_ARGS_OFF_REGKEYS        0u
#define NV_FMC_ARGS_OFF_DESC_TARGET    8u
#define NV_FMC_ARGS_OFF_DESC_SIZE      12u
#define NV_FMC_ARGS_OFF_DESC_OFFSET    16u
#define NV_FMC_ARGS_OFF_CARVEOUT_OFF   24u
#define NV_FMC_ARGS_OFF_CARVEOUT_SIZE  32u
#define NV_FMC_ARGS_OFF_IS_RM_BOOT     36u
#define NV_FMC_ARGS_OFF_ARGS_TARGET    40u
#define NV_FMC_ARGS_OFF_ARGS_OFFSET    48u
#define NV_FMC_ARGS_OFF_SPDM_TARGET    56u
#define NV_FMC_ARGS_OFF_SPDM_BUFFER    64u
#define NV_FMC_ARGS_OFF_SPDM_LENGTH    72u
#define NV_FMC_ARGS_OFF_FLUSH_LO       80u

/* GSP_DMA_TARGET (gspifpub.h:39-44). */
#define NV_FMC_TARGET_LOCAL_FB             0u
#define NV_FMC_TARGET_COHERENT_SYSTEM      1u
#define NV_FMC_TARGET_NONCOHERENT_SYSTEM   2u

/* The buffer's cache attribute, the input the vendor maps to a target. */
enum nv_fmc_cache_attr {
  NV_FMC_CACHE_CACHED = 0,   /* NV_MEMORY_CACHED   */
  NV_FMC_CACHE_UNCACHED = 1, /* NV_MEMORY_UNCACHED */
};

/* _kgspMemdescToDmaTarget for an ADDR_SYSMEM page (kernel_gsp_gh100.c:390-405).
 * 0 for an attribute this file does not know: the caller must not send it. */
uint32_t nv_fmc_args_dma_target(int cache_attr);

/* Write the vendor's pair into a GSP_FMC_BOOT_PARAMS page: +8 from
 * NV_FMC_CACHE_CACHED (1) and +40 from NV_FMC_CACHE_UNCACHED (2).  Every other
 * byte is left alone -- the caller owns the rest of the page.
 *   -EINVAL  NULL, or a page shorter than NV_FMC_ARGS_BYTES. */
int nv_fmc_args_targets_encode(uint8_t *page, uint64_t page_len);

/* Read the pair back out of the page's own bytes and compare with the vendor's
 * values.  This is what the boot row and the unit case assert:
 *   0     +8 == 1 and +40 == 2
 *   -EIO  anything else (including the old 1/1 default A6 removes)
 *   -EINVAL NULL/short page. */
int nv_fmc_args_targets_check(const uint8_t *page, uint64_t page_len);

#endif /* NV_FMC_ARGS_H */
