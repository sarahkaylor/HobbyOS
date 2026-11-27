#ifndef NV_FW_H
#define NV_FW_H

/* struct nv_dma_map: the blobs this file loads and the radix3 tree it builds
 * are mappings, not physical addresses.  A device (SEC2, the booter, GSP-RM)
 * reads them at the `bus` nv_dma_bus() issues, never at `phys`; the physical
 * address exists here only for phys_to_virt() on the CPU side.  The include is
 * inside the guard and costs one small header -- nv_dma.h pulls in nothing. */
#include "nv_dma.h"

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

/* Field-by-field layout of the 256-byte V1 block, from the contract in
 * docs/nv-gsp-wpr-contract.md section 3.3 (authoritative source: the V1
 * structure the booter consumes, NOT the newer revision).  This REPLACES the
 * earlier hypothesis here, which was wrong in shape: there is no region array
 * and no WPR-size field -- the region's end is a field of its own.
 *
 * The block is written by the HOST into system memory, handed to the booter by
 * physical address, and DMA'd by the booter to the top of VRAM where it is
 * locked inside the WPR block.  The host never writes WPR2 itself.
 *
 * Fields marked "policy"/"inferred" in the contract are documented as such
 * there; this struct only carries them. */
struct nv_wpr_meta_in {
  uint64_t sysmem_addr_radix3_elf;
  uint64_t size_radix3_elf;
  uint64_t sysmem_addr_bootloader;
  uint64_t size_bootloader;
  uint64_t bootloader_code_offset;
  uint64_t bootloader_data_offset;
  uint64_t bootloader_manifest_offset;
  uint64_t sysmem_addr_signature;
  uint64_t size_signature;
  uint64_t gsp_fw_rsvd_start;
  uint64_t non_wpr_heap_offset;
  uint64_t non_wpr_heap_size;
  uint64_t gsp_fw_wpr_start;
  uint64_t gsp_fw_heap_offset;
  uint64_t gsp_fw_heap_size;
  uint64_t gsp_fw_offset;
  uint64_t boot_bin_offset;
  uint64_t frts_offset;
  uint64_t frts_size;
  uint64_t gsp_fw_wpr_end;
  uint64_t fb_size;
  uint64_t vga_workspace_offset;
  uint64_t vga_workspace_size;
  uint64_t boot_count;
  uint64_t sysmem_addr_crash_queue;
  uint32_t size_crash_queue;         /* 0 when no CrashCat queue */
  uint8_t heap_vf_partition_count;   /* 0 on bare metal */
  uint8_t flags;                     /* named bits beyond IN_PM_TRANSITION unknown */
  uint16_t paging_config;            /* registry passthrough; 0 = default */
  /* pmuReservedSize: the PMU's reserved FB window, which the vendor's layout
   * diagram puts directly above FRTS and below the FB-end reserve estimate
   * (gsp_fw_wpr_meta.h:198-204, offset 0xF4 -- measured by compiling the
   * vendor header, not computed from this struct).  A 32-bit wire field on
   * purpose: the four bytes at 0xF2 are the vendor's `padding[2]` and its
   * `verified` u64 starts at 0xF8, so a 64-bit field here would move it. */
  uint32_t pmu_reserved_size;
  /* Written by the HOST as 0.  The booter overwrites it with
   * NV_WPR_META_VERIFIED once it accepts the block -- that is the cheap,
   * protocol-free proof that the meta was consumed. */
  uint64_t verified;
};

/* The value the booter writes into `verified`. */
#define NV_WPR_META_VERIFIED 0xA0A0A0A0A0A0A0A0ULL

/* Encode into exactly NV_WPR_META_SIZE bytes; reserved bytes are zeroed. */
int nv_fw_wpr_meta_encode(const struct nv_wpr_meta_in *in,
                          uint8_t out[NV_WPR_META_SIZE]);
/* The same with a length, so a short buffer is -ENOSPC instead of a write past
 * the end.  nv_fw_wpr_meta_encode() is this with len = 256. */
int nv_fw_wpr_meta_encode_n(const struct nv_wpr_meta_in *in, uint8_t *out,
                            uint32_t len);
/* -EINVAL on a null argument, an unknown magic, or a revision that is not 1. */
int nv_fw_wpr_meta_decode(const uint8_t in[NV_WPR_META_SIZE],
                          struct nv_wpr_meta_in *out);
int nv_fw_wpr_meta_decode_n(const uint8_t *in, uint32_t len,
                            struct nv_wpr_meta_in *out);
/* True when the booter has accepted the block.  Decoding does not check this:
 * an unverified block is a valid block that nobody has consumed yet. */
int nv_fw_wpr_meta_verified(const struct nv_wpr_meta_in *m);

/* ---- 4b. the radix3 tree that describes the RM image to the booter -----
 *
 * The metadata block carries the BUS address of a tree root, not of the image
 * (docs/nv-gsp-wpr-contract.md section 4.3).  Three levels of 512 64-bit
 * entries: level 0 points at level-1 pages, level-1 at level-2 pages, and
 * level-2 holds one PTE per 4 KiB image page, ascending.  Our loader hands back
 * one contiguous run, so the tree is arithmetic: no mapping, no scatter list.
 *
 * Every address an entry carries is the one the DEVICE walks with -- the bus
 * address nv_dma_bus() issued for the image mapping (in the PTEs) and for the
 * tree's own mapping (in the two pointer levels and the root).  `phys` appears
 * in this API only so a caller cannot lose the CPU-side address it needs for
 * phys_to_virt(); it is never written into an entry.  While VT-d translation
 * is off bus == phys and both spellings work -- which is exactly why the
 * distinction has to live in the values, not in the arithmetic
 * (src/include/nv_dma.h).
 *
 * Pages needed for the tree itself: 1 level-0 page + L1 level-1 pages + L2
 * level-2 pages, where L2 = ceil(image_pages / 512) and L1 = ceil(L2 / 512).
 * The 38 MB image needs 9297 image pages, 19 level-2 pages and 1 level-1 page,
 * so 21 pages = 84 KiB. */
#define NV_RADIX3_PAGE 4096u
#define NV_RADIX3_ENTRIES 512u

/* Tree pages required to describe `size` bytes, or 0 when the request cannot be
 * described (zero size, or more image pages than the three levels can hold:
 * 512^3 pages = 512 GiB). */
uint32_t nv_radix3_pages(uint64_t size);

/* Fill a caller-provided contiguous tree.
 *   tree        base of the tree, `tree_pages` * 4096 bytes, physically
 *               contiguous
 *   tree_phys   that buffer's physical base (CPU side: validation only)
 *   tree_bus    the same buffer's device-visible base.  This is what the
 *               pointer entries hold: tree_bus + page offset, never tree_phys.
 *   image_phys  page-aligned physical base of the RM image (validation only)
 *   image_bus   page-aligned device-visible base of the RM image; the level-2
 *               PTEs are image_bus + i * 4096
 * Returns 0, or -EINVAL on a bad argument (sizes that disagree, unaligned
 * phys OR bus, a tree too small for the image).  A caller that has no honest
 * bus value must not call this: nv_dma_bus() answering NV_DMA_BUS_INVALID is
 * page-unaligned, so it is refused here rather than written into a PTE. */
int nv_radix3_build_into(uint8_t *tree, uint32_t tree_pages, uint64_t tree_phys,
                         uint64_t tree_bus, uint64_t image_phys,
                         uint64_t image_bus, uint64_t image_size);

/* ---- 4c. the VRAM layout the metadata block describes ------------------
 *
 * Everything the booter needs to know about where things live inside VRAM, in
 * ONE place, because these values must tile exactly: the region runs from
 * gsp_fw_wpr_start to gsp_fw_wpr_end with no gaps and no overlaps, and a
 * mismatch between two of them is a boot failure with nothing but a non-zero
 * mailbox to show for it.
 *
 * Order, from the TOP of VRAM downwards (docs/nv-gsp-wpr-contract.md 3.2):
 *   fb_size
 *   - PRAMIN reserve (16 MiB: the size of BAR0's own window)
 *   = wpr_end            (128 KiB aligned)
 *   - frts_size (1 MiB)  = frts_offset
 *   - bootloader reserve (0x80000) = boot_bin_offset
 *   - image size (page aligned)    = gsp_fw_offset
 *   = gsp_fw_heap end
 *   ... heap ...
 *   gsp_fw_heap_offset   (1 MiB aligned, = wpr_start + 1 MiB)
 *   - nonWprHeap (1 MiB) = gsp_fw_rsvd_start / non_wpr_heap_offset
 *   = wpr_start
 *
 * Every field reproduces the contract's AD102 table EXACTLY, given
 * wpr_start = 0x5F8700000 and fb = 0x5FF400000:
 *   wpr_end 0x5FE400000, frts 0x5FE300000, boot_bin 0x5FE2F8000,
 *   image 0x5FBEB0000 (end lands exactly on boot_bin), heap 0x36B0000,
 *   non-wpr heap 0x5F8600000, span 0x5D00000 = 93 MiB.
 * The named pieces sum to the span: this layout tiles the region exactly.
 *
 * Two arithmetic slips happened while writing this, both caught by the tests in
 * nv_fw_test.c: reading the bootloader reserve as 0x80000 instead of 0x8000 (it
 * is the DATA region's size), and forcing a 128 KiB alignment on boot_bin where
 * the contract's value is only 0x8000 aligned.  Hand-subtracting hex in a
 * comment is how a wrong "the document must be wrong" conclusion gets written
 * down; the test computes it. */
struct nv_wpr_layout_in {
  uint64_t wpr_start;    /* chosen by policy: lowest address the region may use */
  uint64_t fb_size;      /* usable FB in bytes */
  uint64_t image_size;   /* .fwimage length */
};

struct nv_wpr_layout {
  uint64_t wpr_start, wpr_end;
  uint64_t non_wpr_heap_offset, non_wpr_heap_size;
  uint64_t heap_offset, heap_size;
  uint64_t image_offset;
  uint64_t boot_bin_offset;
  uint64_t frts_offset, frts_size;
};

#define NV_WPR_PRAMIN_RESERVE 0x1000000ULL /* 16 MiB */
#define NV_WPR_FRTS_SIZE 0x100000ULL       /* 1 MiB */
#define NV_WPR_NONHEAP_SIZE 0x100000ULL    /* 1 MiB */
#define NV_WPR_BOOTBIN_RESERVE 0x8000ULL /* the bootloader data region's size */
#define NV_WPR_END_ALIGN 0x20000ULL        /* 128 KiB */
#define NV_WPR_START_ALIGN 0x100000ULL     /* 1 MiB */

/* Compute the layout.  Returns 0, or -ENOSPC when the image + reserves cannot
 * fit above `wpr_start`, or -EINVAL for a nonsense request. */
int nv_wpr_layout(const struct nv_wpr_layout_in *in, struct nv_wpr_layout *out);

/* Slide `len` bytes at `buf + off` down to `buf`, inside the same buffer, for
 * a firmware section that does not start on a page boundary.  Byte-wise and
 * backwards, so an overlapping move is safe and no second buffer is needed --
 * the alternative is a 38 MB copy while the original is still allocated. */
void nv_fw_slide_down(uint8_t *buf, uint32_t buf_len, uint32_t off,
                      uint32_t len);

#ifdef __x86_64__
/* Put the metadata block where the GPU can read it: one contiguous frame,
 * encoded in place, phys returned.  The booter DMAs this block, so a stack
 * copy is not good enough even though the bytes are identical.
 *
 * NOT the boot path's allocator any more (WP-G): nv_gsp.c takes that page from
 * nv_dma, because SEC2 receives the block's address in MAILBOX0/1 and a GPU
 * address must come from nv_dma_bus(), not from frame_alloc_contig.  This
 * helper remains for callers that only need encode-in-place RAM. */
int nv_fw_meta_alloc(const struct nv_wpr_meta_in *m, uint64_t *phys_out);
void nv_fw_meta_release(uint64_t phys);

/* The tree a caller gets from the allocating form.  The mapping is the tree's
 * own frames (nv_dma_alloc, so its size is page-granular and its bus address is
 * resolved once); the root the metadata block carries is the mapping's BUS
 * address, kept here so the boot path never has to spell it again. */
struct nv_radix3_tree {
  struct nv_dma_map map; /* owns the tree frames: phys CPU-side, bus in the PTEs */
  uint64_t root_bus;     /* nv_dma_bus(&map): the value the WPR meta carries */
};

/* Allocate the tree from one contiguous nv_dma run and fill it.  On success
 * `out->root_bus` is the device-visible address the metadata block wants and
 * `out->map.pages` is how many frames to free later; -ENOMEM when the frame
 * pool cannot supply a contiguous run, -ENOTSUP when translation is live (there
 * is no honest bus address to put in an entry -- src/include/nv_dma.h). */
int nv_radix3_build(uint64_t image_phys, uint64_t image_bus,
                    uint64_t image_size, struct nv_radix3_tree *out);
void nv_radix3_release(struct nv_radix3_tree *t);
#endif

/* ---- 5. digests and the file loader ----------------------------------- */

/* SHA-256 (FIPS 180-4), byte-oriented: no wide loads, no SIMD.  Returns 0,
 * or -EINVAL on a null buffer/out.  Verified against the NIST vectors in the
 * unit tier (empty string and "abc"). */
int nv_fw_sha256(const uint8_t *buf, uint32_t len, uint8_t out[32]);

#ifdef __x86_64__

#define NV_FW_PATH_PREFIX "/NVIDIA/GSP/"

/* A loaded blob: page-aligned, physically contiguous frames from nv_dma_alloc,
 * so the DMA path has BOTH addresses it needs and cannot confuse them:
 *   map.phys  CPU side only -- phys_to_virt() to read/write the bytes
 *   nv_fw_blob_bus(&blob)  the device-visible base.  Everything an
 *             SEC2/booter/GSP-RM field publishes is this value plus a
 *             file/pbuffer OFFSET inside the blob; a raw physical address there
 *             is the bug nv_dma.h exists to prevent.  There is no stored copy
 *             of it: one accessor, one source of truth. */
struct nv_fw_blob {
  uint8_t *buf;          /* kernel VA (phys_to_virt of map.phys) */
  struct nv_dma_map map; /* owns the frames; the mapping, not an address */
  uint32_t size;         /* file bytes in buf */
  uint32_t pages;        /* map.pages (kept for callers that only free) */
};

/* The device-visible base of a loaded blob.  The accessor exists so a call site
 * never reaches into the mapping's fields directly -- this call is the one
 * place the value is allowed to come from (src/include/nv_dma.h).  Returns
 * NV_DMA_BUS_INVALID when there is no honest answer; a caller must then fail
 * instead of publishing anything. */
static inline uint64_t nv_fw_blob_bus(const struct nv_fw_blob *b) {
  return b ? nv_dma_bus(&b->map) : NV_DMA_BUS_INVALID;
}

/* Read /NVIDIA/GSP/<name> into contiguous frames.
 *   -EINVAL  null/bad name, or the name does not fit the kernel's path limit
 *   -ENOENT  no such file
 *   -EISDIR  it is a directory
 *   -EIO     a short/failed read (the frames are released first)
 *   -ENOMEM  the frame pool cannot supply that many contiguous frames
 *   -ENOTSUP VT-d translation is live: nv_dma_alloc cannot name a bus address
 * On success `out` owns the frames: release with nv_fw_release(). */
int nv_fw_load_blob(const char *name, struct nv_fw_blob *out);

/* The convenience form: the same read, reporting only the mapping.  This is
 * the parse-and-verify path, which needs a pointer and nothing else -- and it
 * must NOT be used where a device address is published: it returns a CPU
 * pointer, so a caller that needs the bus address has to call
 * nv_fw_load_blob() and keep the mapping.  Release the frames with
 * nv_fw_release_buf(buf, size) -- the page count is derived from `size`
 * exactly as it was derived on the way in. */
int nv_fw_load(const char *name, uint8_t **out, uint32_t *size);

void nv_fw_release(struct nv_fw_blob *b);
void nv_fw_release_buf(uint8_t *buf, uint32_t size);

#endif /* __x86_64__ */

#endif /* NV_FW_H */
