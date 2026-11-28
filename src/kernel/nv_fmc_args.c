/* nv_fmc_args -- see src/include/nv_fmc_args.h for the vendor offsets, the
 * two allocations and the citation for each.  Pure memory work. */

#include "nv_fmc_args.h"

#include "errno.h"

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

uint32_t nv_fmc_args_dma_target(int cache_attr) {
  switch (cache_attr) {
    case NV_FMC_CACHE_CACHED:
      return NV_FMC_TARGET_COHERENT_SYSTEM;
    case NV_FMC_CACHE_UNCACHED:
      return NV_FMC_TARGET_NONCOHERENT_SYSTEM;
    default:
      return 0; /* not a GSP_DMA_TARGET: never send it */
  }
}

int nv_fmc_args_targets_encode(uint8_t *page, uint64_t page_len) {
  if (!page || page_len < NV_FMC_ARGS_BYTES) return -EINVAL;
  uint32_t desc = nv_fmc_args_dma_target(NV_FMC_CACHE_CACHED);
  uint32_t args = nv_fmc_args_dma_target(NV_FMC_CACHE_UNCACHED);
  if (desc == 0 || args == 0) return -EINVAL;
  put32(page + NV_FMC_ARGS_OFF_DESC_TARGET, desc);
  put32(page + NV_FMC_ARGS_OFF_ARGS_TARGET, args);
  return 0;
}

int nv_fmc_args_targets_check(const uint8_t *page, uint64_t page_len) {
  if (!page || page_len < NV_FMC_ARGS_BYTES) return -EINVAL;
  uint32_t desc = get32(page + NV_FMC_ARGS_OFF_DESC_TARGET);
  uint32_t args = get32(page + NV_FMC_ARGS_OFF_ARGS_TARGET);
  if (desc != nv_fmc_args_dma_target(NV_FMC_CACHE_CACHED)) return -EIO;
  if (args != nv_fmc_args_dma_target(NV_FMC_CACHE_UNCACHED)) return -EIO;
  return 0;
}
