/* ============================================================================
 *  nv_fsp_cot: send NVDM_PAYLOAD_COT (type 0x14) with the signed GSP-FMC and
 *  watch the GSP core start.
 *
 *  The question this file answers, in one sentence: the FSP at 0x8F0000 is
 *  host-reachable and answers caps queries (t_fe7a1fd7); does it also accept
 *  the vendor's own next command -- the Chain-Of-Trust payload that carries the
 *  GSP-FMC image and its authentication fields -- and start the FMC?
 *
 *  Where every field comes from (open-gpu-kernel-modules 580.178.04, MIT):
 *
 *    kern_fsp_cot_payload.h   NVDM_PAYLOAD_COT, 860 bytes packed:
 *        version@0(u16) size@2(u16) gspFmcSysmemOffset@4(u64)
 *        frtsSysmemOffset@12(u64) frtsSysmemSize@20(u32)
 *        frtsVidmemOffset@24(u64) frtsVidmemSize@32(u32)
 *        hash384@36(48) publicKey@84(384) signature@468(384)
 *        gspBootArgsSysmemOffset@852(u64)
 *      (offsets measured by compiling that header, not re-derived; the
 *      kernel's own struct is checked against them with _Static_assert below)
 *    g_kern_fsp_nvoc.c        GB202: cotPayloadVersion=2, signature 96 bytes
 *      (ECDSA P-384), publicKey 97 bytes (uncompressed 0x04||X||Y)
 *    kern_fsp_gh100.c         kfspPrepareBootCommands_GH100 (version, size,
 *      1 MiB sysmem FRTS, PBUS SW_FRTS_INSECURE_* programmed with it,
 *      kfspSetupGspImages filling hash/sig/key from the FMC bindata) and
 *      kfspSendBootCommands_GH100 (kfspSendAndReadMessage(..., NVDM_TYPE_COT))
 *    kernel_gsp_gh100.c       what "the FMC started" looks like from outside:
 *      HWCFG2's RISCV_BR_PRIV_LOCKDOWN releases, and the FMC clears
 *      FALCON_MAILBOX0/1 (the boot-args address the host stuffed there) at the
 *      start of bootstrap.
 *
 *  The image and its three authentication fields are taken from ONE file --
 *  linux-firmware's nvidia/gb202/gsp/fmc-570.144.bin, whose ELF sections are
 *  image/hash/signature/publickey.  That keeps the set self-consistent, and
 *  the kernel recomputes SHA-384 over the bytes it actually placed in sysmem,
 *  so "the hash describes this image" is a fact from the card, not a copy.
 *
 *  What this file does NOT do: it does not build a WPR2, does not publish
 *  PTE[0]/cmdq/statq, and does not hand the FMC a GSP-RM to boot (its args
 *  page says so explicitly: bIsGspRmBoot=0, no descriptor, no WPR carveout).
 *  Starting the FMC is the whole claim; booting GSP-RM is the next rung.
 *
 *  Gating and safety: everything is behind /COT.CFG (an ordinary boot does
 *  nothing).  The one packet is the vendor's own; the only host-initiated MMIO
 *  besides it is (a) PBUS scratch words the vendor driver also writes, (b) the
 *  GSP falcon mailbox pair the vendor driver also writes, and (c) the EMEM
 *  channel, restored to idle at the end.  Nothing is written to the GSP core.
 * ==========================================================================*/

/* ENODEV is needed by both branches (the non-x86-64 stub at the bottom), so
 * errno.h is included before the guard: without this the ARM unit tier does
 * not compile this TU ("use of undeclared identifier 'ENODEV'").  Prerequisite
 * fix for the ARM gate, not part of the vendor-shape change. */
#include "errno.h"

#ifdef __x86_64__

#include "nv.h"
#include "nv_dma.h"
#include "nv_fw.h"
#include "nv_rpc.h"
#include "nv_gsp_glue.h"
#include "sha384.h"
#include "kmem.h"

#include "errno.h"
#include "fs.h"
#include "fat16.h"
#include "nv_vram.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

/* ---- the PFSP transport (same words as t_fe7a1fd7, nv_fsp.c) ------------ */
#define COT_PFSP_QUEUE_HEAD0 0x2C00u
#define COT_PFSP_QUEUE_TAIL0 0x2C04u
#define COT_PFSP_MSGQ_HEAD0  0x2C80u
#define COT_PFSP_MSGQ_TAIL0  0x2C84u
#define COT_PFSP_EMEMC0      0x2AC0u
#define COT_PFSP_EMEMD0      0x2AC4u

/* ---- PBUS scratch words: where the FSP is told the FRTS lives ----------- */
/* published/hopper/gh100/dev_bus_addendum.h, NV_PBUS_SW_SCRATCH(i) = 0x1400+4i */
#define NV_PBUS_SW_FRTS_ADDR_LO32 0x14F4u
#define NV_PBUS_SW_FRTS_ADDR_HI32 0x14F8u
#define NV_PBUS_SW_FRTS_CONFIG    0x14FCu
#define NV_PBUS_FRTS_CONFIG_SYSMEM(size) \
  ((((uint32_t)(size) >> 12) & 0xFFFFu) | (1u << 16))

/* ---- ... and where GB202 actually keeps them ---------------------------- *
 * Blackwell (published/blackwell/gb100/dev_bus_zb{,_addendum}.h) answers a
 * DIFFERENT scratch block: NV_PBUS_ZB_SW_SCRATCH(i) = 0x400+4i.  The same
 * three FRTS words are 0x04F4/0x04F8/0x04FC there (index 0x3D/0x3E/0x3F), not
 * 0x14F4 (index 0x3D of the GH100 block, which GB202 does not publish).  The
 * same block carries the GSP-FMC's own formatted error word at index 0x37 =
 * 0x04DC -- the register gpuReportGspFmcErrorCode_GB100 reads
 * (gsp_fmc_error_code_formatting.h): variant 31:28, partition 27:24,
 * payload 23:0; variant 0 (SK) = error 15:8 | phase 7:0, variant 1 (generic) =
 * additional info 23:16 | error code 15:0.  NV_PBUS0_PRI_BASE is 0 on this
 * BAR, so the offsets are absolute. */
#define NV_PBUS_ZB_SCRATCH(i)            (0x400u + (i) * 4u)
#define NV_PBUS_ZB_SCRATCH_GSP_FMC_ERROR NV_PBUS_ZB_SCRATCH(0x37) /* 0x04DC */
/* ... but the host does not read that alias: published/blackwell/gb100/
 * hwproject.h puts NV_PBUS0_PRI_BASE at 0x1000, so the driver's
 * gpuReportGspFmcErrorCode_GB100 (and the FRTS_INSECURE writes) land at
 * 0x14DC / 0x14F4 / 0x14F8 / 0x14FC -- which is byte for byte the GH100
 * scratch block this file already writes.  The 0x400 copies answer
 * 0xBADF5040 (private/privileged read) when made from the host. */
#define NV_PBUS_PRI_BASE                 0x1000u
#define NV_PBUS_PRI_GSP_FMC_ERROR        (NV_PBUS_PRI_BASE + NV_PBUS_ZB_SCRATCH(0x37))
#define NV_PBUS_PRI_FRTS_ADDR_LO32       (NV_PBUS_PRI_BASE + NV_PBUS_ZB_SCRATCH(0x3D))
#define NV_PBUS_PRI_FRTS_ADDR_HI32       (NV_PBUS_PRI_BASE + NV_PBUS_ZB_SCRATCH(0x3E))
#define NV_PBUS_PRI_FRTS_CONFIG          (NV_PBUS_PRI_BASE + NV_PBUS_ZB_SCRATCH(0x3F))
#define NV_PBUS_ZB_FRTS_ADDR_LO32        NV_PBUS_ZB_SCRATCH(0x3D) /* 0x04F4 */
#define NV_PBUS_ZB_FRTS_ADDR_HI32        NV_PBUS_ZB_SCRATCH(0x3E) /* 0x04F8 */
#define NV_PBUS_ZB_FRTS_CONFIG           NV_PBUS_ZB_SCRATCH(0x3F) /* 0x04FC */

/* ---- the GSP core's window (published/hopper/gh100/dev_gsp.h etc.) ------ */
#define NV_GSP_BASE             0x00110000u
#define NV_GSP_MAILBOX0         0x040u
#define NV_GSP_MAILBOX1         0x044u
#define NV_GSP_FALCON_CPUCTL    0x100u
#define NV_GSP_FALCON_OS        0x080u /* NV_PFALCON_FALCON_OS */
#define NV_GSP_FALCON_RM        0x084u /* NV_PFALCON_FALCON_RM */
#define NV_GSP_HWCFG2           0x0F4u
#define NV_GSP_RISCV_CPUCTL     0x388u /* dev_riscv_pri.h */
#define NV_GSP_RISCV_RPC        0x3ECu
#define NV_GSP_BCR_DMACFG       0x66Cu
#define NV_GSP_BCR_PKC_LO       0x670u
#define NV_GSP_BCR_FMCCODE_LO   0x678u
#define NV_GSP_BCR_FMCDATA_LO   0x680u
#define NV_GSP_ENGINE_MAILBOX0  0x804u /* NV_PGSP_MAILBOX(0) */
/* The vendor's own failure readbacks (kfspDumpDebugState_GB100): four words of
 * NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2 and the rest of the NV_PGSP_MAILBOX
 * array.  The GSP core's MAILBOX0/1 at 0x110040/44 is a different block from
 * PGSP_MAILBOX(i) at 0x110804+4i, and on GB100 `NV_PGSP_MAILBOX__SIZE_1` = 4,
 * so index 0 (0x804) was the only word this kernel had ever read. */
#define NV_PFSP_SCRATCH_G2_BASE 0x008F0320u /* SCRATCH_GROUP_2(0), +4i, i<4   */
#define NV_PGSP_MAILBOX_BASE    0x00110804u /* NV_PGSP_MAILBOX(0)             */
#define NV_THERM_I2CS_SCRATCH   0x00AD00BCu /* FSP_BOOT_COMPLETE, .STATUS=0xFF */
/* The FSP's CMS2 log window (dev_oob_pri.h: NV_POOBHUB_RCV_INDIRECT_CMS2_MEM_
 * RD_ADDR 0x8AA374 / RD_DATA 0x8AA378; the vendor reads words 0x50..0x7F). */
#define NV_POOBHUB_CMS2_ADDR    0x008AA374u
#define NV_POOBHUB_CMS2_DATA    0x008AA378u

/* NV_PGSP HWCFG2 (and every other word in the window) answers this while the
 * BAR0 decoupler's PRIV target mask is locked to the FSP: privErrTargetLocked,
 * masked with 0xFFFFFF00 by _kfspIsGspTargetMaskReleased().  A blocked target,
 * not an absent device. */
#define NV_GSP_PRIV_TARGET_LOCKED 0xBADF4100u
/* The bus fence word a *running, secure* falcon/RISC-V core answers for the
 * register block it protects -- the same value the running PFSP shows at its
 * own ctl/busy (t_67f09feb measured 0xBADF5620 there while the FSP answered
 * its scratch groups). */
#define NV_GSP_BUS_FENCED 0xBADF5620u

/* ---- the COT payload, as the vendor's packed struct lays it out --------- */
#define NV_COT_BYTES          860u
#define NV_COT_OFF_VERSION    0u
#define NV_COT_OFF_SIZE       2u
#define NV_COT_OFF_FMC        4u
#define NV_COT_OFF_FRTS_SYS   12u
#define NV_COT_OFF_FRTS_SSIZE 20u
#define NV_COT_OFF_FRTS_VID   24u
#define NV_COT_OFF_FRTS_VSIZE 32u
#define NV_COT_OFF_HASH       36u
#define NV_COT_OFF_PKEY       84u
#define NV_COT_OFF_SIG        468u
#define NV_COT_OFF_ARGS       852u
#define NV_COT_VERSION_GB202  2u

/* ---- GSP_FMC_BOOT_PARAMS (gspifpub.h), offsets from the same header ----- */
#define NV_FMCP_BYTES        88u
#define NV_FMCP_OFF_REGKEYS  0u
#define NV_FMCP_OFF_BOOTTGT  8u
#define NV_FMCP_OFF_DESCSIZE 12u
#define NV_FMCP_OFF_DESCOFF  16u
#define NV_FMCP_OFF_WPR_OFF  24u
#define NV_FMCP_OFF_WPR_SIZE 32u
#define NV_FMCP_OFF_ISRMBOOT 36u
#define NV_FMCP_OFF_RMTGT    40u
#define NV_FMCP_OFF_RMARGS   48u
#define NV_FMCP_OFF_SPDMTGT  56u
#define NV_FMCP_OFF_SPDMBUF  64u
#define NV_FMCP_OFF_SPDMLEN  72u
#define GSP_DMA_TARGET_COHERENT_SYSTEM 1u
#define GSP_DMA_TARGET_NONCOHERENT_SYSTEM 2u

/* ---- auth field sizes for GB202 (g_kern_fsp_nvoc.c) --------------------- */
#define NV_COT_HASH_BYTES 48u
#define NV_COT_SIG_BYTES  96u
#define NV_COT_PKEY_BYTES 97u

/* FRTS sysmem: NV_PGC6_AON_FRTS_INPUT_WPR_SIZE_SECURE_SCRATCH_GROUP_03_0_
 * WPR_SIZE_1MB_IN_4K << 12 (kfspPrepareBootCommands_GH100) */
#define NV_COT_FRTS_BYTES 0x100000u

#define NV_COT_HDR_MCTP 0xC0000000u /* SOM|EOM, SEID 0, SEQ 0 */
#define NV_COT_HDR_NVDM 0x1410DE7Eu /* type 0x7e, vendor 0x10de, NVDM 0x14 */
#define NV_COT_BOOST_NVDM 0x2010DE7Eu /* NVDM_TYPE_CLOCK_BOOST 0x20 */

/* The NVDM types this file speaks (fsp_nvdm_format.h). */
#define COT_NVDM_COT          0x14u
#define COT_NVDM_CLOCK_BOOST  0x20u

/* ---- the FSP's DMEM log over the MCTP VDM download-log command ---------- *
 * _kfspGatherDmemLog_GB100 (620.73.04 kern_fsp_gb100.c:357) is the vendor's
 * own "the FSP never booted -- what does its log say" path.  It is NOT an NVDM
 * message: it builds MctpDownloadLogV1 by hand and calls kfspSendPacket_HAL
 * directly, so the frame is the MCTP transport dword followed by a 9-byte
 * MCTP VDM IANA request and a session id -- no NVDM dword.  Laid out from the
 * vendor's own #pragma pack(1) structs:
 *   d0 = SOM|EOM|SEID=0|SEQ=0|TAG=1                0xC1000000
 *   bytes 4..7   = messageType 0x7f | IANA 0x1647   0x0016477F
 *   bytes 8..11  = instance 0 | d 0 | rq 1 |
 *                  vendorMessageType 1 | commandCode 6   0x06018000
 *   bytes 12..13 = messageVersion 1 | sessionId (0xFF = new session);
 *                  the frame is 14 bytes, so d3 = 0x01 | (sid << 8)
 *   (commandCode 6 = NV_DEFINED_DOWNLOAD_LOG_COMMAND)
 * 14 bytes, padded to 16 by kfspSendPacket_GH100, so TAIL = 12.
 * The reply, from the same function's pointer arithmetic, is
 *   bytes 0..3 mctp | 4..12 request echo | 13 completionCode |
 *   14 sessionId | 15 length | 16.. 52 data bytes,
 * i.e. the echo's first dword is resp[1] = 0x0016477F -- the shape test that
 * separates a real answer from any other wrapper.  length 0 ends the log and
 * the first reply carries the session id every later read must repeat.  The
 * vendor caps the log at 4096 bytes and each packet at 68 bytes. */
#define NV_FSPLOG_MAX_BYTES   4096u
#define NV_FSPLOG_DATA_BYTES  52u
#define NV_FSPLOG_TIMEOUT_US  200000
#define NV_FSPLOG_MCTP        0xC1000000u
#define NV_FSPLOG_ECHO_W1     0x0016477Fu
#define NV_FSPLOG_ECHO_W2     0x06018000u
#define NV_FSPLOG_REQ_W3(sid) (0x01u | (((uint32_t)(sid) & 0xFFu) << 8))

/* ------------------------------------------------------------------------ */
/*  rows                                                                    */
/* ------------------------------------------------------------------------ */

static void p(const char *s) {
  uart_puts("[NV-FSPCOT] ");
  uart_puts(s);
}
static void pkv(const char *k, uint64_t v) {
  uart_puts(k);
  uart_puts("=");
  uart_print_hex(v);
}
static void pki(const char *k, int v) {
  uart_puts(k);
  uart_puts("=");
  print_int(v);
}
static void pe(void) { uart_puts("\n"); }

/* Tagged variants: "[NV-FSPCOT] <tag> ..." so one boot can carry more than one
 * round trip (the bad-hash control and the good COT) in the same log. */
static void pt(const char *tag, const char *s) {
  uart_puts("[NV-FSPCOT] ");
  uart_puts(tag);
  uart_puts(" ");
  uart_puts(s);
}
static void ptki(const char *tag, const char *k, int v) {
  uart_puts("[NV-FSPCOT] ");
  uart_puts(tag);
  uart_puts(" ");
  pki(k, v);
}

/* Two 4-byte samples of a byte string, so a log line pins a value without
 * printing 48 or 97 bytes: first and last dword. */
static void pbytes(const char *k, const uint8_t *b, uint32_t n) {
  uint32_t a = 0, z = 0;
  if (n >= 4) {
    a = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
        ((uint32_t)b[3] << 24);
    z = (uint32_t)b[n - 4] | ((uint32_t)b[n - 3] << 8) |
        ((uint32_t)b[n - 2] << 16) | ((uint32_t)b[n - 1] << 24);
  }
  uart_puts(k);
  uart_puts(".a=");
  uart_print_hex(a);
  uart_puts(" ");
  uart_puts(k);
  uart_puts(".z=");
  uart_print_hex(z);
}

/* ---- little-endian stores into a plain byte buffer --------------------- */
static void st16(uint8_t *b, uint32_t off, uint16_t v) {
  b[off] = (uint8_t)v;
  b[off + 1] = (uint8_t)(v >> 8);
}
static void st32(uint8_t *b, uint32_t off, uint32_t v) {
  for (int i = 0; i < 4; i++) b[off + i] = (uint8_t)(v >> (8 * i));
}
static void st64(uint8_t *b, uint32_t off, uint64_t v) {
  for (int i = 0; i < 8; i++) b[off + i] = (uint8_t)(v >> (8 * i));
}
static uint32_t ld32(const uint8_t *b, uint32_t off) {
  return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
         ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}
/* 64-bit wire read, so a row can quote the block the device will see rather
 * than the struct the host built. */
static uint64_t ld64(const uint8_t *b, uint32_t off) {
  return (uint64_t)ld32(b, off) | ((uint64_t)ld32(b, off + 4) << 32);
}

/* ------------------------------------------------------------------------ */
/*  time, on the card's own clock                                           */
/* ------------------------------------------------------------------------ */

static void cot_wait_us(struct nv_dev *d, int us) {
  uint64_t t0 = 0, now = 0;
  uint64_t hz = d->timer_hz ? d->timer_hz : 1000000000ULL;
  if (us <= 0) return;
  if (nv_timer_read(d, &t0) != 0) return;
  for (int i = 0; i < 4000000; i++) {
    if (nv_timer_read(d, &now) != 0) return;
    if ((now - t0) * 1000000ULL >= (uint64_t)us * hz) return;
  }
}

static int cot_us_since(struct nv_dev *d, uint64_t t0) {
  uint64_t now = 0;
  uint64_t hz = d->timer_hz ? d->timer_hz : 1000000000ULL;
  if (nv_timer_read(d, &now) != 0) return -1;
  return (int)((now - t0) * 1000000ULL / hz);
}

/* ------------------------------------------------------------------------ */
/*  the FMC file: an ELF32 whose sections ARE the COT's fields              */
/* ------------------------------------------------------------------------ */

struct cot_fmc {
  const uint8_t *image, *hash, *sig, *key;
  uint32_t image_len, hash_len, sig_len, key_len;
};

/* strcmp without libc: 1 when equal. */
static int cot_streq(const char *a, const char *b) {
  for (uint32_t i = 0;; i++) {
    if (a[i] != b[i]) return 0;
    if (b[i] == '\0') return 1;
  }
}

static int cot_fmc_sections(const uint8_t *b, uint32_t len, struct cot_fmc *o) {
  uint32_t shoff, shentsize, shnum, shstrndx, stroff, strsize;
  if (!b || !o || len < 0x34) return -EINVAL;
  if (b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') return -EINVAL;
  if (b[4] != 1) return -EINVAL; /* ELFCLASS32: the FMC is, GSP-RM is not */
  o->image = o->hash = o->sig = o->key = 0;
  o->image_len = o->hash_len = o->sig_len = o->key_len = 0;

  shoff = ld32(b, 0x20);
  shentsize = (uint32_t)b[0x2E] | ((uint32_t)b[0x2F] << 8);
  shnum = (uint32_t)b[0x30] | ((uint32_t)b[0x31] << 8);
  shstrndx = (uint32_t)b[0x32] | ((uint32_t)b[0x33] << 8);
  if (shentsize < 40 || shnum == 0 || shnum > 64) return -EINVAL;
  if (shoff > len || (uint64_t)shoff + (uint64_t)shentsize * shnum > len)
    return -EINVAL;
  if (shstrndx >= shnum) return -EINVAL;
  stroff = ld32(b, shoff + shstrndx * shentsize + 0x10);
  strsize = ld32(b, shoff + shstrndx * shentsize + 0x14);
  if (stroff > len || strsize > len - stroff) return -EINVAL;

  for (uint32_t i = 0; i < shnum; i++) {
    uint32_t e = shoff + i * shentsize;
    uint32_t name = ld32(b, e + 0x00);
    uint32_t off = ld32(b, e + 0x10);
    uint32_t size = ld32(b, e + 0x14);
    const char *nm;
    if (name >= strsize) continue;
    nm = (const char *)(b + stroff + name);
    if (off > len || size > len - off) continue;
    if (nm[0] == '\0') continue;
    if (cot_streq(nm, "image")) { o->image = b + off; o->image_len = size; }
    else if (cot_streq(nm, "hash")) { o->hash = b + off; o->hash_len = size; }
    else if (cot_streq(nm, "signature")) { o->sig = b + off; o->sig_len = size; }
    else if (cot_streq(nm, "publickey")) { o->key = b + off; o->key_len = size; }
  }
  if (!o->image || !o->hash || !o->sig || !o->key) return -EINVAL;
  if (o->image_len < 0x1000) return -EINVAL;
  return 0;
}

/* ------------------------------------------------------------------------ */
/*  the EMEM channel: write a packet, read the answer                       */
/* ------------------------------------------------------------------------ */

static uint32_t cot_ememc(uint32_t offs, int aincw, int aincr) {
  uint32_t v = ((offs & 0x3Fu) << 2) | (((offs / 64u) & 0xFFu) << 8);
  if (aincw) v |= 0x01000000u;
  if (aincr) v |= 0x02000000u;
  return v;
}

/* Write one packet (2 header dwords + payload dwords) and ring the doorbell.
 * Returns the tail value written, or 0 on failure. */
static uint32_t cot_send(struct nv_dev *d, uint32_t base, const char *tag,
                         uint32_t hdr0, uint32_t hdr1, const uint8_t *payload,
                         uint32_t bytes) {
  uint32_t words = bytes / 4u;
  uint32_t qh, qt;
  nv_wr32(d, base + COT_PFSP_EMEMC0, cot_ememc(0, 1, 0));
  nv_wr32(d, base + COT_PFSP_EMEMD0, hdr0);
  nv_wr32(d, base + COT_PFSP_EMEMD0, hdr1);
  for (uint32_t i = 0; i < words; i++)
    nv_wr32(d, base + COT_PFSP_EMEMD0, ld32(payload, i * 4u));
  /* TAIL points at the last DWORD written (kfspSendPacket_GH100) */
  qt = (2u + words) * 4u - 4u;
  nv_wr32(d, base + COT_PFSP_QUEUE_TAIL0, qt); /* TAIL does not interrupt */
  nv_wr32(d, base + COT_PFSP_QUEUE_HEAD0, 0);  /* HEAD interrupts the FSP   */
  qh = nv_rd32(d, base + COT_PFSP_QUEUE_HEAD0);
  pt(tag, "send dwords=");
  print_int((int)(2u + words));
  pkv(" tail", qt);
  pkv(" qh", qh);
  pkv(" hdr0", hdr0);
  pkv(" hdr1", hdr1);
  pkv(" ememc", nv_rd32(d, base + COT_PFSP_EMEMC0));
  pe();
  return qt ? qt : 1u;
}

/* Read the SP's response out of EMEM exactly as kfspReadPacket_GH100 does:
 * size = (MSGQ_TAIL - MSGQ_HEAD) + 4, EMEMC <- {AINCR}, read the dwords, then
 * MSGQ_TAIL = MSGQ_HEAD = the head seen (that is the ack).  Fills up to `max`
 * dwords; returns the count, 0 when the SP had nothing. */
static int cot_recv(struct nv_dev *d, uint32_t base, const char *tag,
                    uint32_t *out, int max) {
  uint32_t mh = nv_rd32(d, base + COT_PFSP_MSGQ_HEAD0);
  uint32_t mt = nv_rd32(d, base + COT_PFSP_MSGQ_TAIL0);
  uint32_t dwords;
  if (mh == mt) {
    pt(tag, "none (MSGQ_HEAD==MSGQ_TAIL)\n");
    return 0;
  }
  dwords = (mt - mh) / 4u + 1u;
  pt(tag, "head=");
  uart_print_hex(mh);
  pkv(" tail", mt);
  pki(" dwords", (int)dwords);
  pe();
  if ((int)dwords > max) dwords = (uint32_t)max;
  nv_wr32(d, base + COT_PFSP_EMEMC0, cot_ememc(0, 0, 1));
  for (uint32_t i = 0; i < dwords; i++) {
    out[i] = nv_rd32(d, base + COT_PFSP_EMEMD0);
    pt(tag, "i=");
    print_int((int)i);
    pkv(" v", out[i]);
    pe();
  }
  nv_wr32(d, base + COT_PFSP_MSGQ_TAIL0, mh);
  nv_wr32(d, base + COT_PFSP_MSGQ_HEAD0, mh);
  return (int)dwords;
}

/* Wait up to `us` for the SP to move MSGQ_TAIL away from MSGQ_HEAD.
 * Returns the latency in us, or -1. */
static int cot_wait_resp(struct nv_dev *d, uint32_t base, int us) {
  static const int at[] = {0, 50, 200, 1000, 5000, 20000, 100000, 300000, 1000000};
  int last = 0;
  for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) {
    if (at[i] > last) cot_wait_us(d, at[i] - last);
    last = at[i];
    if (nv_rd32(d, base + COT_PFSP_MSGQ_HEAD0) !=
        nv_rd32(d, base + COT_PFSP_MSGQ_TAIL0))
      return at[i];
    if (at[i] >= us) break;
  }
  return -1;
}

/* Write an arbitrary dword frame to the RM EMEM channel and ring the doorbell.
 * Same tail rule as cot_send, but no implicit header dwords: the download-log
 * command's frame already contains its MCTP dword. */
static void cot_send_words(struct nv_dev *d, uint32_t base, const char *tag,
                           const uint32_t *w, int n) {
  uint32_t qt = (uint32_t)n * 4u - 4u;
  pt(tag, "send dwords=");
  print_int(n);
  pkv(" tail", qt);
  nv_wr32(d, base + COT_PFSP_EMEMC0, cot_ememc(0, 1, 0));
  for (int i = 0; i < n; i++) {
    nv_wr32(d, base + COT_PFSP_EMEMD0, w[i]);
    p(" w");
    print_int(i);
    p("=0x");
    uart_print_hex(w[i]);
  }
  nv_wr32(d, base + COT_PFSP_QUEUE_TAIL0, qt); /* TAIL does not interrupt */
  nv_wr32(d, base + COT_PFSP_QUEUE_HEAD0, 0);  /* HEAD interrupts the FSP   */
  pkv(" qh", nv_rd32(d, base + COT_PFSP_QUEUE_HEAD0));
  pe();
}

/* cot_recv without the per-dword rows: the caller prints one compact row, so a
 * 52-byte log packet costs one serial line instead of seventeen. */
static int cot_recv_quiet(struct nv_dev *d, uint32_t base, uint32_t *out,
                          int max) {
  uint32_t mh = nv_rd32(d, base + COT_PFSP_MSGQ_HEAD0);
  uint32_t mt = nv_rd32(d, base + COT_PFSP_MSGQ_TAIL0);
  uint32_t dwords;
  if (mh == mt) return 0;
  dwords = (mt - mh) / 4u + 1u;
  if ((int)dwords > max) return -1;
  nv_wr32(d, base + COT_PFSP_EMEMC0, cot_ememc(0, 0, 1));
  for (uint32_t i = 0; i < dwords; i++)
    out[i] = nv_rd32(d, base + COT_PFSP_EMEMD0);
  nv_wr32(d, base + COT_PFSP_MSGQ_TAIL0, mh); /* ack: TAIL = HEAD = head seen */
  nv_wr32(d, base + COT_PFSP_MSGQ_HEAD0, mh);
  return (int)dwords;
}

/* The FSP's DMEM log, exactly the vendor's loop: request a new session (0xFF),
 * read 52 bytes per answered packet, repeat with the session id the first
 * answer carried, stop on length 0 / timeout / end of the 4 KiB cap.
 *
 * Every packet is printed as one raw dword row, in order, so the whole log can
 * be re-decoded offline from this boot's serial log alone
 * (tools/nv/gb202-dmem-log-decode.py).  The shape test is the request echo at
 * resp[1]: a real download-log answer repeats 0x0016477F, any other wrapper
 * does not, and the attempt then stops after that one packet instead of
 * spamming the channel. */
static void rm_dmem_log(struct nv_dev *d, uint32_t base) {
  uint32_t resp[20];
  uint32_t sid = 0xFFu;
  int pkts = 0, bytes = 0;
  p("dmemlog start cmd=0x06 vmt=1 iana=0x1647\n");
  for (int pn = 0; pn < 80; pn++) {
    uint32_t w[4];
    int nd, lat, cc, s, len;
    w[0] = NV_FSPLOG_MCTP;
    w[1] = NV_FSPLOG_ECHO_W1;
    w[2] = NV_FSPLOG_ECHO_W2;
    w[3] = NV_FSPLOG_REQ_W3(sid);
    cot_send_words(d, base, "dmemlog req", w, 4);
    lat = cot_wait_resp(d, base, NV_FSPLOG_TIMEOUT_US);
    p("dmemlog wait pkt=");
    print_int(pn);
    pki(" lat_us", lat);
    pkv(" req_sid", sid);
    pkv(" msgq_head", nv_rd32(d, base + COT_PFSP_MSGQ_HEAD0));
    pkv(" msgq_tail", nv_rd32(d, base + COT_PFSP_MSGQ_TAIL0));
    pe();
    if (lat < 0) {
      p("dmemlog status=no-response (nothing arrived in 200000 us)\n");
      break;
    }
    nd = cot_recv_quiet(d, base, resp, 20);
    if (nd <= 0) {
      p("dmemlog rsp pkt=");
      print_int(pn);
      pki(" nd", nd);
      pe();
      p("dmemlog status=empty\n");
      break;
    }
    p("dmemlog dw pkt=");
    print_int(pn);
    p(" nd=");
    print_int(nd);
    for (int i = 0; i < nd; i++) {
      p(" 0x");
      uart_print_hex(resp[i]);
    }
    pe();
    if (nd < 4) {
      p("dmemlog status=short-reply\n");
      break;
    }
    if (resp[1] != NV_FSPLOG_ECHO_W1) {
      p("dmemlog shape=other-wrapper echo_w1=0x");
      uart_print_hex(resp[1]);
      p(" echo_w2=0x");
      uart_print_hex(resp[2]);
      pe();
      p("dmemlog status=not-a-download-log-answer\n");
      break;
    }
    cc = (int)((resp[3] >> 8) & 0xFFu);
    s = (int)((resp[3] >> 16) & 0xFFu);
    len = (int)((resp[3] >> 24) & 0xFFu);
    p("dmemlog rsp pkt=");
    print_int(pn);
    pki(" cc", cc);
    pki(" sid", s);
    pki(" len", len);
    pe();
    if (cc != 0) {
      p("dmemlog status=completion-error\n");
      break;
    }
    if (len == 0) {
      p("dmemlog status=end-of-log\n");
      break;
    }
    if (len > (int)NV_FSPLOG_DATA_BYTES) {
      p("dmemlog status=bad-length\n");
      break;
    }
    if (pn == 0) sid = (uint32_t)s;
    {
      /* the same bytes as text, so the in-band log already shows strings */
      char txt[NV_FSPLOG_DATA_BYTES + 1];
      int n = 0;
      for (int i = 0; i < len; i++) {
        uint32_t v = resp[4 + (i >> 2)];
        char ch = (char)((v >> ((i & 3) * 8)) & 0xFFu);
        txt[n++] = (ch >= 0x20 && ch < 0x7F) ? ch : '.';
      }
      txt[n] = 0;
      p("dmemlog txt pkt=");
      print_int(pn);
      p(" \"");
      uart_puts(txt);
      p("\"\n");
    }
    pkts++;
    bytes += len;
    if (bytes + (int)NV_FSPLOG_DATA_BYTES >= (int)NV_FSPLOG_MAX_BYTES) break;
  }
  p("dmemlog done pkts=");
  print_int(pkts);
  pki(" bytes", bytes);
  p(" session=0x");
  uart_print_hex(sid);
  pe();
}

/* One request/response round trip over the EMEM channel.  Prints the answer's
 * dwords and decodes them; returns the errorCode (>=0), or -1 when the SP
 * answered nothing, or -2 when the answer was too short to carry one.
 *
 * The framing, as measured on this part (three answers: caps query 0x16,
 * clock boost 0x20, COT 0x14): the FSP_RESPONSE payload is DWORD aligned --
 *   payload[0] = 1        response class (constant across all three)
 *   payload[1] = <type>   the NVDM type of the command it answers (the echo)
 *   payload[2] = errorCode (FSP_OK = 0)
 *   payload[3..]          command-specific (the caps query answered 0x20 here)
 * The driver's own FSP_CAPS_QUERY_RESPONSE_PAYLOAD_PARAMS is byte-packed
 * (nvdmType at byte 0, a 12-byte cmdResponse at byte 1); read that way, this
 * answer's byte 0 is 0x01 and the taskId comes out as 0x16000000 -- i.e. the
 * vendor struct does not describe what this SP emits, and the dword-aligned
 * reading is the one the data supports.  Read the raw dword rows, not just
 * this decode.
 */
static int cot_roundtrip(struct nv_dev *d, uint32_t base, const char *tag,
                         uint32_t nvdm, const uint8_t *payload,
                         uint32_t bytes) {
  uint32_t resp[24];
  uint32_t hdr1 = ((nvdm & 0xFFu) << 24) | (0x10DEu << 8) | 0x7Eu;
  int nd, err = -1;
  int lat;
  (void)cot_send(d, base, tag, NV_COT_HDR_MCTP, hdr1, payload, bytes);
  lat = cot_wait_resp(d, base, 1000000);
  ptki(tag, "latency_us", lat);
  pe();
  if (lat < 0) return -1;
  nd = cot_recv(d, base, tag, resp, 24);
  if (nd >= 5) {
    ptki(tag, "class", (int)resp[2]);
    ptki(tag, "cmd_nvdm", (int)resp[3]);
    ptki(tag, "errorCode", (int)resp[4]);
    pe();
    err = (int)resp[4];
  } else if (nd >= 1) {
    err = -2;
  }
  return err;
}

/* ------------------------------------------------------------------------ */
/*  the GSP window                                                          */
/* ------------------------------------------------------------------------ */

static int cot_gsp_locked(uint32_t hwcfg2) {
  return hwcfg2 != 0 && (hwcfg2 & 0xFFFFFF00u) == NV_GSP_PRIV_TARGET_LOCKED;
}

/* One row of the GSP core's state.  `tag` is "pre" or a sample time. */
static void cot_gsp_row(struct nv_dev *d, const char *tag, int t_us) {
  uint32_t base = NV_GSP_BASE;
  p("gsp ");
  uart_puts(tag);
  if (t_us >= 0) pki(" t_us", t_us);
  pkv(" hwcfg2", nv_rd32(d, base + NV_GSP_HWCFG2));
  pkv(" m0", nv_rd32(d, base + NV_GSP_MAILBOX0));
  pkv(" m1", nv_rd32(d, base + NV_GSP_MAILBOX1));
  pkv(" cpuctl", nv_rd32(d, base + NV_GSP_FALCON_CPUCTL));
  pkv(" rvctl", nv_rd32(d, base + NV_GSP_RISCV_CPUCTL));
  pkv(" rpc", nv_rd32(d, base + NV_GSP_RISCV_RPC));
  pkv(" fmccode", nv_rd32(d, base + NV_GSP_BCR_FMCCODE_LO));
  pkv(" fmcdata", nv_rd32(d, base + NV_GSP_BCR_FMCDATA_LO));
  pkv(" pkc", nv_rd32(d, base + NV_GSP_BCR_PKC_LO));
  pkv(" dmacfg", nv_rd32(d, base + NV_GSP_BCR_DMACFG));
  pkv(" gmb0", nv_rd32(d, base + NV_GSP_ENGINE_MAILBOX0));
  pe();
}

/* How much of 0x110000..0x110100 decodes?  While the PRIV target mask is
 * locked, every word is 0xBADF4100 and this is 0 words; a released window is
 * never uniform.  The rows after the summary are the words that are neither
 * the lock word nor the bus fence word -- i.e. the registers this window
 * really answers, which is what a reviewer can check against dev_gsp.h. */
static int cot_gsp_shape(struct nv_dev *d, const char *tag) {
  uint32_t words = 0, nonlock = 0, transitions = 0, prev = 0, shown = 0;
  for (uint32_t off = 0; off < 0x100u; off += 4) {
    uint32_t v = nv_rd32(d, NV_GSP_BASE + off);
    if (words > 0 && v != prev) transitions++;
    prev = v;
    words++;
    if ((v & 0xFFFFFF00u) != NV_GSP_PRIV_TARGET_LOCKED) nonlock++;
    if ((v & 0xFFFFFF00u) != NV_GSP_PRIV_TARGET_LOCKED &&
        v != NV_GSP_BUS_FENCED && shown < 14) {
      p("gsp-shape ");
      uart_puts(tag);
      uart_puts(" off=");
      uart_print_hex(NV_GSP_BASE + off);
      pkv(" v", v);
      pe();
      shown++;
    }
  }
  p("gsp-shape ");
  uart_puts(tag);
  pki(" words", (int)words);
  pki(" not-locked", (int)nonlock);
  pki(" transitions", (int)transitions);
  pkv(" first", nv_rd32(d, NV_GSP_BASE));
  uart_puts(nonlock ? " verdict=window-decodes\n" : " verdict=locked-uniform\n");
  return (int)nonlock;
}

/* The three words that say whether the core is still doing anything: sampled
 * far apart after the command.  A state that does not move in 20 s is not a
 * boot in progress. */
static void cot_gsp_short_row(struct nv_dev *d, int t_us) {
  p("gsp quiet");
  pki(" t_us", t_us);
  pkv(" hwcfg2", nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2));
  pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
  pkv(" m1", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX1));
  pkv(" rvctl", nv_rd32(d, NV_GSP_BASE + NV_GSP_RISCV_CPUCTL));
  pkv(" gmb0", nv_rd32(d, NV_GSP_BASE + NV_GSP_ENGINE_MAILBOX0));
  pe();
}

/* ---- gating ------------------------------------------------------------- */

/* /COT.CFG arms the probe.  Its content picks the variant: a 'b' asks for the
 * bad-hash control before the real COT, anything else sends the real COT only.
 * (Same rule the other probes follow: the file IS the record of the variant.) */
static int cot_cfg(int *bad_hash_mode) {
  struct file f;
  char buf[32];
  int n;
  if (bad_hash_mode) *bad_hash_mode = 0;
  if (fat16_open("COT.CFG", &f) != 0) return 0;
  n = fat16_read(&f, buf, sizeof buf - 1);
  fat16_close(&f);
  if (n > 0) {
    for (int i = 0; i < n; i++)
      if (buf[i] == 'b' && bad_hash_mode) *bad_hash_mode = 1;
  }
  return 1;
}

/* ------------------------------------------------------------------------ */
/*  the probe                                                               */
/* ------------------------------------------------------------------------ */

/* The layout has to be the vendor's, byte for byte: the payload is memset to
 * zero and then poked through these offsets, so a drift in either number is a
 * COT the FSP would read as garbage. */
_Static_assert(NV_COT_OFF_ARGS + 8 == NV_COT_BYTES, "COT field map");
_Static_assert(NV_FMCP_OFF_SPDMLEN + 4 <= NV_FMCP_BYTES, "FMC params map");

/* Defined below, after the COT probe it reuses the transport of.  /RM.CFG
 * selects it: one card, one boot, one COT -- the RM rung sends its own COT
 * with a populated GSP_FMC_BOOT_PARAMS rather than sending a second one after
 * the COT probe's. */
int nv_fsp_rm_boot(struct nv_dev *d);

int nv_fsp_cot_probe(struct nv_dev *d) {
  const uint32_t base = d ? d->pfsp_base : 0;
  struct nv_dma_map m_image, m_frts, m_args, m_cot;
  struct nv_fw_blob fmc;
  struct cot_fmc sec;
  int boost_err = -1, cot_err = -1;
  int have_hmac = 0, lock_released = 0, mailbox_cleared = 0;
  int bcr_fenced = 0, bad_hash_mode = 0, bad_err = -1;
  int mailbox_writable = 0;
  int frts_ok = 0, got = 0;
  uint64_t t_send = 0;
  uint32_t hwcfg2_last = 0, m0_after = 0, m1_after = 0;
  uint8_t dg[48];
  uint64_t img_len_aligned;

  if (!d) return -EINVAL;
  if (!base) return -ENODEV;
  if (!cot_cfg(&bad_hash_mode)) return -ENOENT;

  /* /RM.CFG turns this boot into the GSP-RM rung: the same COT transport, one
   * message, with the boot params page populated (WPR meta + GSP-RM image +
   * libos/RMARGS + the publish block's address).  One boot, one COT. */
  {
    struct file rf;
    if (fat16_open("RM.CFG", &rf) == 0) {
      fat16_close(&rf);
      return nv_fsp_rm_boot(d);
    }
  }

  m_image.phys = m_image.bus = m_image.len = 0; m_image.pages = 0;
  m_frts = m_args = m_cot = m_image;
  fmc.buf = 0; fmc.size = 0; fmc.pages = 0;
  fmc.map.phys = fmc.map.bus = fmc.map.len = 0; fmc.map.pages = 0;
  sec.image = sec.hash = sec.sig = sec.key = 0;
  sec.image_len = sec.hash_len = sec.sig_len = sec.key_len = 0;

  p("start gsp=cot nvdm=0x14 file=/NVIDIA/GSP/FMC.BIN"
    " (linux-firmware gb202/gsp/fmc-570.144.bin)\n");

  /* ---- A. the state this boot starts from ------------------------------ */
  pkv("pre therm-boot-complete", nv_rd32(d, 0x00AD00BCu));
  pkv(" fsp-ucode-ver", nv_rd32(d, base + 0x334u));
  pkv(" fsp-channel-idle", (nv_rd32(d, base + COT_PFSP_QUEUE_HEAD0) ==
                            nv_rd32(d, base + COT_PFSP_QUEUE_TAIL0)) ? 1u : 0u);
  pe();
  cot_gsp_row(d, "pre", -1);
  (void)cot_gsp_shape(d, "pre");

  /* ---- B. the FMC file -------------------------------------------------- */
  got = nv_fw_load_blob("FMC.BIN", &fmc);
  if (got != 0) {
    p("load FMC.BIN rc=");
    print_int(got);
    p(" -- no COT sent\n");
    p("verdict=no-cot-file\n");
    return 0;
  }
  pki("fmc bytes", (int)fmc.size);
  pkv(" bus", nv_fw_blob_bus(&fmc));
  {
    /* the file's own digest, so the log names the bytes this boot read even if
     * the image is rebuilt later (sha256 is what the tree already has). */
    uint8_t fh[32];
    if (nv_fw_sha256(fmc.buf, fmc.size, fh) == 0) {
      pbytes("file-sha256", fh, 32);
    }
  }
  pe();
  got = cot_fmc_sections(fmc.buf, fmc.size, &sec);
  if (got != 0) {
    p("elf sections rc=");
    print_int(got);
    p(" -- no COT sent\n");
    p("verdict=no-cot-sections\n");
    nv_fw_release(&fmc);
    return 0;
  }
  pki("fmc sections image", (int)sec.image_len);
  pbytes(" image", sec.image, sec.image_len);
  pbytes(" hash", sec.hash, sec.hash_len);
  pbytes(" sig", sec.sig, sec.sig_len);
  pbytes(" key", sec.key, sec.key_len);
  pe();

  /* the sizes the GB202 HAL uses: 48 / 96 / 97, not the AD102 384-byte set */
  have_hmac = (sec.hash_len == NV_COT_HASH_BYTES &&
               sec.sig_len == NV_COT_SIG_BYTES &&
               sec.key_len == NV_COT_PKEY_BYTES);
  pki("auth sizes gb202(48/96/97)", have_hmac);
  if (!have_hmac) {
    p(" -- the file's auth fields are not the GB202 HAL's sizes; no COT sent\n");
    p("verdict=no-cot-auth-sizes\n");
    nv_fw_release(&fmc);
    return 0;
  }

  /* ---- C. the regions, each through nv_dma ------------------------------ */
  img_len_aligned = ((uint64_t)sec.image_len + NV_DMA_PAGE - 1) &
                    ~(NV_DMA_PAGE - 1);
  got = nv_dma_alloc(img_len_aligned, &m_image);
  if (got == 0) {
    const uint8_t *src = sec.image;
    uint8_t *dst = (uint8_t *)phys_to_virt(m_image.phys);
    for (uint32_t i = 0; i < sec.image_len; i++) dst[i] = src[i];
    sha384_digest(dst, sec.image_len, dg);
    p("dma name=fmc_image");
    pkv(" phys", m_image.phys);
    pkv(" bus", nv_dma_bus(&m_image));
    pki(" pages", (int)m_image.pages);
    pkv(" len", m_image.len);
    pe();
    /* the check that makes hash384 a fact about THIS buffer, not a copy */
    {
      int match = 1;
      for (uint32_t i = 0; i < NV_COT_HASH_BYTES; i++)
        if (dg[i] != sec.hash[i]) match = 0;
      pbytes("sha384(image-in-sysmem)", dg, NV_COT_HASH_BYTES);
      uart_puts(" ");
      pbytes("file-hash384", sec.hash, NV_COT_HASH_BYTES);
      uart_puts(" match=");
      uart_puts(match ? "yes" : "NO");
      uart_puts(" (FIPS 180-4 in-kernel, over the bytes the SP will read)\n");
      if (!match) {
        p("verdict=no-cot-hash-mismatch\n");
        nv_dma_free(&m_image);
        nv_fw_release(&fmc);
        return 0;
      }
    }
  } else {
    p("dma name=fmc_image rc=");
    print_int(got);
    p(" -- no COT sent\nverdict=no-cot-nomem\n");
    nv_fw_release(&fmc);
    return 0;
  }

  got = nv_dma_alloc(NV_COT_FRTS_BYTES, &m_frts);
  if (got == 0) {
    frts_ok = 1;
    p("dma name=frts_sysmem");
    pkv(" phys", m_frts.phys);
    pkv(" bus", nv_dma_bus(&m_frts));
    pki(" pages", (int)m_frts.pages);
    pkv(" len", m_frts.len);
    pe();
  } else {
    p("dma name=frts_sysmem rc=");
    print_int(got);
    p(" (1 MiB contiguous: no run). The COT carries 0/0, the vendor's own"
      " \"FRTS in sysmem not requested\" shape\n");
  }

  got = nv_dma_alloc(NV_DMA_PAGE, &m_args);
  if (got != 0) {
    p("dma name=boot_args rc=");
    print_int(got);
    p(" -- no COT sent\nverdict=no-cot-nomem\n");
    goto out;
  } else {
    uint8_t *a = (uint8_t *)phys_to_virt(m_args.phys);
    /* GSP_FMC_BOOT_PARAMS: what the FMC is told to do once it runs.  This boot
     * has no WPR meta and no GSP-RM, so it says exactly that. */
    st32(a, NV_FMCP_OFF_REGKEYS, 0);
    st32(a, NV_FMCP_OFF_BOOTTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
    st32(a, NV_FMCP_OFF_DESCSIZE, 0);
    st64(a, NV_FMCP_OFF_DESCOFF, 0);
    st64(a, NV_FMCP_OFF_WPR_OFF, 0);
    st32(a, NV_FMCP_OFF_WPR_SIZE, 0);
    a[NV_FMCP_OFF_ISRMBOOT] = 0; /* no GSP-RM to boot in this rung */
    st32(a, NV_FMCP_OFF_RMTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
    st64(a, NV_FMCP_OFF_RMARGS, 0);
    st32(a, NV_FMCP_OFF_SPDMTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
    st64(a, NV_FMCP_OFF_SPDMBUF, 0);
    st32(a, NV_FMCP_OFF_SPDMLEN, 0);
    p("dma name=boot_args");
    pkv(" phys", m_args.phys);
    pkv(" bus", nv_dma_bus(&m_args));
    pki(" bytes", (int)NV_FMCP_BYTES);
    pki(" bIsGspRmBoot", a[NV_FMCP_OFF_ISRMBOOT]);
    p(" (no WPR meta, no libos args, no GSP-RM: the FMC is asked to start,"
      " not to boot RM)\n");
  }

  got = nv_dma_alloc(NV_DMA_PAGE, &m_cot);
  if (got != 0) {
    p("dma name=cot rc=");
    print_int(got);
    p(" -- no COT sent\nverdict=no-cot-nomem\n");
    goto out;
  }

  /* ---- D. the translation rows: what IS the bus address? ---------------- */
  {
    struct { const char *n; const struct nv_dma_map *m; } r[4] = {
      {"fmc_image", &m_image}, {"frts_sysmem", &m_frts},
      {"boot_args", &m_args},  {"cot", &m_cot},
    };
    int hw = nv_dma_translation_enabled();
    pki("xlate VT-d(GSTS.TES) live", hw);
    pe();
    for (int i = 0; i < 4; i++) {
      uint64_t bus_hw;
      if (!r[i].m->pages) continue;
      bus_hw = nv_dma_bus(r[i].m);
      p("xlate name=");
      uart_puts(r[i].n);
      pkv(" phys", r[i].m->phys);
      pkv(" bus", bus_hw);
      pki(" bus==phys", (bus_hw == r[i].m->phys) ? 1 : 0);
      p(" (translation off: the number the SP is given is the CPU physical"
        " page, and that is the only reason it is an address at all)");
      pe();
    }
    /* The acceptance row: what the same accessor answers once translation is
     * live.  This is a MEASUREMENT of the accessor's refusal branch, taken
     * after every mapping above was resolved with the hardware answer; the
     * seam is restored immediately and no address from this block is used
     * anywhere. */
    nv_dma_test_set_translation(1);
    for (int i = 0; i < 4; i++) {
      uint64_t bus_t;
      if (!r[i].m->pages) continue;
      bus_t = nv_dma_bus(r[i].m);
      p("xlate-under-translation name=");
      uart_puts(r[i].n);
      pkv(" bus", bus_t);
      pkv(" phys", r[i].m->phys);
      pki(" bus!=phys", (bus_t != r[i].m->phys) ? 1 : 0);
      p(" (NV_DMA_BUS_INVALID: no IOVA allocator, so no bus address exists)");
      pe();
    }
    nv_dma_test_set_translation(-1);
    pki("xlate seam restored, VT-d live", nv_dma_translation_enabled());
    pe();
  }

  /* ---- E. build the COT ------------------------------------------------- */
  {
    uint8_t *c = (uint8_t *)phys_to_virt(m_cot.phys);
    uint64_t fmc_bus = nv_dma_bus(&m_image);
    uint64_t frts_bus = frts_ok ? nv_dma_bus(&m_frts) : 0;
    uint64_t args_bus = nv_dma_bus(&m_args);
    for (uint32_t i = 0; i < NV_COT_BYTES; i++) c[i] = 0;
    st16(c, NV_COT_OFF_VERSION, NV_COT_VERSION_GB202);
    st16(c, NV_COT_OFF_SIZE, NV_COT_BYTES);
    st64(c, NV_COT_OFF_FMC, fmc_bus);
    st64(c, NV_COT_OFF_FRTS_SYS, frts_bus);
    st32(c, NV_COT_OFF_FRTS_SSIZE, frts_ok ? NV_COT_FRTS_BYTES : 0);
    /* FRTS in FB: the vendor derives this offset from the memory manager's
     * FB-end reserve estimate, which does not exist here -- so it is the
     * vendor's own "not requested" shape (0/0) and is named as such. */
    st64(c, NV_COT_OFF_FRTS_VID, 0);
    st32(c, NV_COT_OFF_FRTS_VSIZE, 0);
    for (uint32_t i = 0; i < NV_COT_HASH_BYTES; i++) c[NV_COT_OFF_HASH + i] = sec.hash[i];
    for (uint32_t i = 0; i < NV_COT_PKEY_BYTES; i++) c[NV_COT_OFF_PKEY + i] = sec.key[i];
    for (uint32_t i = 0; i < NV_COT_SIG_BYTES; i++) c[NV_COT_OFF_SIG + i] = sec.sig[i];
    st64(c, NV_COT_OFF_ARGS, args_bus);

    p("cot");
    pki(" bytes", (int)NV_COT_BYTES);
    pki(" version", (int)NV_COT_VERSION_GB202);
    pkv(" gspFmcSysmemOffset", fmc_bus);
    pkv(" frtsSysmemOffset", frts_bus);
    pki(" frtsSysmemSize", frts_ok ? (int)NV_COT_FRTS_BYTES : 0);
    pkv(" frtsVidmemOffset", 0);
    pkv(" gspBootArgsSysmemOffset", args_bus);
    pe();
  }

  /* ---- F. the PBUS words the vendor programs with the FRTS address ------ */
  if (frts_ok) {
    uint32_t lo = (uint32_t)m_frts.bus, hi = (uint32_t)(m_frts.bus >> 32);
    nv_wr32(d, NV_PBUS_SW_FRTS_ADDR_LO32, lo);
    nv_wr32(d, NV_PBUS_SW_FRTS_ADDR_HI32, hi);
    nv_wr32(d, NV_PBUS_SW_FRTS_CONFIG, NV_PBUS_FRTS_CONFIG_SYSMEM(NV_COT_FRTS_BYTES));
    p("pbus frts wr");
    pkv(" lo", lo);
    pkv(" hi", hi);
    pkv(" config", NV_PBUS_FRTS_CONFIG_SYSMEM(NV_COT_FRTS_BYTES));
    pkv(" rb_lo", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_LO32));
    pkv(" rb_hi", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_HI32));
    pkv(" rb_config", nv_rd32(d, NV_PBUS_SW_FRTS_CONFIG));
    pe();
  }

  /* ---- G. clock boost: the vendor's own step before the COT ------------- */
  {
    uint8_t one[4] = {1, 0, 0, 0}; /* FSP_CLOCK_BOOST_FEATURE_ENABLE_SUBMESSAGE_ID */
    boost_err = cot_roundtrip(d, base, "boost", COT_NVDM_CLOCK_BOOST, one, 4);
    ptki("boost", "errorCode", boost_err);
    pe();
    cot_wait_us(d, 5000); /* let the queue go idle again */
  }

  /* ---- H. the GSP mailboxes: the boot-args address, as kgsp does -------- */
  {
    uint32_t write_lo = (uint32_t)m_args.bus;
    uint32_t write_hi = (uint32_t)(m_args.bus >> 32);
    nv_wr32(d, NV_GSP_BASE + NV_GSP_MAILBOX0, write_lo);
    nv_wr32(d, NV_GSP_BASE + NV_GSP_MAILBOX1, write_hi);
    m0_after = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0);
    m1_after = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX1);
    mailbox_writable = (m0_after == write_lo && m1_after == write_hi);
    p("gsp mailbox args=");
    uart_print_hex(m_args.bus);
    pkv(" wrote_m0", write_lo);
    pkv(" rb_m0", m0_after);
    pkv(" rb_m1", m1_after);
    pki(" landed", mailbox_writable);
    if (mailbox_writable)
      p(" (the FMC reads this address and clears the pair at the start of"
        " bootstrap -- so a later 0/0 is the FMC's own write)\n");
    else
      p(" (the write did NOT land: the window answered the decoupler's"
        " privErrTargetLocked word, so the host cannot stuff the args here."
        " Whatever the pair reads afterwards was written on the GPU side)\n");
  }

  /* ---- I. the bad-hash control, then the COT ---------------------------- */
  /* /COT.CFG with a 'b' asks for the control first: the same 860-byte payload
   * with exactly one byte of hash384 flipped.  Nothing else differs -- same
   * image, same signature, same key, same addresses -- so the only thing the
   * SP can be reacting to is the digest. */
  if (bad_hash_mode) {
    uint8_t *c = (uint8_t *)phys_to_virt(m_cot.phys);
    c[NV_COT_OFF_HASH] ^= 0xFFu;
    pbytes("bad-hash", c + NV_COT_OFF_HASH, NV_COT_HASH_BYTES);
    p(" (one byte of hash384 flipped: the control that says whether"
      " FSP_OK means \"I checked this image\" or \"I take any COT\")\n");
    bad_err = cot_roundtrip(d, base, "badcot", COT_NVDM_COT, c, NV_COT_BYTES);
    cot_gsp_row(d, "post-bad", -1);
    p("badcot verdict=");
    uart_puts(bad_err == 0 ? "accepted" : "rejected");
    ptki("badcot", "errorCode", bad_err);
    pki(" window_locked", cot_gsp_locked(
             nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2)));
    pe();
    c[NV_COT_OFF_HASH] ^= 0xFFu; /* put the file's digest back */
  }

  (void)nv_timer_read(d, &t_send);
  cot_err = cot_roundtrip(d, base, "cot", COT_NVDM_COT,
                          (const uint8_t *)phys_to_virt(m_cot.phys),
                          NV_COT_BYTES);

  /* ---- J. the GSP core, sampled after the command ----------------------- */
  {
    static const int at[] = {0, 1000, 5000, 20000, 100000, 300000, 1000000, 2000000};
    int last = 0;
    uint32_t hw_last = 0;
    for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) {
      int t_us;
      if (at[i] > last) cot_wait_us(d, at[i] - last);
      last = at[i];
      t_us = cot_us_since(d, t_send);
      cot_gsp_row(d, "tl", t_us);
      hw_last = nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2);
      m0_after = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0);
      m1_after = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX1);
      if (!cot_gsp_locked(hw_last) && hw_last != 0) {
        lock_released = 1;
        hwcfg2_last = hw_last;
      }
      if (mailbox_writable && m0_after == 0 && m1_after == 0)
        mailbox_cleared = 1;
      /* The RISC-V BCR block (CPUCTL/RPC/FMCCODE/FMCDATA/PKC/DMACFG) answers
       * the bus fence word while the core runs secure: the same 0xBADF5620 the
       * running PFSP shows at its ctl/busy.  So "the FSP wrote the image
       * address into the BCR" is NOT readable from here; the row says which. */
      if (nv_rd32(d, NV_GSP_BASE + NV_GSP_BCR_FMCCODE_LO) == NV_GSP_BUS_FENCED &&
          nv_rd32(d, NV_GSP_BASE + NV_GSP_BCR_FMCDATA_LO) == NV_GSP_BUS_FENCED)
        bcr_fenced = 1;
    }
    (void)cot_gsp_shape(d, "post");
    /* and then far apart: is anything still moving? */
    cot_wait_us(d, 3000000);
    cot_gsp_short_row(d, cot_us_since(d, t_send));
    cot_wait_us(d, 7000000);
    cot_gsp_short_row(d, cot_us_since(d, t_send));
    cot_wait_us(d, 10000000);
    cot_gsp_short_row(d, cot_us_since(d, t_send));
    /* the three words that say what the core is doing now, decoded */
    pkv("gsp hwcfg2", hwcfg2_last);
    pki(" riscv_br_priv_lockdown", (int)((hwcfg2_last >> 13) & 1));
    pki(" hs", (int)((hwcfg2_last >> 8) & 1));
    pki(" riscv", (int)((hwcfg2_last >> 10) & 1));
    pki(" bcr_block_fenced", bcr_fenced);
    pkv(" mailbox0", m0_after);
    pkv(" mailbox1", m1_after);
    pe();
  }

  /* ---- K. the verdict --------------------------------------------------- */
  /* The words that decide it, in the order the vendor's own gates read them:
   *   1. the answer's errorCode       -- did the SP take the command at all
   *   2. the PRIV target mask          -- did it open the GSP to the host
   *   3. HWCFG2[13] RISCV_BR_PRIV_LOCKDOWN -- did the FMC reach the point where
   *      it releases the boot ROM's lockdown (the vendor's own "FMC is up"
   *      signal, _kgspLockdownReleasedOrFmcError)
   *   4. FALCON_MAILBOX0               -- the FMC's error channel; 0 = no error
   * So "fmc-started" is not one condition but a pair: open + no FMC error.  A
   * target that opens while mailbox0 reports an error is the FMC having run and
   * stopped, and this row says that instead of rounding it up. */
  p("verdict=");
  if (cot_err != 0) {
    p(cot_err > 0 ? "sp-rejected" : "sp-silent");
  } else if (!lock_released) {
    p("sp-answered-no-start");
  } else if (((hwcfg2_last >> 13) & 1) == 0 && m0_after == 0) {
    p("fmc-running"); /* lockdown released, no error logged */
  } else if (m0_after != 0) {
    p("fmc-started-stopped"); /* started, then reported 0x.. in mailbox0 */
  } else {
    p("gsp-open"); /* mask released, freedom not yet taken: no evidence of code */
  }
  pki(" resp_error", cot_err);
  pki(" lock_released", lock_released);
  pki(" mailbox_writable", mailbox_writable);
  pki(" mailbox_cleared", mailbox_writable ? mailbox_cleared : -1);
  pki(" bcr_block_fenced", bcr_fenced);
  pkv(" hwcfg2_last", hwcfg2_last);
  pkv(" m0_last", m0_after);
  pkv(" m1_last", m1_after);
  pki(" boost_error", boost_err);
  pki(" badhash_mode", bad_hash_mode);
  pki(" badcot_error", bad_err);
  pe();

  /* ---- L. leave the channel and the PBUS words as an idle host would ---- */
  {
    uint32_t mh = nv_rd32(d, base + COT_PFSP_MSGQ_HEAD0);
    nv_wr32(d, base + COT_PFSP_MSGQ_TAIL0, mh);
    nv_wr32(d, base + COT_PFSP_MSGQ_HEAD0, mh);
    nv_wr32(d, base + COT_PFSP_QUEUE_TAIL0, 0);
    nv_wr32(d, base + COT_PFSP_QUEUE_HEAD0, 0);
    p("end qh=");
    uart_print_hex(nv_rd32(d, base + COT_PFSP_QUEUE_HEAD0));
    pkv(" qt", nv_rd32(d, base + COT_PFSP_QUEUE_TAIL0));
    pkv(" mh", nv_rd32(d, base + COT_PFSP_MSGQ_HEAD0));
    pkv(" mt", nv_rd32(d, base + COT_PFSP_MSGQ_TAIL0));
    pe();
  }

out:
  nv_dma_free(&m_cot);
  nv_dma_free(&m_args);
  if (frts_ok) nv_dma_free(&m_frts);
  nv_dma_free(&m_image);
  nv_fw_release(&fmc);
  return 0;
}

/* ============================================================================
 *  THE GSP-RM RUNG
 *
 *  What this adds over the COT rung above: the FMC (ACR) is told where a
 *  GspFwWprMeta lives, and that block says where the GSP-RM image, its
 *  signature and the GSP-FMC "boot image" are in sysmem.  If the ACR accepts
 *  them it builds WPR2, DMAs and authenticates GSP-RM, starts the RISC-V core,
 *  and GSP-RM's RM task then writes the status queue's producer header and
 *  sends NV_VGPU_MSG_EVENT_GSP_INIT_DONE (function 0x1001).
 *
 *  Every constant here is traced to a file, and the two that are NOT measured
 *  on this part are named as such in the rows:
 *
 *    .fwimage / .fwsignature_gb20x   linux-firmware nvidia/gb202/gsp -> the
 *      unified gsp-570.144.bin (ELF64, ET_REL, EM_RISCV=243, 14 sections:
 *      .fwimage 0x3C99000 @0x40, .fwversion @0x3C99064, then one
 *      .fwsignature_<family> per family; GB202's is .fwsignature_gb20x 0x1000
 *      @0x3C9906C).  The driver's own _kgspPrepareGspRmBinaryImage picks the
 *      signature by family name and kgspCreateRadix3() wraps .fwimage in a
 *      radix3 page tree -- that is the shape implemented here.
 *
 *    boot image + its RM_RISCV_UCODE_DESC   vendor bindata
 *      kgspGetBinArchiveGspRmBoot_GB202: image 217088 B (BINDATA_LABEL_UCODE_IMAGE_PROD,
 *      g_gsp_gb20x_dyn_prd_image.bin) and the 84-byte desc
 *      (g_gsp_gb20x_dyn_prd_desc.bin, raw-deflate) = {version 5, manifestOffset 0,
 *      manifestSize 0xA00, monitorDataOffset 0xA00, monitorDataSize 0xE800,
 *      monitorCodeOffset 0xF200, monitorCodeSize 0x25800, bIsMonitorEnabled 1}.
 *      kgspPopulateWprMeta_GH100 puts exactly those four numbers into the
 *      block's bootloader{Code,Data,Manifest}Offset fields.
 *
 *    WPR meta shape             vendor gsp_fw_wpr_meta.h + kgspPopulateWprMeta_GH100:
 *      on Hopper+ the host fills only the sysmem addresses/sizes, the FRTS size,
 *      the heap sizes, the 128 KiB VGA-workspace request and magic/revision; the
 *      FB offsets are the ACR's to compute and are left 0.
 *
 *    RMARGS layout              GSP_ARGUMENTS_CACHED (vendor gsp_init_args.h),
 *      offsets verified by compiling the header: sharedMemPhysAddr +0, pteCount
 *      +8, cmdQueueOffset +0x10, statQueueOffset +0x18, sr +0x20, gpuInstance
 *      +0x2C, bDmemStack +0x30.  This is the shape the 570 driver's own
 *      kgspPopulateGspRmInitArgs fills (the 535-era 'GSP '-magic variant in
 *      docs/nv-gsp-rpc-contract.md 1.5 is selectable with /RM.CFG = '2', because
 *      this firmware image also contains that magic literal once and the tree
 *      cannot settle which one 570 reads without a boot).
 * ========================================================================== */

#define RM_BOOTIMG_BYTES  217088u       /* vendor GspRmBoot_GB202 PROD image   */
#define RM_BOOT_CODE_OFF  0xF200u       /* monitorCodeOffset in its desc       */
#define RM_BOOT_DATA_OFF  0xA00u        /* monitorDataOffset                   */
#define RM_BOOT_MAN_OFF   0x0u          /* manifestOffset                      */
/* The 615.78.08 pair (this card's one-variable boot): FMC image 287,304 B,
 * input sha256 5b74fefd…, container 287,884 B (hash==SHA384(image), prod key
 * 048bf118…); its own boot image `GspRmBoot_GB202` PROD, 290,816 B, sha256
 * 37159b4b…, desc v6 monitorCode 0xF200/0x37000, data 0xA00.  Staged as
 * /NVIDIA/GSP/{FMC615.BIN,BOOT615.BIN}; the COT payload is the container's
 * `image` section (287,304 B), exactly as the 570/620 pairs were. */
#define RM_BOOT615_BYTES   290816u
#define RM_BOOT615_CODE_OFF 0xF200u
#define RM_BOOT615_DATA_OFF 0xA00u
/* The 580.178.04 pair (§6 rank 2, spent in §13): FMC image 215,624 B, sha256
 * cea2190c…, container 216,204 B (hash == SHA384(image), same prod key
 * 048bf118…), header +0x0C = 0x25800.  Its own GspRmBoot_GB202 PROD is the
 * 217,088-byte image ac068832… — which is the BOOTIMG.BIN every wall boot
 * already staged (monitorCode 0xF200/0x25800, data 0xA00).  So mode 65 changes
 * the FMC image and nothing else: same boot image, same descriptor, same COT. */
#define RM_FMC580_BYTES    216204u
/* linux-firmware nvidia/gb202/gsp/bootloader-570.144.bin: an nvfw container
 * (magic 0x10de, version 1, size 0x31100, header@0x18) whose header IS an
 * RM_RISCV_UCODE_DESC (version 5): manifestOffset 0 size 0xA00, monitorData
 * 0xA00 size 0xA800, monitorCode 0xB200 size 0x25800, bIsMonitorEnabled 1.
 * Its content ends at 0x30A00; the staged file is 200,812 B (the container
 * declares 0x31100 = 200,960, so the tail after the code is the manifest/
 * signature trailer the FSP does not need).  This is the *other* build of the
 * same ACR payload the linux-firmware fmc image carries. */
#define RM_BLB_BYTES      200812u
#define RM_BLB_CODE_OFF   0xB200u
#define RM_BLB_DATA_OFF   0xA00u
#define RM_SIG_BYTES      4096u         /* .fwsignature_gb20x                  */
#define RM_HEAP_BYTES     (128ULL << 20)/* >= vendor's 88 MiB floor + 32 GB term */
#define RM_NONHEAP_BYTES  2293760u      /* kgspGetNonWprHeapSize_GB202 (0x230000) */
/* The vendor's own numbers for this part, from 580.178.04:
 *   heap  = osCarveout(22) + fwHeapParamBaseSize(GH100/GB10x 14) +
 *           align(96 KiB * fbSizeGB, 1 MiB) + align(48 KiB*2048, 1 MiB)
 *           = 22 + 14 + 3 + 96 = 135 MiB for a 32 GB (or 24 GB) part; min 88,
 *           max 280 (g_kernel_gsp_nvoc.c, gsp_fw_heap.h, kgspGetFwHeapSize_IMPL).
 *   nonWpr = 2293760 = 0x230000 -- kgspGetNonWprHeapSize_ad951d
 *           (g_kernel_gsp_nvoc.h:1677-1679), the body GB202 binds.
 * The previous value here, 2228224 = 0x220000, is NOT a non-WPR heap at all:
 * it is memmgrGetFBEndReserveSizeEstimate_GB100's ESTIMATED_RESERVE_FB
 * (mem_mgr_gb100.c:57-67), i.e. the FB-end reserve the *FRTS offset* is
 * computed from.  Sending it as nonWprHeapSize made fh58/rm63/rm64/rm65 label a
 * 0x220000 heap with a name it does not have; the 7 MiB WPR2 move those boots
 * measured stands (it was the heap alone), the label did not.  Corrected here:
 * modes 58/63/64/67 now send the vendor's descriptor value. */
#define RM_HEAP_BYTES_GB202   (135ULL << 20)
#define RM_NONHEAP_BYTES_GB202 2293760u  /* kgspGetNonWprHeapSize_ad951d */
/* The vendor's own 615 stock `GspFwWprMeta.nonWprHeapSize` for GB202 PRO
 * (corpus specs/01_boot-chain.md#wprmeta; its field is honoured 1:1 in the
 * ACR's WPR2 layout, unlike `frtsSize`, which experiments/05 §05.12 measured
 * as decorative -- "the COT value wins").  This lane has sent 0x220000 in
 * every 615/620 boot; mode 64 changes this one field. */
#define RM_NONHEAP_BYTES_V615 0x300000u

#define RM_VGA_WS_BYTES   0x20000u      /* kgspPopulateWprMeta_GH100           */
#define RM_WPRM_BYTES     256u          /* sizeof(GspFwWprMeta), ct_assert'd    */

/* ---- the vendor's FB-tail arithmetic, spelled out ----------------------
 * GspFwWprMeta.pmuReservedSize: kpmuReservedMemorySizeGet()
 *   = align128K( backing 0x900000 + surfaces 0x1000000 + misc 0x4000 )
 *   = align128K( 0x1904000 ) = 0x1920000 = 25.125 MiB
 * (kern_pmu.c:65-76 with g_kern_pmu_nvoc.h:63,68-69; the 0x900000/0x4000 terms
 * are gated on kpmuGetIsSelfInit, which GB202 binds to the TRUE body --
 * g_kern_pmu_nvoc.c:249-251).  The vendor computes this on every boot
 * (kernel_gsp_gh100.c:318) and puts it in the descriptor at +0xF4. */
#define RM_PMU_RESERVED_BYTES 0x1920000u
/* The COT's FB half is an offset from the END of FB, not an address:
 *   frtsVidmemOffset = align2M( memmgrGetFBEndReserveSizeEstimate_GB100
 *                               + kfspGetExtraReservedMemorySize_GH100
 *                               + pmuReservedSize )
 *                    = align2M( 0x220000 + 0x1000 + 0x1920000 ) = 0x1C00000
 * and frtsVidmemSize = 1 MiB (kern_fsp_gh100.c:1427-1460, mem_mgr_gb100.c:57-67,
 * kern_fsp_cot_payload.h:44-47).  Every value this lane sent before mode 67 was
 * 64 MiB (or the 176 MiB probe) -- 28 MiB is the vendor's own. */
#define RM_FBEND_RESERVE_BYTES 0x220000u
#define RM_EXTRA_RESERVED_BYTES 0x1000u
#define RM_FRTS_VID_OFF      0x1C00000ULL
#define RM_FRTS_VID_BYTES    0x100000u

#define RM_LIBOS_ENTRY    32u
#define RM_KIND_CONTIG    1u
#define RM_LOC_SYSMEM     1u

/* /RM.CFG arms this rung and picks the variant:
 *   '1' (default)  full boot, RMARGS = GSP_ARGUMENTS_CACHED (570 host shape)
 *   '2'            same, RMARGS = the 0xC8 'GSP '-magic shape
 *   '3'            same as 1 but WITHOUT the GSP-RM image/signature (control:
 *                  bIsGspRmBoot=1 with a 0 radix3 must not move the FMC)
 *   '4'            same as 1 plus the four libos LOG* regions, zeroed
 *                  (control for "is a missing log region what stops RM")
 *   '5'            same as 1 but with an all-zero signature blob (control for
 *                  "does this ACR verify the GSP-RM signature at all")
 *   '6'            bIsGspRmBoot=1 with gspRmDescSize/Offset = 0 and no image
 *                  (control for "is 5 the answer to a MISSING descriptor")
 *   '7'            same as 1 but gspRmParams.target = 2 (NONCOHERENT_SYSTEM,
 *                  the vendor's attribute for the libos-args page)
 *   '8'            same as 1 but the block's two heap sizes are the vendor's
 *                  f(FB size) values for this part (135 MiB / 0x220000)
 *   '12'           same as 1 but the boot image is linux-firmware's
 *                  bootloader-570.144.bin with ITS desc offsets (code@0xB200),
 *                  i.e. the (FMC, BL) pair linux-firmware ships together
 *   '13'           same as 1 but the COT payload is the vendor's own FMC
 *                  (bindata GspRmFmcGfwProdSigned_GB202, /NVIDIA/GSP/FMCV.BIN)
 *                  instead of linux-firmware's fmc-570.144.bin
 *   '14'           same as 1 but the block's magic is 0xDEADBEEFCAFEF00D
 *                  (control: is the block's magic read at all?)
 *   '15'           same as 1 but the whole block is zeros (control)
 *   '16'           same as 1 but fbSize = 0x800000000 (this part's FB aperture)
 *   '20'           same as 1, then five more COTs in the same boot, each with
 *                  one block field changed (the FMC-restart / field sweep)
 *   '22'           the sweep again, but every COT carries the vendor's
 *                  normal-boot FB half (frtsVidmem = 64 MiB from FB end, 1 MiB)
 *   '67'           the vendor's own shape in one first COT (t_240a0ac8):
 *                  580.178.04's FMC + its own GspRmBoot, nonWprHeapSize
 *                  0x230000, pmuReservedSize 0x1920000 at +0xF4, COT
 *                  frtsVidmem 0x1C00000 / 1 MiB, args-page target 2
 */
static int rm_cfg(int *mode) {
  struct file f;
  char buf[32];
  int n, m = 0, seen = 0;
  if (!mode) return 0;
  *mode = 1;
  if (fat16_open("RM.CFG", &f) != 0) return 0;
  n = fat16_read(&f, buf, sizeof buf - 1);
  fat16_close(&f);
  /* Every digit is part of the number: '1' is mode 1, '12' is mode 12.  (The
   * first version stopped at the first digit, which silently made every
   * two-digit variant a repeat of mode 1 -- a boot that "changed nothing".) */
  for (int i = 0; i < n; i++) {
    if (buf[i] >= '0' && buf[i] <= '9') {
      m = (seen ? m * 10 : 0) + (buf[i] - '0');
      seen = 1;
      if (m > 99) { m /= 10; break; }
    } else if (seen) {
      break;
    }
  }
  *mode = seen ? m : 1;
  return 1;
}

/* The eight characters of a libos region name, first character in the LOW byte
 * (vendor _kgspGenerateInitArgId: id = (id << 8) | c, so "LOGINIT" is
 * 0x004C4F47494E4954 and "RMARGS" is 0x0000524D41524753). */
static uint64_t rm_id8(const char *s) {
  uint64_t v = 0;
  for (int i = 0; i < 8 && s[i]; i++) v = (v << 8) | (uint8_t)s[i];
  return v;
}

/* One LibosMemoryRegionInitArgument: id8, pa, size, kind, loc. */
static void rm_libos_entry(uint8_t *p, const char *name, uint64_t pa,
                           uint64_t size) {
  st64(p, 0, rm_id8(name));
  st64(p, 8, pa);
  st64(p, 16, size);
  p[24] = RM_KIND_CONTIG;
  p[25] = RM_LOC_SYSMEM;
}

/* RMARGS, variant A: GSP_ARGUMENTS_CACHED (offsets from the compiled header). */
static void rm_args_a(uint8_t *p, uint64_t blk, uint32_t ptes) {
  st64(p, 0x00, blk);            /* mq.sharedMemPhysAddr = PTE[0]  */
  st32(p, 0x08, ptes);           /* mq.pageTableEntryCount         */
  st64(p, 0x10, NV_RPC_CMD_OFF); /* mq.cmdQueueOffset              */
  st64(p, 0x18, NV_RPC_STAT_OFF);/* mq.statQueueOffset             */
  /* sr.oldLevel/flags/bInPMTransition = 0 (cold boot) */
  st32(p, 0x2C, 0);              /* gpuInstance                    */
  p[0x30] = 1;                   /* bDmemStack (default placement) */
}

/* RMARGS, variant B: the 0xC8 'GSP '-magic shape (docs/nv-gsp-rpc-contract 1.5). */
static void rm_args_b(uint8_t *p, uint64_t blk, uint32_t ptes) {
  st32(p, 0x00, 0x20505347u);    /* 'G','S','P',' ' little-endian   */
  st16(p, 0x04, 0xC8);           /* size of the structure          */
  st64(p, 0x10, 0);              /* mq.flags                       */
  st64(p, 0x18, blk);            /* mq.sharedMemPhysAddr           */
  st64(p, 0x20, NV_RPC_CMD_OFF);
  st64(p, 0x28, NV_RPC_STAT_OFF);
  st32(p, 0x30, 0x30);           /* queueElementHdrSize            */
  st32(p, 0x34, 0x1000);         /* queueElementSizeMin            */
  st32(p, 0x38, 0x10000);        /* queueElementSizeMax            */
  st32(p, 0x3C, 4);              /* queueHeaderAlign (log2)        */
  st32(p, 0x40, 12);             /* queueElementAlign (log2)       */
  st32(p, 0x44, ptes);           /* pageTableEntryCount            */
  st32(p, 0x64, 0);              /* gpuInstance                    */
}

/* The rung's own prefix.  Most rows below are printed with p() (this file's
 * [NV-FSPCOT] prefix, because the transport and the samplers are the COT
 * rung's); the rung BOUNDARIES -- the start line and the verdict -- carry
 * [NV-FSPRM], and the status-queue rows carry [NV-RPC] from nv_gsp.c. */
static void rm_p(const char *s) { uart_puts("[NV-FSPRM] "); uart_puts(s); }

/* The GSP-FMC's formatted error word (GB100 map, index 0x37 = 0x04DC).  The
 * driver writes 0 here before it hands the FMC its boot args
 * (gpuResetGspFmcErrorCode_HAL in _kgspBootGspFmc_GH100) and reads it back as
 * the *reason* when the lockdown was never released; a non-zero value is the
 * FMC naming the check it stopped on, which MAILBOX0 (an 8-bit code) cannot. */
static uint32_t rm_fmc_error_raw(struct nv_dev *d) {
  return nv_rd32(d, NV_PBUS_PRI_GSP_FMC_ERROR);   /* 0x14DC, the host's alias */
}
static void rm_fmc_error_row(struct nv_dev *d, const char *tag) {
  uint32_t v = rm_fmc_error_raw(d);
  uint32_t variant = (v >> 28) & 0xFu;
  uint32_t part = (v >> 24) & 0xFu;
  p("fmcerr "); uart_puts(tag);
  pkv(" reg=0x14dc", v);
  pkv(" alias_0x4dc", nv_rd32(d, NV_PBUS_ZB_SCRATCH_GSP_FMC_ERROR));
  pkv(" variant", variant);
  pkv(" partition", (part - 1u) & 0xFu);   /* encoded: id+1 */
  if (variant == 1u) {
    pkv(" err", v & 0xFFFFu);              /* generic error code 15:0  */
    pkv(" info", (v >> 16) & 0xFFu);       /* additional info 23:16    */
  } else {
    pkv(" sk_err", (v >> 8) & 0xFFu);      /* SK error 15:8            */
    pkv(" sk_phase", v & 0xFFu);           /* phase 7:0                */
  }
  pe();
}

/* ---- the FB side (BAR1): what the FSP and the ACR wrote, if anything ---- *
 * The GSP window answers the PRIV fence while the boot ROM holds lockdown, so
 * the only read path not covered by it is the framebuffer aperture.  The WPR
 * meta's own header (gsp_fw_wpr_meta.h) says the *copy* the ACR verifies and
 * patches is DMA'd into FB at gspFwWprStart and locked in WPR2 -- "Initialized
 * by CPU-RM and DMA'd to FB, at the end of what will be WPR2.  Verified, and
 * locked in WPR2 by Booter" -- so the sysmem copy this file hands over may
 * legitimately come back untouched while the FB copy carries the ACR's work.
 * These rows look for both magics in FB and dump the candidate addresses the
 * PFB WPR2 pair can plausibly mean (bytes, or 256-byte units). */
#define NV_FB_WPR_MAGIC_D0 0x371A60B3u /* 0xdc3aae21371a60b3, low dword */
#define NV_FB_GFW_MAGIC    0x49574647u /* "GFWI": the FRTS blob's header    */

static void fb_dump(struct nv_dev *d, uint64_t off, int ndw, const char *tag) {
  struct nv_vram_win w;
  if (nv_vram_open(d, off, 4096, &w) != 0) {
    p("fb "); uart_puts(tag); p(" off="); uart_print_hex(off); p(" open=FAIL\n");
    return;
  }
  p("fb "); uart_puts(tag); p(" off="); uart_print_hex(off);
  for (int i = 0; i < ndw; i++) {
    p(" d"); print_int(i); uart_puts("=0x");
    uart_print_hex(nv_vram_rd32(&w, (uint32_t)(i * 4)));
  }
  pe();
  nv_vram_close(&w);
}

/* Scan [base, base+len) for one magic dword at `stride`, 64 MiB ioremap
 * chunks.  A 4 KiB stride covers every page of the band the WPR layout lives
 * in; 8192 reads/GiB at that stride is a few hundred ms of MMIO. */
static void fb_scan(struct nv_dev *d, uint64_t base, uint64_t len, uint32_t stride,
                    uint32_t mag, const char *name) {
  const uint64_t chunk = 64ULL << 20;
  uint32_t hits = 0;
  uint64_t off = 0;
  p("fbs "); uart_puts(name); p(" base="); uart_print_hex(base);
  p(" len="); uart_print_hex(len); p(" stride=0x"); uart_print_hex(stride); pe();
  while (off < len && hits < 8) {
    struct nv_vram_win w;
    uint64_t clen = (len - off < chunk) ? (len - off) : chunk;
    if (nv_vram_open(d, base + off, (uint32_t)clen, &w) != 0) {
      p("fbs "); uart_puts(name); p(" chunk="); uart_print_hex(base + off);
      p(" open=FAIL\n");
      off += clen;
      continue;
    }
    for (uint64_t o = 0; o + 8 <= clen; o += stride) {
      if (nv_vram_rd32(&w, (uint32_t)o) == mag) {
        hits++;
        p("fbs "); uart_puts(name); p(" HIT off="); uart_print_hex(base + off + o);
        for (int i = 0; i < 6; i++) {
          p(" d"); print_int(i); uart_puts("=0x");
          uart_print_hex(nv_vram_rd32(&w, (uint32_t)(o + (uint64_t)i * 4)));
        }
        pe();
        if (hits >= 8) break;
      }
    }
    nv_vram_close(&w);
    off += clen;
  }
  p("fbs "); uart_puts(name); p(" hits="); print_int((int)hits); pe();
}

/* The 128 KiB census above cannot see a 256-byte structure: a WPR meta at
 * WPR2_start + nonWprHeapSize (0x230000) lands between two samples of that
 * stride, so "meta=0" from it is not evidence that the ACR wrote nothing.
 * This is the measurement that can: a 256-byte stride over the head of WPR2
 * (where the WPR/LSB header, the meta copy and the heap start) plus a 64 KiB
 * stride over the whole band (which is enough to see a 60 MB image copy). */
static void wpr2_census_at(struct nv_dev *d, uint64_t lo, uint64_t hi, const char *tag) {
  uint64_t head_len = 8ULL << 20;
  const uint64_t chunk = 16ULL << 20;
  int nz_head = 0, nz_all = 0, meta = 0, print_budget = 8;
  uint64_t first_nz_head = 0, first_nz_all = 0, last_nz_all = 0;
  uint32_t last_v = 0;

  p("wpr2c "); uart_puts(tag);
  pkv(" lo", lo); pkv(" hi", hi);
  pe();
  if (lo >= 0x800000000ULL || hi <= lo) {
    p("wpr2c "); uart_puts(tag); p(" skipped (bad pair)\n");
    return;
  }
  /* head: 256-byte stride over the first 8 MiB */
  for (uint64_t off = 0; off < head_len; off += chunk) {
    struct nv_vram_win w;
    uint64_t clen = (head_len - off < chunk) ? (head_len - off) : chunk;
    if (nv_vram_open(d, lo + off, (uint32_t)clen, &w) != 0) continue;
    for (uint64_t o = 0; o + 8 <= clen; o += 0x100ULL) {
      uint32_t v = nv_vram_rd32(&w, (uint32_t)o);
      if (v == 0xDEADDEADu) continue;
      if (v == 0) continue;
      nz_head++;
      if (!first_nz_head) first_nz_head = lo + off + o;
      if (v == NV_FB_WPR_MAGIC_D0 && print_budget > 0) {
        meta++;
        print_budget--;
        p("wpr2c META at 0x"); uart_print_hex(lo + off + o);
        for (int i = 0; i < 8; i++) {
          p(" d"); print_int(i); uart_puts("=0x");
          uart_print_hex(nv_vram_rd32(&w, (uint32_t)(o + (uint64_t)i * 4)));
        }
        pe();
      }
    }
    nv_vram_close(&w);
  }
  /* whole band: 64 KiB stride */
  for (uint64_t off = 0; off < hi - lo; off += chunk) {
    struct nv_vram_win w;
    uint64_t clen = (hi - lo - off < chunk) ? (hi - lo - off) : chunk;
    if (nv_vram_open(d, lo + off, (uint32_t)clen, &w) != 0) continue;
    for (uint64_t o = 0; o + 8 <= clen; o += 0x10000ULL) {
      uint32_t v = nv_vram_rd32(&w, (uint32_t)o);
      if (v == 0xDEADDEADu || v == 0) continue;
      nz_all++;
      if (!first_nz_all) first_nz_all = lo + off + o;
      last_nz_all = lo + off + o;
      last_v = v;
    }
    nv_vram_close(&w);
  }
  p("wpr2c done nz_head="); print_int(nz_head);
  p(" first_nz_head=0x"); uart_print_hex(first_nz_head);
  p(" meta="); print_int(meta);
  p(" nz_all="); print_int(nz_all);
  p(" first_nz_all=0x"); uart_print_hex(first_nz_all);
  p(" last_nz_all=0x"); uart_print_hex(last_nz_all);
  p(" last_val=0x"); uart_print_hex(last_v);
  pe();
}

static void rm_fb_probe(struct nv_dev *d, const char *tag) {
  static const uint64_t cand[] = {
    0x0000000007E61200ULL,  /* 0x1FA824 as bytes (132.6 MB)              */
    0x00000007E6120000ULL,  /* 0x1FA824 as 256-byte units (PFB grain)    */
    0x0000000007F1EE00ULL,  /* 0x1FA828 as bytes                         */
    0x00000007F1EE0000ULL,  /* 0x1FA828 as 256-byte units                */
    0x0000000004000000ULL,  /* this COT's frtsVidmemOffset (64 MiB)      */
    0x0000000004100000ULL,  /* ... + 1 MiB (where the FRTS would end)    */
    0x00000007FFEE0000ULL,  /* fbSize - 1.25 MiB: FRTS at the FB end     */
    0x00000007FFFE0000ULL,  /* fbSize - 128 KiB: gspFwWprEnd slot        */
    0x00000007FFF00000ULL,  /* fbSize - 1 MiB                            */
  };
  p("fbprobe "); uart_puts(tag);
  pkv(" fbSize", 0x800000000ULL);
  pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
  pkv(" wpr2_hi", nv_rd32(d, 0x1FA828u));
  pkv(" wpr1_lo", nv_rd32(d, 0x1FA81Cu));
  pkv(" pbus_frts_lo", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_LO32));
  pkv(" pbus_frts_hi", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_HI32));
  pkv(" pbus_frts_cfg", nv_rd32(d, NV_PBUS_SW_FRTS_CONFIG));
  pkv(" zb_frts_lo", nv_rd32(d, NV_PBUS_ZB_FRTS_ADDR_LO32));
  pkv(" pfb_moves_a", nv_rd32(d, 0x1FA804u));
  pkv(" pfb_moves_b", nv_rd32(d, 0x1FA808u));
  pe();
  for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++)
    fb_dump(d, cand[i], 4, "cand");
  /* The layout diagram puts FRTS/heap/meta in the top of FB; the COT's own
   * frtsVidmemOffset points at the bottom.  Both bands, both magics. */
  fb_scan(d, 0x7C0000000ULL, 0x40000000ULL, 0x1000u, NV_FB_WPR_MAGIC_D0, "meta-top1G");
  fb_scan(d, 0x000000000ULL, 0x04000000ULL, 0x1000u, NV_FB_WPR_MAGIC_D0, "meta-first64M");
  fb_scan(d, 0x7C0000000ULL, 0x40000000ULL, 0x1000u, NV_FB_GFW_MAGIC, "gfw-top1G");
  fb_scan(d, 0x000000000ULL, 0x04000000ULL, 0x1000u, NV_FB_GFW_MAGIC, "gfw-first64M");
}

/* Full-FB census at 128 KiB stride: classify every sampled dword and report
 * the first offset of each class plus any WPR-meta / GFW magic.  One pass over
 * 32 GiB is 262144 MMIO reads; it answers "is there ANY FB address the ACR
 * wrote a copy to" without guessing a layout. */
static void fb_census(struct nv_dev *d, const char *tag) {
  const uint64_t len = 0x800000000ULL;
  const uint64_t chunk = 64ULL << 20;
  uint64_t off = 0;
  int zero = 0, dead = 0, fence = 0, other = 0, meta = 0, gfw = 0;
  uint64_t first_dead = 0, first_fence = 0, first_other = 0, last_off = 0;
  uint32_t last_val = 0;
  p("census "); uart_puts(tag); p(" len=0x"); uart_print_hex(len);
  p(" stride=0x20000\n");
  while (off < len) {
    struct nv_vram_win w;
    uint64_t clen = (len - off < chunk) ? (len - off) : chunk;
    if (nv_vram_open(d, off, (uint32_t)clen, &w) != 0) { off += clen; continue; }
    for (uint64_t o = 0; o + 8 <= clen; o += 0x20000ULL) {
      uint32_t v = nv_vram_rd32(&w, (uint32_t)o);
      if (v == 0) { zero++; continue; }
      if (v == 0xDEADDEADu) { if (!dead) first_dead = off + o; dead++; continue; }
      if (v == 0xBAD0AC00u) { if (!fence) first_fence = off + o; fence++; continue; }
      other++;
      last_off = off + o;
      last_val = v;
      if (!first_other) first_other = off + o;
      if (v == NV_FB_WPR_MAGIC_D0) {
        meta++;
        p("census META at 0x"); uart_print_hex(off + o);
        p(" d1=0x"); uart_print_hex(nv_vram_rd32(&w, (uint32_t)(o + 4)));
        p(" d2=0x"); uart_print_hex(nv_vram_rd32(&w, (uint32_t)(o + 8)));
        p(" f8=0x"); uart_print_hex(nv_vram_rd32(&w, (uint32_t)(o + 0xF8)));
        p(" wprstart=0x"); uart_print_hex(nv_vram_rd32(&w, (uint32_t)(o + 0x70)));
        p(" wprend=0x"); uart_print_hex(nv_vram_rd32(&w, (uint32_t)(o + 0xA8)));
        pe();
      }
      if (v == NV_FB_GFW_MAGIC) {
        gfw++;
        p("census GFW at 0x"); uart_print_hex(off + o); pe();
      }
    }
    nv_vram_close(&w);
    off += clen;
  }
  p("census done zero="); print_int(zero);
  p(" dead="); print_int(dead); p(" first_dead=0x"); uart_print_hex(first_dead);
  p(" fence="); print_int(fence); p(" first_fence=0x"); uart_print_hex(first_fence);
  p(" other="); print_int(other);
  p(" first_other=0x"); uart_print_hex(first_other);
  p(" last_other=0x"); uart_print_hex(last_off);
  p(" last_val=0x"); uart_print_hex(last_val);
  p(" meta="); print_int(meta); p(" gfw="); print_int(gfw); pe();
}

/* The FSP's CMS2 log, through the OOB hub's indirect window -- the vendor's own
 * kfspDumpDebugState_GB100 reads it exactly this way when the FSP never
 * completes: write NV_POOBHUB_RCV_INDIRECT_CMS2_MEM_RD_ADDR (0x8AA374) with
 * CMS2_LOG_START+i (0x50..0x7F) and read the data word (0x8AA378).  48 dwords.
 * The FMC's code is encrypted; this buffer is not, and it is the only in-band
 * place the FSP's own view of the abort could be written in text.  Read-only
 * except for the address latch, which is the register's defined use. */
static void rm_cms2_log(struct nv_dev *d, const char *tag) {
  p("cms2 ");
  uart_puts(tag);
  pe();
  for (int row = 0; row < 6; row++) {
    uint32_t v[8];
    char txt[9];
    for (int k = 0; k < 8; k++) {
      uint32_t a = 0x50u + (uint32_t)(row * 8 + k);
      nv_wr32(d, NV_POOBHUB_CMS2_ADDR, a);
      v[k] = nv_rd32(d, NV_POOBHUB_CMS2_DATA);
      char c = (char)(v[k] & 0xFFu);
      txt[k] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    txt[8] = 0;
    p("cms2 ");
    uart_puts(tag);
    p(" w=");
    print_int(row * 8);
    for (int k = 0; k < 8; k++) {
      p(" ");
      uart_print_hex(v[k]);
    }
    p("  \"");
    uart_puts(txt);
    p("\"\n");
  }
}

/* One sample of everything the RM's progress would move. */
static void rm_sample(struct nv_dev *d, const char *tag, int t_ms) {
  p("rm "); uart_puts(tag);
  pki(" t_ms", t_ms);
  pkv(" hwcfg2", nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2));
  pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
  pkv(" m1", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX1));
  pkv(" os", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_OS));
  pkv(" rm", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_RM));
  pkv(" rvctl", nv_rd32(d, NV_GSP_BASE + NV_GSP_RISCV_CPUCTL));
  pkv(" gmb0", nv_rd32(d, NV_GSP_BASE + NV_GSP_ENGINE_MAILBOX0));
  /* §8 of docs/gb202-rm-objects.md: the vendor's failure readbacks, added
   * before this boot and costing none.  The FSP's scratch is not encrypted,
   * and the 0x0B transition has never been sampled against it. */
  pkv(" s2_0", nv_rd32(d, NV_PFSP_SCRATCH_G2_BASE + 0u));
  pkv(" s2_1", nv_rd32(d, NV_PFSP_SCRATCH_G2_BASE + 4u));
  pkv(" s2_2", nv_rd32(d, NV_PFSP_SCRATCH_G2_BASE + 8u));
  pkv(" s2_3", nv_rd32(d, NV_PFSP_SCRATCH_G2_BASE + 12u));
  pkv(" mb1", nv_rd32(d, NV_PGSP_MAILBOX_BASE + 4u));
  pkv(" mb2", nv_rd32(d, NV_PGSP_MAILBOX_BASE + 8u));
  pkv(" mb3", nv_rd32(d, NV_PGSP_MAILBOX_BASE + 12u));
  pkv(" i2cs", nv_rd32(d, NV_THERM_I2CS_SCRATCH));
  pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
  pkv(" wpr2_hi", nv_rd32(d, 0x1FA828u));
  pkv(" fmcerr", rm_fmc_error_raw(d));
  pe();
  nv_gsp_rpc_peer_row(tag, t_ms);
}

int nv_fsp_rm_boot(struct nv_dev *d) {
  const uint32_t base = d ? d->pfsp_base : 0;
  struct nv_dma_map m_image, m_frts, m_args, m_cot, m_bootimg, m_sig, m_wprm,
                     m_rmargs, m_libos, m_rmimg, m_tree, m_alt;
  struct nv_fw_blob fmc, bootimg, sig, gsprm;
  struct cot_fmc sec;
  int mode = 1, frts_ok = 0, boost_err = -1, cot_err = -1;
  int lock_released = 0, linked = 0, initdone = 0, statq_written = 0;
  int falcon_os_written = 0;
  int mailbox_writable = 0, radix3_ok = 0;
  uint64_t t_send = 0, blk_bus = 0;
  uint32_t blk_ptes = 0;
  uint32_t hwcfg2_last = 0, m0_last = 0, m1_last = 0;
  struct nv_wpr_meta_in meta;
  struct nv_wpr_meta_in back;
  uint32_t tree_pages = 0;
  /* Which boot image this mode hands the ACR, and the offsets inside it that
   * GspFwWprMeta.bootloader{Code,Data,Manifest}Offset declare.  Mode 12 swaps
   * the pair for linux-firmware's own: bootloader-570.144.bin is an nvfw
   * container whose 84-byte RM_RISCV_UCODE_DESC (version 5) at +0x18 says
   * manifest@0/0xA00, monitorData@0xA00/0xA800, monitorCode@0xB200/0x25800 --
   * a *different build* (0x30A00 content) from the vendor's dyn_prd image
   * (0x34A00 content, code@0xF200) the 570 driver ships. */
  const char *bl_name = "BOOTIMG.BIN";
  uint32_t bl_want = RM_BOOTIMG_BYTES;
  uint64_t bl_code = RM_BOOT_CODE_OFF;
  uint64_t bl_data = RM_BOOT_DATA_OFF;
  uint64_t bl_man = RM_BOOT_MAN_OFF;

  if (!d) return -EINVAL;
  if (!base) return -ENODEV;
  if (!rm_cfg(&mode)) return -ENOENT;
  if (mode == 12 || mode == 55) {
    bl_name = "BLB.BIN";
    bl_want = RM_BLB_BYTES;
    bl_code = RM_BLB_CODE_OFF;
    bl_data = RM_BLB_DATA_OFF;
    bl_man = 0;
  }
  if (mode == 63 || mode == 64 || mode == 66) {
    /* Mode 63 = mode 58's host state (vendor heap pair, COT FRTS 64 MiB/1 MiB,
     * descriptor frts_size 0) with the ONLY variable the zero-boot survey names:
     * the 615.78.08 FMC image and its own GspRmBoot pair.  Nothing else moves.
     * Mode 64 = mode 63 with exactly one descriptor field changed:
     * nonWprHeapSize 0x220000 (kgspGetNonWprHeapSize_GB202) -> 0x300000 (the
     * vendor's 615 stock).  CMS2 is not re-read: §11.4 proved that window
     * static and textless, so mode 64 carries no extra instrument. */
    bl_name = "BOOT615.BIN";
    bl_want = RM_BOOT615_BYTES;
    bl_code = RM_BOOT615_CODE_OFF;
    bl_data = RM_BOOT615_DATA_OFF;
    bl_man = 0;
  }

  m_image.phys = m_image.bus = m_image.len = 0; m_image.pages = 0;
  m_frts = m_args = m_cot = m_bootimg = m_sig = m_wprm = m_image;
  m_rmargs = m_libos = m_rmimg = m_tree = m_alt = m_image;
  fmc.buf = 0; fmc.size = 0; fmc.pages = 0;
  fmc.map.phys = fmc.map.bus = fmc.map.len = 0; fmc.map.pages = 0;
  bootimg = sig = gsprm = fmc;
  sec.image = sec.hash = sec.sig = sec.key = 0;
  sec.image_len = sec.hash_len = sec.sig_len = sec.key_len = 0;

  rm_p("start gsp=rm mode=");
  print_int(mode);
  p(" files=/NVIDIA/GSP/{FMC.BIN,BOOTIMG.BIN,GSPRM.BIN,GSPRM.SIG}"
    " (mode 63/64/66: FMC615.BIN,BOOT615.BIN; mode 65/67: FMCV.BIN,"
    "BOOTIMG.BIN)\n");
  pkv("pre therm-boot-complete", nv_rd32(d, 0x00AD00BCu));
  pkv(" fsp-ucode-ver", nv_rd32(d, base + 0x334u));
  pe();
  cot_gsp_row(d, "pre", -1);
  if (mode == 63) rm_cms2_log(d, "pre");

  /* ---- 1. the COT payload image (identical to the COT rung) ------------- */
  /* Mode 13 boots the vendor's own FMC (bindata GspRmFmcGfwProdSigned_GB202,
   * 215,624 B) instead of linux-firmware's fmc-570.144.bin (199,240 B): two
   * different prod builds of this part's ACR, both of which the FSP accepts.
   * Everything else in that mode is mode 1. */
  /* The blob every mode loads, by name, and where each name comes from (the
   * sizes and sha256s are restated in SWARM-REPORT-GB202-FMCHARVEST.md and in
   * docs/gb202-rm-objects.md 10):
   *
   *   FMC.BIN      199,820 B  linux-firmware gb202/gsp/fmc-570.144.bin
   *   FMCV.BIN     216,204 B  580.178.04's GspRmFmcGfwProdSigned_GB202
   *                           (sha256 6adbd92d...) -- mode 13 and mode 65
   *   FMC615.BIN   287,884 B  615.78.08's container, built from the survey
   *                           artifact fmc-615.78.08.bin (287,304 B image,
   *                           sha256 5b74fefd...) wrapped by fw/mkfmc.py with
   *                           that release's own hash/sig/pkey -- modes 63/64/66
   *
   * Mode 65 used to name FMC580C.BIN, a name that existed only in the staging
   * script (build65.sh copied fw/FMCV.BIN onto it in the image).  It now asks
   * for the blob that actually exists.  Firmware images are *.BIN and stay
   * out of git; the fw/ staging dir and the recipe are what is preserved. */
  {
    const char *fmc_name = (mode == 13) ? "FMCV.BIN"
                         : (mode == 63 || mode == 64 || mode == 66) ? "FMC615.BIN"
                         : (mode == 65 || mode == 67) ? "FMCV.BIN"
                                                       : "FMC.BIN";
    if (nv_fw_load_blob(fmc_name, &fmc) != 0) {
      p("load ");
      uart_puts(fmc_name);
      p(" failed -- no COT sent\nverdict=no-fmc-file\n");
      return 0;
    }
  }
  if ((mode == 65 || mode == 67) && fmc.size != RM_FMC580_BYTES) {
    p("mode 65/67 needs the 216,204-byte 580.178.04 container -- no COT sent\n"
      "verdict=fmc-size-mismatch\n");
    nv_fw_release(&fmc);
    return 0;
  }
  if (cot_fmc_sections(fmc.buf, fmc.size, &sec) != 0 ||
      sec.hash_len != NV_COT_HASH_BYTES || sec.sig_len != NV_COT_SIG_BYTES ||
      sec.key_len != NV_COT_PKEY_BYTES) {
    p("FMC sections are not the GB202 48/96/97 set -- no COT sent\n"
      "verdict=no-cot-auth-sizes\n");
    nv_fw_release(&fmc);
    return 0;
  }
  pki("fmc image bytes", (int)sec.image_len);
  pkv(" fmc img_d0", ld32(sec.image, 0));
  pkv(" img_d1", ld32(sec.image, 4));
  pe();

  /* ---- 2. the three GSP-RM-side artifacts ------------------------------- */
  if (mode != 3 && mode != 6) {
    if (nv_fw_load_blob("GSPRM.BIN", &gsprm) != 0) {
      p("load GSPRM.BIN failed (the 63.5 MB .fwimage) -- no COT sent\n"
        "verdict=no-gsprm-file\n");
      nv_fw_release(&fmc);
      return 0;
    }
    pki("gsprm bytes", (int)gsprm.size);
    pkv(" bus", nv_fw_blob_bus(&gsprm));
    pe();
    if (nv_fw_load_blob("GSPRM.SIG", &sig) != 0) {
      p("load GSPRM.SIG failed (.fwsignature_gb20x) -- no COT sent\n"
        "verdict=no-gsprm-sig-file\n");
      nv_fw_release(&gsprm);
      nv_fw_release(&fmc);
      return 0;
    }
    pki("sig bytes", (int)sig.size);
    pe();
  }
  if (nv_fw_load_blob(bl_name, &bootimg) != 0 || bootimg.size != bl_want) {
    p("load ");
    uart_puts(bl_name);
    p(" failed (wrong size for this mode) -- no COT sent\n"
      "verdict=no-bootimg-file\n");
    if (mode != 3 && mode != 6) { nv_fw_release(&gsprm); nv_fw_release(&sig); }
    nv_fw_release(&fmc);
    return 0;
  }
  pki("bootimg bytes", (int)bootimg.size);
  p(" file="); uart_puts(bl_name);
  pkv(" d0", ld32(bootimg.buf, 0));
  pkv(" d1", ld32(bootimg.buf, 4));
  pkv(" d2", ld32(bootimg.buf, 8));
  pkv(" d3", ld32(bootimg.buf, 12));
  pkv(" code_off", bl_code);
  pkv(" data_off", bl_data);
  pe();

  /* ---- 3. sysmem regions, every address through nv_dma ------------------ */
  if (nv_dma_alloc(((uint64_t)sec.image_len + NV_DMA_PAGE - 1) &
                   ~(NV_DMA_PAGE - 1), &m_image) != 0) {
    p("dma fmc_image: no run -- no COT sent\nverdict=no-cot-nomem\n");
    goto out_files;
  }
  {
    const uint8_t *src = sec.image;
    uint8_t *dst = (uint8_t *)phys_to_virt(m_image.phys);
    for (uint32_t i = 0; i < sec.image_len; i++) dst[i] = src[i];
  }
  if (nv_dma_alloc(NV_COT_FRTS_BYTES, &m_frts) == 0) frts_ok = 1;

  if (nv_dma_alloc(NV_DMA_PAGE, &m_args) != 0 ||
      nv_dma_alloc(NV_DMA_PAGE, &m_cot) != 0) {
    p("dma args/cot: no run -- no COT sent\nverdict=no-cot-nomem\n");
    goto out;
  }
  if (nv_dma_alloc(NV_DMA_PAGE, &m_wprm) != 0 ||
      nv_dma_alloc(NV_DMA_PAGE, &m_rmargs) != 0 ||
      nv_dma_alloc(NV_DMA_PAGE, &m_libos) != 0 ||
      nv_dma_alloc(NV_DMA_PAGE, &m_alt) != 0) {
    p("dma wprm/rmargs/libos/alt: no run -- no COT sent\nverdict=no-rmargs-nomem\n");
    goto out;
  }

  /* ---- 4. the publish block, BEFORE the RMARGS that names it ------------ */
  {
    int prc = nv_gsp_rpc_publish_bus(d, &blk_bus, &blk_ptes);
    if (prc != 0) {
      p("publish block FAIL rc=");
      print_int(prc);
      p(" -- the RM would have nothing to link; no COT sent\n"
        "verdict=no-block\n");
      goto out;
    }
    /* A7: the vendor queues GSP_SET_SYSTEM_INFO then SET_REGISTRY into the
     * command queue before the RM image starts (kgspQueueAsyncInitRpcs,
     * kernel_gsp.c:3792-3815, called at :4179).  Committed here -- after the
     * block is published, before the COT -- and NOT notified: nothing is
     * listening yet, and RM reads the queue at its own start. */
    int qrc = nv_gsp_rpc_async_init(d);
    p("async-init rc=");
    print_int(qrc);
    p(" (fn 72 then 73; no doorbell)\n");
  }

  /* ---- 5. WPR meta: the block the FMC's ACR reads ----------------------- */
  {
    uint8_t *wp = (uint8_t *)phys_to_virt(m_wprm.phys);
    int i;
    for (i = 0; i < (int)sizeof meta; i++) ((uint8_t *)&meta)[i] = 0;
    meta.gsp_fw_heap_size = (mode == 8) ? RM_HEAP_BYTES_GB202 : RM_HEAP_BYTES;
    meta.non_wpr_heap_size = (mode == 8) ? RM_NONHEAP_BYTES_GB202
                                         : RM_NONHEAP_BYTES;
    meta.vga_workspace_size = RM_VGA_WS_BYTES;
    /* Mode 16: the FB half, one field.  GspFwWprMeta's diagram calls fbSize
     * "end of FB, 1M aligned"; the Hopper HAL leaves it 0 (the ACR is supposed
     * to own the layout) but the Turing HAL fills it from
     * kmemsysGetUsableFbSize.  This part's FB aperture is 0x800000000 (32 GiB,
     * the same number BAR1 reports in every [NV] state row of these boots), so
     * that is the value a driver would have written. */
    if (mode == 16 || mode == 18 || mode == 24 || mode == 26 || mode == 27) meta.fb_size = 0x800000000ULL;
    /* Modes 51/52: the SAME one-field question in the state where the FMC
     * actually runs.  mode 16 asked it in the 0x5 state (descriptor accepted,
     * FMC never ran), so it could not answer "does the ACR need fbSize".  The
     * layout code above (kgspCalculateFbLayout_GH100's diagram) places FRTS,
     * the ELF and the heap as offsets from fbSize -- an ACR validating the
     * FSP-built WPR2 against a 0-byte FB would have nothing to validate
     * against, and the census says it aborted before its first FB write. */
    if (mode == 51 || mode == 52) meta.fb_size = 0x800000000ULL;
    meta.size_bootloader = bootimg.size;
    meta.frts_size = 0;              /* kgspGetFrtsSize_GB202 = 0 */
    if (mode == 57) {
      /* The descriptor and the COT have to agree about the FB FRTS.  The COT
       * carries frtsVidmemSize = 1 MiB (the vendor's normal-boot shape and the
       * one value the FSP validates); kgspGetFrtsSize_GB202 returns 0, so every
       * boot so far told the ACR "no FRTS in the layout" while telling the FSP
       * "1 MiB at fbSize-64MiB".  One field, running state. */
      meta.frts_size = 0x100000ULL;
    }
    if (mode == 58 || mode == 63 || mode == 64 || mode == 65 || mode == 66 ||
        mode == 67) {
      /* The vendor's own heap pair for this part, asked in the state where the
       * FMC runs (mode 8 asked it in the 0x5 state).  Positive control: the
       * FSP sizes WPR2 from these, so wpr2_lo/hi must move by 7 MiB. */
      meta.gsp_fw_heap_size = RM_HEAP_BYTES_GB202;
      meta.non_wpr_heap_size = RM_NONHEAP_BYTES_GB202;
      /* Mode 64: the ONE field this boot spends.  The vendor's 615 stock
       * nonWprHeapSize (0x300000) instead of the 570 body's 0x220000. */
      if (mode == 64) meta.non_wpr_heap_size = RM_NONHEAP_BYTES_V615;
      /* Mode 67: the vendor's own shape -- every field at the value the
       * 580.178.04 driver computes for this part, in one first COT.  The
       * descriptor gains pmuReservedSize (A2's field, 0xF4), which had never
       * been sent in any first-COT boot; the COT's FRTS-in-FB pair and the
       * args-page target follow below. */
      if (mode == 67) meta.pmu_reserved_size = RM_PMU_RESERVED_BYTES;
    }
    if (mode == 19) {                 /* vendor heap pair + FRTS size */
      meta.gsp_fw_heap_size = RM_HEAP_BYTES_GB202;
      meta.non_wpr_heap_size = RM_NONHEAP_BYTES_GB202;
      meta.frts_size = 0x100000ULL;
    }
    if (mode == 27) {
      /* All three sysmem artifacts at an address the GPU cannot reach: if the
       * FMC DMA'd any of them the read would NAK and it would report an error;
       * a still-silent run means it is not reading them yet. */
      meta.sysmem_addr_radix3_elf = 0x1000000000ULL;
      meta.sysmem_addr_bootloader = 0x1000000000ULL;
      meta.sysmem_addr_signature = 0x1000000000ULL;
    }
    if (mode != 3 && mode != 6) {
      meta.size_radix3_elf = gsprm.size;
      radix3_ok = 1;
    }
    /* The FMC's own boot image in sysmem: its address must be a bus address,
     * so it gets its own nv_dma mapping (a 53-page run) rather than the
     * loader's raw frames. */
    if (nv_dma_alloc(((uint64_t)bootimg.size + NV_DMA_PAGE - 1) &
                     ~(NV_DMA_PAGE - 1), &m_bootimg) != 0) {
      p("dma bootimg: no run -- no COT sent\nverdict=no-bootimg-nomem\n");
      goto out;
    }
    {
      uint8_t *dst = (uint8_t *)phys_to_virt(m_bootimg.phys);
      for (uint32_t k = 0; k < bootimg.size; k++) dst[k] = bootimg.buf[k];
    }
    meta.sysmem_addr_bootloader = nv_dma_bus(&m_bootimg);
    if (mode == 56) {
      /* The "does the ACR read the boot image at all" control: the bytes are
       * staged and the sizes stay honest, only the published address is one
       * the GPU cannot reach.  The same trick as mode 54 did for the ELF, and
       * the same trick mode 27 did for both in the 0x5 state. */
      meta.sysmem_addr_bootloader = 0x1000000000ULL;
    }
    meta.bootloader_code_offset = bl_code;
    meta.bootloader_data_offset = bl_data;
    meta.bootloader_manifest_offset = bl_man;

    if (mode != 3 && mode != 6) {
      /* the 63.5 MB image: one contiguous run (the loader's own rule), then a
       * radix3 tree over it -- the ACR walks the tree, not the image. */
      if (nv_dma_alloc(((uint64_t)gsprm.size + NV_DMA_PAGE - 1) &
                       ~(NV_DMA_PAGE - 1), &m_rmimg) != 0) {
        p("dma gsprm image: no contiguous run -- no COT sent\n"
          "verdict=no-gsprm-nomem\n");
        goto out;
      }
      {
        uint8_t *dst = (uint8_t *)phys_to_virt(m_rmimg.phys);
        for (uint32_t k = 0; k < gsprm.size; k++) dst[k] = gsprm.buf[k];
      }
      tree_pages = nv_radix3_pages(gsprm.size);
      if (tree_pages == 0 ||
          nv_dma_alloc((uint64_t)tree_pages * NV_RADIX3_PAGE, &m_tree) != 0) {
        p("radix3 tree: no run -- no COT sent\nverdict=no-radix3-nomem\n");
        goto out;
      }
      if (nv_radix3_build_into((uint8_t *)phys_to_virt(m_tree.phys), tree_pages,
                               m_tree.phys, nv_dma_bus(&m_tree), m_rmimg.phys,
                               nv_dma_bus(&m_rmimg), gsprm.size) != 0) {
        p("radix3 build refused the image\nverdict=no-radix3\n");
        goto out;
      }
      meta.sysmem_addr_radix3_elf = nv_dma_bus(&m_tree);
      if (mode == 54) {
        /* The "does the ACR walk the RM image at all" control, in the state
         * where the FMC runs (mode 27 asked the same question in the 0x5
         * state).  The radix3 tree is still built and valid; only the address
         * the descriptor publishes is one the GPU cannot reach, so a read
         * would NAK and a booter that consumes it would fail differently from
         * a booter that never opens the ELF. */
        meta.sysmem_addr_radix3_elf = 0x1000000000ULL;
      }

      if (nv_dma_alloc(RM_SIG_BYTES, &m_sig) != 0) {
        p("dma signature: no run -- no COT sent\nverdict=no-sig-nomem\n");
        goto out;
      }
      {
        uint8_t *dst = (uint8_t *)phys_to_virt(m_sig.phys);
        for (uint32_t k = 0; k < sig.size && k < RM_SIG_BYTES; k++)
          dst[k] = (mode == 5 || mode == 53) ? 0u : sig.buf[k];
      }
      meta.sysmem_addr_signature = nv_dma_bus(&m_sig);
      meta.size_signature = RM_SIG_BYTES;
    }

    if (nv_fw_wpr_meta_encode(&meta, wp) != 0 ||
        nv_fw_wpr_meta_decode(wp, &back) != 0) {
      p("WPR meta encode/decode FAILED\nverdict=no-wprmeta\n");
      goto out;
    }
    /* Controls on the block's own first 16 bytes: mode 14 writes a magic that
     * is not GSP_FW_WPR_META_MAGIC, mode 15 sends an all-zero block.  Both are
     * "the FMC must reject this" probes -- if the answer does not move from
     * 0x5, then 0x5 is not (only) a magic/field check and no descriptor
     * content can be the discriminator. */
    if (mode == 14) st64(wp, 0, 0xDEADBEEFCAFEF00DULL);
    if (mode == 15) { for (uint32_t z = 0; z < RM_WPRM_BYTES; z++) wp[z] = 0; }
    p("wprm dwords");
    for (int q = 0; q < 8; q++) {
      p(" d");
      print_int(q);
      uart_puts("=");            /* uart_print_hex already prints 0x */
      uart_print_hex(ld32(wp, (uint32_t)(q * 4)));
    }
    pe();
    p("wprm bus=");
    uart_print_hex(nv_dma_bus(&m_wprm));
    pkv(" magic", (uint64_t)(back.gsp_fw_heap_size ? 1 : 0));
    pkv(" radix3", back.sysmem_addr_radix3_elf);
    pkv(" elf_bytes", back.size_radix3_elf);
    pkv(" tree_pages", (uint64_t)tree_pages);
    pkv(" bootimg", back.sysmem_addr_bootloader);
    pkv(" bootimg_bytes", back.size_bootloader);
    pe();
    p("wprm offsets code=");
    uart_print_hex(back.bootloader_code_offset);
    pkv(" data", back.bootloader_data_offset);
    pkv(" manifest", back.bootloader_manifest_offset);
    pkv(" sig", back.sysmem_addr_signature);
    pkv(" sig_bytes", back.size_signature);
    pkv(" sig_d0", back.size_signature ? ld32((const uint8_t *)phys_to_virt(m_sig.phys), 0) : 0);
    pkv(" sig_d4", back.size_signature ? ld32((const uint8_t *)phys_to_virt(m_sig.phys), 4) : 0);
    pe();
    p("wprm sizes heap=");
    uart_print_hex(back.gsp_fw_heap_size);
    pkv(" nonwpr_heap", back.non_wpr_heap_size);
    pkv(" frts", back.frts_size);
    pkv(" vga_ws", back.vga_workspace_size);
    pkv(" fb_size", back.fb_size);
    pkv(" verified", (uint64_t)nv_fw_wpr_meta_verified(&back));
    pe();
    /* The same words read out of the WIRE buffer rather than the struct: the
     * device reads these bytes.  +0x68 nonWprHeapSize, +0x80 gspFwHeapSize,
     * +0xF4 pmuReservedSize and +0xF8 verified are 580's own offsets
     * (gsp_fw_wpr_meta.h compiled at that tag: sizeof 256, those four at
     * 0x68/0x80/0xf4/0xf8). */
    p("wprm wire +0x68=");
    uart_print_hex(ld64(wp, 0x68));
    pkv(" +0x80", ld64(wp, 0x80));
    pkv(" +0xf4", ld32(wp, 0xF4));
    pkv(" +0xf8", ld64(wp, 0xF8));
    pkv(" pmu_dec", back.pmu_reserved_size);
    pe();
  }

  /* ---- 6. RMARGS + the libos argument table ----------------------------- */
  {
    uint8_t *ra = (uint8_t *)phys_to_virt(m_rmargs.phys);
    uint8_t *la = (uint8_t *)phys_to_virt(m_libos.phys);
    if (mode == 2) rm_args_b(ra, blk_bus, blk_ptes);
    else           rm_args_a(ra, blk_bus, blk_ptes);

    rm_libos_entry(la + 0 * RM_LIBOS_ENTRY, "RMARGS",
                   nv_dma_bus(&m_rmargs), NV_DMA_PAGE);
    if (mode == 4) {
      /* Control: the four LOG* regions the RM's own id table lists, as plain
       * zeroed 1 MiB sysmem regions.  A zeroed region is NOT a valid libos
       * nvlog header, so this variant is a probe of "does the RM require the
       * region at all", not a logging implementation. */
      static struct nv_dma_map mlog;
      mlog = m_image;
      if (nv_dma_alloc(0x100000, &mlog) == 0) {
        rm_libos_entry(la + 1 * RM_LIBOS_ENTRY, "LOGINIT",
                       nv_dma_bus(&mlog), 0x100000);
        rm_libos_entry(la + 2 * RM_LIBOS_ENTRY, "LOGRM",
                       nv_dma_bus(&mlog), 0x100000);
        rm_libos_entry(la + 3 * RM_LIBOS_ENTRY, "LOGINTR",
                       nv_dma_bus(&mlog), 0x100000);
      }
    }
    p("rmargs layout=");
    uart_puts(mode == 2 ? "B('GSP ' magic, 0xC8)" : "A(GSP_ARGUMENTS_CACHED)");
    pkv(" bus", nv_dma_bus(&m_rmargs));
    pkv(" blk", blk_bus);
    pkv(" ptes", (uint64_t)blk_ptes);
    pkv(" libos_page", nv_dma_bus(&m_libos));
    pe();
    p("rmargs dwords");
    for (int i = 0; i < 10; i++) {
      p(" d");
      print_int(i);
      uart_puts("=0x");
      uart_print_hex(ld32(ra, (uint32_t)(i * 4)));
    }
    pe();
    p("libos rmargs entry id8=0x");
    uart_print_hex(rm_id8("RMARGS"));
    pkv(" pa", nv_dma_bus(&m_rmargs));
    pkv(" size", NV_DMA_PAGE);
    p(" kind=1 loc=1\n");
  }

  /* ---- 7. the boot params page: this is the one that changes ------------- */
  {
    uint8_t *a = (uint8_t *)phys_to_virt(m_args.phys);
    st32(a, NV_FMCP_OFF_REGKEYS, 0);
    st32(a, NV_FMCP_OFF_BOOTTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
    st32(a, NV_FMCP_OFF_DESCSIZE, mode == 6 ? 0u : RM_WPRM_BYTES);
    st64(a, NV_FMCP_OFF_DESCOFF, mode == 6 ? 0ull : nv_dma_bus(&m_wprm));
    st64(a, NV_FMCP_OFF_WPR_OFF, (mode == 26) ? 0x7F0000000ULL : 0);
    st32(a, NV_FMCP_OFF_WPR_SIZE, (mode == 26) ? 0x10000000u : 0);
    a[NV_FMCP_OFF_ISRMBOOT] = 1;       /* the only field that differs from cot05 */
    /* The libos-args page is allocated NV_MEMORY_UNCACHED by the 570 driver
     * (kgspAllocBootArgs_TU102, pLibosInitArgumentsDescriptor), and
     * _kgspMemdescToDmaTarget maps that to NONCOHERENT_SYSTEM.  The WPR meta
     * page is NV_MEMORY_CACHED -> COHERENT_SYSTEM, so the two targets differ in
     * the vendor's own block; mode 7 sends the vendor pair, mode 1 (the parent's)
     * sent COHERENT for both. */
    st32(a, NV_FMCP_OFF_RMTGT, (mode == 7 || mode == 67)
                                   ? GSP_DMA_TARGET_NONCOHERENT_SYSTEM
                                   : GSP_DMA_TARGET_COHERENT_SYSTEM);
    st64(a, NV_FMCP_OFF_RMARGS, nv_dma_bus(&m_libos));
    st32(a, NV_FMCP_OFF_SPDMTGT, 0);
    st64(a, NV_FMCP_OFF_SPDMBUF, 0);
    st32(a, NV_FMCP_OFF_SPDMLEN, 0);
    if (mode == 50) {
      /* t_0936a17f section 6, item 1: the two descriptor-side targets that had
       * only ever been sent in the 0x5 state, never while the FMC runs.
       * gspRmDescTarget = 2 (NONCOHERENT_SYSTEM -- _kgspMemdescToDmaTarget's
       * answer for NV_MEMORY_UNCACHED) and the same value for the args page at
       * +56.  One boot, one pair of fields, mode 17 otherwise. */
      st32(a, NV_FMCP_OFF_BOOTTGT, GSP_DMA_TARGET_NONCOHERENT_SYSTEM);
      st32(a, NV_FMCP_OFF_SPDMTGT, GSP_DMA_TARGET_NONCOHERENT_SYSTEM);
    }
    p("boot_args bus=");
    uart_print_hex(nv_dma_bus(&m_args));
    pki(" bIsGspRmBoot", a[NV_FMCP_OFF_ISRMBOOT]);
    /* Read the page back rather than echoing the constants: this row is the
     * evidence for what the FMC was told, so it must come from the bytes. */
    pki(" gspRmDescSize", (int)ld32(a, NV_FMCP_OFF_DESCSIZE));
    p(" gspRmDescOffset=");
    uart_print_hex(ld32(a, NV_FMCP_OFF_DESCOFF) |
                   ((uint64_t)ld32(a, NV_FMCP_OFF_DESCOFF + 4) << 32));
    p(" gspRmArgsOffset=");
    uart_print_hex(ld32(a, NV_FMCP_OFF_RMARGS) |
                   ((uint64_t)ld32(a, NV_FMCP_OFF_RMARGS + 4) << 32));
    pe();
    p("boot_args dwords");
    for (int i = 0; i < 22; i++) {
      p(" d");
      print_int(i);
      uart_puts("=0x");
      uart_print_hex(ld32(a, (uint32_t)(i * 4)));
    }
    pe();
  }

  /* ---- 8. the COT, identical to the COT rung ---------------------------- */
  {
    uint8_t *c = (uint8_t *)phys_to_virt(m_cot.phys);
    uint64_t fmc_bus = nv_dma_bus(&m_image);
    uint64_t frts_bus = frts_ok ? nv_dma_bus(&m_frts) : 0;
    uint64_t args_bus = nv_dma_bus(&m_args);
    for (uint32_t i = 0; i < NV_COT_BYTES; i++) c[i] = 0;
    st16(c, NV_COT_OFF_VERSION, NV_COT_VERSION_GB202);
    st16(c, NV_COT_OFF_SIZE, NV_COT_BYTES);
    st64(c, NV_COT_OFF_FMC, fmc_bus);
    st64(c, NV_COT_OFF_FRTS_SYS, frts_bus);
    st32(c, NV_COT_OFF_FRTS_SSIZE, frts_ok ? NV_COT_FRTS_BYTES : 0);
    /* Mode 17: the vendor's *normal boot* shape for the FB half of FRTS.  The
     * driver always sends a non-zero frtsVidmemOffset (FB-end reserve estimate,
     * 2M aligned) and frtsVidmemSize = 1 MiB; 0/0 is only what it sends for
     * GC6/resume, where FRTS stays alive in WPR2.  This is the one COT field
     * pair a normal boot never leaves at 0, so it gets a first-COT boot of its
     * own (a repeated COT may not make the FSP re-place FRTS). */
    {
      uint64_t fvid = (mode == 17 || mode == 18 || mode == 19 ||
                       mode == 22 || mode == 23 || mode == 24 ||
                       mode == 26 || mode == 27 || mode == 30 ||
                       mode == 31 || mode == 34 || mode == 35 ||
                       mode == 50 || mode == 51 || mode == 52 ||
                       mode == 53 || mode == 54 || mode == 55 ||
                       mode == 56 || mode == 57 || mode == 58 || mode == 59 ||
                       mode == 63 || mode == 64 || mode == 65 || mode == 66) ? 0x4000000ULL : 0;
      if (mode == 36 || mode == 37 || mode == 38) fvid = 0x0B000000ULL;   /* 176 MiB: inside the zero gap */
      /* Mode 67: the vendor's own pair.  0x1C00000 is what its arithmetic
       * produces with the pmuReservedSize this same boot puts in the
       * descriptor -- so the two halves of the vendor shape agree by
       * construction, and the row below checks that they do. */
      if (mode == 67) fvid = RM_FRTS_VID_OFF;
      st64(c, NV_COT_OFF_FRTS_VID, fvid);
    }
    st32(c, NV_COT_OFF_FRTS_VSIZE, (mode == 17 || mode == 18 || mode == 19 ||
                                    mode == 22 || mode == 23 || mode == 24 ||
                                    mode == 26 || mode == 27 || mode == 30 ||
                                    mode == 31 || mode == 34 || mode == 35 ||
                                    mode == 36 || mode == 37 || mode == 38 ||
                                    mode == 50 || mode == 51 || mode == 52 ||
                                    mode == 53 || mode == 54 || mode == 55 ||
                                    mode == 56 || mode == 57 || mode == 58 || mode == 59 ||
                                    mode == 63 || mode == 64 || mode == 65 ||
                                    mode == 66 || mode == 67) ? 0x100000u : 0);
    for (uint32_t i = 0; i < NV_COT_HASH_BYTES; i++)
      c[NV_COT_OFF_HASH + i] = sec.hash[i];
    for (uint32_t i = 0; i < NV_COT_PKEY_BYTES; i++)
      c[NV_COT_OFF_PKEY + i] = sec.key[i];
    for (uint32_t i = 0; i < NV_COT_SIG_BYTES; i++)
      c[NV_COT_OFF_SIG + i] = sec.sig[i];
    st64(c, NV_COT_OFF_ARGS, args_bus);
    p("cot gspFmcSysmemOffset=");
    uart_print_hex(fmc_bus);
    pkv(" frtsSysmemOffset", frts_bus);
    pkv(" gspBootArgsSysmemOffset", args_bus);
    pkv(" bytes", NV_COT_BYTES);
    pe();
    /* B1 + B2 read out of the COT buffer itself, and the vendor's own
     * expression recomputed from the descriptor bytes this boot sends.  The
     * two must agree: frtsVidmemOffset = align2M(fbEndReserve + extra +
     * pmuReservedSize) with the SAME pmu that went into the block. */
    {
      uint64_t vend = RM_FBEND_RESERVE_BYTES;
      if (back.pmu_reserved_size != 0)
        vend += RM_EXTRA_RESERVED_BYTES + (uint64_t)back.pmu_reserved_size;
      vend = (vend + 0x1FFFFFULL) & ~0x1FFFFFULL;
      p("cot frtsVidmem off=");
      uart_print_hex(ld64(c, NV_COT_OFF_FRTS_VID));
      pkv(" size", ld32(c, NV_COT_OFF_FRTS_VSIZE));
      pkv(" vendor_off", vend);
      pkv(" pmu_desc", back.pmu_reserved_size);
      pkv(" nonwpr_desc", back.non_wpr_heap_size);
      pkv(" desc_target", ld32((const uint8_t *)phys_to_virt(m_args.phys), NV_FMCP_OFF_BOOTTGT));
      pkv(" args_target", ld32((const uint8_t *)phys_to_virt(m_args.phys), NV_FMCP_OFF_RMTGT));
      p(" verdict=");
      uart_puts(ld64(c, NV_COT_OFF_FRTS_VID) == vend ? "vendor-shape"
                                                     : "diverges");
      p("\n");
    }
  }
  if (frts_ok) {
    uint32_t lo = (uint32_t)m_frts.bus, hi = (uint32_t)(m_frts.bus >> 32);
    /* A10: program ONLY the GH100 pair at 0x14F4/8/C, which is where this
     * part's PBUS block lands (NV_PBUS0_PRI_BASE = 0x1000; the GB202 HAL's
     * FRTS_INSECURE writes are 0x14F4/8/C -- vendor kern_fsp_gh100.c:1646-1679
     * writes this pair and nothing else).  The 0x4F4/8/C copy this lane used
     * to write as well is gone; the read-backs stay, so a future boot still
     * records that the GB202 alias answers 0xBADF5040/0 and was never touched
     * by us. */
    nv_wr32(d, NV_PBUS_SW_FRTS_ADDR_LO32, lo);
    nv_wr32(d, NV_PBUS_SW_FRTS_ADDR_HI32, hi);
    nv_wr32(d, NV_PBUS_SW_FRTS_CONFIG, NV_PBUS_FRTS_CONFIG_SYSMEM(NV_COT_FRTS_BYTES));
    p("frts regs lo=");
    uart_print_hex(m_frts.bus);
    pki(" wrote_zb", 0);
    pkv(" gh100_rb_lo", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_LO32));
    pkv(" gh100_rb_cfg", nv_rd32(d, NV_PBUS_SW_FRTS_CONFIG));
    pkv(" gb202_rb_lo", nv_rd32(d, NV_PBUS_ZB_FRTS_ADDR_LO32));
    pkv(" gb202_rb_hi", nv_rd32(d, NV_PBUS_ZB_FRTS_ADDR_HI32));
    pkv(" gb202_rb_cfg", nv_rd32(d, NV_PBUS_ZB_FRTS_CONFIG));
    pe();
  }

  /* The FMC's error word is a latch: zero it (as gpuResetGspFmcErrorCode_HAL
   * does before every boot) so whatever is there after the COT was written by
   * THIS FMC, not left over from reset. */
  rm_fmc_error_row(d, "pre(cold)");
  nv_wr32(d, NV_PBUS_PRI_GSP_FMC_ERROR, 0);
  rm_fmc_error_row(d, "pre(zeroed)");

  /* ---- 9. GPU time, clock boost, mailbox, COT -------------------------- *
   * A5 first: the vendor anchors the GPU clock to the wall clock after GFW
   * boot and before it starts GSP-RM (kernel_gsp.c:4145).  Then the boost
   * query and the COT.  A9: the GSP MAILBOX0/1 write that used to sit here is
   * GONE -- on the FSP path the boot-args address travels in the COT
   * (gspBootArgsSysmemOffset) and 580.178.04 never writes those words before
   * the COT.  The read-back stays so the log still records what the pair
   * holds: post-reset it is 0, and "landed" is then a fact about the hardware,
   * not about a write this host made. */
  nv_gsp_set_gpu_time(d);
  {
    uint8_t one[4] = {1, 0, 0, 0};
    boost_err = cot_roundtrip(d, base, "boost", COT_NVDM_CLOCK_BOOST, one, 4);
    ptki("boost", "errorCode", boost_err);
    pe();
    cot_wait_us(d, 5000);
  }
  {
    uint32_t m0 = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0);
    uint32_t m1 = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX1);
    mailbox_writable = (m0 == (uint32_t)m_args.bus);
    p("gsp mailbox pre-cot args=");
    uart_print_hex(m_args.bus);
    pkv(" m0", (uint64_t)m0);
    pkv(" m1", (uint64_t)m1);
    pki(" wrote", 0);
    pki(" landed", mailbox_writable);
    pe();
  }
  (void)nv_timer_read(d, &t_send);
  if (mode == 21 || mode == 23 || mode == 24) {
    /* Phase trace: send the COT and then poll the GSP window and the status
     * queue every ~500 us, printing a row only when one of the words changes.
     * mode 21 sends frtsVidmem 0/0 (the parent's COT), mode 23 the vendor's
     * normal-boot shape (64 MiB from FB end, 1 MiB). */
    uint32_t hdr1 = ((COT_NVDM_COT & 0xFFu) << 24) | (0x10DEu << 8) | 0x7Eu;
    uint32_t last_hw = 0xFFFFFFFFu, last_m0 = 0xFFFFFFFFu, last_os = 0xFFFFFFFFu;
    uint32_t last_mt = 0xFFFFFFFFu, last_rm = 0xFFFFFFFFu, last_w2 = 0xFFFFFFFFu;
    uint32_t last_sq = 0xFFFFFFFFu, last_fs = 0xFFFFFFFFu;
    const uint8_t *sq = (const uint8_t *)phys_to_virt(blk_bus + NV_RPC_STAT_OFF);
    uint32_t resp[24];
    int nd;
    cot_send(d, base, "trace", NV_COT_HDR_MCTP, hdr1,
             (const uint8_t *)phys_to_virt(m_cot.phys), NV_COT_BYTES);
    rm_p("trace start\n");
    for (int i = 0; i < 4000; i++) {
      uint32_t hw = nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2);
      uint32_t m0 = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0);
      uint32_t os = nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_OS);
      uint32_t rm = nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_RM);
      uint32_t mt = nv_rd32(d, base + COT_PFSP_MSGQ_TAIL0);
      uint32_t w2 = nv_rd32(d, 0x1FA824u);
      uint32_t fs = nv_rd32(d, base + 0x320u);
      uint32_t sqv = ld32(sq, 0);
      int t = cot_us_since(d, t_send);
      if (hw != last_hw || m0 != last_m0 || os != last_os || rm != last_rm ||
          mt != last_mt || w2 != last_w2 || sqv != last_sq || fs != last_fs) {
        p("trace t_us=");
        print_int(t);
        pkv(" hwcfg2", hw);
        pkv(" m0", m0);
        pkv(" m1", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX1));
        pkv(" os", os);
        pkv(" rm", rm);
        pkv(" wpr2_lo", w2);
        pkv(" statq_d0", sqv);
        pkv(" fsp_scratch0", fs);
        pkv(" msgq_tail", mt);
        pe();
        last_hw = hw; last_m0 = m0; last_os = os; last_rm = rm; last_mt = mt;
        last_w2 = w2; last_sq = sqv; last_fs = fs;
      }
      cot_wait_us(d, 500);
    }
    rm_p("trace end fsp-scratch");
    pkv(" s0", nv_rd32(d, base + 0x320u));
    pkv(" s1", nv_rd32(d, base + 0x324u));
    pkv(" s2", nv_rd32(d, base + 0x328u));
    pkv(" s3", nv_rd32(d, base + 0x32Cu));
    pkv(" ucode", nv_rd32(d, base + 0x334u));
    pkv(" fuse", nv_rd32(d, base + 0x33Cu));
    pe();
    /* still ack the FSP's answer so the channel is left idle */
    if (cot_wait_resp(d, base, 1000000) >= 0) {
      nd = cot_recv(d, base, "cot", resp, 24);
      pki(" trace answer dwords", nd);
      if (nd >= 5) pki(" trace answer errorCode", (int)resp[4]);
      pe();
    }
    cot_err = 0;
  } else {
    cot_err = cot_roundtrip(d, base, "cot", COT_NVDM_COT,
                            (const uint8_t *)phys_to_virt(m_cot.phys),
                            NV_COT_BYTES);
  }
  p("cot answer errorCode=");
  print_int(cot_err);
  uart_puts(cot_err == 0 ? " (FSP_OK)\n" : " (not FSP_OK)\n");
  rm_fmc_error_row(d, "post-cot");
  if (mode == 66) {
    /* Mode 66 = mode 63's host state (615 FMC, whose terminal code 0x824 is
     * written inside the COT round trip) plus the one instrument the survey
     * ranks first: the FSP's own DMEM log over the MCTP VDM download-log
     * command.  One second lets the terminal code settle, so the log is asked
     * for AFTER the abort and a record can be tied to it.  The log read is
     * bounded by its own shape test (one packet then stop if the answer is a
     * foreign wrapper) and by the vendor's 4 KiB cap; the clock-boost query
     * afterwards is the positive control that the same channel still answers
     * -- a timeout above is then a refusal of this command, not a dead
     * mailbox. */
    cot_wait_us(d, 1000000);
    rm_sample(d, "t1s", 1000);
    rm_fmc_error_row(d, "t1s");
    {
      /* Control BEFORE the log read: is the RM channel still answered after
       * the FMC's terminal code?  Without this row, a log timeout could be
       * "the command is unsupported" or "the FSP is gone", and the two mean
       * different things. */
      uint8_t one[4] = {1, 0, 0, 0};
      int e0 = cot_roundtrip(d, base, "prelog-boost", COT_NVDM_CLOCK_BOOST,
                             one, 4);
      pki("dmemlog prelog boost_errorCode", e0);
      pe();
    }
    rm_dmem_log(d, base);
    rm_fmc_error_row(d, "postlog");
    {
      uint8_t one[4] = {1, 0, 0, 0};
      int e2 = cot_roundtrip(d, base, "postlog-boost", COT_NVDM_CLOCK_BOOST,
                             one, 4);
      pki("dmemlog postlog boost_errorCode", e2);
      pe();
    }
    rm_sample(d, "postlog", 2000);
  }
  if (frts_ok) {
    /* The FRTS sysmem region is the FSP's to fill (that is why the COT carries
     * its address and size: kfspPrepareBootCommands allocates it, zeroes it and
     * hands it over).  If it is still all zeros after the COT, nothing filled
     * it, and the FMC's FRTS step has nothing to copy. */
    const uint8_t *f = (const uint8_t *)phys_to_virt(m_frts.phys);
    uint32_t nz = 0;
    p("frts-after dwords");
    for (int i = 0; i < 8; i++) {
      p(" d");
      print_int(i);
      uart_puts("=0x");
      uart_print_hex(ld32(f, (uint32_t)(i * 4)));
    }
    for (uint32_t i = 0; i < NV_COT_FRTS_BYTES / 4u; i++)
      if (ld32(f, i * 4u) != 0) nz++;
    pkv(" nonzero_dwords", nz);
    pkv(" of", NV_COT_FRTS_BYTES / 4u);
    pe();
  }
  {
    /* Did the FMC consume the boot-args page?  It reads it (the 0x1C->0x5
     * ladder moved with a byte inside it), and the vendor's ucode clears what
     * it consumed; a page that comes back byte-identical says the FMC read it
     * without writing, which is itself a row. */
    const uint8_t *a = (const uint8_t *)phys_to_virt(m_args.phys);
    p("boot_args-after dwords");
    for (int i = 0; i <= 13; i++) {
      p(" d");
      print_int(i);
      uart_puts("=0x");
      uart_print_hex(ld32(a, (uint32_t)(i * 4)));
    }
    pe();
  }

  /* ---- 10. the RM's progress, sampled ---------------------------------- */
  if (mode == 20 || mode == 22) {
    /* Field sweep.  The FMC is restarted by every accepted COT (measured:
     * rm20 step 0 re-answered 0x1C for a poisoned magic after COT #1 had
     * answered 0x5), so one boot can ask a dozen single-field questions.  Every
     * step rewrites both pages from the mode-1 block, applies one change, sends
     * the COT and reads the FMC's answer 1.5 s later.  Step 0 is the positive
     * control (poisoned magic -> 0x1C); step 1 is the base (-> 0x5). */
    enum { SW_BASE, SW_WPRM32, SW_WPRM64, SW_ARGS32, SW_ARGS64, SW_COT32, SW_COT64,
           SW_CARVE, SW_ALT, SW_ZERO, SW_ARGSMOVE, SW_FRTSVID };
    static const struct { const char *n; int k; uint32_t off; uint64_t v; } sw[] = {
      {"poison-magic",      SW_WPRM64, 0x00, 0xDEADBEEFCAFEF00DULL},
      {"base(frts in FB)",  SW_BASE,   0x00, 0},
      {"frtsVid off (0/0)", SW_FRTSVID, 0x00, 0},
      {"frtsVid size=0",    SW_FRTSVID, 0x00, 1},
      {"frtsVid at 32M",    SW_FRTSVID, 0x00, 2},
      {"frtsVid at 128M",   SW_FRTSVID, 0x00, 3},
      {"heap vendor",       SW_WPRM64, 0x80, 135ULL << 20},
      {"nonwpr vendor",     SW_WPRM64, 0x68, 2228224ULL},
      {"vga_ws=0",          SW_WPRM64, 0xC0, 0},
      {"pmu_rsvd=9M",       SW_WPRM32, 0xF4, 0x900000ULL},
      {"frts_size=1M",      SW_WPRM64, 0xA0, 0x100000ULL},
      {"bl addr=64G",       SW_WPRM64, 0x20, 0x1000000000ULL},
      {"bl addr=0",         SW_WPRM64, 0x20, 0},
      {"radix3 addr=64G",   SW_WPRM64, 0x10, 0x1000000000ULL},
      {"isRmBoot=0",        SW_ARGS32, NV_FMCP_OFF_ISRMBOOT, 0},
    };
    const int steps = (int)(sizeof sw / sizeof sw[0]);
    uint8_t *wp = (uint8_t *)phys_to_virt(m_wprm.phys);
    uint8_t *a = (uint8_t *)phys_to_virt(m_args.phys);
    uint8_t *c = (uint8_t *)phys_to_virt(m_cot.phys);
    rm_p("sweep start\n");
    for (int s = 0; s < steps; s++) {
      /* base block + base args page + this mode's COT, then the one change */
      nv_fw_wpr_meta_encode(&meta, wp);
      rm_args_a(a, blk_bus, blk_ptes);
      a[NV_FMCP_OFF_ISRMBOOT] = 1;
      st32(a, NV_FMCP_OFF_DESCSIZE, RM_WPRM_BYTES);
      st64(a, NV_FMCP_OFF_DESCOFF, nv_dma_bus(&m_wprm));
      st32(a, NV_FMCP_OFF_BOOTTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
      st32(a, NV_FMCP_OFF_RMTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
      st64(a, NV_FMCP_OFF_RMARGS, nv_dma_bus(&m_libos));
      if (mode == 22) {
        st64(c, NV_COT_OFF_FRTS_VID, 0x4000000ULL);
        st32(c, NV_COT_OFF_FRTS_VSIZE, 0x100000u);
      } else {
        st64(c, NV_COT_OFF_FRTS_VID, 0);
        st32(c, NV_COT_OFF_FRTS_VSIZE, 0);
      }
      switch (sw[s].k) {
      case SW_WPRM32: st32(wp, sw[s].off, (uint32_t)sw[s].v); break;
      case SW_WPRM64: st64(wp, sw[s].off, sw[s].v); break;
      case SW_ARGS32: st32(a, sw[s].off, (uint32_t)sw[s].v); break;
      case SW_COT32:  st32(c, sw[s].off, (uint32_t)sw[s].v); break;
      case SW_COT64:  st64(c, sw[s].off, sw[s].v); break;
      case SW_ARGS64:
        if (sw[s].off == NV_FMCP_OFF_DESCOFF && sw[s].v == 0) {
          st64(a, sw[s].off, nv_dma_bus(&m_wprm) + 8);       /* magic skipped */
        } else if (sw[s].off == NV_FMCP_OFF_DESCOFF &&
                   sw[s].v == 0xFFFFFFFFFFFFFFFFULL) {
          st64(a, sw[s].off, 0);                             /* no descriptor */
        } else {
          st64(a, sw[s].off, sw[s].v);
        }
        break;
      case SW_ALT: {
          uint8_t *alt = (uint8_t *)phys_to_virt(m_alt.phys);
          for (int z = 0; z < (int)RM_WPRM_BYTES; z++) alt[(int)sw[s].v + z] = wp[z];
          st64(a, NV_FMCP_OFF_DESCOFF, nv_dma_bus(&m_alt) + sw[s].v);
          break;
        }
      case SW_ZERO:
        for (uint32_t z = 0; z < RM_WPRM_BYTES; z++) wp[z] = 0;
        break;
      case SW_FRTSVID:
        if (sw[s].v == 0) { st64(c, NV_COT_OFF_FRTS_VID, 0); st32(c, NV_COT_OFF_FRTS_VSIZE, 0); }
        else if (sw[s].v == 1) { st64(c, NV_COT_OFF_FRTS_VID, 0x4000000ULL); st32(c, NV_COT_OFF_FRTS_VSIZE, 0); }
        else if (sw[s].v == 2) { st64(c, NV_COT_OFF_FRTS_VID, 0x2000000ULL); st32(c, NV_COT_OFF_FRTS_VSIZE, 0x100000u); }
        else { st64(c, NV_COT_OFF_FRTS_VID, 0x8000000ULL); st32(c, NV_COT_OFF_FRTS_VSIZE, 0x100000u); }
        break;
      case SW_ARGSMOVE: {
          /* Move GSP_FMC_BOOT_PARAMS to another page and point the COT at it,
           * blanking the original: if the FMC still answers 0x5 it obeyed the
           * COT's gspBootArgsSysmemOffset; 0x1C would mean it reads the original
           * address (i.e. it never used the COT field). */
          uint8_t *alt = (uint8_t *)phys_to_virt(m_alt.phys);
          for (uint32_t z = 0; z < NV_DMA_PAGE; z++) alt[z] = a[z];
          for (uint32_t z = 0; z < NV_DMA_PAGE; z++) a[z] = 0;
          st64(c, NV_COT_OFF_ARGS, nv_dma_bus(&m_alt));
          break;
        }
      case SW_CARVE:
        st64(a, NV_FMCP_OFF_WPR_OFF, 0x7F0000000ULL);   /* 31.75 GiB         */
        st32(a, NV_FMCP_OFF_WPR_SIZE, 0x10000000u);     /* 256 MiB carveout  */
        break;
      default: break;
      }
      {
        int e = cot_roundtrip(d, base, "cot2", COT_NVDM_COT,
                              (const uint8_t *)phys_to_virt(m_cot.phys),
                              NV_COT_BYTES);
        cot_wait_us(d, 1500);
        p("sw ");
        print_int(s);
        p(" ");
        uart_puts(sw[s].n);
        pki(" cot_error", e);
        pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
        pkv(" m1", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX1));
        pkv(" hwcfg2", nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2));
        pkv(" os", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_OS));
        pkv(" rm", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_RM));
        pkv(" wprm_d0", ld32(wp, 0));
        pkv(" wprm_verified", ld32(wp, 0xF8));
        pkv(" wprm_start", ld32(wp, 0x70));
        pkv(" wprm_end", ld32(wp, 0xA8));
        pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
        pkv(" cot_vid", ld32(c, NV_COT_OFF_FRTS_VID));
        pe();
      }
    }
    rm_p("sweep done\n");
    /* leave both pages as mode 1 would have sent them */
    nv_fw_wpr_meta_encode(&meta, wp);
    rm_args_a(a, blk_bus, blk_ptes);
    a[NV_FMCP_OFF_ISRMBOOT] = 1;
    st32(a, NV_FMCP_OFF_DESCSIZE, RM_WPRM_BYTES);
    st64(a, NV_FMCP_OFF_DESCOFF, nv_dma_bus(&m_wprm));
    st32(a, NV_FMCP_OFF_BOOTTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
    st32(a, NV_FMCP_OFF_RMTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
    st64(a, NV_FMCP_OFF_RMARGS, nv_dma_bus(&m_libos));
    st64(c, NV_COT_OFF_FRTS_VID, (mode == 22) ? 0x4000000ULL : 0);
    st32(c, NV_COT_OFF_FRTS_VSIZE, (mode == 22) ? 0x100000u : 0);
    st64(c, NV_COT_OFF_ARGS, nv_dma_bus(&m_args));
  }
  {
    static const int at[] = {150, 250, 400, 600, 1000, 3000, 6000, 10000, 15000, 21000, 27000};
    int last = 0;
    for (unsigned i = 0; i < sizeof at / sizeof at[0] && mode != 33 && mode != 34 &&
                        mode != 35 && mode != 37 && mode != 38 &&
                        mode != 59; i++) {
      int t_us;
      if (at[i] > last) cot_wait_us(d, at[i] - last);
      last = at[i];
      t_us = cot_us_since(d, t_send);
      rm_sample(d, "tl", at[i]);
      hwcfg2_last = nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2);
      m0_last = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0);
      m1_last = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX1);
      if (!cot_gsp_locked(hwcfg2_last) && hwcfg2_last != 0) lock_released = 1;
      /* A3: the vendor writes FALCON_OS once the lockdown is released and
       * MAILBOX0 is still 0 (kernel_gsp_gh100.c:946-956), before it links the
       * status queue.  One write, latched: the value is the boot image
       * descriptor's appVersion -- 0 for 580.178.04's GB202 descriptor. */
      if (lock_released && !falcon_os_written && m0_last == 0) {
        nv_gsp_write_falcon_os(d, NV_GSP_GB202_BOOT_APP_VERSION);
        falcon_os_written = 1;
      }
      if (nv_gsp_rpc_peer_written()) statq_written = 1;
      if (!linked && nv_gsp_rpc_peer_link(d) == 0) linked = 1;
      if (linked) {
        int got = nv_gsp_rpc_peer_take("init");
        if (got == 1) initdone = 1;
      }
      (void)t_us;
    }
    /* Modes 53/54 are the two "does the ACR touch the RM artifacts" controls.
     * They need a window that covers the parent's 27-28 s transition and some
     * time past it, or a 0x0B that arrives at 28 s would look like a quiet
     * run.  Register reads only, so the window is not perturbed by BAR1. */
    if (mode == 53 || mode == 54 || mode == 55 || mode == 56 ||
        mode == 57 || mode == 58 || mode == 63 || mode == 64 || mode == 65) {
      cot_wait_us(d, 13000);
      rm_sample(d, "tl", 40000);
      cot_wait_us(d, 15000);
      rm_sample(d, "tl", 55000);
    }
    /* Hypothesis H2 (mode 63 only): the FSP's own CMS2 log carries the text of
     * the 0x824 abort.  Same boot state, one instrument; compared against the
     * pre-COT read, so a stale buffer is visible as "identical". */
    if (mode == 63) rm_cms2_log(d, "post");
    (void)cot_gsp_shape(d, "post");

    /* Did the ACR consume the block?  GspFwWprMeta is the FMC's own input: the
     * vendor's ACR fills in every FB offset and, on acceptance, writes
     * `verified = 0xa0a0...`.  Reading the block back after the COT is the only
     * host-side view of "the FMC read this, not somebody else's". */
    {
      struct nv_wpr_meta_in after;
      const uint8_t *wp2 = (const uint8_t *)phys_to_virt(m_wprm.phys);
      p("wprm-after dwords");
      for (int q = 0; q < 8; q++) {
        p(" d");
        print_int(q);
        uart_puts("=");          /* uart_print_hex already prints 0x */
        uart_print_hex(ld32(wp2, (uint32_t)(q * 4)));
      }
      pe();
      if (nv_fw_wpr_meta_decode(wp2, &after) == 0) {
        p("wprm-after verified=");
        uart_print_hex((uint64_t)nv_fw_wpr_meta_verified(&after));
        pkv(" wpr_start", after.gsp_fw_wpr_start);
        pkv(" wpr_end", after.gsp_fw_wpr_end);
        pkv(" heap_off", after.gsp_fw_heap_offset);
        pkv(" heap_size", after.gsp_fw_heap_size);
        pkv(" nonwpr_off", after.non_wpr_heap_offset);
        pkv(" image_off", after.gsp_fw_offset);
        pkv(" bootbin_off", after.boot_bin_offset);
        pkv(" frts_off", after.frts_offset);
        pkv(" frts_size", after.frts_size);
        pe();
        p("wprm-after untouched=");
        uart_puts(after.gsp_fw_wpr_start == 0 && after.gsp_fw_wpr_end == 0 &&
                  after.gsp_fw_heap_offset == 0 ? "yes (no field the ACR owns "
                                                 "was written)\n"
                                               : "no (the ACR patched the block)\n");
      }
    }
  }

  if (mode == 30) {
    /* The FB side, and then a long look.  An ACR that is inside a wait
     * (rather than stopped) leaves its work in FB, not in the sysmem copies
     * this file reads from the CPU: gsp_fw_wpr_meta.h says the verified copy
     * is DMA'd to FB at gspFwWprStart.  27 s was the parent's window; if the
     * wait is a timer, the state changes after it. */
    static const int at2[] = {35000, 45000, 60000};
    int last2 = 27000;
    rm_fb_probe(d, "t27s");
    for (unsigned i = 0; i < sizeof at2 / sizeof at2[0]; i++) {
      if (at2[i] > last2) cot_wait_us(d, at2[i] - last2);
      last2 = at2[i];
      rm_sample(d, "tl2", at2[i]);
    }
    rm_fb_probe(d, "t60s");
  }

  if (mode == 31) {
    /* Timing probe, and deliberately nothing else: no BAR1 access, no FB
     * scan, one group of register reads per second.  rm31's 0x0B at +245 ms
     * came from a 500 us poll loop, and rm40's WPR2 teardown (wpr2_lo
     * 0x07E61200 -> 0x7FFFFE00, hi -> 0) was seen only after a 1 GiB BAR1
     * scan; this run asks whether the state decays on its own, and when. */
    uint32_t l_m0 = 0xFFFFFFFFu, l_m1 = 0xFFFFFFFFu, l_hw = 0xFFFFFFFFu;
    uint32_t l_os = 0xFFFFFFFFu, l_rm = 0xFFFFFFFFu;
    uint32_t l_lo = 0xFFFFFFFFu, l_hi = 0xFFFFFFFFu, l_sq = 0xFFFFFFFFu;
    const uint8_t *sq = (const uint8_t *)phys_to_virt(blk_bus + NV_RPC_STAT_OFF);
    for (int i = 0; i < 90; i++) {
      uint32_t m0 = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0);
      uint32_t m1 = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX1);
      uint32_t hw = nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2);
      uint32_t os = nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_OS);
      uint32_t rm = nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_RM);
      uint32_t lo = nv_rd32(d, 0x1FA824u);
      uint32_t hi = nv_rd32(d, 0x1FA828u);
      uint32_t sqv = ld32(sq, 0);
      if (m0 != l_m0 || m1 != l_m1 || hw != l_hw || os != l_os || rm != l_rm ||
          lo != l_lo || hi != l_hi || sqv != l_sq || (i % 10) == 0) {
        p("ttl t_s="); print_int(27 + i);
        pkv(" hwcfg2", hw); pkv(" m0", m0); pkv(" m1", m1);
        pkv(" os", os); pkv(" rm", rm);
        pkv(" wpr2_lo", lo); pkv(" wpr2_hi", hi);
        pkv(" statq_d0", sqv);
        pe();
        l_m0 = m0; l_m1 = m1; l_hw = hw; l_os = os; l_rm = rm;
        l_lo = lo; l_hi = hi; l_sq = sqv;
      }
      cot_wait_us(d, 1000000);
    }
    p("ttl-end pbus_frts_lo=0x"); uart_print_hex(nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_LO32));
    pkv(" hi", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_HI32));
    pkv(" cfg", nv_rd32(d, NV_PBUS_SW_FRTS_CONFIG));
    pkv(" pfb_moves_a", nv_rd32(d, 0x1FA804u));
    pkv(" pfb_moves_b", nv_rd32(d, 0x1FA808u));
    pkv(" m0_final", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
    pe();
  }

  if (mode == 59) {
    /* t_0936a17f's "the ACR writes nothing into FB" came from a 128 KiB stride
     * over 32 GiB.  A 256-byte WPR meta at WPR2_start+0x230000 falls between
     * two samples of that stride, so the census could not have seen it.  This
     * boot waits out the attempt (the pair is torn down WITH the terminal
     * 0x0B, and the poison band stays), then looks at the band the FSP built
     * with an instrument that can see 256 bytes.  When the pair has already
     * been torn down, the extent is the one every live sample of this COT
     * shape reported (lo 0x7E6120000 / hi 0x7F1EE0000, rm40/41/50/51/57). */
    uint64_t lo, hi;
    cot_wait_us(d, 15000000);
    rm_sample(d, "pre-fine", 15000);
    lo = (uint64_t)nv_rd32(d, 0x1FA824u) << 8;
    hi = (uint64_t)nv_rd32(d, 0x1FA828u) << 8;
    if (hi == 0 || hi <= lo) {
      p("wpr2c pair torn down -- using the measured extent for this COT\n");
      lo = 0x7E6120000ULL;
      hi = 0x7F1EE0000ULL;
    }
    wpr2_census_at(d, lo, hi, "t15s");
    rm_sample(d, "post-fine", 15000);
  }

  if (mode == 33) {
    /* Stop-then-start sweep.  In this state a COT sent while the FMC is live
     * is not a fresh start (measured rm30: the step that follows a running COT
     * answers -1 and m0 becomes 0x0B), but a COT with frtsVidmem 0/0 parks the
     * FMC at 0x5 and the next COT runs again.  So each step is: park, rewrite
     * both pages from the mode-17 block, apply ONE change, send, look 2.5 s. */
    enum { K_BASE, K_A32, K_A64, K_W32, K_W64, K_C32, K_C64, K_LAYOUT };
    static const struct { const char *n; int k; uint32_t off; uint64_t v, v2; } st[] = {
      {"base",                     K_BASE,  0, 0, 0},
      {"args+56 spdm target=1",    K_A32,  56, 1, 0},
      {"args+60/64 spdm buf",      K_A64,  60, 0x1000, 0},
      {"args+80 flush sysmem",     K_A64,  80, 0x0, 0},          /* v = bus of m_alt */
      {"carve 0x7E61200/1M",       K_A64,  24, 0x07E61200ULL, 0x100000},
      {"carve 0x7E6120000/198M",   K_A64,  24, 0x7E6120000ULL, 0xBDC0000},
      {"meta frts_size=1M",        K_W64,  0xA0, 0x100000ULL, 0},
      {"meta pmu_reserved=9M",     K_W32,  0xF4, 0x900000ULL, 0},
      {"meta nonwpr_heap=0",       K_W64,  0x68, 0, 0},
      {"meta fb_size=0x7E6120000", K_W64,  0xB0, 0x7E6120000ULL, 0},
      {"meta wpr_end=wpr2_hi",     K_W64,  0xA8, 0x7E6120000ULL, 0},
      {"meta host layout",         K_LAYOUT, 0, 0, 0},
      {"cot frts size=2M",         K_C32,  NV_COT_OFF_FRTS_VSIZE, 0x200000, 0},
      {"cot frts off=0",           K_C64,  NV_COT_OFF_FRTS_VID, 0, 0},
      {"cot frts off=fbtop-434M",  K_C64,  NV_COT_OFF_FRTS_VID, 0x19EE0000ULL, 0},
      {"meta verified=a0a0",       K_W64,  0xF8, 0xA0A0A0A0A0A0A0A0ULL, 0},
    };
    const int steps = (int)(sizeof st / sizeof st[0]);
    uint8_t *wp = (uint8_t *)phys_to_virt(m_wprm.phys);
    uint8_t *a = (uint8_t *)phys_to_virt(m_args.phys);
    uint8_t *c = (uint8_t *)phys_to_virt(m_cot.phys);
    uint64_t alt_bus = nv_dma_bus(&m_alt);
    rm_p("sweep2 start\n");
    for (int s = 0; s < steps; s++) {
      int e;
      /* park */
      st64(c, NV_COT_OFF_FRTS_VID, 0);
      st32(c, NV_COT_OFF_FRTS_VSIZE, 0);
      (void)cot_roundtrip(d, base, "park", COT_NVDM_COT,
                          (const uint8_t *)c, NV_COT_BYTES);
      cot_wait_us(d, 700);
      /* base pages */
      nv_fw_wpr_meta_encode(&meta, wp);
      rm_args_a(a, blk_bus, blk_ptes);
      a[NV_FMCP_OFF_ISRMBOOT] = 1;
      st32(a, NV_FMCP_OFF_DESCSIZE, RM_WPRM_BYTES);
      st64(a, NV_FMCP_OFF_DESCOFF, nv_dma_bus(&m_wprm));
      st32(a, NV_FMCP_OFF_BOOTTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
      st32(a, NV_FMCP_OFF_RMTGT, GSP_DMA_TARGET_COHERENT_SYSTEM);
      st64(a, NV_FMCP_OFF_RMARGS, nv_dma_bus(&m_libos));
      st64(c, NV_COT_OFF_FRTS_VID, 0x4000000ULL);
      st32(c, NV_COT_OFF_FRTS_VSIZE, 0x100000u);
      st64(c, NV_COT_OFF_ARGS, nv_dma_bus(&m_args));
      /* the one change */
      switch (st[s].k) {
      case K_A32: st32(a, st[s].off, (uint32_t)st[s].v); break;
      case K_A64:
        if (st[s].off == 60) {          /* spdm size + buffer */
          st32(a, 60, (uint32_t)st[s].v);
          st64(a, 64, alt_bus);
        } else if (st[s].off == 80) {   /* flush sysmem address (d20/d21) */
          st64(a, 80, alt_bus);
        } else if (st[s].off == 24) {   /* wprCarveoutOffset/Size */
          st64(a, NV_FMCP_OFF_WPR_OFF, st[s].v);
          st32(a, NV_FMCP_OFF_WPR_SIZE, (uint32_t)st[s].v2);
        }
        break;
      case K_W32: st32(wp, st[s].off, (uint32_t)st[s].v); break;
      case K_W64: st64(wp, st[s].off, st[s].v); break;
      case K_C32: st32(c, st[s].off, (uint32_t)st[s].v); break;
      case K_C64: st64(c, st[s].off, st[s].v); break;
      case K_LAYOUT: {
          /* the Turing-era shape: a complete WPR layout, 128K/1M aligned,
           * 415 MiB band at the top of a 32 GiB FB. */
          st64(wp, 0xB0, 0x800000000ULL);          /* fbSize                  */
          st64(wp, 0xA0, 0x100000ULL);             /* frtsSize (1 MiB)        */
          st64(wp, 0xA8, 0x7FD500000ULL);          /* gspFwWprEnd             */
          st64(wp, 0x98, 0x7FD500000ULL);          /* frtsOffset = wprEnd     */
          st64(wp, 0x90, 0x7FD4CB000ULL);          /* bootBinOffset           */
          st64(wp, 0x88, 0x7F1930000ULL);          /* gspFwOffset             */
          st64(wp, 0x80, 0x8000000ULL);            /* gspFwHeapSize (128 MiB) */
          st64(wp, 0x78, 0x7F1900000ULL);          /* gspFwHeapOffset         */
          st64(wp, 0x70, 0x7F1800000ULL);          /* gspFwWprStart           */
          st64(wp, 0x60, 0x7F1700000ULL);          /* nonWprHeapOffset        */
          st64(wp, 0x58, 0x7F1700000ULL);          /* gspFwRsvdStart          */
          break;
        }
      default: break;
      }
      e = cot_roundtrip(d, base, "cot2", COT_NVDM_COT, (const uint8_t *)c,
                        NV_COT_BYTES);
      cot_wait_us(d, 2500);
      p("sw2 ");
      print_int(s);
      p(" ");
      uart_puts(st[s].n);
      pki(" cot_error", e);
      pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
      pkv(" hwcfg2", nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2));
      pkv(" os", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_OS));
      pkv(" rm", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_RM));
      pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
      pkv(" wpr2_hi", nv_rd32(d, 0x1FA828u));
      pkv(" statq_d0", ld32((const uint8_t *)phys_to_virt(blk_bus + NV_RPC_STAT_OFF), 0));
      pkv(" wprm_d0", ld32(wp, 0));
      pkv(" wprm_f8", ld32(wp, 0xF8));
      pkv(" args_d3", ld32(a, NV_FMCP_OFF_DESCSIZE));
      pkv(" args_d20", ld32(a, 80));
      pkv(" cot_vid", ld32(c, NV_COT_OFF_FRTS_VID));
      pe();
    }
    rm_p("sweep2 done\n");
  }

  if (mode == 34) {
    /* The FB census, taken while the FMC is still inside its silent window and
     * again after it: is there ANY FB address carrying the WPR meta copy or the
     * FRTS blob, and what does the rest of FB look like. */
    cot_wait_us(d, 3000000);
    p("m34 pre"); pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
    pkv(" hwcfg2", nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2));
    pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
    pkv(" wpr2_hi", nv_rd32(d, 0x1FA828u));
    pe();
    fb_census(d, "t3s");
    p("m34 post"); pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
    pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
    pkv(" wpr2_hi", nv_rd32(d, 0x1FA828u));
    pkv(" os", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_OS));
    pkv(" rm", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_RM));
    pe();
  }

  if (mode == 35) {
    /* frtsVidmemOffset sweep with the *FSP's* reaction as the measurement.
     * The cursors the FSP moves are PBUS SW_FRTS_INSECURE_{ADDR_LO32,HI32,
     * CONFIG} (0x14F4/8/C) and PFB WPR2 (0x1FA824/28); the FB address the
     * offset names is fbSize - offset (the COT's own comment: "this is an
     * offset from the end of FB").  rm40's census found the top ~161 MiB of FB
     * answering the 0xBAD0AC00 fence and a 194 MiB 0xDEADDEAD band just below
     * it, so 32/64/128 MiB all name an address inside the FB-end reserve --
     * this sweep walks up to values that land below it. */
    static const uint64_t xs[] = {0x4000000ULL, 0x10000000ULL, 0x20000000ULL,
                                  0x40000000ULL, 0x80000000ULL};
    uint8_t *c = (uint8_t *)phys_to_virt(m_cot.phys);
    for (unsigned s = 0; s < sizeof xs / sizeof xs[0]; s++) {
      uint64_t addr = 0x800000000ULL - xs[s];
      st64(c, NV_COT_OFF_FRTS_VID, xs[s]);
      st32(c, NV_COT_OFF_FRTS_VSIZE, 0x100000u);
      {
        int e = cot_roundtrip(d, base, "cot2", COT_NVDM_COT,
                              (const uint8_t *)c, NV_COT_BYTES);
        cot_wait_us(d, 1200000);
        p("off2 ");
        print_int((int)s);
        p(" frts_off=0x");
        uart_print_hex(xs[s]);
        p(" fbaddr=0x");
        uart_print_hex(addr);
        pki(" cot_error", e);
        pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
        pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
        pkv(" wpr2_hi", nv_rd32(d, 0x1FA828u));
        pkv(" pbus_lo", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_LO32));
        pkv(" pbus_hi", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_HI32));
        pkv(" pbus_cfg", nv_rd32(d, NV_PBUS_SW_FRTS_CONFIG));
        pe();
        fb_dump(d, addr, 4, "frtsaddr");
        fb_dump(d, addr > 0x4000000ULL ? addr - 0x4000000ULL : 0, 2, "frts-64M");
      }
    }
  }

  if (mode == 36) {
    /* The FRTS-in-FB placement test that the census earned: with the COT's
     * 64 MiB offset the FB address the offset names (fbSize - offset =
     * 0x7FC000000) answers the 0xBAD0AC00 FB-end fence, i.e. every offset the
     * lane ever sent named a FENCED address.  176 MiB 2M-aligned lands in the
     * zero gap the census found between the FSP's 0xDEADDEAD WPR2 band and
     * that fence; 512 MiB lands below the poison band entirely.  This boot
     * sends 176 MiB as its FIRST COT (the only one that gets a fresh WPR2) and
     * 512 MiB as a second probe for the FSP's reaction. */
    uint8_t *c = (uint8_t *)phys_to_virt(m_cot.phys);
    fb_census(d, "t27s-after-176M");
    fb_dump(d, 0x800000000ULL - 0x0B000000ULL, 8, "frts176M");
    p("m36 pbus lo=0x"); uart_print_hex(nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_LO32));
    pkv(" hi", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_HI32));
    pkv(" cfg", nv_rd32(d, NV_PBUS_SW_FRTS_CONFIG));
    pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
    pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
    pkv(" wpr2_hi", nv_rd32(d, 0x1FA828u));
    pe();
    st64(c, NV_COT_OFF_FRTS_VID, 0x20000000ULL);
    st32(c, NV_COT_OFF_FRTS_VSIZE, 0x100000u);
    {
      int e = cot_roundtrip(d, base, "cot512M", COT_NVDM_COT, (const uint8_t *)c,
                            NV_COT_BYTES);
      cot_wait_us(d, 2000000);
      p("m36 cot512M");
      pki(" cot_error", e);
      pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
      pkv(" pbus_lo", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_LO32));
      pkv(" pbus_hi", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_HI32));
      pkv(" pbus_cfg", nv_rd32(d, NV_PBUS_SW_FRTS_CONFIG));
      pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
      pkv(" wpr2_hi", nv_rd32(d, 0x1FA828u));
      pe();
      fb_dump(d, 0x800000000ULL - 0x20000000ULL, 8, "frts512M");
      fb_dump(d, 0x800000000ULL - 0x0B000000ULL, 4, "frts176M-b");
    }
  }

  if (mode == 37 || mode == 38) {
    /* The correction this card's rows point at, tested directly: place the
     * FRTS blob the FSP produced (1 MiB, GFW header) into FB at a 2M-aligned
     * address BELOW the FB-end fence, point PBUS at it, and see whether the
     * waiting ACR proceeds.  COT #1 uses 176 MiB so the FMC is alive while the
     * copy happens (that is the only COT that gets a fresh WPR2). */
    struct nv_vram_win w;
    const uint64_t fbaddr = 0x800000000ULL - 0x0B000000ULL;   /* 0x7F5000000 */
    const uint8_t *src;
    uint32_t wrote = 0, rbv = 0;
    cot_wait_us(d, mode == 37 ? 200000 : 0);
    p("m37 pre"); pkv(" m0", nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0));
    pkv(" hwcfg2", nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2));
    pkv(" wpr2_lo", nv_rd32(d, 0x1FA824u));
    pkv(" wpr2_hi", nv_rd32(d, 0x1FA828u));
    pkv(" src_d0", ld32((const uint8_t *)phys_to_virt(m_frts.phys), 0));
    pe();
    src = (const uint8_t *)phys_to_virt(m_frts.phys);
    if (nv_vram_open(d, fbaddr, 0x100000, &w) != 0) {
      p("m37 fb open FAILED\n");
    } else {
      for (uint32_t i = 0; i < 0x100000u / 4u; i++) {
        uint32_t v = ld32(src, i * 4u);
        nv_vram_wr32(&w, i * 4u, v);
        if (v) wrote++;
      }
      rbv = nv_vram_rd32(&w, 0);
      p("m37 copy nz_dwords="); print_int((int)wrote);
      pkv(" rb_d0", rbv);
      pkv(" rb_d1", nv_vram_rd32(&w, 4));
      pkv(" rb_d2", nv_vram_rd32(&w, 8));
      pkv(" rb_last", nv_vram_rd32(&w, 0xFFFFC));
      nv_vram_close(&w);
      pe();
    }
    /* point PBUS at the FB copy: media type 0 = FB, size in 4 KiB units */
    nv_wr32(d, NV_PBUS_SW_FRTS_ADDR_LO32, (uint32_t)fbaddr);
    nv_wr32(d, NV_PBUS_SW_FRTS_ADDR_HI32, (uint32_t)(fbaddr >> 32));
    nv_wr32(d, NV_PBUS_SW_FRTS_CONFIG, (0x100000u >> 12));   /* media FB, 1 MiB */
    p("m37 pbus_after lo=0x"); uart_print_hex(nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_LO32));
    pkv(" hi", nv_rd32(d, NV_PBUS_SW_FRTS_ADDR_HI32));
    pkv(" cfg", nv_rd32(d, NV_PBUS_SW_FRTS_CONFIG));
    pe();
    {
      uint32_t l_m0 = 0xFFFFFFFFu, l_hw = 0xFFFFFFFFu, l_lo = 0xFFFFFFFFu;
      uint32_t l_hi = 0xFFFFFFFFu, l_os = 0xFFFFFFFFu;
      for (int i = 0; i < (mode == 38 ? 30 : 20); i++) {
        uint32_t m0 = nv_rd32(d, NV_GSP_BASE + NV_GSP_MAILBOX0);
        uint32_t hw = nv_rd32(d, NV_GSP_BASE + NV_GSP_HWCFG2);
        uint32_t lo = nv_rd32(d, 0x1FA824u);
        uint32_t hi = nv_rd32(d, 0x1FA828u);
        uint32_t os = nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_OS);
        if (m0 != l_m0 || hw != l_hw || lo != l_lo || hi != l_hi || os != l_os) {
          p("m37 tl i="); print_int(i);
          pkv(" m0", m0); pkv(" hwcfg2", hw); pkv(" os", os);
          pkv(" wpr2_lo", lo); pkv(" wpr2_hi", hi);
          pkv(" statq_d0", ld32((const uint8_t *)phys_to_virt(blk_bus + NV_RPC_STAT_OFF), 0));
          pe();
          l_m0 = m0; l_hw = hw; l_lo = lo; l_hi = hi; l_os = os;
        }
        cot_wait_us(d, 1000000);
      }
    }
  }

  rm_p("verdict=");
  uart_puts(initdone ? "rm-answers" : (linked ? "rm-linked-no-initdone"
                                              : (lock_released ? "fmc-or-rm-window-open"
                                                               : "fmc-started-stopped")));
  pki(" mode", mode);
  pki(" cot_error", cot_err);
  pki(" lock_released", lock_released);
  pki(" statq_written", statq_written);
  pki(" linked", linked);
  pki(" initdone", initdone);
  pkv(" hwcfg2_last", hwcfg2_last);
  pkv(" m0_last", m0_last);
  pkv(" m1_last", m1_last);
  pkv(" os_last", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_OS));
  pkv(" rm_last", nv_rd32(d, NV_GSP_BASE + NV_GSP_FALCON_RM));
  pkv(" fmcerr_last", rm_fmc_error_raw(d));
  pki(" radix3", radix3_ok);
  pe();
  rm_fmc_error_row(d, "verdict");

out:
  nv_dma_free(&m_tree);
  nv_dma_free(&m_rmimg);
  nv_dma_free(&m_sig);
  nv_dma_free(&m_bootimg);
  nv_dma_free(&m_libos);
  nv_dma_free(&m_rmargs);
  nv_dma_free(&m_wprm);
  nv_dma_free(&m_cot);
  nv_dma_free(&m_args);
  if (frts_ok) nv_dma_free(&m_frts);
  nv_dma_free(&m_image);
out_files:
  if (mode != 3 && mode != 6) { nv_fw_release(&gsprm); nv_fw_release(&sig); }
  nv_fw_release(&bootimg);
  nv_fw_release(&fmc);
  return 0;
}

#else /* the COT probe is x86-64 hardware code; other arches need the symbol */

#include "errno.h"

struct nv_dev;

int nv_fsp_cot_probe(struct nv_dev *d) {
  (void)d;
  return -ENODEV;
}

#endif /* __x86_64__ */
