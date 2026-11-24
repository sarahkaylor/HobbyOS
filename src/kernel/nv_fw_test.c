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

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
  return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
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

  /* A data region enlarged without the load header following it.  Zero bytes
   * are indistinguishable from real padding (booter_unload carries 0x100 of
   * them), so the parser accepts a zero tail up to one 256-byte block; what it
   * must still reject is a tail that carries data, which is what a header and a
   * payload mixed from two blobs look like. */
  put32(buf + 0x14, 0x700 + 0x100);
  ASSERT(nv_fw_container_parse(buf, len + 0x100, &c) == 0);
  ASSERT(nv_fw_hs_parse(&c, &hs) == 0);
  buf[len + 0x100 - 4] = 0xA5; /* one nonzero byte in the tail */
  ASSERT(nv_fw_hs_parse(&c, &hs) == -EINVAL);
  buf[len + 0x100 - 4] = 0x00;
  put32(buf + 0x14, 0x700);
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
  /* Values from the AD102 / 535.113.01 case in the contract (section 3.3), so
   * the test fails if a field is written to the wrong offset. */
  struct nv_wpr_meta_in in, out;
  zero(&in, sizeof in);
  zero(&out, sizeof out);
  in.sysmem_addr_radix3_elf = 0x0000000123400000ULL;
  in.size_radix3_elf = 0x2448000;
  in.sysmem_addr_bootloader = 0x0000000123500000ULL;
  in.size_bootloader = 0x8000;
  in.bootloader_code_offset = 0x4800;
  in.bootloader_data_offset = 0x800;
  in.sysmem_addr_signature = 0x0000000123600000ULL;
  in.size_signature = 0x1000;
  in.gsp_fw_rsvd_start = 0x5F8600000ULL;
  in.non_wpr_heap_offset = 0x5F8600000ULL;
  in.non_wpr_heap_size = 0x100000;
  in.gsp_fw_wpr_start = 0x5F8700000ULL;
  in.gsp_fw_heap_offset = 0x5F8800000ULL;
  in.gsp_fw_heap_size = 0x3600000;
  in.gsp_fw_offset = 0x5FBEB0000ULL;
  in.boot_bin_offset = 0x5FE2F8000ULL;
  in.frts_offset = 0x5FE300000ULL;
  in.frts_size = 0x100000;
  in.gsp_fw_wpr_end = 0x5FE400000ULL;
  in.fb_size = 0x5FF400000ULL;
  in.vga_workspace_offset = 0x5FE400000ULL;
  in.vga_workspace_size = in.fb_size - in.vga_workspace_offset;
  in.boot_count = 3;
  in.sysmem_addr_crash_queue = 0x0000000123700000ULL;
  in.size_crash_queue = 0x4000;
  in.heap_vf_partition_count = 1;
  in.flags = 2;
  in.paging_config = 0x1234;

  uint8_t meta[NV_WPR_META_SIZE];
  fill(meta, sizeof meta, 0xEE); /* encode must write every byte */
  ASSERT(nv_fw_wpr_meta_encode(&in, meta) == 0);

  /* Spot-check the wire offsets that matter most, independently of decode. */
  ASSERT(le64(meta + 0x10) == in.sysmem_addr_radix3_elf);
  ASSERT(le64(meta + 0x70) == in.gsp_fw_wpr_start);
  ASSERT(le64(meta + 0xA8) == in.gsp_fw_wpr_end);
  ASSERT(le64(meta + 0xB0) == in.fb_size);
  ASSERT(le64(meta + 0xC8) == 3);
  ASSERT(le32(meta + 0xE8) == 0x4000);
  EXPECT_EQ((int)meta[0xF0], 1);
  EXPECT_EQ((int)meta[0xF1], 2);
  ASSERT(le32(meta + 0xF2) == 0x1234);
  /* verified is HOST-written as 0: the booter owns that word. */
  ASSERT(le64(meta + 0xF8) == 0);
  /* Reserved space is zeroed, not left as the caller's stack. */
  EXPECT_EQ((int)meta[0x0C], 0);
  EXPECT_EQ((int)meta[0xFF], 0);

  ASSERT(nv_fw_wpr_meta_decode(meta, &out) == 0);
  ASSERT(out.size_radix3_elf == 0x2448000);
  ASSERT(out.gsp_fw_wpr_start == 0x5F8700000ULL);
  ASSERT(out.gsp_fw_wpr_end == 0x5FE400000ULL);
  ASSERT(out.fb_size == 0x5FF400000ULL);
  ASSERT(out.frts_size == 0x100000);
  ASSERT(out.boot_count == 3);
  EXPECT_EQ((int)out.size_crash_queue, 0x4000);
  EXPECT_EQ((int)out.heap_vf_partition_count, 1);
  EXPECT_EQ((int)out.flags, 2);
  EXPECT_EQ((int)out.paging_config, 0x1234);
}

/* The booter's acceptance proof: it overwrites `verified` with 0xA0A0...  A
 * host-written block must never read as verified, or a boot that never started
 * would look like one that succeeded. */
static void test_nv_fw_wpr_meta_verified(void) {
  uart_puts("  Running test_nv_fw_wpr_meta_verified...\n");
  tests_run++;
  struct nv_wpr_meta_in in, out;
  zero(&in, sizeof in);
  in.fb_size = 0x5FF400000ULL;
  uint8_t meta[NV_WPR_META_SIZE];
  ASSERT(nv_fw_wpr_meta_encode(&in, meta) == 0);
  ASSERT(nv_fw_wpr_meta_decode(meta, &out) == 0);
  EXPECT_EQ(nv_fw_wpr_meta_verified(&out), 0);
  put64(meta + 0xF8, NV_WPR_META_VERIFIED);
  ASSERT(nv_fw_wpr_meta_decode(meta, &out) == 0);
  EXPECT_EQ(nv_fw_wpr_meta_verified(&out), 1);
  /* Almost the magic value is not acceptance. */
  put64(meta + 0xF8, NV_WPR_META_VERIFIED ^ 1u);
  ASSERT(nv_fw_wpr_meta_decode(meta, &out) == 0);
  EXPECT_EQ(nv_fw_wpr_meta_verified(&out), 0);
  EXPECT_EQ(nv_fw_wpr_meta_verified(0), 0);
}

static void test_nv_fw_wpr_meta_errors(void) {
  uart_puts("  Running test_nv_fw_wpr_meta_errors...\n");
  tests_run++;
  struct nv_wpr_meta_in in, out;
  zero(&in, sizeof in);
  in.fb_size = 0x5FF400000ULL;

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

  EXPECT_EQ(nv_fw_wpr_meta_encode(0, meta), -EINVAL);
  EXPECT_EQ(nv_fw_wpr_meta_decode(meta, 0), -EINVAL);
}

/* ---- radix3 ----------------------------------------------------------- */

/* The arithmetic of the tree, against the real image size (38,061,600 B =
 * 9297 pages) and against the boundary where a second level-1 page appears. */
static void test_nv_radix3_pages(void) {
  uart_puts("  Running test_nv_radix3_pages...\n");
  tests_run++;
  /* 38,061,600 B -> 9297 image pages -> 19 level-2 pages, 1 level-1, 1 root. */
  EXPECT_EQ((int)nv_radix3_pages(38061600), 21);
  /* One page, and exactly 512 pages (one full level-2 page). */
  EXPECT_EQ((int)nv_radix3_pages(4096), 3);   /* 1 img -> 1 l2 -> 1 l1 + 1 */
  EXPECT_EQ((int)nv_radix3_pages(512ull * 4096), 3);
  /* 513 pages needs a second level-2 page. */
  EXPECT_EQ((int)nv_radix3_pages(512ull * 4096 + 1), 4);
  /* A size that is not a page multiple still rounds up. */
  EXPECT_EQ((int)nv_radix3_pages(4097), 3);
  /* Nothing to describe, and more than three levels can address. */
  EXPECT_EQ((int)nv_radix3_pages(0), 0);
  EXPECT_EQ((int)nv_radix3_pages(0x10000000000ull), 0); /* 1 TiB of pages */
}

/* A tree that points at the right physical pages, in the right order, with the
 * holes marked absent rather than left as garbage. */
static void test_nv_radix3_build(void) {
  uart_puts("  Running test_nv_radix3_build...\n");
  tests_run++;
  static uint8_t tree[4 * 4096];
  const uint64_t tree_phys = 0x20000000ULL;
  const uint64_t img_phys = 0x30000000ULL;
  const uint64_t img_size = 512ull * 4096; /* exactly one level-2 page */

  fill(tree, sizeof tree, 0xEE);
  ASSERT(nv_radix3_build_into(tree, 4, tree_phys, img_phys, img_size) == 0);

  /* Root: one entry, pointing at the level-1 page (the second page). */
  ASSERT(le64(tree + 0) == tree_phys + 4096);
  EXPECT_EQ((int)le64(tree + 8), 0); /* no second level-1 page */
  /* Level 1: one entry, pointing at the level-2 page (the third page). */
  ASSERT(le64(tree + 4096 + 0) == tree_phys + 8192);
  EXPECT_EQ((int)le64(tree + 4096 + 8), 0);
  /* Level 2: 512 PTEs, ascending, contiguous. */
  for (int i = 0; i < 512; i++)
    ASSERT(le64(tree + 8192 + i * 8) == img_phys + (uint64_t)i * 4096);

  /* A partial last level-2 page leaves the tail zeroed, not stale. */
  fill(tree, sizeof tree, 0xEE);
  ASSERT(nv_radix3_build_into(tree, 4, tree_phys, img_phys, 4096) == 0);
  ASSERT(le64(tree + 8192 + 0) == img_phys);
  EXPECT_EQ((int)le64(tree + 8192 + 8), 0);
  EXPECT_EQ((int)le64(tree + 8192 + 511 * 8), 0);

  /* Refusals: short tree, unaligned addresses, zero size, null. */
  EXPECT_EQ(nv_radix3_build_into(tree, 2, tree_phys, img_phys, img_size),
            -EINVAL);
  EXPECT_EQ(nv_radix3_build_into(tree, 4, tree_phys + 8, img_phys, img_size),
            -EINVAL);
  EXPECT_EQ(nv_radix3_build_into(tree, 4, tree_phys, img_phys + 1, img_size),
            -EINVAL);
  EXPECT_EQ(nv_radix3_build_into(tree, 4, tree_phys, img_phys, 0), -EINVAL);
  EXPECT_EQ(nv_radix3_build_into(0, 4, tree_phys, img_phys, img_size), -EINVAL);
}

/* The slide that page-aligns .fwimage: overalloc-safe on overlap, and a refusal
 * (not a wrap) when the request does not fit the buffer. */
static void test_nv_fw_slide_down(void) {
  uart_puts("  Running test_nv_fw_slide_down...\n");
  tests_run++;
  uint8_t buf[64];
  for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
  /* Move the 32 bytes at +16 down to 0: overlap of 16 bytes. */
  nv_fw_slide_down(buf, sizeof buf, 16, 32);
  for (int i = 0; i < 32; i++) EXPECT_EQ((int)buf[i], i + 16);
  /* Off 0 is a no-op, and an impossible request changes nothing. */
  for (int i = 0; i < 64; i++) buf[i] = 0xAA;
  nv_fw_slide_down(buf, sizeof buf, 0, 8);
  EXPECT_EQ((int)buf[0], 0xAA);
  nv_fw_slide_down(buf, sizeof buf, 60, 8); /* runs off the end */
  EXPECT_EQ((int)buf[0], 0xAA);
  nv_fw_slide_down(0, 64, 16, 8); /* null */
}

/* The VRAM layout, against the contract's own AD102 table: every field must
 * reproduce it except gspFwOffset, where the document's numbers leave a gap and
 * this arithmetic tiles exactly (see the comment in nv_fw.h). */
static void test_nv_wpr_layout(void) {
  uart_puts("  Running test_nv_wpr_layout...\n");
  tests_run++;
  struct nv_wpr_layout_in in;
  struct nv_wpr_layout l;
  in.wpr_start = 0x5F8700000ULL; /* the contract's AD102 value */
  in.fb_size = 0x5FF400000ULL;   /* 24564 MiB */
  in.image_size = 0x2448000ULL;
  ASSERT(nv_wpr_layout(&in, &l) == 0);
  ASSERT(l.wpr_end == 0x5FE400000ULL);
  ASSERT(l.frts_offset == 0x5FE300000ULL);
  ASSERT(l.frts_size == 0x100000);
  ASSERT(l.boot_bin_offset == 0x5FE2F8000ULL);
  ASSERT(l.heap_offset == 0x5F8800000ULL);
  ASSERT(l.non_wpr_heap_offset == 0x5F8700000ULL);
  ASSERT(l.non_wpr_heap_size == 0x100000);
  ASSERT(l.image_offset == 0x5FBEB0000ULL); /* the contract's own value */
  ASSERT(l.image_offset + in.image_size == l.boot_bin_offset);
  ASSERT(l.heap_size == 0x36B0000ULL);
  ASSERT(l.wpr_end - l.wpr_start == 0x5D00000ULL); /* 93 MiB */
  uint64_t named = l.non_wpr_heap_size + l.heap_size + in.image_size +
                   (l.boot_bin_offset - l.image_offset - in.image_size) +
                   (l.frts_offset - l.boot_bin_offset) + l.frts_size;
  ASSERT(named == 0x5D00000ULL);

  /* A region too small for the image is refused, not squeezed.  The start has
   * to be high enough that the 1 MiB non-wpr heap runs INTO the image: with the
   * start one MiB lower the layout still fits, just with a 0xB0000 heap -- which
   * is how a first attempt at this case managed to "pass" the wrong way. */
  struct nv_wpr_layout_in tight = in;
  tight.wpr_start = (l.image_offset + 0x100000) & ~0xFFFFFULL;
  EXPECT_EQ(nv_wpr_layout(&tight, &l), -ENOSPC);
  /* Nonsense in, refusal out. */
  struct nv_wpr_layout_in bad = in;
  bad.fb_size = 0;
  EXPECT_EQ(nv_wpr_layout(&bad, &l), -EINVAL);
  bad = in;
  bad.wpr_start += 0x800; /* not 1 MiB aligned */
  EXPECT_EQ(nv_wpr_layout(&bad, &l), -EINVAL);
  EXPECT_EQ(nv_wpr_layout(0, &l), -EINVAL);
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
  test_nv_fw_wpr_meta_verified();
  test_nv_radix3_pages();
  test_nv_radix3_build();
  test_nv_fw_slide_down();
  test_nv_wpr_layout();
  test_nv_fw_sha256_nist();
  test_nv_fw_load_missing();
}

#endif /* KERNEL_MODE_UNIT_TEST */
#endif /* __x86_64__ */
