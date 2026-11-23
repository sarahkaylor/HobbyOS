/* ============================================================================
 *  nv_flcn_test: kernel unit-tier coverage for src/kernel/nv_flcn.c.
 *
 *  Each case builds its input in the test: no firmware is embedded, and none
 *  is needed, because the subject is the encoding.  The values chosen are the
 *  ones that actually bit this driver on the card -- a 256-byte block whose
 *  SIZE field was written as 8 (masking to 0, i.e. four bytes per transfer),
 *  and a destination that never advanced -- plus the DMEM layout measured off
 *  this card's VBIOS (appif at 0x1C, DMEMMAPPER at 0xAE0, command buffer at
 *  0xD40 of a 0xD80-byte DMEM image).
 *
 *  What this tier cannot prove: that the GPU accepts the encoding.  It proves
 *  the encoder says what the contract says, which is the half a card run
 *  cannot debug cheaply.
 * ========================================================================== */

#ifdef __x86_64__
#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "nv_flcn.h"
#include "errno.h"

static void tput32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t tget32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* A DMEM image with this card's appif geometry: header at 0x1C
 * {01 04 08 02}, entry 0 = DMEMMAPPER at 0xAE0, entry 1 = id 5 at 0xD2C, and
 * the DMEMMAPPER block at 0xAE0 carrying "DMAP" v3 size 0x40 with a 0x40-byte
 * command buffer at 0xD40.  All zero-filled except those fields, which is what
 * the card's image looks like before the host patches it. */
static void build_dmem(uint8_t *d, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) d[i] = 0;
  d[0x1C] = 1; /* ver  */
  d[0x1D] = 4; /* hdr  */
  d[0x1E] = 8; /* entry length */
  d[0x1F] = 2; /* count */
  tput32(d + 0x20, 4);    /* app id: DMEMMAPPER */
  tput32(d + 0x24, 0xAE0);
  tput32(d + 0x28, 5);    /* app id: the second entry */
  tput32(d + 0x2C, 0xD2C);
  tput32(d + 0xAE0, 0x50414D44u); /* "DMAP" */
  d[0xAE4] = 3;                   /* version */
  d[0xAE5] = 0;
  d[0xAE6] = 0x40; /* block size, the measured value */
  d[0xAE7] = 0;
  tput32(d + 0xAE8, 0xD40); /* cmd_in_buffer_offset */
  tput32(d + 0xAEC, 0x40);  /* cmd_in_buffer_size   */
  tput32(d + 0xB0C, 0);     /* init_cmd: zero until patched */
}

/* The write the driver actually got wrong.  SIZE = ilog2(bytes) - 2: writing
 * the log2 itself (8) masks to 0, which is a 4-byte transfer, and the engine
 * accepts it -- so the image never lands and nothing reports an error. */
static void test_nv_flcn_xfer_cmd(void) {
  uart_puts("  Running test_nv_flcn_xfer_cmd...\n");
  tests_run++;
  /* A 256-byte IMEM block, secure: SIZE 6, IMEM bit 4, SEC 1 in bits 3:2
   * (0x600 | 0x10 | 0x04). */
  EXPECT_EQ((int)nv_flcn_xfer_cmd(256u, 1, 1), 0x614);
  /* The same block as DMEM, non-secure. */
  EXPECT_EQ((int)nv_flcn_xfer_cmd(256u, 0, 0), 0x600);
  /* The regression, spelled out: 8 is not a size encoding for 256 bytes. */
  ASSERT(nv_flcn_xfer_cmd(256u, 1, 1) != (int)((8u & 7u) << 8 | 0x10 | 0x4));
  /* The field's ends and its refusals. */
  EXPECT_EQ((int)nv_flcn_xfer_cmd(4u, 0, 0), 0x0);
  EXPECT_EQ((int)nv_flcn_xfer_cmd(16u, 0, 0), 0x200);
  EXPECT_EQ((int)nv_flcn_xfer_cmd(128u, 0, 1), 0x504);
  EXPECT_EQ((int)nv_flcn_xfer_cmd(2u, 0, 0), 0xffffffffu);
  EXPECT_EQ((int)nv_flcn_xfer_cmd(512u, 0, 0), 0xffffffffu);
  EXPECT_EQ((int)nv_flcn_xfer_cmd(200u, 0, 0), 0xffffffffu);
}

/* Signature selection: the fused version and the descriptor's bitmask must
 * agree, and the index that comes out is the one whose 384 bytes get copied
 * into DMEM.  A wrong index is rejected by the Boot ROM with no host-visible
 * reason, which is exactly why this is pinned in a test. */
static void test_nv_fwsec_sig_index(void) {
  uart_puts("  Running test_nv_fwsec_sig_index...\n");
  tests_run++;
  /* The card's own numbers: SignatureVersions 0x3, SignatureCount 2.  A fuse
   * word of 1 selects the version bit above its MSB (bit 1), which both
   * masks carry, at index 1. */
  EXPECT_EQ((int)nv_fwsec_sig_index(1u, 0x3u, 2u), 1);
  /* A fuse word that shares no version with the descriptor: no index, and the
   * caller must stop rather than patch a signature that cannot verify. */
  EXPECT_EQ((int)nv_fwsec_sig_index(1u, 0x1u, 2u), (int)NV_FWSEC_SIG_NONE);
  EXPECT_EQ((int)nv_fwsec_sig_index(3u, 0x3u, 2u), (int)NV_FWSEC_SIG_NONE);
  EXPECT_EQ((int)nv_fwsec_sig_index(0u, 0x3u, 2u), (int)NV_FWSEC_SIG_NONE);
  /* An index past the number of signatures the descriptor carries is refused
   * rather than reading beyond the signature block. */
  EXPECT_EQ((int)nv_fwsec_sig_index(1u, 0x3u, 1u), (int)NV_FWSEC_SIG_NONE);
}

/* The FRTS command: the offsets are the measured ones, and the values are in
 * 4 KiB pages (the one place in this chain that is not bytes). */
static void test_nv_fwsec_patch_frts(void) {
  uart_puts("  Running test_nv_fwsec_patch_frts...\n");
  tests_run++;
  uint8_t dmem[0xD80];
  build_dmem(dmem, sizeof dmem);
  /* Nothing patched yet: the card's image has init_cmd 0 and a zero buffer. */
  EXPECT_EQ((int)tget32(dmem + 0xB0C), 0);
  EXPECT_EQ((int)nv_fwsec_cmd_buffer(dmem, sizeof dmem, 0x1C), 0xD40);
  ASSERT(nv_fwsec_patch_frts(dmem, sizeof dmem, 0x1C, 0x5FE300u, 0x100u) == 0);
  /* The command selector and the structure it selects. */
  EXPECT_EQ((int)tget32(dmem + 0xB0C), (int)NV_FWSEC_CMD_FRTS);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x00), 1);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x04), 24);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x08), 0);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x10), 0);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x14), 2);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x18), 1);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x1C), 20);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x20), 0x5FE300);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x24), 0x100);
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x28), 2);
  /* 44 bytes in a 0x40-byte buffer: it fits, and nothing past it is touched. */
  EXPECT_EQ((int)tget32(dmem + 0xD40 + 0x2C), 0);
  /* The readback path the boot log uses: same bytes, read back, so the row in
   * the serial log is evidence about the image and not about the arguments. */
  {
    uint32_t init_cmd = 0, w[11];
    for (uint32_t i = 0; i < 11; i++) w[i] = 0;
    ASSERT(nv_fwsec_read_cmd(dmem, sizeof dmem, 0x1C, &init_cmd, w, 11) == 0);
    EXPECT_EQ((int)init_cmd, (int)NV_FWSEC_CMD_FRTS);
    /* The 8-byte read_vbios.addr occupies two dwords, which is why flags is
     * dword 5 and not dword 4 -- the mistake this expectation caught. */
    EXPECT_EQ((int)w[0], 1);         /* read_vbios.ver   */
    EXPECT_EQ((int)w[1], 24);        /* read_vbios.hdr   */
    EXPECT_EQ((int)w[2], 0);         /* read_vbios.addr lo */
    EXPECT_EQ((int)w[3], 0);         /* read_vbios.addr hi */
    EXPECT_EQ((int)w[4], 0);         /* read_vbios.size  */
    EXPECT_EQ((int)w[5], 2);         /* read_vbios.flags */
    EXPECT_EQ((int)w[6], 1);         /* frts_region.ver  */
    EXPECT_EQ((int)w[7], 20);        /* frts_region.hdr  */
    EXPECT_EQ((int)w[8], 0x5FE300);  /* frts_region.addr in 4 KiB pages */
    EXPECT_EQ((int)w[9], 0x100);     /* frts_region.size in 4 KiB pages */
    EXPECT_EQ((int)w[10], 2);        /* frts_region.type = FB */
  }
}

/* Negatives, so the patcher cannot pass by writing into a buffer that is not
 * the image it thinks it is. */
static void test_nv_fwsec_patch_refusals(void) {
  uart_puts("  Running test_nv_fwsec_patch_refusals...\n");
  tests_run++;
  uint8_t dmem[0xD80];
  build_dmem(dmem, sizeof dmem);
  /* Wrong appif version. */
  dmem[0x1C] = 2;
  EXPECT_EQ(nv_fwsec_patch_frts(dmem, sizeof dmem, 0x1C, 1, 1), -EINVAL);
  EXPECT_EQ((int)nv_fwsec_cmd_buffer(dmem, sizeof dmem, 0x1C),
            (int)0xffffffffu);
  build_dmem(dmem, sizeof dmem);
  /* No DMEMMAPPER entry: this is not the image the descriptor described. */
  tput32(dmem + 0x20, 9);
  EXPECT_EQ(nv_fwsec_patch_frts(dmem, sizeof dmem, 0x1C, 1, 1), -EINVAL);
  build_dmem(dmem, sizeof dmem);
  /* A command buffer too small for the 44-byte structure is -ERANGE, not a
   * partial write that the ucode would read as a valid command. */
  tput32(dmem + 0xAEC, 32);
  EXPECT_EQ(nv_fwsec_patch_frts(dmem, sizeof dmem, 0x1C, 1, 1), -ERANGE);
  EXPECT_EQ((int)tget32(dmem + 0xB0C), 0);
  /* An interface offset past the end of the image. */
  EXPECT_EQ(nv_fwsec_patch_frts(dmem, sizeof dmem, 0xFF0, 1, 1), -EINVAL);
}

/* ---- falcon memory addresses ------------------------------------------
 *
 * The conversion the DMEM read-back depends on, pinned as arithmetic rather
 * than as a comment: DMEM 0xB0C is byte offset 0xB0C and the port's index
 * field takes word 0x2C3.  Both wrong answers are silent on the card -- a byte
 * offset used as a word index reads four times too far, and an address masked
 * into the 14-bit field wraps to the start of the memory and answers with real
 * data -- so this is the one place a mistake is cheap to catch. */
static void test_nv_flcn_idx_word(void) {
  uart_puts("  Running test_nv_flcn_idx_word...\n");
  tests_run++;
  /* The three addresses the FWSEC read-back uses, in this card's DMEM image. */
  EXPECT_EQ((int)nv_flcn_idx_word(0xB0C), 0x2C3);
  EXPECT_EQ((int)nv_flcn_idx_word(0xD40), 0x350);
  EXPECT_EQ((int)nv_flcn_idx_word(0xB24), 0x2C9);
  EXPECT_EQ((int)nv_flcn_idx_word(0x000), 0);
  /* The top of the field: word 0x3FFF is byte 0xFFFC. */
  EXPECT_EQ((int)nv_flcn_idx_word(0xFFFC), 0x3FFF);
  /* One word past it is refused, not wrapped: 0x10000 >> 2 is 0x4000, which
   * the field cannot hold, and masking it would read DMEM word 0. */
  EXPECT_EQ((int)nv_flcn_idx_word(0x10000), (int)NV_FLCN_IDX_NONE);
  /* Unaligned byte offsets have no word index at all. */
  EXPECT_EQ((int)nv_flcn_idx_word(0xB0D), (int)NV_FLCN_IDX_NONE);
  EXPECT_EQ((int)nv_flcn_idx_word(0x2), (int)NV_FLCN_IDX_NONE);
}

/* The diagnosis: when the falcon returns a word the staged image does not have
 * at that offset, name the offset it does appear at. */
static void test_nv_flcn_find_word(void) {
  uart_puts("  Running test_nv_flcn_find_word...\n");
  tests_run++;
  uint8_t img[0x20];
  for (uint32_t i = 0; i < sizeof img; i++) img[i] = 0;
  tput32(img + 0x0C, 0x5EC2B0Cu);
  tput32(img + 0x1C, 0x5EC2B0Cu); /* a second copy: the first one wins */
  EXPECT_EQ((int)nv_flcn_find_word(img, sizeof img, 0x5EC2B0Cu), 0x0C);
  EXPECT_EQ((int)nv_flcn_find_word(img, sizeof img, 0x5EC2B0Du),
            (int)NV_FLCN_IDX_NONE);
  /* A value straddling the end of the image is not a match. */
  img[0x1F] = 0x77;
  EXPECT_EQ((int)nv_flcn_find_word(img, sizeof img, 0x77000000u),
            (int)NV_FLCN_IDX_NONE);
  EXPECT_EQ((int)nv_flcn_find_word(0, sizeof img, 1u), (int)NV_FLCN_IDX_NONE);
}

/* The read-back comparator: it must point AT the word that disagreed, which is
 * what turns "MISMATCH" into an offset a human can look up in the image. */
static void test_nv_flcn_compare_words(void) {
  uart_puts("  Running test_nv_flcn_compare_words...\n");
  tests_run++;
  uint8_t img[0x40];
  for (uint32_t i = 0; i < sizeof img; i++) img[i] = 0;
  tput32(img + 0x1C, 0x15); /* the staged init_cmd byte offset, 0xB0C, is
                             * relative to a DMEM image starting at 0xAF0 */
  tput32(img + 0x1C + 4, 0x2);
  uint32_t words[4] = {0x15, 0x2, 0, 0};
  struct nv_flcn_mismatch mm;
  for (uint32_t i = 0; i < sizeof mm; i++) ((uint8_t *)&mm)[i] = 0;
  EXPECT_EQ(nv_flcn_compare_words(words, 4, img, sizeof img, 0x1C, &mm), 0);
  words[1] = 0x30; /* what round 1 actually read back at DMEM 0xD40 */
  ASSERT(nv_flcn_compare_words(words, 4, img, sizeof img, 0x1C, &mm) == 1);
  EXPECT_EQ((int)mm.index, 1);
  EXPECT_EQ((int)mm.off, 0x20);
  EXPECT_EQ((int)mm.got, 0x30);
  EXPECT_EQ((int)mm.want, 0x2);
  /* A range the image cannot cover is refused rather than compared against
   * whatever follows it in memory. */
  EXPECT_EQ(nv_flcn_compare_words(words, 4, img, sizeof img, 0x34, &mm), -ERANGE);
  EXPECT_EQ(nv_flcn_compare_words(0, 4, img, sizeof img, 0, &mm), -EINVAL);
}

void nv_flcn_test_suite(void) {
  test_nv_flcn_xfer_cmd();
  test_nv_fwsec_sig_index();
  test_nv_fwsec_patch_frts();
  test_nv_fwsec_patch_refusals();
  test_nv_flcn_idx_word();
  test_nv_flcn_find_word();
  test_nv_flcn_compare_words();
}

#endif /* KERNEL_MODE_UNIT_TEST */
#endif /* __x86_64__ */
