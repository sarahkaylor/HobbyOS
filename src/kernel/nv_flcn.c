/* ============================================================================
 *  nv_flcn: pure encoders for the falcon firmware path (see nv_flcn.h).
 *
 *  No MMIO and no architecture-specific code, so this translation unit
 *  compiles and runs identically on both ports and is driven by the kernel
 *  unit tier against byte buffers built in the test.
 * ========================================================================== */

#include "nv_flcn.h"
#include "errno.h"

/* ---- little-endian byte access ----------------------------------------
 * The DMEM image is a byte buffer that may sit at any offset inside the
 * caller's frames, so multi-byte fields are assembled and stored byte-wise.
 * Reading a packed struct field here would be an alignment fault on the
 * Device-typed mappings this kernel runs on (src/include/nv.h's contract), and
 * it costs nothing to do it in the one shape that is always safe. */

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

/* ---- 1. the XFER command word ----------------------------------------- */

uint32_t nv_flcn_xfer_cmd(uint32_t bytes, int is_imem, int sec) {
  if (bytes < 4u || bytes > NV_FLCN_BLOCK) return 0xffffffffu;
  if (bytes & (bytes - 1u)) return 0xffffffffu; /* not a power of two */
  uint32_t sh = 0;
  while ((1u << sh) < bytes) sh++;
  uint32_t cmd = ((sh - 2u) & 0x7u) << 8;
  if (is_imem) cmd |= 1u << 4;
  cmd |= ((uint32_t)sec & 0x3u) << 2;
  return cmd;
}

/* ---- 2. signature selection ------------------------------------------- */

uint32_t nv_fwsec_sig_index(uint32_t fuse_reg, uint32_t sigvers,
                            uint32_t sigcount) {
  if (fuse_reg == 0 || sigvers == 0 || sigcount == 0) return NV_FWSEC_SIG_NONE;
  /* The fuse word is a 1-based version number, and the descriptor offers a
   * bitmask of versions: the version the part reports is selected by the bit
   * above the fuse word's own top set bit.  (Rule re-derived from the
   * reference launch code's arithmetic -- docs/nv-pmu-acr-contract.md §1.6
   * item 10 -- and stated here in one place so the unit tier can pin it.) */
  uint32_t top = 0;
  int found = 0;
  for (uint32_t i = 0; i < 31; i++) {
    if (fuse_reg & (1u << i)) {
      top = i;
      found = 1;
    }
  }
  if (!found || top >= 31) return NV_FWSEC_SIG_NONE;
  uint32_t mask = 1u << (top + 1u);
  if (!(mask & sigvers)) return NV_FWSEC_SIG_NONE;
  /* Which signature index carries that version: walk both masks from the LSB,
   * counting the versions the descriptor offers below the selected one. */
  uint32_t idx = 0;
  uint32_t m = mask, s = sigvers;
  while (!((m & s) & 1u)) {
    idx += (s & 1u);
    m >>= 1;
    s >>= 1;
    if (idx >= 32u) return NV_FWSEC_SIG_NONE;
  }
  if (idx >= sigcount) return NV_FWSEC_SIG_NONE;
  return idx;
}

/* ---- 3. falcon memory addresses --------------------------------------- */

uint32_t nv_flcn_idx_word(uint32_t byte_addr) {
  /* Aligned, and inside what the 14-bit word-address field can express.  The
   * mask used to be applied silently (0x3fff after the shift), so a request
   * for DMEM 0x10000 read DMEM 0 and answered with real data -- undetectable
   * from the value alone.  Refusing is the only honest answer: a caller that
   * asked for memory the port cannot address has a bug, not a result. */
  if (byte_addr & 3u) return NV_FLCN_IDX_NONE;
  if (byte_addr > (0x3fffu << 2)) return NV_FLCN_IDX_NONE;
  return byte_addr >> 2;
}

uint32_t nv_flcn_find_word(const uint8_t *img, uint32_t len, uint32_t val) {
  if (!img) return NV_FLCN_IDX_NONE;
  for (uint32_t off = 0; off + 4u <= len; off += 4u) {
    if (get32(img + off) == val) return off;
  }
  return NV_FLCN_IDX_NONE;
}

int nv_flcn_compare_words(const uint32_t *words, uint32_t n, const uint8_t *img,
                          uint32_t img_len, uint32_t img_off,
                          struct nv_flcn_mismatch *out) {
  if (!words || !img || n == 0u) return -EINVAL;
  if (img_off + n * 4u < img_off) return -ERANGE; /* overflow */
  if (img_off + n * 4u > img_len) return -ERANGE;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t want = get32(img + img_off + i * 4u);
    if (words[i] != want) {
      if (out) {
        out->index = i;
        out->off = img_off + i * 4u;
        out->got = words[i];
        out->want = want;
      }
      return 1;
    }
  }
  return 0;
}

/* ---- 4. the FRTS command ---------------------------------------------- */

#define APPIF_ID_DMEMMAPPER 4u
#define DMAP_SIGNATURE 0x50414D44u /* "DMAP" (little-endian bytes 44 4d 41 50) */
#define DMAP_VERSION 3u
#define DMAP_SIZE 0x40u
#define DMAP_CMD_IN_OFF 0x08u
#define DMAP_CMD_IN_SIZE 0x0Cu
#define DMAP_INIT_CMD 0x2Cu

/* Walk the appif header at `iface_off` and return the DMEM offset of the
 * DMEMMAPPER block, or 0xFFFFFFFFu.  Bounds are checked against `dmem_len` at
 * every hop, because the offsets come out of the image itself. */
static uint32_t find_dmemmapper(const uint8_t *dmem, uint32_t dmem_len,
                                uint32_t iface_off) {
  if (iface_off + 4u > dmem_len) return 0xffffffffu;
  uint32_t ver = dmem[iface_off];
  uint32_t hdr = dmem[iface_off + 1];
  uint32_t elen = dmem[iface_off + 2];
  uint32_t cnt = dmem[iface_off + 3];
  /* ver 1 is the only appif version documented for this firmware; a
   * different one means the offsets below do not describe this image. */
  if (ver != 1u || hdr < 4u || elen < 8u || cnt == 0u || cnt > 32u)
    return 0xffffffffu;
  for (uint32_t i = 0; i < cnt; i++) {
    uint32_t e = iface_off + hdr + i * elen;
    if (e + 8u > dmem_len) return 0xffffffffu;
    if (get32(dmem + e) != APPIF_ID_DMEMMAPPER) continue;
    uint32_t base = get32(dmem + e + 4);
    if (base + DMAP_SIZE > dmem_len) return 0xffffffffu;
    return base;
  }
  return 0xffffffffu;
}

uint32_t nv_fwsec_cmd_buffer(const uint8_t *dmem, uint32_t dmem_len,
                             uint32_t iface_off) {
  if (!dmem || !dmem_len) return 0xffffffffu;
  uint32_t base = find_dmemmapper(dmem, dmem_len, iface_off);
  if (base == 0xffffffffu) return 0xffffffffu;
  if (get32(dmem + base) != DMAP_SIGNATURE) return 0xffffffffu;
  uint32_t ver = (uint32_t)dmem[base + 4] | ((uint32_t)dmem[base + 5] << 8);
  if (ver != DMAP_VERSION) return 0xffffffffu;
  uint32_t off = get32(dmem + base + DMAP_CMD_IN_OFF);
  uint32_t size = get32(dmem + base + DMAP_CMD_IN_SIZE);
  if (size < NV_FWSEC_FRTS_CMD_BYTES) return 0xffffffffu;
  if (off + NV_FWSEC_FRTS_CMD_BYTES > dmem_len) return 0xffffffffu;
  return off;
}

int nv_fwsec_read_cmd(const uint8_t *dmem, uint32_t dmem_len, uint32_t iface_off,
                      uint32_t *init_cmd, uint32_t *words, uint32_t nwords) {
  if (!dmem || !dmem_len) return -EINVAL;
  uint32_t base = find_dmemmapper(dmem, dmem_len, iface_off);
  if (base == 0xffffffffu) return -EINVAL;
  uint32_t cmd_off = get32(dmem + base + DMAP_CMD_IN_OFF);
  uint32_t cmd_size = get32(dmem + base + DMAP_CMD_IN_SIZE);
  if (init_cmd) *init_cmd = get32(dmem + base + DMAP_INIT_CMD);
  if (words && nwords) {
    if (nwords > 11u) nwords = 11u;
    if (cmd_size < nwords * 4u) return -ERANGE;
    if (cmd_off + nwords * 4u > dmem_len) return -ERANGE;
    for (uint32_t i = 0; i < nwords; i++) words[i] = get32(dmem + cmd_off + i * 4u);
  }
  return 0;
}

int nv_fwsec_patch_frts(uint8_t *dmem, uint32_t dmem_len, uint32_t iface_off,
                        uint32_t frts_addr_pages, uint32_t frts_size_pages) {
  if (!dmem || !dmem_len) return -EINVAL;
  uint32_t base = find_dmemmapper(dmem, dmem_len, iface_off);
  if (base == 0xffffffffu) return -EINVAL;
  if (get32(dmem + base) != DMAP_SIGNATURE) return -EINVAL;
  uint32_t ver = (uint32_t)dmem[base + 4] | ((uint32_t)dmem[base + 5] << 8);
  if (ver != DMAP_VERSION) return -EINVAL;
  uint32_t cmd_off = get32(dmem + base + DMAP_CMD_IN_OFF);
  uint32_t cmd_size = get32(dmem + base + DMAP_CMD_IN_SIZE);
  if (cmd_size < NV_FWSEC_FRTS_CMD_BYTES) return -ERANGE;
  if (cmd_off + NV_FWSEC_FRTS_CMD_BYTES > dmem_len) return -ERANGE;

  /* Select the command first, then fill it: a ucode that fetched the command
   * word between the two writes would otherwise run an empty FRTS. */
  uint8_t *p = dmem + cmd_off;
  put32(p + 0x00, 1);  /* read_vbios.ver   */
  put32(p + 0x04, 24); /* read_vbios.hdr   = sizeof(read_vbios) */
  put64(p + 0x08, 0);  /* read_vbios.addr  */
  put32(p + 0x10, 0);  /* read_vbios.size  */
  put32(p + 0x14, 2);  /* read_vbios.flags */
  put32(p + 0x18, 1);  /* frts_region.ver  */
  put32(p + 0x1C, 20); /* frts_region.hdr  = sizeof(frts_region) */
  put32(p + 0x20, frts_addr_pages);
  put32(p + 0x24, frts_size_pages);
  put32(p + 0x28, 2);  /* frts_region.type = FB */
  put32(dmem + base + DMAP_INIT_CMD, NV_FWSEC_CMD_FRTS);
  return 0;
}
