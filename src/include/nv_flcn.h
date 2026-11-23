#ifndef NV_FLCN_H
#define NV_FLCN_H

/* ============================================================================
 *  nv_flcn: the falcon firmware path as pure functions.
 *
 *  Three things the launch gets wrong if they are guessed, and none of which
 *  needs an MMIO backend to get right:
 *
 *    1. the XFER command word (block size, code/data, secure) -- a wrong SIZE
 *       field asks the engine to move 4 bytes instead of 256 and looks like
 *       "the DMA was accepted but nothing ran";
 *    2. which of the descriptor's signatures to copy into DMEM -- chosen by the
 *       part's fused key version;
 *    3. the FRTS command inside the staged DMEM image -- the ucode's whole
 *       purpose, and an unpatched image makes a successful-looking launch do
 *       nothing.
 *
 *  Everything here is a function of its arguments: no MMIO, no globals, no
 *  allocation, and no architecture-specific code.  That is deliberate: the
 *  kernel unit tier drives the same functions the boot path calls, and the ARM
 *  build compiles them too, so a mistake shows up in either without a card.
 *
 *  Field layouts are taken from docs/nv-pmu-acr-contract.md (descriptor),
 *  docs/nv-gsp-wpr-contract.md (DMEMMAPPER/appif walking) and the byte dump of
 *  this card's own VBIOS (docs/nv-vbios-evidence/), all cited at the point of
 *  use.  No driver code was copied; the encodings are re-derived here.
 * ========================================================================== */

#include <stdint.h>

/* One falcon DMA request moves exactly this many bytes on this generation:
 * the image sizes are multiples of it (0xF200 and 0xD80 both are) and it is
 * the largest size the SIZE field can express. */
#define NV_FLCN_BLOCK 256u

/* The XFER command word (falcon 0x118) for one block.
 *
 *   bits 10:8  SIZE   = ilog2(bytes) - 2   (4 B = 0 ... 256 B = 6)
 *   bit  4     IMEM   (0 = DMEM/DATA, 1 = IMEM/CODE)
 *   bits 3:2   SEC    (secure-memory segment)
 *
 * The SIZE encoding is the one fact that is easy to get wrong twice: writing
 * "8" for 256 bytes masks to 0, which is a 4-byte transfer, and the engine
 * accepts it.  Returns 0xFFFFFFFFu for a size the field cannot express
 * (not a power of two, or outside 4..256) so a caller fails loudly.
 */
uint32_t nv_flcn_xfer_cmd(uint32_t bytes, int is_imem, int sec);

/* ---- descriptor signature selection -------------------------------------
 *
 * The descriptor carries `sigcount` 384-byte PKC signatures (V3: at
 * desc+0x2C + i*384) and a bitmask of the key versions it covers
 * (`SignatureVersions`).  The part's fused word at 0x8241C0 + (UcodeId-1)*4
 * says which version it will accept.  The Boot ROM verifies the signature the
 * host put in DMEM, so picking the wrong one is rejected with no host-visible
 * reason.
 *
 * Returns the index to copy, or NV_FWSEC_SIG_NONE when there is no version the
 * fuse and the descriptor agree on (in which case the image is invalid for
 * this part and nothing else is worth trying).
 */
#define NV_FWSEC_SIG_NONE 0xffffffffu
uint32_t nv_fwsec_sig_index(uint32_t fuse_reg, uint32_t sigvers,
                            uint32_t sigcount);

/* ---- the FRTS command --------------------------------------------------
 *
 * FWSEC's payload is a small operating system; the host hands it one command
 * through the DMEMMAPPER application interface inside the DMEM image:
 *
 *   dmem + InterfaceOffset      appif header {ver=1, hdr, entry_len, cnt}
 *   ... + hdr + i*entry_len     entries {id, dmem_base}; id 4 = DMEMMAPPER
 *   dmem + dmem_base            the DMEMMAPPER block: signature "DMAP",
 *                               version 3, size 0x40;
 *                                 +0x08 cmd_in_buffer_offset
 *                                 +0x0C cmd_in_buffer_size
 *                                 +0x2C init_cmd   (0x15 = FRTS)
 *   dmem + cmd_in_buffer_offset the 44-byte command itself:
 *                                 +0x00 read_vbios.ver    = 1
 *                                 +0x04 read_vbios.hdr    = 24 (its own size)
 *                                 +0x08 read_vbios.addr   = 0
 *                                 +0x10 read_vbios.size   = 0
 *                                 +0x14 read_vbios.flags  = 2
 *                                 +0x18 frts_region.ver   = 1
 *                                 +0x1C frts_region.hdr   = 20
 *                                 +0x20 frts_region.addr  = frtsOffset >> 12
 *                                 +0x24 frts_region.size  = frtsSize   >> 12
 *                                 +0x28 frts_region.type  = 2 (FB)
 *
 * The address and size are in 4 KiB pages -- the only place in this chain that
 * is not bytes.  `frts_addr_pages`/`frts_size_pages` are therefore already
 * shifted by the caller, and the comment above says so because a shift applied
 * twice is indistinguishable from a wrong region until the boot fails.
 *
 * Returns 0, -EINVAL when the image has no appif/DMEMMAPPER block where the
 * descriptor says it should, -ERANGE when the command buffer is too small for
 * the 44 bytes, so a caller can distinguish "not this image" from "this image
 * but no room".
 */
#define NV_FWSEC_CMD_FRTS 0x15u
#define NV_FWSEC_FRTS_CMD_BYTES 44u
int nv_fwsec_patch_frts(uint8_t *dmem, uint32_t dmem_len, uint32_t iface_off,
                        uint32_t frts_addr_pages, uint32_t frts_size_pages);

/* Read back what the patch left in the image: the command selector
 * (`init_cmd`) and up to 11 dwords of the FRTS structure itself.  The boot path
 * logs these instead of re-deriving them, so the evidence is the staged bytes.
 * Returns 0, or -EINVAL/-ERANGE when the image has no DMEMMAPPER block or no
 * room for the requested words. */
int nv_fwsec_read_cmd(const uint8_t *dmem, uint32_t dmem_len, uint32_t iface_off,
                      uint32_t *init_cmd, uint32_t *words, uint32_t nwords);

/* Where the FRTS command goes, without touching the image.  Returns the DMEM
 * offset of the command buffer, or 0xFFFFFFFFu when there is none -- used by
 * the log row that reports what was patched and by the unit tier. */
uint32_t nv_fwsec_cmd_buffer(const uint8_t *dmem, uint32_t dmem_len,
                             uint32_t iface_off);

/* Kernel unit tier (nv_flcn_test.c). */
void nv_flcn_test_suite(void);

#endif /* NV_FLCN_H */
