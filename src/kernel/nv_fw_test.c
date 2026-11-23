/* ============================================================================
 *  nv_fw_test: kernel unit-tier coverage for the firmware parsers
 *  (src/kernel/nv_fw.c).
 *
 *  Every case here builds its input bytes in the test.  No firmware is
 *  embedded and none is needed: the formats are the subject, and the values
 *  used are the ones measured on the real 535.113.01 blobs (the two AD102 /
 *  GA102 booter shapes and the RM image's section map), so a change to the
 *  parser that would reject the real files fails here first.
 *
 *  What this tier cannot prove: that the GPU agrees.  The parsers are pure,
 *  so their verdicts here are the same verdicts the boot path gets -- but a
 *  layout hypothesis (the WPR metadata block) round-trips whether or not it
 *  matches the reference driver.  The unknowns are listed at the top of
 *  nv_fw.c; this file only proves the codec is self-consistent and the
 *  validation is not vacuous.
 * ========================================================================== */

#ifdef __x86_64__
#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "nv_fw.h"
#include "errno.h"

/* ---- byte helpers for building inputs --------------------------------- */

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

static void zero(void *p, uint32_t n) {
  uint8_t *b = (uint8_t *)p;
  for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

/* One synthetic container, sized to `dsz` bytes of data.

 *   data layout (offsets relative to the data region):
 *     os code   [0,        0x100)
 *     app code  [0x100,    0x100 + acsz)
 *     os data   [osd_off,  osd_off + osd_sz)
 *   and the load header's sizes are chosen to sum to `dsz` by the caller.
 */
static uint32_t build_container(uint8_t *buf, uint32_t cap, uint32_t dsz,
                                uint32_t osd_off, uint32_t osd_sz,
                                uint32_t acsz) {
  uint32_t data_off = 0x378;
  uint32_t len = data_off + dsz;
  if (len > cap) return 0;
  zero(buf, len);

  put32(buf + 0x00, NV_FW_CONTAINER_MAGIC);
  put32(buf + 0x04, NV_FW_CONTAINER_VERSION);
  put32(buf + 0x08, dsz + 0x400); /* measured: bin_size = data_size + 0x400 */
  put32(buf + 0x0C, 0x18);        /* hdr_off */
  put32(buf + 0x10, data_off);
  put32(buf + 0x14, dsz);

  /* 9-dword HS header at 0x18: the measured AD102 values, absolute offsets. */
  uint8_t *h = buf + 0x18;
  put32(h + 0x00, 0x3C);  /* production signature offset */
  put32(h + 0x04, 0x300); /* production signature size */
  put32(h + 0x08, 0x33C); /* patch location */
  put32(h + 0x0C, 0x340); /* patch signature offset */
  put32(h + 0x10, 0x344); /* device mask offset */
  put32(h + 0x14, 0x00C); /* device mask size */
  put32(h + 0x18, 0x350); /* offset OF the signature count */
  put32(h + 0x1C, 0x354); /* load header offset */
  put32(h + 0x20, 0x024); /* load header size */
  for (uint32_t i = 0x3C; i < 0x33C; i++) buf[i] = 0xAA; /* the signature */
  put32(buf + 0x344, 1); /* device mask: three dwords, as measured */
  put32(buf + 0x348, 1);
  put32(buf + 0x34C, 3);
  put32(buf + 0x350, 2); /* signature count */

  /* Load header: os code, os data, one app. */
  uint8_t *l = buf + 0x354;
  put32(l + 0x00, 0x00);
  put32(l + 0x04, 0x100);
  put32(l + 0x08, osd_off);
  put32(l + 0x0C, osd_sz);
  put32(l + 0x10, 1);
  put32(l + 0x14, 0x100); /* app code offset */
  put32(l + 0x18, acsz);
  put32(l + 0x1C, 0x100); /* app data offset */
  put32(l + 0x20, 0x00);  /* app data size */
  return len;
}

static void fill(uint8_t *p, uint32_t n, uint8_t v) {
  for (uint32_t i = 0; i < n; i++) p[i] = v;
}

/* ---- container -------------------------------------------------------- */

static void test_nv_fw_container_parse(void) {
  uart_puts("  Running test_nv_fw_container_parse...\n");
  tests_run++;
  static uint8_t buf[0xA80];
  /* 0x100 os code + 0x300 os data + 0x300 app code = 0x700. */
  uint32_t len = build_container(buf, 0xA78, 0x700, 0x400, 0x300, 0x300);
  ASSERT(len == 0xA78);

  struct nv_fw_container c;
  ASSERT(nv_fw_container_parse(buf, len, &c) == 0);
  ASSERT(c.magic == NV_FW_CONTAINER_MAGIC);
  EXPECT_EQ((int)c.version, (int)NV_FW_CONTAINER_VERSION);
  EXPECT_EQ((int)c.hdr_off, 0x18);
  EXPECT_EQ((int)c.data_off, 0x378);
  EXPECT_EQ((int)c.data_size, 0x700);
  EXPECT_EQ((int)c.bin_size, 0xB00);
  EXPECT_EQ((int)c.desc_bytes, 0x360);
  EXPECT_EQ((int)c.trailing, 0);
  /* The pointers are the buffer, rebased once: hdr at 0x18, data at 0x378. */
  ASSERT(c.buf == buf);
  ASSERT(c.hdr == buf + 0x18);
  ASSERT(c.data == buf + 0x378);
  ASSERT(c.data[0] == 0); /* the data region was zeroed by the builder */
}

static void test_nv_fw_container_reject_magic(void) {
  uart_puts("  Running test_nv_fw_container_reject_magic...\n");
  tests_run++;
  static uint8_t buf[0xB80];
  uint32_t len = build_container(buf, 0xA78, 0x700, 0x400, 0x300, 0x300);
  ASSERT(len != 0);
  struct nv_fw_container c;

  put32(buf + 0x00, 0x10DF); /* one bit off the vendor id */
  EXPECT_EQ(nv_fw_container_parse(buf, len, &c), -EINVAL);
  put32(buf + 0x00, NV_FW_CONTAINER_MAGIC);

  put32(buf + 0x04, 2); /* unknown container version */
  EXPECT_EQ(nv_fw_container_parse(buf, len, &c), -EINVAL);
  put32(buf + 0x04, NV_FW_CONTAINER_VERSION);

  /* Short of the six-dword header entirely. */
  EXPECT_EQ(nv_fw_container_parse(buf, 20, &c), -EINVAL);
  EXPECT_EQ(nv_fw_container_parse(buf, len, 0), -EINVAL);
  EXPECT_EQ(nv_fw_container_parse(0, len, &c), -EINVAL);
}

/* The failure a half-copied file produces: the declared data region runs off
 * the end of the buffer.  Rejecting it is the difference between a clear
 * -EINVAL and a booter started on a partial image. */
static void test_nv_fw_container_reject_truncation(void) {
  uart_puts("  Running test_nv_fw_container_reject_truncation...\n");
  tests_run++;
  static uint8_t buf[0xB80];
  uint32_t len = build_container(buf, 0xA78, 0x700, 0x400, 0x300, 0x300);
  ASSERT(len != 0);
  struct nv_fw_container c;
  EXPECT_EQ(nv_fw_container_parse(buf, len - 1, &c), -EINVAL);
  EXPECT_EQ(nv_fw_container_parse(buf, 0x378, &c), -EINVAL); /* no data at all */
  /* Extra trailing bytes are tolerated and reported, not silently dropped. */
  buf[len] = 0x5A;
  ASSERT(nv_fw_container_parse(buf, len + 1, &c) == 0);
  EXPECT_EQ((int)c.trailing, 1);
}

/* The header describing itself inconsistently: data_off before hdr_off, or a
 * bin_size that cannot cover its own data region. */
static void test_nv_fw_container_reject_bad_geometry(void) {
  uart_puts("  Running test_nv_fw_container_reject_bad_geometry...\n");
  tests_run++;
  static uint8_t buf[0xB80];
  uint32_t len = build_container(buf, 0xA78, 0x700, 0x400, 0x300, 0x300);
  ASSERT(len != 0);
  struct nv_fw_container c;

  put32(buf + 0x10, 0x10); /* data_off inside the container header */
  EXPECT_EQ(nv_fw_container_parse(buf, len, &c), -EINVAL);
  put32(buf + 0x10, 0x378);

  put32(buf + 0x08, 0x700 - 1); /* bin_size smaller than its own data */
  EXPECT_EQ(nv_fw_container_parse(buf, len, &c), -EINVAL);
  put32(buf + 0x08, 0xB00);

  put32(buf + 0x08, 0x700 + 0x2000); /* a different container shape */
  EXPECT_EQ(nv_fw_container_parse(buf, len, &c), -EINVAL);
}

/* ---- HS header -------------------------------------------------------- */

static void test_nv_fw_hs_parse(void) {
  uart_puts("  Running test_nv_fw_hs_parse...\n");
  tests_run++;
  static uint8_t buf[0xB80];
  uint32_t len = build_container(buf, 0xA78, 0x700, 0x400, 0x300, 0x300);
  ASSERT(len != 0);
  struct nv_fw_container c;
  ASSERT(nv_fw_container_parse(buf, len, &c) == 0);

  struct nv_fw_hs hs;
  ASSERT(nv_fw_hs_parse(&c, &hs) == 0);
  EXPECT_EQ((int)hs.sig_prod_off, 0x3C);
  EXPECT_EQ((int)hs.sig_prod_size, 0x300);
  EXPECT_EQ((int)hs.patch_loc, 0x33C);
  EXPECT_EQ((int)hs.patch_sig, 0x340);
  EXPECT_EQ((int)hs.meta_off, 0x344);
  EXPECT_EQ((int)hs.meta_size, 0x0C);
  EXPECT_EQ((int)hs.loadhdr_off, 0x354);
  EXPECT_EQ((int)hs.loadhdr_size, 0x24);
  /* dword 6 is an offset; the count is what lives at it. */
  EXPECT_EQ((int)hs.num_sig_off, 0x350);
  EXPECT_EQ((int)hs.num_sig, 2);

  EXPECT_EQ((int)hs.os.code_off, 0x00);
  EXPECT_EQ((int)hs.os.code_size, 0x100);
  EXPECT_EQ((int)hs.os.data_off, 0x400);
  EXPECT_EQ((int)hs.os.data_size, 0x300);
  EXPECT_EQ((int)hs.num_apps, 1);
  EXPECT_EQ((int)hs.app[0].code_off, 0x100);
  EXPECT_EQ((int)hs.app[0].code_size, 0x300);
  EXPECT_EQ((int)hs.app[0].data_off, 0x100);
  EXPECT_EQ((int)hs.app[0].data_size, 0x00);
  /* The convergence assertion the parser exists for. */
  EXPECT_EQ((int)hs.total_size, 0x700);
  EXPECT_EQ((int)hs.total_size, (int)c.data_size);
}

/* A container whose load header does not cover its data region is either a
 * booter for a different chip or two blobs mixed together.  Both must be
 * refused: loading either leaves the GPU's processor with an image that
 * cannot be authenticated and no host-visible reason why. */
static void test_nv_fw_hs_reject_size_mismatch(void) {
  uart_puts("  Running test_nv_fw_hs_reject_size_mismatch...\n");
  tests_run++;
  static uint8_t buf[0xB80];
  uint32_t len = build_container(buf, 0xA78, 0x700, 0x400, 0x300, 0x300);
  ASSERT(len != 0);
  struct nv_fw_container c;
  struct nv_fw_hs hs;

  /* One byte more of app code than the data region holds. */
  put32(buf + 0x354 + 0x18, 0x301);
  ASSERT(nv_fw_container_parse(buf, len, &c) == 0);
  EXPECT_EQ(nv_fw_hs_parse(&c, &hs), -EINVAL);

  /* And one byte less (the sum undershoots). */
  put32(buf + 0x354 + 0x18, 0x2FF);
  ASSERT(nv_fw_container_parse(buf, len, &c) == 0);
  EXPECT_EQ(nv_fw_hs_parse(&c, &hs), -EINVAL);
  put32(buf + 0x354 + 0x18, 0x300);

  /* A data region enlarged without the load header following it. */
  put32(buf + 0x14, 0x700 + 0x100);
  ASSERT(nv_fw_container_parse(buf, len + 0x100, &c) == 0);
  EXPECT_EQ(nv_fw_hs_parse(&c, &hs), -EINVAL);
}

static void test_nv_fw_hs_reject_bad_offsets(void) {
  uart_puts("  Running test_nv_fw_hs_reject_bad_offsets...\n");
  tests_run++;
  static uint8_t buf[0xB80];
  uint32_t len = build_container(buf, 0xA78, 0x700, 0x400, 0x300, 0x300);
  ASSERT(len != 0);
  struct nv_fw_container c;
  struct nv_fw_hs hs;

  /* num_apps that the declared load-header size cannot hold. */
  put32(buf + 0x354 + 0x10, 3);
  ASSERT(nv_fw_container_parse(buf, len, &c) == 0);
  EXPECT_EQ(nv_fw_hs_parse(&c, &hs), -EINVAL);
  put32(buf + 0x354 + 0x10, 1);

  /* num_apps beyond this parser's array. */
  put32(buf + 0x354 + 0x10, NV_FW_APP_MAX + 1);
  EXPECT_EQ(nv_fw_hs_parse(&c, &hs), -EINVAL);
  put32(buf + 0x354 + 0x10, 1);

  /* A load header that starts outside the descriptor region. */
  put32(buf + 0x18 + 0x1C, 0x800);
  EXPECT_EQ(nv_fw_hs_parse(&c, &hs), -EINVAL);
  put32(buf + 0x18 + 0x1C, 0x354);

  /* A patch-location field outside the descriptor region. */
  put32(buf + 0x18 + 0x08, 0x800);
  EXPECT_EQ(nv_fw_hs_parse(&c, &hs), -EINVAL);
  put32(buf + 0x18 + 0x08, 0x33C);
  /* 0 means "no patch field at all", which is legal. */
  put32(buf + 0x18 + 0x08, 0);
  EXPECT_EQ(nv_fw_hs_parse(&c, &hs), 0);
  put32(buf + 0x18 + 0x08, 0x33C);

  /* A production signature that runs past the data region. */
  put32(buf + 0x18 + 0x04, 0x2000);
  EXPECT_EQ(nv_fw_hs_parse(&c, &hs), -EINVAL);
  put32(buf + 0x18 + 0x04, 0x300);

  /* A null container. */
  EXPECT_EQ(nv_fw_hs_parse(0, &hs), -EINVAL);
}

/* ---- ELF -------------------------------------------------------------- */

#define ELF_SHNUM 5
#define ELF_SHOFF 0xA0
#define ELF_SHSTR 0x68
#define ELF_FWIMAGE_OFF 0x40u
#define ELF_FWVERSION_OFF 0x50u
#define ELF_SIG_OFF 0x60u

/* A 0x200-byte ELF64 with five sections: null, .fwimage, .fwversion,
 * .fwsignature_ad10x, .shstrtab.  The header fields are the real image's
 * (ET_REL, EM_RISCV, 64-byte section headers). */
static uint32_t build_elf(uint8_t *buf) {
  uint32_t len = 0x200;
  zero(buf, len);

  buf[0] = 0x7F;
  buf[1] = 'E';
  buf[2] = 'L';
  buf[3] = 'F';
  buf[4] = 2; /* ELFCLASS64 */
  buf[5] = 1; /* ELFDATA2LSB */
  buf[6] = 1; /* EV_CURRENT */
  put16(buf + 0x10, 1);   /* ET_REL */
  put16(buf + 0x12, 243); /* EM_RISCV */
  put32(buf + 0x14, 1);   /* e_version */
  put64(buf + 0x28, ELF_SHOFF);
  put16(buf + 0x34, 64); /* e_ehsize */
  put16(buf + 0x3A, 64); /* e_shentsize */
  put16(buf + 0x3C, ELF_SHNUM);
  put16(buf + 0x3E, 4); /* e_shstrndx = .shstrtab */

  /* The string table. */
  /* Offsets into this table: 1 = .fwimage, 10 = .fwversion,
   * 21 = .fwsignature_ad10x, 40 = .shstrtab. */
  const char *names = "\0.fwimage\0.fwversion\0.fwsignature_ad10x\0.shstrtab";
  uint32_t nl = 1 + 8 + 1 + 10 + 1 + 18 + 1 + 9 + 1;
  for (uint32_t i = 0; i < nl; i++) buf[ELF_SHSTR + i] = (uint8_t)names[i];

  fill(buf + ELF_FWIMAGE_OFF, 16, 0xC3);
  const char *ver = "535.113.01";
  for (uint32_t i = 0; i < 11; i++) buf[ELF_FWVERSION_OFF + i] = (uint8_t)ver[i];
  fill(buf + ELF_SIG_OFF, 8, 0x7E);

  /* Written by index so the offsets above stay readable. */
  uint8_t *s = buf + ELF_SHOFF;
  /* 0: the null section stays zero. */
  put32(s + 64 + 0x00, 1);
  put32(s + 64 + 0x04, 1);
  put64(s + 64 + 0x18, ELF_FWIMAGE_OFF);
  put64(s + 64 + 0x20, 16);
  put32(s + 128 + 0x00, 10);
  put32(s + 128 + 0x04, 1);
  put64(s + 128 + 0x18, ELF_FWVERSION_OFF);
  put64(s + 128 + 0x20, 11);
  put32(s + 192 + 0x00, 21);
  put32(s + 192 + 0x04, 1);
  put64(s + 192 + 0x18, ELF_SIG_OFF);
  put64(s + 192 + 0x20, 8);
  put32(s + 256 + 0x00, 40);
  put32(s + 256 + 0x04, 3);
  put64(s + 256 + 0x18, ELF_SHSTR);
  put64(s + 256 + 0x20, nl);
  return len;
}

static void test_nv_fw_elf_sections(void) {
  uart_puts("  Running test_nv_fw_elf_sections...\n");
  tests_run++;
  static uint8_t buf[0x200];
  uint32_t len = build_elf(buf);

  struct nv_fw_elf e;
  ASSERT(nv_fw_elf_sections(buf, len, &e) == 0);
  EXPECT_EQ((int)e.type, 1);
  EXPECT_EQ((int)e.machine, 243);
  EXPECT_EQ((int)e.shnum, ELF_SHNUM);
  EXPECT_EQ((int)e.shentsize, 64);
  EXPECT_EQ((int)e.shstrndx, 4);
  EXPECT_EQ((int)e.fwimage.off, (int)ELF_FWIMAGE_OFF);
  EXPECT_EQ((int)e.fwimage.size, 16);
  EXPECT_EQ((int)e.fwversion.off, (int)ELF_FWVERSION_OFF);
  EXPECT_EQ((int)e.fwversion.size, 11);
  ASSERT(e.nsig == 1);
  ASSERT(e.sig[0].name[0] == '.');
  EXPECT_EQ((int)e.sig[0].off, (int)ELF_SIG_OFF);

  /* The name is stored so a log row can say which section was used. */
  const char *want = ".fwsignature_ad10x";
  for (int i = 0; i < 19; i++) { /* 18 characters and the NUL */
    ASSERT(e.sig[0].name[i] == want[i]);
  }

  const struct nv_fw_section *p = 0;
  ASSERT(nv_fw_elf_sig_for_chipset(&e, 0x192, &p) == 0);
  ASSERT(p == &e.sig[0]);
  /* An image with no GA10x signature says so; it does not pick another. */
  EXPECT_EQ(nv_fw_elf_sig_for_chipset(&e, 0x172, &p), -ENOENT);
  /* And an unverified chipset is refused outright. */
  EXPECT_EQ(nv_fw_elf_sig_for_chipset(&e, 0x999, &p), -ENOTSUP);
  EXPECT_EQ(nv_fw_elf_sig_lookup(&e, "ga10x", &p), -ENOENT);
}

static void test_nv_fw_elf_reject(void) {
  uart_puts("  Running test_nv_fw_elf_reject...\n");
  tests_run++;
  static uint8_t buf[0x200];
  uint32_t len = build_elf(buf);
  struct nv_fw_elf e;

  buf[1] = 'X'; /* not an ELF */
  EXPECT_EQ(nv_fw_elf_sections(buf, len, &e), -EINVAL);
  buf[1] = 'E';

  put16(buf + 0x12, 62); /* EM_X86_64: a host ELF, not the RM image */
  EXPECT_EQ(nv_fw_elf_sections(buf, len, &e), -EINVAL);
  put16(buf + 0x12, 243);

  put16(buf + 0x10, 2); /* ET_EXEC: nothing here is relocatable */
  EXPECT_EQ(nv_fw_elf_sections(buf, len, &e), -EINVAL);
  put16(buf + 0x10, 1);

  put16(buf + 0x3A, 40); /* ELF32-sized section headers are not this format */
  EXPECT_EQ(nv_fw_elf_sections(buf, len, &e), -EINVAL);
  put16(buf + 0x3A, 64);

  put64(buf + 0x28, len - 32); /* section table past the end */
  EXPECT_EQ(nv_fw_elf_sections(buf, len - 32, &e), -EINVAL);
  put64(buf + 0x28, ELF_SHOFF);

  put16(buf + 0x3E, ELF_SHNUM); /* shstrndx == shnum */
  EXPECT_EQ(nv_fw_elf_sections(buf, len, &e), -EINVAL);
  put16(buf + 0x3E, 4);

  /* Without .fwversion this is not the RM image, whatever else it is. */
  put32(buf + ELF_SHOFF + 128 + 0x00, 1); /* .fwversion's name -> .fwimage */
  EXPECT_EQ(nv_fw_elf_sections(buf, len, &e), -EINVAL);
  put32(buf + ELF_SHOFF + 128 + 0x00, 10);
}

/* ---- WPR metadata ----------------------------------------------------- */

static void test_nv_fw_wpr_meta_roundtrip(void) {
  uart_puts("  Running test_nv_fw_wpr_meta_roundtrip...\n");
  tests_run++;
  struct nv_wpr_meta_in in, out;
  zero(&in, sizeof in);
  zero(&out, sizeof out);
  in.fb_size = 0x600000000ULL; /* 24 GiB */
  in.wpr_start = 0x5FFC00000ULL;
  in.wpr_size = 0x400000;
  in.num_regions = 2;
  in.region[0].start_offset = 0x100000;
  in.region[0].size = 0x300000;
  in.region[1].start_offset = 0x400000;
  in.region[1].size = 0x80000;
  in.region[1].reserved = 0x12345678;

  uint8_t meta[NV_WPR_META_SIZE];
  fill(meta, sizeof meta, 0xEE); /* encode must write every byte */
  ASSERT(nv_fw_wpr_meta_encode(&in, meta) == 0);

  /* The two fields that ARE established, checked at their offsets. */
  for (int i = 0; i < 8; i++) {
    uint8_t want = (uint8_t)(NV_WPR_META_MAGIC >> (8 * i));
    ASSERT(meta[i] == want);
  }
  EXPECT_EQ((int)meta[8], (int)NV_WPR_META_REVISION);
  EXPECT_EQ((int)meta[9], 0);
  /* Reserved space is zeroed, not left as the caller's stack. */
  EXPECT_EQ((int)meta[0x0C], 0);
  EXPECT_EQ((int)meta[0xFF], 0);

  ASSERT(nv_fw_wpr_meta_decode(meta, &out) == 0);
  ASSERT(out.fb_size == in.fb_size);
  ASSERT(out.wpr_start == in.wpr_start);
  EXPECT_EQ((int)out.wpr_size, (int)in.wpr_size);
  EXPECT_EQ((int)out.num_regions, 2);
  ASSERT(out.region[0].start_offset == in.region[0].start_offset);
  EXPECT_EQ((int)out.region[0].size, 0x300000);
  ASSERT(out.region[1].start_offset == 0x400000);
  EXPECT_EQ((int)out.region[1].size, 0x80000);
  EXPECT_EQ((int)out.region[1].reserved, 0x12345678);
}

static void test_nv_fw_wpr_meta_errors(void) {
  uart_puts("  Running test_nv_fw_wpr_meta_errors...\n");
  tests_run++;
  struct nv_wpr_meta_in in, out;
  zero(&in, sizeof in);
  in.num_regions = 1;
  in.region[0].size = 0x1000;

  uint8_t meta[NV_WPR_META_SIZE];
  /* A short buffer is refused before a byte is written. */
  fill(meta, sizeof meta, 0xEE);
  EXPECT_EQ(nv_fw_wpr_meta_encode_n(&in, meta, NV_WPR_META_SIZE - 1), -ENOSPC);
  EXPECT_EQ((int)meta[0], 0xEE);
  /* The same encoder accepts a larger buffer than it needs. */
  uint8_t big[NV_WPR_META_SIZE + 8];
  fill(big, sizeof big, 0xEE);
  ASSERT(nv_fw_wpr_meta_encode_n(&in, big, sizeof big) == 0);
  EXPECT_EQ((int)big[NV_WPR_META_SIZE], 0xEE); /* never written past 256 */
  EXPECT_EQ(nv_fw_wpr_meta_decode_n(big, NV_WPR_META_SIZE - 1, &out), -ENOSPC);

  /* Magic and revision are the identity of the block. */
  ASSERT(nv_fw_wpr_meta_encode(&in, meta) == 0);
  meta[0] ^= 0x01;
  EXPECT_EQ(nv_fw_wpr_meta_decode(meta, &out), -EINVAL);
  meta[0] ^= 0x01;
  meta[8] = 2; /* a revision from the future is not ours to interpret */
  EXPECT_EQ(nv_fw_wpr_meta_decode(meta, &out), -EINVAL);
  meta[8] = (uint8_t)NV_WPR_META_REVISION;

  /* A region count beyond the array must not be walked. */
  put32(meta + 0x24, NV_WPR_META_MAX_REGIONS + 1);
  EXPECT_EQ(nv_fw_wpr_meta_decode(meta, &out), -EINVAL);
  put32(meta + 0x24, 1);

  /* Encode refuses what decode refuses. */
  in.num_regions = NV_WPR_META_MAX_REGIONS + 1;
  EXPECT_EQ(nv_fw_wpr_meta_encode(&in, meta), -EINVAL);
  EXPECT_EQ(nv_fw_wpr_meta_encode(0, meta), -EINVAL);
  EXPECT_EQ(nv_fw_wpr_meta_decode(meta, 0), -EINVAL);
}

static void test_nv_fw_wpr_base(void) {
  uart_puts("  Running test_nv_fw_wpr_base...\n");
  tests_run++;
  /* Top-down from the end of VRAM, 128 KiB aligned. */
  ASSERT(nv_fw_wpr_base(0x600000000ULL, 0x400000) == 0x5FFC00000ULL);
  /* An unaligned size costs the alignment, and the result stays inside. */
  uint64_t b = nv_fw_wpr_base(0x600000000ULL, 0x400001);
  ASSERT((b & (NV_WPR_ALIGN - 1)) == 0);
  ASSERT(b + 0x400001 <= 0x600000000ULL);
  /* Impossible requests are refused rather than wrapped. */
  ASSERT(nv_fw_wpr_base(0x1000, 0x2000) == 0);
  ASSERT(nv_fw_wpr_base(0x100000, 0) == 0);
}

/* ---- SHA-256 ---------------------------------------------------------- */

static void sha_hex(const uint8_t *buf, uint32_t len, char *out) {
  static const char *d = "0123456789abcdef";
  uint8_t h[32];
  ASSERT(nv_fw_sha256(buf, len, h) == 0);
  for (int i = 0; i < 32; i++) {
    out[i * 2] = d[h[i] >> 4];
    out[i * 2 + 1] = d[h[i] & 0xF];
  }
  out[64] = 0;
}

static void test_nv_fw_sha256_nist(void) {
  uart_puts("  Running test_nv_fw_sha256_nist...\n");
  tests_run++;
  char got[65];

  /* FIPS 180-4 / NIST CAVP, the empty message. */
  sha_hex((const uint8_t *)"", 0, got);
  const char *empty =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  for (int i = 0; i < 64; i++) ASSERT(got[i] == empty[i]);

  /* The one-block vector. */
  sha_hex((const uint8_t *)"abc", 3, got);
  const char *abc =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  for (int i = 0; i < 64; i++) ASSERT(got[i] == abc[i]);

  /* The two-block vector: exercises the padding when the remainder is 56..63
   * bytes, the case a naive implementation gets wrong. */
  const char *v2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  uint32_t n2 = 56;
  sha_hex((const uint8_t *)v2, n2, got);
  const char *want2 =
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
  for (int i = 0; i < 64; i++) ASSERT(got[i] == want2[i]);

  /* A one-byte message, and the argument guard. */
  uint8_t one = 'a';
  ASSERT(nv_fw_sha256(&one, 1, (uint8_t *)got) == 0);
  EXPECT_EQ(nv_fw_sha256(&one, 1, 0), -EINVAL);
}

/* ---- file loader ------------------------------------------------------ */

/* The one entry that touches the filesystem.  This tier has no firmware
 * staged, so the value here is the failure path: a missing blob must be a
 * named error, not a null pointer the boot path dereferences. */
static void test_nv_fw_load_missing(void) {
  uart_puts("  Running test_nv_fw_load_missing...\n");
  tests_run++;
  uint8_t dummy = 0;
  uint8_t *buf = &dummy;
  uint32_t size = 0x5A5A;
  EXPECT_EQ(nv_fw_load("not-present-535.113.01.bin", &buf, &size), -ENOENT);
  EXPECT_EQ((int)size, 0x5A5A); /* untouched on failure */
  uint8_t *p = 0;
  EXPECT_EQ(nv_fw_load(0, &p, &size), -EINVAL);
  EXPECT_EQ(nv_fw_load("x.bin", 0, &size), -EINVAL);

  struct nv_fw_blob b;
  EXPECT_EQ(nv_fw_load_blob("not-present-535.113.01.bin", &b), -ENOENT);
  /* Releasing nothing is a no-op, not a fault. */
  nv_fw_release(&b);
  nv_fw_release_buf(0, 0);
}

void nv_fw_test_suite(void) {
  uart_puts("[NVFW] firmware parser suite\n");
  test_nv_fw_container_parse();
  test_nv_fw_container_reject_magic();
  test_nv_fw_container_reject_truncation();
  test_nv_fw_container_reject_bad_geometry();
  test_nv_fw_hs_parse();
  test_nv_fw_hs_reject_size_mismatch();
  test_nv_fw_hs_reject_bad_offsets();
  test_nv_fw_elf_sections();
  test_nv_fw_elf_reject();
  test_nv_fw_wpr_meta_roundtrip();
  test_nv_fw_wpr_meta_errors();
  test_nv_fw_wpr_base();
  test_nv_fw_sha256_nist();
  test_nv_fw_load_missing();
}

#endif /* KERNEL_MODE_UNIT_TEST */
#endif /* __x86_64__ */
