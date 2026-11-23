/* ============================================================================
 *  nv_fw: parsing and loading of the GSP firmware blobs.
 *
 *  Pure where it can be: nv_fw_container_parse, nv_fw_hs_parse,
 *  nv_fw_elf_sections, the WPR metadata codec and nv_fw_sha256 read only the
 *  buffer they are handed.  nv_fw_load* is the one filesystem entry and is
 *  x86_64-only, like the rest of the driver.
 *
 *  Four of these formats were measured against the real 535.113.01 bytes
 *  (sha256 7d4ef325.../3ab46bf4... for the two booters, e30231ec... for
 *  gsp-535.113.01.bin):
 *
 *    - container: data_off + data_size == file size exactly, on both booters;
 *      bin_size - data_size == 0x400 on both.
 *    - HS header: the 9 dwords are ABSOLUTE file offsets, and they tile the
 *      descriptor region exactly -- production signature [0x3C,0x33C), patch
 *      location 0x33C, patch signature 0x340 (=0: none), device mask at 0x344
 *      (0xC bytes), signature count at 0x350 (=2), load header at 0x354
 *      (0x24 bytes) ending exactly at data_off 0x378.
 *    - load header: os_code(0,0x100), os_data(0x7700,0x6000) -- 0x8400 on
 *      GA102, app_code(0x100,0x7600), app_data(0x100,0), num_apps 1; the
 *      sizes sum to 0xD700 == data_size (0xE600 on GA102).
 *    - RM ELF: ET_REL/EM_RISCV, 10 sections, shstrndx 9, and FOUR signature
 *      sections (see below).
 *
 *  ---- UNKNOWNS --------------------------------------------------------
 *  Fields whose layout is not established.  Each is encoded at a named
 *  offset so a diff against a reference driver settles it in one comparison,
 *  and the codec round-trips whatever it is given.
 *
 *  1. unverified: field fb_size (offset 0x10, u64).  Assumed total VRAM in
 *     bytes.  Present in no measurement taken here; settle by diffing the
 *     256-byte block a reference driver writes against this encoder's output.
 *  2. unverified: field wpr_start (offset 0x18, u64).  Assumed the WPR2 base
 *     as an offset from the VRAM base (docs/nv-gsp-firmware.md §6 says the
 *     block sits at a 128 KiB-aligned base computed top-down from the end of
 *     VRAM, but not which field carries it).
 *  3. unverified: field wpr_size (offset 0x20, u32).  Assumed WPR2 size.
 *  4. unverified: field num_regions (offset 0x24, u32) and the 16-byte
 *     region[] entries at 0x28.. (start_offset u64, size u32, reserved u32).
 *     A plausible shape from public firmware-interface headers; entirely
 *     unconfirmed, including whether the count precedes the array at all.
 *  5. unverified: bytes 0x0C..0x10 and 0x58..0x100 of the block.  Written as
 *     zero (the reference driver is expected to zero its reserved area), read
 *     back as "do not care": decode() does not reject non-zero reserved
 *     bytes, so a firmware-side extension cannot look like corruption.
 *  6. unverified: nv_fw_wpr_base()'s alignment and top-down placement rule.
 *     From the doc, not from a register dump.  Also unverified for hardware:
 *     the WPR2 register set itself (docs/nv-gsp-firmware.md §5 item 1).
 *  7. unverified: which signature section a chipset selects.  Only
 *     AD102 (0x192) -> .fwsignature_ad10x and GA102 (0x172) ->
 *     .fwsignature_ga10x are in the table, both from directory/engine names
 *     rather than from a boot.  The image also carries .fwsignature_gh100 and
 *     .fwsignature_cc_gh100; what selects those is not established here.
 *  8. Not a layout question but a cost one: hashing the 38 MB RM image with
 *     this byte-oriented SHA-256 under TCG takes seconds.  Verify it once at
 *     load, not per boot stage.
 * ========================================================================== */

#ifdef __x86_64__
#include "frame.h"
#include "fat16.h"
#include "kmem.h"
#endif

#include "errno.h"
#include "nv_fw.h"

/* ---- little-endian assembly -------------------------------------------
 * Byte at a time on purpose: this code parses a DMA buffer as readily as a
 * file image, and a wide load at an odd offset is an alignment fault on the
 * Device-typed mappings the kernel runs on. */

static uint32_t fw_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint16_t fw_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t fw_le64(const uint8_t *p) {
  return (uint64_t)fw_le32(p) | ((uint64_t)fw_le32(p + 4) << 32);
}

static void fw_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void fw_put64(uint8_t *p, uint64_t v) {
  fw_put32(p, (uint32_t)v);
  fw_put32(p + 4, (uint32_t)(v >> 32));
}

static void fw_put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void fw_zero(void *p, uint32_t n) {
  uint8_t *b = (uint8_t *)p;
  for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

/* Byte-wise, because the destination is a wire buffer and the source is a
 * struct: alignment must not enter into it. */
static void fw_copy(void *dst, const void *src, uint32_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* ---- 1. container ------------------------------------------------------ */

int nv_fw_container_parse(const uint8_t *buf, uint32_t len,
                          struct nv_fw_container *out) {
  if (!buf || !out) return -EINVAL;
  /* The six-dword header must be present before anything is read from it. */
  if (len < NV_FW_CONTAINER_HDR) return -EINVAL;

  uint32_t magic = fw_le32(buf + 0x00);
  uint32_t version = fw_le32(buf + 0x04);
  uint32_t bin_size = fw_le32(buf + 0x08);
  uint32_t hdr_off = fw_le32(buf + 0x0C);
  uint32_t data_off = fw_le32(buf + 0x10);
  uint32_t data_size = fw_le32(buf + 0x14);

  if (magic != NV_FW_CONTAINER_MAGIC) return -EINVAL;
  if (version != NV_FW_CONTAINER_VERSION) return -EINVAL;

  /* The descriptor region starts after the container header and must hold
   * something (the 9-dword HS header at minimum) before the data does. */
  if (hdr_off < NV_FW_CONTAINER_HDR || hdr_off >= data_off) return -EINVAL;
  if (data_off < hdr_off + NV_FW_HS_DWORDS * 4u) return -EINVAL;

  /* Everything the header names must be inside the buffer.  data_size == 0
   * is a blob with no image in it, which is not a booter. */
  if (data_size == 0) return -EINVAL;
  if (data_off >= len) return -EINVAL;
  if (data_size > len - data_off) return -EINVAL; /* truncated file */

  /* bin_size is the image size declared for the GPU, not the file size: the
   * measured relation is bin_size == data_size + 0x400 (descriptors padded to
   * 1 KiB).  Accept the range rather than the equality -- two blobs are not
   * a format -- but reject a declared size that cannot cover its own data. */
  if (bin_size < data_size) return -EINVAL;
  if (bin_size - data_size > NV_FW_CONTAINER_DESC_MAX) return -EINVAL;

  out->magic = magic;
  out->version = version;
  out->bin_size = bin_size;
  out->hdr_off = hdr_off;
  out->data_off = data_off;
  out->data_size = data_size;
  out->len = len;
  out->desc_bytes = data_off - hdr_off;
  out->trailing = len - (data_off + data_size);
  out->buf = buf;
  out->hdr = buf + hdr_off;
  out->data = buf + data_off;
  return 0;
}

/* ---- 2. HS descriptor header ------------------------------------------ */

/* A region the HS header names, in absolute file offsets, must sit inside
 * the descriptor region [hdr_off, data_off). */
static int fw_hs_in_desc(const struct nv_fw_container *c, uint32_t off,
                         uint32_t size) {
  if (off < c->hdr_off) return 0;
  if (off > c->data_off) return 0;
  return size <= c->data_off - off;
}

int nv_fw_hs_parse(const struct nv_fw_container *c, struct nv_fw_hs *out) {
  if (!c || !out || !c->buf) return -EINVAL;
  if (c->desc_bytes < NV_FW_HS_DWORDS * 4u) return -EINVAL;

  const uint8_t *h = c->hdr;
  struct nv_fw_hs hs;
  fw_zero(&hs, sizeof hs);

  hs.sig_prod_off = fw_le32(h + 0x00);
  hs.sig_prod_size = fw_le32(h + 0x04);
  hs.patch_loc = fw_le32(h + 0x08);
  hs.patch_sig = fw_le32(h + 0x0C);
  hs.meta_off = fw_le32(h + 0x10);
  hs.meta_size = fw_le32(h + 0x14);
  hs.num_sig_off = fw_le32(h + 0x18);
  hs.loadhdr_off = fw_le32(h + 0x1C);
  hs.loadhdr_size = fw_le32(h + 0x20);

  /* The named fields tile the descriptor area.  0 means "not present" for the
   * patch fields (measured on both 535 booters for patch_sig); anything else
   * must name a dword inside the descriptor region. */
  if (!fw_hs_in_desc(c, hs.sig_prod_off, hs.sig_prod_size)) return -EINVAL;
  if (hs.patch_loc != 0 && !fw_hs_in_desc(c, hs.patch_loc, 4)) return -EINVAL;
  if (hs.patch_sig != 0 && !fw_hs_in_desc(c, hs.patch_sig, 4)) return -EINVAL;
  if (!fw_hs_in_desc(c, hs.meta_off, hs.meta_size)) return -EINVAL;
  if (hs.meta_size == 0) return -EINVAL;
  if (!fw_hs_in_desc(c, hs.num_sig_off, 4)) return -EINVAL;
  if (!fw_hs_in_desc(c, hs.loadhdr_off, hs.loadhdr_size)) return -EINVAL;
  if (hs.loadhdr_size < NV_FW_LOAD_HDR_MIN) return -EINVAL;

  /* dword 6 is an offset, the count lives there (measured 2). */
  hs.num_sig = fw_le32(c->buf + hs.num_sig_off);

  const uint8_t *l = c->buf + hs.loadhdr_off;
  hs.os.code_off = fw_le32(l + 0x00);
  hs.os.code_size = fw_le32(l + 0x04);
  hs.os.data_off = fw_le32(l + 0x08);
  hs.os.data_size = fw_le32(l + 0x0C);
  hs.num_apps = fw_le32(l + 0x10);

  /* num_apps must both fit the declared load-header size and fit the array
   * here; a blob that claims more apps than its header can hold is not
   * something to read past. */
  if (hs.num_apps > NV_FW_APP_MAX) return -EINVAL;
  if (hs.num_apps > (hs.loadhdr_size - NV_FW_LOAD_HDR_MIN) / 16u) return -EINVAL;

  uint32_t total = hs.os.code_size + hs.os.data_size;
  for (uint32_t i = 0; i < hs.num_apps; i++) {
    const uint8_t *a = l + NV_FW_LOAD_HDR_MIN + i * 16u;
    hs.app[i].code_off = fw_le32(a + 0x00);
    hs.app[i].code_size = fw_le32(a + 0x04);
    hs.app[i].data_off = fw_le32(a + 0x08);
    hs.app[i].data_size = fw_le32(a + 0x0C);
    total += hs.app[i].code_size + hs.app[i].data_size;
  }
  hs.total_size = total;

  /* The convergence check: the images the load header places must exactly
   * cover the data region the container declares.  This is what rejects a
   * booter for the wrong chip, or a container whose header and payload were
   * mixed from two different blobs. */
  if (total != c->data_size) return -EINVAL;

  *out = hs;
  return 0;
}

/* ---- 3. the RM ELF ---------------------------------------------------- */

static int fw_streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int fw_startswith(const char *s, const char *pfx) {
  while (*pfx) {
    if (*s++ != *pfx++) return 0;
  }
  return 1;
}

static void fw_copy_name(char *dst, const char *src, uint32_t cap) {
  uint32_t i = 0;
  for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

/* Section table entry -> struct nv_fw_section.  Bounds are checked by the
 * caller, which owns `len`. */
static void fw_elf_section(const uint8_t *sh, const char *name,
                           struct nv_fw_section *s) {
  fw_copy_name(s->name, name, NV_FW_SECTION_NAME_MAX);
  s->type = fw_le32(sh + 0x04);
  s->off = (uint32_t)fw_le64(sh + 0x18);
  s->size = (uint32_t)fw_le64(sh + 0x20);
}

int nv_fw_elf_sections(const uint8_t *buf, uint32_t len,
                       struct nv_fw_elf *out) {
  if (!buf || !out || len < 64) return -EINVAL;
  if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F')
    return -EINVAL;
  if (buf[4] != 2 || buf[5] != 1) return -EINVAL; /* ELFCLASS64, ELFDATA2LSB */

  struct nv_fw_elf e;
  fw_zero(&e, sizeof e);
  e.len = len;
  e.type = fw_le16(buf + 0x10);
  e.machine = fw_le16(buf + 0x12);
  if (e.type != 1 || e.machine != 243) return -EINVAL; /* ET_REL, EM_RISCV */

  uint64_t shoff = fw_le64(buf + 0x28);
  e.shentsize = fw_le16(buf + 0x3A);
  e.shnum = fw_le16(buf + 0x3C);
  e.shstrndx = fw_le16(buf + 0x3E);
  if (e.shentsize != 64) return -EINVAL;
  if (e.shnum == 0) return -EINVAL;
  if (e.shstrndx >= e.shnum) return -EINVAL;
  if (shoff > len) return -EINVAL;
  if ((uint64_t)e.shnum * 64u > (uint64_t)len - shoff) return -EINVAL;
  e.shoff = (uint32_t)shoff;

  /* The string table the section names live in. */
  const uint8_t *sst_sh = buf + e.shoff + (uint64_t)e.shstrndx * 64u;
  uint64_t sst_off = fw_le64(sst_sh + 0x18);
  uint64_t sst_size = fw_le64(sst_sh + 0x20);
  if (sst_off > len) return -EINVAL;
  if (sst_size > (uint64_t)len - sst_off) return -EINVAL;
  const char *sst = (const char *)(buf + sst_off);

  for (uint32_t i = 0; i < e.shnum; i++) {
    const uint8_t *sh = buf + e.shoff + (uint64_t)i * 64u;
    uint32_t name_off = fw_le32(sh + 0x00);
    if (name_off >= sst_size) continue; /* unnamed: nothing to match */
    const char *name = sst + name_off;
    uint32_t room = (uint32_t)(sst_size - name_off);
    /* NUL must be inside the table, or the name is not a name. */
    uint32_t nl = 0;
    while (nl < room && name[nl]) nl++;
    if (nl == room) continue;

    if (fw_streq(name, ".fwimage")) {
      fw_elf_section(sh, name, &e.fwimage);
    } else if (fw_streq(name, ".fwversion")) {
      fw_elf_section(sh, name, &e.fwversion);
    } else if (fw_startswith(name, ".fwsignature_") &&
               e.nsig < NV_FW_SIG_MAX) {
      fw_elf_section(sh, name, &e.sig[e.nsig]);
      e.nsig++;
    }
  }

  /* Both must be present: .fwimage is the payload, .fwversion is the
   * identity.  A booter passed to this parser fails on e_type first. */
  if (e.fwimage.size == 0) return -EINVAL;
  if (e.fwversion.size == 0 || e.fwversion.size >= 32) return -EINVAL;
  if (e.fwimage.off > len || e.fwimage.size > len - e.fwimage.off)
    return -EINVAL;
  if (e.fwversion.off > len || e.fwversion.size > len - e.fwversion.off)
    return -EINVAL;

  *out = e;
  return 0;
}

/* The section named ".fwsignature_<engine>". */
int nv_fw_elf_sig_lookup(const struct nv_fw_elf *e, const char *engine,
                         const struct nv_fw_section **out) {
  if (!e || !engine || !out) return -EINVAL;
  char want[NV_FW_SECTION_NAME_MAX];
  const char *pfx = ".fwsignature_";
  uint32_t i = 0;
  while (pfx[i] && i + 1 < NV_FW_SECTION_NAME_MAX) {
    want[i] = pfx[i];
    i++;
  }
  uint32_t j = 0;
  while (engine[j] && i + 1 < NV_FW_SECTION_NAME_MAX) want[i++] = engine[j++];
  want[i] = 0;

  for (uint32_t k = 0; k < e->nsig; k++) {
    if (fw_streq(e->sig[k].name, want)) {
      *out = &e->sig[k];
      return 0;
    }
  }
  return -ENOENT;
}

int nv_fw_elf_sig_for_chipset(const struct nv_fw_elf *e, uint32_t chipset,
                              const struct nv_fw_section **out) {
  /* Only checked pairs.  An unknown chipset is refused rather than mapped by
   * range: picking the wrong signature section fails authentication on the
   * real card with no host-visible reason (docs/nv-gsp-firmware.md §5). */
  const char *engine = 0;
  switch (chipset) {
  case 0x192: engine = "ad10x"; break; /* AD102, the card this lane targets */
  case 0x172: engine = "ga10x"; break; /* GA102, the image's own directory */
  default: return -ENOTSUP;
  }
  return nv_fw_elf_sig_lookup(e, engine, out);
}

/* ---- 4. WPR metadata -------------------------------------------------- */

/* Offsets from docs/nv-gsp-wpr-contract.md section 3.3 (the V1 block).  Named
 * individually rather than computed from a struct: these are wire offsets into
 * a buffer the firmware reads, so a compiler's padding must never be able to
 * move them. */
#define WPR_OFF_MAGIC 0x00u
#define WPR_OFF_REV 0x08u
#define WPR_OFF_VERIFIED 0xF8u

/* Explicit offset/value pairs rather than a struct walk: the destination is a
 * wire buffer the firmware reads, so compiler padding must never be able to
 * move a field. */
int nv_fw_wpr_meta_encode_n(const struct nv_wpr_meta_in *in, uint8_t *out,
                            uint32_t len) {
  if (!in || !out) return -EINVAL;
  if (len < NV_WPR_META_SIZE) return -ENOSPC;

  fw_zero(out, NV_WPR_META_SIZE);
  fw_put64(out + WPR_OFF_MAGIC, NV_WPR_META_MAGIC);
  fw_put32(out + WPR_OFF_REV, NV_WPR_META_REVISION);
  fw_put64(out + 0x10, in->sysmem_addr_radix3_elf);
  fw_put64(out + 0x18, in->size_radix3_elf);
  fw_put64(out + 0x20, in->sysmem_addr_bootloader);
  fw_put64(out + 0x28, in->size_bootloader);
  fw_put64(out + 0x30, in->bootloader_code_offset);
  fw_put64(out + 0x38, in->bootloader_data_offset);
  fw_put64(out + 0x40, in->bootloader_manifest_offset);
  fw_put64(out + 0x48, in->sysmem_addr_signature);
  fw_put64(out + 0x50, in->size_signature);
  fw_put64(out + 0x58, in->gsp_fw_rsvd_start);
  fw_put64(out + 0x60, in->non_wpr_heap_offset);
  fw_put64(out + 0x68, in->non_wpr_heap_size);
  fw_put64(out + 0x70, in->gsp_fw_wpr_start);
  fw_put64(out + 0x78, in->gsp_fw_heap_offset);
  fw_put64(out + 0x80, in->gsp_fw_heap_size);
  fw_put64(out + 0x88, in->gsp_fw_offset);
  fw_put64(out + 0x90, in->boot_bin_offset);
  fw_put64(out + 0x98, in->frts_offset);
  fw_put64(out + 0xA0, in->frts_size);
  fw_put64(out + 0xA8, in->gsp_fw_wpr_end);
  fw_put64(out + 0xB0, in->fb_size);
  fw_put64(out + 0xB8, in->vga_workspace_offset);
  fw_put64(out + 0xC0, in->vga_workspace_size);
  fw_put64(out + 0xC8, in->boot_count);
  fw_put64(out + 0xE0, in->sysmem_addr_crash_queue);
  fw_put32(out + 0xE8, in->size_crash_queue);
  out[0xF0] = in->heap_vf_partition_count;
  out[0xF1] = in->flags;
  fw_put16(out + 0xF2, in->paging_config);
  fw_put64(out + WPR_OFF_VERIFIED, in->verified);
  return 0;
}

int nv_fw_wpr_meta_encode(const struct nv_wpr_meta_in *in,
                          uint8_t out[NV_WPR_META_SIZE]) {
  return nv_fw_wpr_meta_encode_n(in, out, NV_WPR_META_SIZE);
}

int nv_fw_wpr_meta_decode_n(const uint8_t *in, uint32_t len,
                            struct nv_wpr_meta_in *out) {
  if (!in || !out) return -EINVAL;
  if (len < NV_WPR_META_SIZE) return -ENOSPC;
  if (fw_le64(in + WPR_OFF_MAGIC) != NV_WPR_META_MAGIC) return -EINVAL;
  if (fw_le32(in + WPR_OFF_REV) != NV_WPR_META_REVISION) return -EINVAL;

  fw_zero(out, sizeof *out);
  out->sysmem_addr_radix3_elf = fw_le64(in + 0x10);
  out->size_radix3_elf = fw_le64(in + 0x18);
  out->sysmem_addr_bootloader = fw_le64(in + 0x20);
  out->size_bootloader = fw_le64(in + 0x28);
  out->bootloader_code_offset = fw_le64(in + 0x30);
  out->bootloader_data_offset = fw_le64(in + 0x38);
  out->bootloader_manifest_offset = fw_le64(in + 0x40);
  out->sysmem_addr_signature = fw_le64(in + 0x48);
  out->size_signature = fw_le64(in + 0x50);
  out->gsp_fw_rsvd_start = fw_le64(in + 0x58);
  out->non_wpr_heap_offset = fw_le64(in + 0x60);
  out->non_wpr_heap_size = fw_le64(in + 0x68);
  out->gsp_fw_wpr_start = fw_le64(in + 0x70);
  out->gsp_fw_heap_offset = fw_le64(in + 0x78);
  out->gsp_fw_heap_size = fw_le64(in + 0x80);
  out->gsp_fw_offset = fw_le64(in + 0x88);
  out->boot_bin_offset = fw_le64(in + 0x90);
  out->frts_offset = fw_le64(in + 0x98);
  out->frts_size = fw_le64(in + 0xA0);
  out->gsp_fw_wpr_end = fw_le64(in + 0xA8);
  out->fb_size = fw_le64(in + 0xB0);
  out->vga_workspace_offset = fw_le64(in + 0xB8);
  out->vga_workspace_size = fw_le64(in + 0xC0);
  out->boot_count = fw_le64(in + 0xC8);
  out->sysmem_addr_crash_queue = fw_le64(in + 0xE0);
  out->size_crash_queue = fw_le32(in + 0xE8);
  out->heap_vf_partition_count = in[0xF0];
  out->flags = in[0xF1];
  out->paging_config = fw_le16(in + 0xF2);
  out->verified = fw_le64(in + WPR_OFF_VERIFIED);
  return 0;
}

int nv_fw_wpr_meta_decode(const uint8_t in[NV_WPR_META_SIZE],
                          struct nv_wpr_meta_in *out) {
  return nv_fw_wpr_meta_decode_n(in, NV_WPR_META_SIZE, out);
}

int nv_fw_wpr_meta_verified(const struct nv_wpr_meta_in *m) {
  return m && m->verified == NV_WPR_META_VERIFIED;
}

/* ---- 5. SHA-256 (FIPS 180-4) ----------------------------------------- */

static const uint32_t sha_k[64] = {
  0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
  0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
  0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
  0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
  0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
  0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
  0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
  0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
  0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
  0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
  0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
  0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
  0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t sha_ror(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

static void sha_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = sha_ror(w[i - 15], 7) ^ sha_ror(w[i - 15], 18) ^
                  (w[i - 15] >> 3);
    uint32_t s1 = sha_ror(w[i - 2], 17) ^ sha_ror(w[i - 2], 19) ^
                  (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t s1 = sha_ror(e, 6) ^ sha_ror(e, 11) ^ sha_ror(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + s1 + ch + sha_k[i] + w[i];
    uint32_t s0 = sha_ror(a, 2) ^ sha_ror(a, 13) ^ sha_ror(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

int nv_fw_sha256(const uint8_t *buf, uint32_t len, uint8_t out[32]) {
  if ((!buf && len) || !out) return -EINVAL;

  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

  uint32_t off = 0;
  while (len - off >= 64) {
    sha_block(h, buf + off);
    off += 64;
  }

  /* Tail: the remainder, 0x80, zeros, and the 64-bit bit length.  One block
   * when the remainder leaves room in [56,64), two otherwise. */
  uint8_t tail[128];
  uint32_t rem = len - off;
  fw_zero(tail, sizeof tail);
  for (uint32_t i = 0; i < rem; i++) tail[i] = buf[off + i];
  tail[rem] = 0x80;
  uint32_t tail_len = (rem + 1 + 8 <= 64) ? 64 : 128;
  uint64_t bits = (uint64_t)len * 8u;
  for (int i = 0; i < 8; i++) tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
  sha_block(h, tail);
  if (tail_len == 128) sha_block(h, tail + 64);

  for (int i = 0; i < 8; i++) {
    out[i * 4 + 0] = (uint8_t)(h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)h[i];
  }
  return 0;
}

/* ---- 4b. radix3 -------------------------------------------------------- */

void nv_fw_slide_down(uint8_t *buf, uint32_t buf_len, uint32_t off,
                      uint32_t len) {
  if (!buf || off == 0 || len == 0) return;
  if (off > buf_len || len > buf_len - off) return; /* refuse rather than wrap */
  /* ASCENDING, because the destination is below the source: a high-to-low loop
   * would read bytes the previous iteration already overwrote.  (Got this
   * backwards first; the unit test caught it with buf[0] == 32 instead of 16.) */
  for (uint32_t i = 0; i < len; i++) buf[i] = buf[off + i];
}

uint32_t nv_radix3_pages(uint64_t size) {
  if (size == 0) return 0;
  uint64_t img_pages = (size + NV_RADIX3_PAGE - 1) / NV_RADIX3_PAGE;
  /* Three levels of 512 entries each: 512^3 pages is the ceiling. */
  if (img_pages > (uint64_t)NV_RADIX3_ENTRIES * NV_RADIX3_ENTRIES *
                      NV_RADIX3_ENTRIES)
    return 0;
  uint64_t l2 = (img_pages + NV_RADIX3_ENTRIES - 1) / NV_RADIX3_ENTRIES;
  uint64_t l1 = (l2 + NV_RADIX3_ENTRIES - 1) / NV_RADIX3_ENTRIES;
  uint64_t total = 1 + l1 + l2;
  if (total > 0xFFFFu) return 0; /* a tree this large is a bug, not a card */
  return (uint32_t)total;
}

int nv_radix3_build_into(uint8_t *tree, uint32_t tree_pages, uint64_t tree_phys,
                         uint64_t image_phys, uint64_t image_size) {
  if (!tree || image_size == 0) return -EINVAL;
  if ((tree_phys & (NV_RADIX3_PAGE - 1)) ||
      (image_phys & (NV_RADIX3_PAGE - 1)))
    return -EINVAL;
  uint32_t need = nv_radix3_pages(image_size);
  if (need == 0 || tree_pages < need) return -EINVAL;

  uint64_t img_pages = (image_size + NV_RADIX3_PAGE - 1) / NV_RADIX3_PAGE;
  uint32_t l2 = (uint32_t)((img_pages + NV_RADIX3_ENTRIES - 1) /
                           NV_RADIX3_ENTRIES);
  uint32_t l1 = (l2 + NV_RADIX3_ENTRIES - 1) / NV_RADIX3_ENTRIES;

  /* The tree is zeroed first: an entry beyond the image is 0 ("not present"),
   * never a stale pointer into whatever the frame held before. */
  fw_zero(tree, need * NV_RADIX3_PAGE);

  uint8_t *lvl0 = tree;
  uint8_t *lvl1 = tree + NV_RADIX3_PAGE;
  uint8_t *lvl2p = lvl1 + (uint64_t)l1 * NV_RADIX3_PAGE;

  /* Level 2 first: one PTE per image page, ascending, because the image is a
   * single contiguous run. */
  for (uint64_t i = 0; i < img_pages; i++) {
    uint32_t page = (uint32_t)(i / NV_RADIX3_ENTRIES);
    uint32_t slot = (uint32_t)(i % NV_RADIX3_ENTRIES);
    fw_put64(lvl2p + (uint64_t)page * NV_RADIX3_PAGE + slot * 8,
             image_phys + i * NV_RADIX3_PAGE);
  }
  /* Level 1 points at the level-2 pages, level 0 at the level-1 pages. */
  for (uint32_t j = 0; j < l2; j++)
    fw_put64(lvl1 + (uint64_t)(j / NV_RADIX3_ENTRIES) * NV_RADIX3_PAGE +
                 (j % NV_RADIX3_ENTRIES) * 8,
             (uint64_t)(lvl2p - tree) + tree_phys + (uint64_t)j * NV_RADIX3_PAGE);
  for (uint32_t k = 0; k < l1; k++)
    fw_put64(lvl0 + (uint64_t)k * 8,
             (uint64_t)NV_RADIX3_PAGE + tree_phys + (uint64_t)k * NV_RADIX3_PAGE);
  return 0;
}

#ifdef __x86_64__
int nv_radix3_build(uint64_t image_phys, uint64_t image_size,
                    uint64_t *root_phys, uint32_t *tree_pages) {
  if (!root_phys || !tree_pages) return -EINVAL;
  uint32_t need = nv_radix3_pages(image_size);
  if (need == 0) return -EINVAL;
  uint64_t phys = frame_alloc_contig((int)need);
  if (!phys) return -ENOMEM;
  int rc = nv_radix3_build_into((uint8_t *)phys_to_virt(phys), need, phys,
                                image_phys, image_size);
  if (rc != 0) {
    for (uint32_t i = 0; i < need; i++)
      frame_free(phys + (uint64_t)i * FRAME_SIZE);
    return rc;
  }
  *root_phys = phys;
  *tree_pages = need;
  return 0;
}

void nv_radix3_release(uint64_t root_phys, uint32_t tree_pages) {
  for (uint32_t i = 0; i < tree_pages; i++)
    frame_free(root_phys + (uint64_t)i * FRAME_SIZE);
}
#endif

/* ---- 6. the file loader (x86_64 only) --------------------------------- */

#ifdef __x86_64__

static int fw_build_path(const char *name, char *path, uint32_t cap) {
  const char *pfx = NV_FW_PATH_PREFIX;
  uint32_t i = 0;
  for (; pfx[i]; i++) {
    if (i + 1 >= cap) return -ENAMETOOLONG;
    path[i] = pfx[i];
  }
  for (uint32_t j = 0; name[j]; j++) {
    if (i + 1 >= cap) return -ENAMETOOLONG;
    path[i++] = name[j];
  }
  path[i] = 0;
  return 0;
}

int nv_fw_load_blob(const char *name, struct nv_fw_blob *out) {
  if (!name || !name[0] || !out) return -EINVAL;
  fw_zero(out, sizeof *out);

  char path[64];
  int err = fw_build_path(name, path, sizeof path);
  if (err != 0) return err;

  /* The FAT16 route, the same one the program loader uses for an image it
   * wants contiguous: resolve the dirent, then read the whole file into one
   * run of frames at a physical address. */
  struct fat16_dir_entry e;
  uint32_t sec = 0, eoff = 0;
  if (fat16_resolve_path(path, &e, &sec, &eoff) != 0 || sec == 0)
    return -ENOENT;
  if (e.attr & 0x10) return -EISDIR;
  if (e.file_size == 0) return -EINVAL;
  if (e.file_size > 0x4000000u) return -EINVAL; /* 64 MiB: not a firmware blob */

  uint32_t pages = (e.file_size + FRAME_SIZE - 1) / FRAME_SIZE;
  uint64_t phys = frame_alloc_contig((int)pages);
  if (!phys) return -ENOMEM;

  int got = fat16_read_direct_pos(e.start_cluster, e.file_size, phys,
                                  (int)e.file_size, 0);
  if (got != (int)e.file_size) {
    for (uint32_t i = 0; i < pages; i++)
      frame_free(phys + (uint64_t)i * FRAME_SIZE);
    return -EIO;
  }

  out->phys = phys;
  out->buf = (uint8_t *)phys_to_virt(phys);
  out->size = e.file_size;
  out->pages = pages;
  return 0;
}

int nv_fw_load(const char *name, uint8_t **out, uint32_t *size) {
  if (!out || !size) return -EINVAL;
  struct nv_fw_blob b;
  int err = nv_fw_load_blob(name, &b);
  if (err != 0) return err;
  *out = b.buf;
  *size = b.size;
  return 0;
}

void nv_fw_release(struct nv_fw_blob *b) {
  if (!b || !b->phys) return;
  for (uint32_t i = 0; i < b->pages; i++)
    frame_free(b->phys + (uint64_t)i * FRAME_SIZE);
  fw_zero(b, sizeof *b);
}

void nv_fw_release_buf(uint8_t *buf, uint32_t size) {
  if (!buf || size == 0) return;
  uint64_t phys = virt_to_phys((void *)buf);
  uint32_t pages = (size + FRAME_SIZE - 1) / FRAME_SIZE;
  for (uint32_t i = 0; i < pages; i++)
    frame_free(phys + (uint64_t)i * FRAME_SIZE);
}

#endif /* __x86_64__ */
