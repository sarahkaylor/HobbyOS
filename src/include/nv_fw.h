#ifndef NV_FW_H
#define NV_FW_H

/* ============================================================================
 *  nv_fw: the GSP firmware blobs, as bytes.
 *
 *  Everything in this header is a pure function over a caller-supplied
 *  buffer: no MMIO, no kernel globals, no allocation (except nv_fw_load*,
 *  which is the one entry that touches the filesystem and is x86_64-only).
 *  That is what lets the whole of docs/nv-gsp-firmware.md §3 be checked in
 *  the kernel unit tier, on the same code the boot path runs.
 *
 *  Three formats, in the order the boot uses them:
 *
 *    1. nv_fw_container  the vendor-tagged wrapper around booter_load /
 *                        booter_unload / bootloader: magic 0x000010DE,
 *                        version 1, then (hdr_off, data_off, data_size).
 *    2. nv_fw_hs         the 9-dword heavy-secure descriptor header inside
 *                        that wrapper, which names the production signature,
 *                        the patch site, the device mask and the 9-dword
 *                        load header.  The load header's sizes summing to
 *                        data_size is the assertion that catches "this is a
 *                        booter for a different chip than the one you built
 *                        the load layout for".
 *    3. nv_fw_elf        the RISC-V ET_REL RM image (gsp-<ver>.bin): a map of
 *                        .fwimage / .fwversion / .fwsignature_<engine>.
 *                        Nothing is relocated or loaded here -- the booter
 *                        does that in secure mode, against keys fused into
 *                        the GPU (docs/nv-gsp-firmware.md §7).
 *
 *  Every offset stored in these structures is a byte offset into the buffer
 *  the caller passed in, exactly as it appears in the file.  Nothing is
 *  rebased: a struct nv_fw_container carries pointers for convenience, but
 *  the numbers it reports are the numbers on disk, so a log row can be
 *  compared against `xxd` output without arithmetic.
 *
 *  All fields are assembled byte by byte (see the LE helpers in nv_fw.c).
 *  Packed-struct field reads would be an alignment fault on the Device-typed
 *  mappings the kernel runs on, and this code is expected to parse a DMA
 *  buffer as readily as a file image.
 * ========================================================================== */

#include <stdint.h>

/* ---- 1. the vendor-tagged container ------------------------------------ */

/* Measured on the 535.113.01 booters (both directories):
 *   +0x00 magic   0x000010DE   +0x04 version 1
 *   +0x08 bin_size             +0x0C hdr_off  (24: descriptors start here)
 *   +0x10 data_off             +0x14 data_size
 * and, for both files, data_off + data_size == the file size exactly. */
#define NV_FW_CONTAINER_MAGIC 0x000010DEu
#define NV_FW_CONTAINER_VERSION 1u
#define NV_FW_CONTAINER_HDR 24u

/* bin_size - data_size measured 0x400 for both AD102 and GA102 booters (the
 * descriptor region rounded up to 1 KiB).  Two samples do not make a rule,
 * so the parser tolerates 0..NV_FW_CONTAINER_DESC_MAX instead of demanding
 * the equality; anything larger is a different container shape. */
#define NV_FW_CONTAINER_DESC_MAX 0x1000u

struct nv_fw_container {
  uint32_t magic, version, bin_size;
  uint32_t hdr_off, data_off, data_size;
  uint32_t len;        /* bytes the caller actually supplied */
  uint32_t desc_bytes; /* data_off - hdr_off: descriptors + load header */
  uint32_t trailing;   /* len - (data_off + data_size): 0 for the pinned blobs */
  const uint8_t *buf;  /* the caller's buffer, unchanged */
  const uint8_t *hdr;  /* buf + hdr_off */
  const uint8_t *data; /* buf + data_off */
};

/* Validate magic/version and that every region the header names lies inside
 * `len`.  Returns 0, or -EINVAL for a short buffer, a foreign magic, an
 * unknown version, or a data region that runs off the end (truncation --
 * the failure a half-copied file produces). */
int nv_fw_container_parse(const uint8_t *buf, uint32_t len,
                          struct nv_fw_container *out);

/* ---- 2. the HS-v2 descriptor header ------------------------------------ */

#define NV_FW_HS_DWORDS 9        /* the header itself, at container hdr_off */
#define NV_FW_LOAD_HDR_MIN 20u   /* os 4 + num_apps, before any app entry */
#define NV_FW_APP_MAX 4          /* entries this parser will record */

/* One image: {code_off, code_size, data_off, data_size}, offsets relative to
 * the container's data region.  app[] entries continue immediately after the
 * os pair, 16 bytes each. */
struct nv_fw_app {
  uint32_t code_off, code_size;
  uint32_t data_off, data_size;
};

struct nv_fw_hs {
  /* The 9 dwords, named as in the reference driver's loader. */
  /* The 9 dwords are locations and sizes INSIDE the descriptor region:
   * dwords 0 and 1 are the production signature's offset and size; dwords 2
   * to 8 are the file offsets of the patch-location field, the patch-signature
   * field, the device mask, the signature count and the load header (and the
   * two sizes that go with them).  Measured: they tile [0x3C,0x378) with no
   * gaps, ending exactly at data_off. */
  uint32_t sig_prod_off, sig_prod_size; /* production signature (file offsets) */
  uint32_t patch_loc;                   /* file offset OF the patch-location
                                         * field; the dword there is the patch
                                         * site (measured 0x7710, inside the
                                         * data region).  0 = none. */
  uint32_t patch_sig;                   /* file offset of the patch-signature
                                         * field; the dword there is 0 on both
                                         * 535 booters, i.e. no signature */
  uint32_t meta_off, meta_size;         /* HS device-mask blob (measured 0xC) */
  uint32_t num_sig_off;                 /* dword 6 is an OFFSET (measured
                                         * 0x350), not a count -- see num_sig */
  uint32_t num_sig;                     /* the count stored AT num_sig_off
                                         * (measured 2 on both booters) */
  uint32_t loadhdr_off, loadhdr_size;   /* 9-dword load header */

  /* Decoded load header. */
  struct nv_fw_app os;      /* os_code_* in .code_*, os_data_* in .data_* */
  uint32_t num_apps;
  struct nv_fw_app app[NV_FW_APP_MAX];
  uint32_t total_size;      /* os + every app, code and data, summed */
};

/* Fill `out` from a parsed container.  Returns 0, or:
 *   -EINVAL  the descriptor region is too small to hold the 9 dwords, an
 *            offset the header names falls outside the descriptor region,
 *            num_apps does not fit in loadhdr_size, or the load header's
 *            sizes do not sum to the container's data_size (wrong kind of
 *            blob: this is the convergence check).
 */
int nv_fw_hs_parse(const struct nv_fw_container *c, struct nv_fw_hs *out);

/* ---- 3. the RISC-V ET_REL RM image ------------------------------------ */

#define NV_FW_SECTION_NAME_MAX 32
#define NV_FW_SIG_MAX 8

struct nv_fw_section {
  char name[NV_FW_SECTION_NAME_MAX]; /* NUL-terminated, truncated if longer */
  uint32_t type;
  uint32_t off;
  uint32_t size;
};

/* The map, not a load: nothing is relocated, nothing is copied. */
struct nv_fw_elf {
  uint16_t type, machine;
  uint32_t len;
  uint32_t shoff, shnum, shentsize, shstrndx;
  struct nv_fw_section fwimage;  /* the RM code+data image */
  struct nv_fw_section fwversion; /* the "535.113.01" string, NUL included */
  uint32_t nsig;                 /* .fwsignature_* sections found */
  struct nv_fw_section sig[NV_FW_SIG_MAX];
};

/* Walk the section table.  Requires ELF64 little-endian, ET_REL, EM_RISCV,
 * a section header table inside `len`, a valid .shstrtab, and both .fwimage
 * and .fwversion (which is what distinguishes the RM image from every other
 * ELF an operator could hand the loader).  Returns 0 or -EINVAL.
 *
 * Measured shape of gsp-535.113.01.bin: 10 sections, shstrndx 9, and FOUR
 * signature sections -- .fwsignature_cc_gh100, .fwsignature_gh100,
 * .fwsignature_ad10x and .fwsignature_ga10x, 4096 bytes each.  A GA102 RM
 * image therefore serves AD102, GH100 and the CC variant from one file. */
int nv_fw_elf_sections(const uint8_t *buf, uint32_t len,
                       struct nv_fw_elf *out);

/* Pick the signature section for a named engine ("ad10x", "ga10x", "gh100",
 * "cc_gh100").  Returns 0, -ENOENT when this image carries no signature for
 * that engine, or -EINVAL on bad arguments. */
int nv_fw_elf_sig_lookup(const struct nv_fw_elf *e, const char *engine,
                         const struct nv_fw_section **out);

/* The same, keyed by the chipset number decoded from PMC_BOOT_0.  Only the
 * pairs that have been checked against a real card are in the table (0x192
 * AD102 -> ad10x, 0x172 GA102 -> ga10x); every other chipset returns
 * -ENOTSUP rather than a plausible-looking guess, because the wrong signature
 * section is a silent authentication failure on the real hardware. */
int nv_fw_elf_sig_for_chipset(const struct nv_fw_elf *e, uint32_t chipset,
                              const struct nv_fw_section **out);

/* ---- 4. the WPR metadata block ---------------------------------------- */

/* 256 bytes, magic at +0x00, revision at +0x08.  Everything after the
 * revision is a hypothesis: see the "unknowns" block at the top of nv_fw.c
 * for each field and the experiment that would settle it.  The encode and
 * decode below round-trip whatever they are given, which is the honest
 * property for a layout that has not been diffed against a reference driver
 * yet -- a wrong-but-documented layout fails loudly at the first real boot
 * instead of looking like success. */
#define NV_WPR_META_MAGIC 0xDC3AAE21371A60B3ULL
#define NV_WPR_META_REVISION 1u
#define NV_WPR_META_SIZE 256u
#define NV_WPR_META_MAX_REGIONS 3

/* Offset from the base of VRAM, 128 KiB aligned (unverified: the alignment
 * rule comes from docs/nv-gsp-firmware.md §6, not from a measurement). */
struct nv_wpr_region {
  uint64_t start_offset;
  uint32_t size;
  uint32_t reserved;
};

struct nv_wpr_meta_in {
  uint64_t fb_size;   /* unverified: total VRAM in bytes */
  uint64_t wpr_start; /* unverified: WPR2 base, offset from VRAM base */
  uint32_t wpr_size;  /* unverified: WPR2 size in bytes */
  uint32_t num_regions;
  struct nv_wpr_region region[NV_WPR_META_MAX_REGIONS];
};

/* Encode into exactly NV_WPR_META_SIZE bytes; the buffer is fully written
 * (reserved bytes zeroed).  -EINVAL on a bad argument or num_regions >
 * NV_WPR_META_MAX_REGIONS. */
int nv_fw_wpr_meta_encode(const struct nv_wpr_meta_in *in,
                          uint8_t out[NV_WPR_META_SIZE]);
/* The same with a length, so a short buffer is -ENOSPC instead of a write
 * past the end.  nv_fw_wpr_meta_encode() is this with len = 256. */
int nv_fw_wpr_meta_encode_n(const struct nv_wpr_meta_in *in, uint8_t *out,
                            uint32_t len);
/* -EINVAL on a null argument, or magic/revision that is not ours. */
int nv_fw_wpr_meta_decode(const uint8_t in[NV_WPR_META_SIZE],
                          struct nv_wpr_meta_in *out);
int nv_fw_wpr_meta_decode_n(const uint8_t *in, uint32_t len,
                            struct nv_wpr_meta_in *out);

/* Where the WPR2 block goes: top-down from the end of VRAM, 128 KiB aligned
 * (docs/nv-gsp-firmware.md §6).  Unverified; pure so it can be checked
 * before it is trusted with real VRAM.  Returns 0 when the request cannot
 * fit. */
#define NV_WPR_ALIGN 0x20000ULL
uint64_t nv_fw_wpr_base(uint64_t fb_size, uint64_t wpr_size);

/* ---- 5. digests and the file loader ----------------------------------- */

/* SHA-256 (FIPS 180-4), byte-oriented: no wide loads, no SIMD.  Returns 0,
 * or -EINVAL on a null buffer/out.  Verified against the NIST vectors in the
 * unit tier (empty string and "abc"). */
int nv_fw_sha256(const uint8_t *buf, uint32_t len, uint8_t out[32]);

#ifdef __x86_64__

#define NV_FW_PATH_PREFIX "/NVIDIA/GSP/"

/* A loaded blob: page-aligned, physically contiguous frames, so the DMA path
 * can program an address without a scatter list. */
struct nv_fw_blob {
  uint8_t *buf;  /* kernel VA (phys_to_virt of phys) */
  uint64_t phys; /* page-aligned physical base */
  uint32_t size; /* file bytes in buf */
  uint32_t pages;
};

/* Read /NVIDIA/GSP/<name> into contiguous frames.
 *   -EINVAL  null/bad name, or the name does not fit the kernel's path limit
 *   -ENOENT  no such file
 *   -EISDIR  it is a directory
 *   -EIO     a short/failed read (the frames are released first)
 *   -ENOMEM  the frame pool cannot supply that many contiguous frames
 * On success `out` owns the frames: release with nv_fw_release(). */
int nv_fw_load_blob(const char *name, struct nv_fw_blob *out);

/* The convenience form: the same read, reporting only the mapping.  The DMA
 * path should use nv_fw_load_blob() and keep the physical address; this is
 * for the parse-and-verify path, which only needs a pointer.  Release the
 * frames with nv_fw_release_buf(buf, size) -- the page count is derived from
 * `size` exactly as it was derived on the way in. */
int nv_fw_load(const char *name, uint8_t **out, uint32_t *size);

void nv_fw_release(struct nv_fw_blob *b);
void nv_fw_release_buf(uint8_t *buf, uint32_t size);

#endif /* __x86_64__ */

#endif /* NV_FW_H */
