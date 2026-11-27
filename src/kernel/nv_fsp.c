/* ============================================================================
 *  nv_fsp: is GB202's already-running FSP reachable from a host driver?
 *
 *  The question, in one sentence: the PFSP at 0x8F0000 was started by the
 *  pre-OS chain (its own ucode published 4.76 into SCRATCH_GROUP_3(1)), so the
 *  5090 has nothing for a host to authenticate -- but is there a channel a host
 *  driver can actually speak over, and can it carry GSP-RM's boot?
 *
 *  Where the channel is (NVIDIA open-gpu-kernel-modules 580.178.04, MIT):
 *
 *    src/nvidia/generated/g_kern_fsp_nvoc.c binds GB202 (and GB203/205/206/207)
 *    the *Hopper* transport HALs: __kfspSendPacket__ = kfspSendPacket_GH100,
 *    __kfspReadPacket__ = kfspReadPacket_GH100, __kfspCanSendPacket__ =
 *    kfspCanSendPacket_GH100, __kfspIsResponseAvailable__ = ..._GH100.  GB202's
 *    own published dev_fsp_pri.h carries only the scratch groups, so these
 *    addresses are the GH100 ones *by HAL selection*, not by guess:
 *
 *      NV_PFSP_EMEMC(i)      0x008F2AC0 + 8i   RW   EMEM control
 *      NV_PFSP_EMEMD(i)      0x008F2AC4 + 8i   RW   EMEM data port
 *      NV_PFSP_QUEUE_HEAD(i) 0x008F2C00 + 8i   RW   host writes (interrupts)
 *      NV_PFSP_QUEUE_TAIL(i) 0x008F2C04 + 8i   RW   host writes
 *      NV_PFSP_MSGQ_HEAD(i)  0x008F2C80 + 8i   RW   FSP writes (response)
 *      NV_PFSP_MSGQ_TAIL(i)  0x008F2C84 + 8i   RW   FSP writes
 *      (published/hopper/gh100/dev_fsp_pri.h; FSP_EMEM_CHANNEL_RM = 0, 1 KiB)
 *
 *  Send, verbatim from kfspSendPacket_GH100 / _kfspWriteToEmem_GH100:
 *    1. channel idle = QUEUE_HEAD == QUEUE_TAIL and MSGQ_HEAD == MSGQ_TAIL
 *       (kfspCanSendPacket_GH100)
 *    2. EMEMC(0) <- {BLK=0, OFFS=0, AINCW=1}
 *    3. EMEMD(0) <- each dword of the packet
 *    4. re-read EMEMC(0): its OFFS/BLK fields must have advanced by the number
 *       of words written -- the hardware's own counter; the vendor HAL's
 *       NV_ASSERT_OR_RETURN is exactly on that delta
 *    5. QUEUE_TAIL(0) <- last dword index, then QUEUE_HEAD(0) <- 0; "The write
 *       to HEAD needs to happen after TAIL because it will interrupt FSP"
 *  Receive:
 *    6. the FSP moves MSGQ_HEAD away from MSGQ_TAIL after writing a response
 *       (kfspIsResponseAvailable_GH100 is literally msgqHead != msgqTail)
 *    7. EMEMC(0) <- {AINCR=1}, read dwords from EMEMD(0), then MSGQ_HEAD =
 *       MSGQ_TAIL, which acknowledges it
 *
 *  What the channel carries, and why it is the interesting one on Blackwell:
 *  kfspSendBootCommands_GH100 sends NVDM_TYPE_COT (0x14) = NVDM_PAYLOAD_COT --
 *  {version, size, gspFmcSysmemOffset, frtsSysmemOffset/Size,
 *   frtsVidmemOffset/Size, gspBootArgsSysmemOffset, hash384, signature,
 *   publicKey} -- and on silicon "GSP-FMC can only be booted by FSP"
 *  (kfspSafeToSendBootCommands).  So the FSP is not a side channel on GB202: it
 *  is the GSP-RM loader.  Nothing is pre-published in a GPU scratch page; the
 *  args page GSP-RM boots from is a *sysmem* descriptor whose GPU-physical
 *  address travels in that COT payload (kfspGetGspBootArgs).
 *
 *  So the measurement is five questions, each with a counter that must move:
 *
 *    (a) does the block at 0x8F2A00..0x8F2D00 decode, or does it answer a
 *        sentinel the way the absent AD102 windows do?
 *    (b) does a write land and read back, per register?  (write sentinel)
 *    (c) does the EMEM port's own OFFS counter advance on EMEMD writes?
 *    (d) does the FSP *consume* our packet (QUEUE_HEAD -> TAIL)?
 *    (e) does the FSP *answer* (MSGQ_HEAD != MSGQ_TAIL)?
 *
 *  (c), (d) and (e) cannot be faked by our own write pointer.  They are the
 *  card.
 *
 *  Safety: every write is restored to the value found, and the sentinels are
 *  *legal* words -- EMEMC's sentinel stays inside the 1 KiB channel (BLK 0,
 *  OFFS 17), so the port is never pointed out of its range, and each queue pair
 *  is written as (TAIL = HEAD = 4), so the interrupt the HEAD write raises
 *  finds an empty queue instead of a bogus packet.  The one packet sent is the
 *  vendor's own first one -- NVDM_TYPE_CAPS_QUERY with subMessageId
 *  NVDM_TYPE_CLOCK_BOOST (kfspCheckForClockBoostCapability_GB100): no address,
 *  no image, no side effect.  Gated on /FSPCH.CFG so an ordinary boot writes
 *  nothing at all.
 * ==========================================================================*/

/* uint32_t and ENODEV are needed by BOTH branches of this file: the
 * non-x86-64 branch at the bottom defines the same symbols for the unit tier.
 * They are included before the guard so the ARM build compiles (the base tree
 * did not: 11 errors, "unknown type name 'uint32_t'" + "undeclared ENODEV",
 * measured by compiling this one TU on the untouched base). */
#include <stdint.h>
#include "errno.h"

#ifdef __x86_64__

#include "nv.h"
#include "nv_regs.h"

#include "errno.h"
#include "fs.h"
#include "fat16.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

/* ---- the PFSP window's EMEM/queue block -------------------------------- */
#define NV_PFSP_OFF_QUEUE_HEAD0 0x2C00u
#define NV_PFSP_OFF_QUEUE_TAIL0 0x2C04u
#define NV_PFSP_OFF_MSGQ_HEAD0 0x2C80u
#define NV_PFSP_OFF_MSGQ_TAIL0 0x2C84u
#define NV_PFSP_OFF_EMEMC0 0x2AC0u
#define NV_PFSP_OFF_EMEMD0 0x2AC4u

#define NV_PFSP_EMEMC_OFFS(v) (((v) >> 2) & 0x3Fu)
#define NV_PFSP_EMEMC_BLK(v) (((v) >> 8) & 0xFFu)
#define NV_PFSP_EMEMC_AINCW 0x01000000u
#define NV_PFSP_EMEMC_AINCR 0x02000000u
#define NV_PFSP_EMEM_BLOCK_DWORDS 64u
#define NV_PFSP_RM_CHANNEL_DWORDS 256u /* FSP_EMEM_CHANNEL_RM_SIZE / 4 */

/* A legal EMEMC word: offset 17 dwords into block 0, autoincrement on writes.
 * Inside the 1 KiB RM channel, and different from every value the port can be
 * in when the probe starts (0x0 before first use, 0x01000000 after the FSP's
 * own last send). */
#define NV_PFSP_EMEMC_SENT 0x01000044u
/* A legal head/tail value: dword offset 4 inside the 1 KiB channel. */
#define NV_PFSP_PAIR_SENT 0x00000004u

static uint32_t ememc_word(uint32_t offs, uint32_t blk, int aincw, int aincr) {
  uint32_t v = ((offs & 0x3Fu) << 2) | ((blk & 0xFFu) << 8);
  if (aincw) v |= NV_PFSP_EMEMC_AINCW;
  if (aincr) v |= NV_PFSP_EMEMC_AINCR;
  return v;
}

/* Autoincrement offset as a flat dword index: the vendor HAL's own decode. */
uint32_t nv_fsp_ememc_offset(uint32_t ememc) {
  return NV_PFSP_EMEMC_OFFS(ememc) +
         NV_PFSP_EMEMC_BLK(ememc) * NV_PFSP_EMEM_BLOCK_DWORDS;
}

/* The EMEMC word that points the port at flat dword `offs`.  Pure (unit tier):
 * it is what keeps the write sentinels inside the channel. */
uint32_t nv_fsp_ememc_val(uint32_t offs, int aincw, int aincr) {
  uint32_t blk = offs / NV_PFSP_EMEM_BLOCK_DWORDS;
  return ememc_word(offs % NV_PFSP_EMEM_BLOCK_DWORDS, blk, aincw, aincr);
}

/* Write-sentinel verdict: what did the register do with a word we wrote?
 *   0 = LANDED   (read back the sentinel)
 *   1 = DROPPED  (read back the original value)
 *   2 = OTHER    (read back something neither we wrote nor found)
 * Pure, so the unit tier pins it without a card. */
int nv_fsp_wr_verdict(uint32_t orig, uint32_t sent, uint32_t rb) {
  if (rb == sent) return 0;
  if (rb == orig) return 1;
  return 2;
}

const char *nv_fsp_wr_verdict_name(int v) {
  switch (v) {
  case 0: return "landed";
  case 1: return "dropped";
  case 2: return "other";
  default: return "?";
  }
}

/* The packet the vendor driver sends first: MCTP header + NVDM header + the
 * one-byte caps-query payload, as the three dwords the EMEM port receives in
 * write order.  Pure (unit tier).
 *   kfspCreateMctpHeader_GH100: SOM=1 EOM=1 SEID=0 SEQ=0 (no version field)
 *   kfspCreateNvdmHeader_GH100: TYPE=0x7E VENDOR_ID=0x10DE NVDM_TYPE=0x16
 *   fsp_caps_query_rpc.h:       { NvU8 subMessageId } = NVDM_TYPE_CLOCK_BOOST
 */
void nv_fsp_caps_packet(uint32_t out[3]) {
  out[0] = (1u << 31) | (1u << 30);
  out[1] = (0x16u << 24) | (0x10DEu << 8) | 0x7Eu;
  out[2] = 0x20u;
}

/* Tail for an n-dword packet: "TAIL points to the last DWORD written, so
 * subtract 1 DWORD". */
uint32_t nv_fsp_packet_tail(uint32_t dwords) {
  return dwords ? (dwords - 1u) * 4u : 0u;
}

/* ---- rows ------------------------------------------------------------- */

static void row_rd(const char *name, uint32_t off, uint32_t v) {
  uart_puts("[NV-GB202FSP] rd off=");
  uart_print_hex(off);
  uart_puts(" name=");
  uart_puts(name);
  uart_puts(" v=");
  uart_print_hex(v);
  uart_puts("\n");
}

static void row_wr(const char *name, uint32_t off, uint32_t orig,
                   uint32_t sent, uint32_t rb, uint32_t rb2) {
  int v = nv_fsp_wr_verdict(orig, sent, rb);
  uart_puts("[NV-GB202FSP] wr off=");
  uart_print_hex(off);
  uart_puts(" name=");
  uart_puts(name);
  uart_puts(" orig=");
  uart_print_hex(orig);
  uart_puts(" sent=");
  uart_print_hex(sent);
  uart_puts(" rb=");
  uart_print_hex(rb);
  uart_puts(" verdict=");
  uart_puts(nv_fsp_wr_verdict_name(v));
  uart_puts(" rb2=");
  uart_print_hex(rb2);
  uart_puts(rb2 == orig ? " restored=ok" : " restored=OTHER");
  uart_puts("\n");
}

/* One write-sentinel row: read, write the sentinel, read back, restore the
 * value we found, read back again.  Never leaves a foreign word behind. */
static void sentinel(struct nv_dev *d, uint32_t off, const char *name,
                     uint32_t sent) {
  uint32_t orig = nv_rd32(d, off);
  uint32_t rb, rb2;
  nv_wr32(d, off, sent);
  rb = nv_rd32(d, off);
  nv_wr32(d, off, orig);
  rb2 = nv_rd32(d, off);
  row_wr(name, off, orig, sent, rb, rb2);
}

/* A head/tail pair that is only ever written equal, so the interrupt a HEAD
 * write raises always finds an empty queue.  Two rows, one verdict each. */
static void pair_sentinel(struct nv_dev *d, uint32_t head_off, uint32_t tail_off,
                          const char *name, uint32_t sent) {
  uint32_t oh = nv_rd32(d, head_off), ot = nv_rd32(d, tail_off);
  uint32_t rh, rt, rh2, rt2;
  nv_wr32(d, tail_off, sent); /* TAIL does not interrupt */
  nv_wr32(d, head_off, sent); /* HEAD interrupts; head == tail here */
  rh = nv_rd32(d, head_off);
  rt = nv_rd32(d, tail_off);
  nv_wr32(d, tail_off, ot); /* restore equal first, then ... */
  nv_wr32(d, head_off, oh); /* ... HEAD, so the queue ends empty */
  rh2 = nv_rd32(d, head_off);
  rt2 = nv_rd32(d, tail_off);
  row_wr(name, head_off, oh, sent, rh, rh2);
  row_wr(name, tail_off, ot, sent, rt, rt2);
}

/* ---- time -------------------------------------------------------------- */

/* Wait ~`ns` nanoseconds on the card's own clock, so "it did not move" is
 * measured against the GPU's PTIMER and not the host's timer.  Bounded: a
 * stopped timer must not hang the probe. */
static void wait_ns(struct nv_dev *d, uint64_t ns) {
  uint64_t t0 = 0, now = 0;
  uint64_t hz = d->timer_hz ? d->timer_hz : 1000000000ULL;
  uint64_t want = ns * (hz / 1000000ULL) / 1000ULL; /* ns -> ticks */
  if (nv_timer_read(d, &t0) != 0) return;
  for (int i = 0; i < 2000000; i++) {
    if (nv_timer_read(d, &now) != 0) return;
    if (now - t0 >= want) return;
  }
}

static void row_ch(const char *tag, int t_us, uint32_t qh, uint32_t qt,
                   uint32_t mh, uint32_t mt) {
  uart_puts("[NV-GB202FSP] ");
  uart_puts(tag);
  uart_puts(" t_us=");
  print_int(t_us);
  uart_puts(" qh=");
  uart_print_hex(qh);
  uart_puts(" qt=");
  uart_print_hex(qt);
  uart_puts(" mh=");
  uart_print_hex(mh);
  uart_puts(" mt=");
  uart_print_hex(mt);
  uart_puts("\n");
}

/* ---- the packet test --------------------------------------------------- */

struct nv_fsp_verdict {
  int consumed; /* QUEUE_HEAD left the 0 we wrote: the FSP took our packet */
  int answered; /* MSGQ_HEAD != MSGQ_TAIL: the FSP put a word of its own */
  int autoinc;  /* EMEMC's OFFS advanced by the number of dwords written */
  int dwords;   /* response size in dwords, 0 when none */
};

/* Read one response out of EMEM and acknowledge it exactly as
 * kfspReadPacket_GH100 does: size = (MSGQ_TAIL - MSGQ_HEAD) + 4 bytes,
 * EMEMC <- {AINCR}, read dwords, then MSGQ_TAIL = MSGQ_HEAD = the head seen.
 * Returns the response size in dwords, 0 when the SP had nothing. */
static int read_response(struct nv_dev *d, uint32_t base, const char *tag) {
  uint32_t mh = nv_rd32(d, base + NV_PFSP_OFF_MSGQ_HEAD0);
  uint32_t mt = nv_rd32(d, base + NV_PFSP_OFF_MSGQ_TAIL0);
  uint32_t dwords;
  if (mh == mt) {
    uart_puts("[NV-GB202FSP] ");
    uart_puts(tag);
    uart_puts(" none (MSGQ_HEAD==MSGQ_TAIL: no message the SP answered)\n");
    return 0;
  }
  dwords = ((mt - mh) / 4u) + 1u; /* tail = last dword index */
  uart_puts("[NV-GB202FSP] ");
  uart_puts(tag);
  uart_puts(" head=");
  uart_print_hex(mh);
  uart_puts(" tail=");
  uart_print_hex(mt);
  uart_puts(" size=");
  print_int((int)(dwords * 4u));
  uart_puts(" dwords=");
  print_int((int)dwords);
  uart_puts(" (MSGQ_HEAD!=MSGQ_TAIL: the SP moved a word of its own)\n");
  if (dwords <= NV_PFSP_RM_CHANNEL_DWORDS) {
    nv_wr32(d, base + NV_PFSP_OFF_EMEMC0, nv_fsp_ememc_val(0, 0, 1));
    uart_puts("[NV-GB202FSP] ");
    uart_puts(tag);
    uart_puts(" ememc=");
    uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_EMEMC0));
    uart_puts("\n");
    for (uint32_t i = 0; i < dwords; i++) {
      uart_puts("[NV-GB202FSP] ");
      uart_puts(tag);
      uart_puts(" i=");
      print_int((int)i);
      uart_puts(" v=");
      uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_EMEMD0));
      uart_puts("\n");
    }
    uart_puts("[NV-GB202FSP] ");
    uart_puts(tag);
    uart_puts(" ememc_end=");
    uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_EMEMC0));
    uart_puts("\n");
  }
  /* acknowledge exactly as kfspReadPacket_GH100 does: MSGQ_TAIL =
   * MSGQ_HEAD = the head we saw */
  nv_wr32(d, base + NV_PFSP_OFF_MSGQ_TAIL0, mh);
  nv_wr32(d, base + NV_PFSP_OFF_MSGQ_HEAD0, mh);
  return (int)dwords;
}

/* One row per step, so a failure names itself. */
static struct nv_fsp_verdict packet_test(struct nv_dev *d, uint32_t base) {
  struct nv_fsp_verdict r = {0, 0, 0, 0};
  uint32_t pkt[3];
  uint32_t qh0, qt0, mh0, mt0;
  uint32_t ememc, off_start, off_end;
  uint32_t tail;
  uint32_t qh_last = 0, qt_last = 0;
  int first_resp_us = -1;

  nv_fsp_caps_packet(pkt);
  tail = nv_fsp_packet_tail(3);

  qh0 = nv_rd32(d, base + NV_PFSP_OFF_QUEUE_HEAD0);
  qt0 = nv_rd32(d, base + NV_PFSP_OFF_QUEUE_TAIL0);
  mh0 = nv_rd32(d, base + NV_PFSP_OFF_MSGQ_HEAD0);
  mt0 = nv_rd32(d, base + NV_PFSP_OFF_MSGQ_TAIL0);
  row_ch("send pre", 0, qh0, qt0, mh0, mt0);
  uart_puts("[NV-GB202FSP] send idle=");
  uart_puts((qh0 == qt0 && mh0 == mt0) ? "yes" : "no");
  uart_puts(" (kfspCanSendPacket_GH100: QUEUE_HEAD==TAIL and"
            " MSGQ_HEAD==MSGQ_TAIL)\n");

  /* 2. point the EMEM port at channel 0, dword 0, autoincrement on writes */
  nv_wr32(d, base + NV_PFSP_OFF_EMEMC0, nv_fsp_ememc_val(0, 1, 0));
  ememc = nv_rd32(d, base + NV_PFSP_OFF_EMEMC0);
  off_start = nv_fsp_ememc_offset(ememc);
  uart_puts("[NV-GB202FSP] send ememc wr=");
  uart_print_hex(nv_fsp_ememc_val(0, 1, 0));
  uart_puts(" rb=");
  uart_print_hex(ememc);
  uart_puts(" offs=");
  print_int((int)off_start);
  uart_puts("\n");

  /* 3. the three payload dwords */
  for (int i = 0; i < 3; i++) nv_wr32(d, base + NV_PFSP_OFF_EMEMD0, pkt[i]);
  ememc = nv_rd32(d, base + NV_PFSP_OFF_EMEMC0);
  off_end = nv_fsp_ememc_offset(ememc);
  uart_puts("[NV-GB202FSP] send ememd words=3 off_start=");
  print_int((int)off_start);
  uart_puts(" off_end=");
  print_int((int)off_end);
  uart_puts(" advanced=");
  print_int((int)(off_end - off_start));
  r.autoinc = (off_end - off_start) == 3;
  uart_puts(r.autoinc ? " verdict=autoincrement-ok"
                      : " verdict=autoincrement-FAILED");
  uart_puts(" (the port's own counter, the vendor HAL's NV_ASSERT)\n");

  /* 5. hand the packet to the FSP: TAIL first, then HEAD (the interrupt) */
  nv_wr32(d, base + NV_PFSP_OFF_QUEUE_TAIL0, tail);
  nv_wr32(d, base + NV_PFSP_OFF_QUEUE_HEAD0, 0);
  uart_puts("[NV-GB202FSP] send qt=");
  uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_QUEUE_TAIL0));
  uart_puts(" qh=");
  uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_QUEUE_HEAD0));
  uart_puts(" (read back after the HEAD write, which is the interrupt)\n");

  /* 6. timeline: sample all four words for 300 ms, even after the answer
   * appears.  The SP's response and its *command-queue ack* (HEAD <- TAIL)
   * are two separate writes, so breaking on the response hides the ack --
   * and without the ack a second packet cannot be sent (kfspCanSendPacket). */
  {
    static const int at_us[] = {0, 50, 200, 1000, 5000, 20000, 100000, 300000};
    uint64_t t0 = 0;
    (void)nv_timer_read(d, &t0);
    for (unsigned i = 0; i < sizeof(at_us) / sizeof(at_us[0]); i++) {
      uint32_t qh, qt, mh, mt;
      uint64_t now = 0;
      uint64_t hz = d->timer_hz ? d->timer_hz : 1000000000ULL;
      for (int spin = 0; spin < 2000000; spin++) {
        if (nv_timer_read(d, &now) != 0) break;
        if ((now - t0) * 1000000ULL >= (uint64_t)at_us[i] * hz) break;
      }
      qh = nv_rd32(d, base + NV_PFSP_OFF_QUEUE_HEAD0);
      qt = nv_rd32(d, base + NV_PFSP_OFF_QUEUE_TAIL0);
      mh = nv_rd32(d, base + NV_PFSP_OFF_MSGQ_HEAD0);
      mt = nv_rd32(d, base + NV_PFSP_OFF_MSGQ_TAIL0);
      row_ch("tl", at_us[i], qh, qt, mh, mt);
      if (mt != mh && first_resp_us < 0) first_resp_us = at_us[i];
      qh_last = qh;
      qt_last = qt;
    }
    /* Pre-ack state only: the SP may free the command queue only after its
     * response has been consumed, so this row is not the ack. */
    uart_puts("[NV-GB202FSP] preack qh=");
    uart_print_hex(qh_last);
    uart_puts(" qt=");
    uart_print_hex(qt_last);
    uart_puts(" tail_written=");
    uart_print_hex(tail);
    uart_puts((qh_last == tail && qt_last == tail) ? " head_acked=yes"
                                                   : " head_acked=no");
    uart_puts(" first_resp_us=");
    print_int(first_resp_us);
    uart_puts("\n");
  }

  /* 7. the response, if any: read it out, acknowledge it */
  r.dwords = read_response(d, base, "recv");
  r.answered = r.dwords > 0;

  /* 7b. the command-queue release, sampled *after* the response was read and
   * acknowledged.  The SP can only free its command queue once its response
   * has been consumed, so a sample taken before the read (the preack row
   * above) cannot see it.  Measured: the SP clears QUEUE_TAIL back to the
   * idle value rather than copying TAIL into HEAD, so "free" is HEAD==TAIL,
   * not HEAD==the tail we wrote. */
  {
    static const int wait_us[] = {0, 50, 200, 1000, 5000, 20000, 100000, 300000};
    uint64_t t0 = 0;
    uint32_t qh = 0, qt = 0;
    int idle_us = -1;
    (void)nv_timer_read(d, &t0);
    for (unsigned i = 0; i < sizeof(wait_us) / sizeof(wait_us[0]); i++) {
      uint64_t now = 0;
      uint64_t hz = d->timer_hz ? d->timer_hz : 1000000000ULL;
      for (int spin = 0; spin < 2000000; spin++) {
        if (nv_timer_read(d, &now) != 0) break;
        if ((now - t0) * 1000000ULL >= (uint64_t)wait_us[i] * hz) break;
      }
      qh = nv_rd32(d, base + NV_PFSP_OFF_QUEUE_HEAD0);
      qt = nv_rd32(d, base + NV_PFSP_OFF_QUEUE_TAIL0);
      if (qh == qt) {
        idle_us = wait_us[i];
        break;
      }
    }
    r.consumed = (idle_us >= 0);
    uart_puts("[NV-GB202FSP] postack qh=");
    uart_print_hex(qh);
    uart_puts(" qt=");
    uart_print_hex(qt);
    uart_puts(" idle=");
    uart_puts(r.consumed ? "yes" : "no");
    uart_puts(" idle_us=");
    print_int(idle_us);
    uart_puts("\n");
  }

  /* 8. a second packet, only if the SP acknowledged the first one.  With
   * QUEUE_HEAD != TAIL, kfspCanSendPacket_GH100 refuses to send, so a raw
   * write there would be a protocol violation and not a measurement. */
  if (r.consumed) {
    static const int wait_us[] = {0, 50, 200, 1000, 5000, 20000, 100000, 300000};
    int second_us = -1;
    uint64_t t0 = 0;
    nv_wr32(d, base + NV_PFSP_OFF_EMEMC0, nv_fsp_ememc_val(0, 1, 0));
    for (int i = 0; i < 3; i++) nv_wr32(d, base + NV_PFSP_OFF_EMEMD0, pkt[i]);
    nv_wr32(d, base + NV_PFSP_OFF_QUEUE_TAIL0, tail);
    nv_wr32(d, base + NV_PFSP_OFF_QUEUE_HEAD0, 0);
    (void)nv_timer_read(d, &t0);
    for (unsigned i = 0; i < sizeof(wait_us) / sizeof(wait_us[0]); i++) {
      uint64_t now = 0;
      uint64_t hz = d->timer_hz ? d->timer_hz : 1000000000ULL;
      for (int spin = 0; spin < 2000000; spin++) {
        if (nv_timer_read(d, &now) != 0) break;
        if ((now - t0) * 1000000ULL >= (uint64_t)wait_us[i] * hz) break;
      }
      if (nv_rd32(d, base + NV_PFSP_OFF_MSGQ_HEAD0) !=
          nv_rd32(d, base + NV_PFSP_OFF_MSGQ_TAIL0)) {
        second_us = wait_us[i];
        break;
      }
    }
    uart_puts("[NV-GB202FSP] second resp_us=");
    print_int(second_us);
    uart_puts(" (a second request over the same channel)\n");
    (void)read_response(d, base, "second");
    /* and the queue returns to idle again: the same release as the first */
    wait_ns(d, 5000000ULL);
    uart_puts("[NV-GB202FSP] second postack qh=");
    uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_QUEUE_HEAD0));
    uart_puts(" qt=");
    uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_QUEUE_TAIL0));
    uart_puts("\n");
  } else {
    uart_puts("[NV-GB202FSP] second skipped: the queue was not released"
              " (HEAD!=TAIL), so kfspCanSendPacket_GH100 would refuse\n");
  }
  return r;
}

/* ---- the region shape -------------------------------------------------- */

/* Is the EMEM block a real window or a uniform sentinel?  The absent AD102-era
 * addresses answer one word at every offset (docs/nv-gb202-bringup.md 4.3.2);
 * a real window is never uniform.  Print the counts and the first words that
 * are not one of the three sentinel classes this part uses. */
static void region_shape(struct nv_dev *d, uint32_t base, uint32_t lo,
                         uint32_t hi) {
  uint32_t words = 0, readable = 0, shown = 0, first = 0, transitions = 0;
  uint32_t prev = 0;
  for (uint32_t off = lo; off < hi; off += 4) {
    uint32_t v = nv_rd32(d, base + off);
    if (words == 0) first = v;
    if (words > 0 && v != prev) transitions++;
    prev = v;
    words++;
    if (v != NV_REG_BLOCK_SENTINEL && v != NV_REG_UNREADABLE &&
        v != NV_REG_FENCED)
      readable++;
    if (v != NV_REG_BLOCK_SENTINEL && v != NV_REG_UNREADABLE && shown < 12) {
      uart_puts("[NV-GB202FSP] shape off=");
      uart_print_hex(base + off);
      uart_puts(" v=");
      uart_print_hex(v);
      uart_puts("\n");
      shown++;
    }
  }
  uart_puts("[NV-GB202FSP] shape lo=");
  uart_print_hex(base + lo);
  uart_puts(" hi=");
  uart_print_hex(base + hi);
  uart_puts(" words=");
  print_int((int)words);
  uart_puts(" not-a-sentinel=");
  print_int((int)readable);
  uart_puts(" transitions=");
  print_int((int)transitions);
  uart_puts(" first=");
  uart_print_hex(first);
  uart_puts(readable ? " verdict=decodes\n" : " verdict=uniform-sentinel\n");
}

/* ---- entry ------------------------------------------------------------- */

/* Which variant does /FSPCH.CFG ask for?  0 = the file is absent (do nothing),
 * otherwise a bitmask: 1 = register map, 2 = the caps-query packet.  The file
 * is the on-image record of the variant, the same rule /NVCTL.CFG follows. */
static int fsp_cfg(void) {
  struct file f;
  char buf[64];
  if (fat16_open("FSPCH.CFG", &f) != 0) return 0;
  int n = fat16_read(&f, buf, sizeof buf - 1);
  fat16_close(&f);
  if (n <= 0) return 1 | 2; /* an empty file means "the whole experiment" */
  buf[n] = '\0';
  int mask = 0;
  for (int i = 0; i < n; i++) {
    if (buf[i] == 'm') mask |= 1;          /* map only */
    else if (buf[i] == 'p') mask |= 2;     /* packet only */
    else if (buf[i] == 'a') mask |= 1 | 2; /* all */
  }
  return mask ? mask : (1 | 2);
}

int nv_fsp_channel_probe(struct nv_dev *d) {
  if (!d) return -EINVAL;
  int mask = fsp_cfg();
  if (!mask) return -ENOENT; /* not requested: say nothing at all */
  if (d->pfsp_base == 0) {
    uart_puts("[NV-GB202FSP] skipped: no PFSP window on this part\n");
    return -ENODEV;
  }
  const uint32_t base = d->pfsp_base;
  uart_puts("[NV-GB202FSP] start base=");
  uart_print_hex(base);
  uart_puts(" variant=");
  uart_puts((mask & 2) ? "map+packet" : "map");
  uart_puts(" map=kern_fsp_nvoc.c binds GB202->*_GH100; EMEMC/EMEMD/QUEUE/MSGQ"
            " at +0x2AC0/+0x2C00/+0x2C80\n");

  /* the two words that say the FSP is the thing we are talking to, and the
   * THERM gate that says it finished its own boot (t_67f09feb's premise) */
  row_rd("fsp_boot_complete", 0x00AD00BCu, nv_rd32(d, 0x00AD00BCu));
  row_rd("ucode_ver", base + NV_PFSP_SCRATCH_GROUP_3 + 4u,
         nv_rd32(d, base + NV_PFSP_SCRATCH_GROUP_3 + 4u));
  row_rd("scratch_g2_0", base + NV_PFSP_SCRATCH_GROUP_2,
         nv_rd32(d, base + NV_PFSP_SCRATCH_GROUP_2));
  row_rd("scratch_g2_3", base + NV_PFSP_SCRATCH_GROUP_2 + 12u,
         nv_rd32(d, base + NV_PFSP_SCRATCH_GROUP_2 + 12u));

  /* the falcon-shaped anchors, read-only, so one log says which of the words
   * the [NV-PFSP] row carries still hold here */
  row_rd("caps", base + NV_FALCON_UC_CAPS,
         nv_rd32(d, base + NV_FALCON_UC_CAPS));
  row_rd("caps2", base + NV_FALCON_UC_CAPS2,
         nv_rd32(d, base + NV_FALCON_UC_CAPS2));
  row_rd("ctl", base + NV_FALCON_UC_CTRL,
         nv_rd32(d, base + NV_FALCON_UC_CTRL));
  row_rd("busy", base + NV_FALCON_STATUS_BUSY,
         nv_rd32(d, base + NV_FALCON_STATUS_BUSY));
  row_rd("sec_mode", base + NV_FALCON_SEC_MODE,
         nv_rd32(d, base + NV_FALCON_SEC_MODE));

  if (mask & 1) {
    /* (a) does the block decode? */
    region_shape(d, base, 0x2A00u, 0x2D00u);

    /* the six channel registers as read-only rows first: the values the FSP
     * left behind before any write of ours */
    row_rd("queue_head0", base + NV_PFSP_OFF_QUEUE_HEAD0,
           nv_rd32(d, base + NV_PFSP_OFF_QUEUE_HEAD0));
    row_rd("queue_tail0", base + NV_PFSP_OFF_QUEUE_TAIL0,
           nv_rd32(d, base + NV_PFSP_OFF_QUEUE_TAIL0));
    row_rd("msgq_head0", base + NV_PFSP_OFF_MSGQ_HEAD0,
           nv_rd32(d, base + NV_PFSP_OFF_MSGQ_HEAD0));
    row_rd("msgq_tail0", base + NV_PFSP_OFF_MSGQ_TAIL0,
           nv_rd32(d, base + NV_PFSP_OFF_MSGQ_TAIL0));
    row_rd("ememc0", base + NV_PFSP_OFF_EMEMC0,
           nv_rd32(d, base + NV_PFSP_OFF_EMEMC0));
    row_rd("ememd0", base + NV_PFSP_OFF_EMEMD0,
           nv_rd32(d, base + NV_PFSP_OFF_EMEMD0));

    /* (b) write sentinel, per register.  The falcon mailbox pair and one
     * scratch word are the controls: same window, plain RW registers, so a
     * dead EMEM block next to a live mailbox is a statement about the block
     * and not about the BAR.  SCRATCH_GROUP_3(1) is NOT written: it is the
     * ucode version the FSP published, i.e. the evidence.  The queue pairs go
     * last, written matched, so the HEAD interrupt finds an empty queue. */
    sentinel(d, base + NV_PFSP_OFF_EMEMC0, "ememc0", NV_PFSP_EMEMC_SENT);
    sentinel(d, base + NV_FALCON_MAILBOX0_FLCN, "mailbox0", 0xA5A51234u);
    sentinel(d, base + NV_FALCON_MAILBOX1_FLCN, "mailbox1", 0xA5A51234u);
    sentinel(d, base + NV_PFSP_SCRATCH_GROUP_2 + 12u, "scratch_g2_3",
             0xA5A51234u);
    pair_sentinel(d, base + NV_PFSP_OFF_MSGQ_HEAD0,
                  base + NV_PFSP_OFF_MSGQ_TAIL0, "msgq0", NV_PFSP_PAIR_SENT);
    pair_sentinel(d, base + NV_PFSP_OFF_QUEUE_HEAD0,
                  base + NV_PFSP_OFF_QUEUE_TAIL0, "queue0", NV_PFSP_PAIR_SENT);

    /* "changes on its own": the same words again after ~50 ms of card time */
    {
      uint32_t qh, qt, mh, mt, g2;
      wait_ns(d, 50000000ULL);
      qh = nv_rd32(d, base + NV_PFSP_OFF_QUEUE_HEAD0);
      qt = nv_rd32(d, base + NV_PFSP_OFF_QUEUE_TAIL0);
      mh = nv_rd32(d, base + NV_PFSP_OFF_MSGQ_HEAD0);
      mt = nv_rd32(d, base + NV_PFSP_OFF_MSGQ_TAIL0);
      g2 = nv_rd32(d, base + NV_PFSP_SCRATCH_GROUP_2 + 12u);
      uart_puts("[NV-GB202FSP] later qh=");
      uart_print_hex(qh);
      uart_puts(" qt=");
      uart_print_hex(qt);
      uart_puts(" mh=");
      uart_print_hex(mh);
      uart_puts(" mt=");
      uart_print_hex(mt);
      uart_puts(" g2_3=");
      uart_print_hex(g2);
      uart_puts(" (after 50 ms of PTIMER)\n");
    }
  }

  /* (c)+(d)+(e): the only tests that can come back "yes" */
  if (mask & 2) {
    struct nv_fsp_verdict v = packet_test(d, base);
    uart_puts("[NV-GB202FSP] verdict=");
    if (v.answered)
      uart_puts("sp-answered");
    else if (v.consumed)
      uart_puts("sp-consumed-no-answer");
    else if (v.autoinc)
      uart_puts("port-live-sp-silent");
    else
      uart_puts("no-channel");
    uart_puts(" consumed=");
    print_int(v.consumed);
    uart_puts(" answered=");
    print_int(v.answered);
    uart_puts(" autoinc=");
    print_int(v.autoinc);
    uart_puts(" resp_dwords=");
    print_int(v.dwords);
    uart_puts("\n");
  }

  /* leave the channel as we found it: our command queue at 0/0 and the two
   * FSP-owned words equal, so nothing downstream sees an unfinished
   * transaction of ours */
  {
    uint32_t mh = nv_rd32(d, base + NV_PFSP_OFF_MSGQ_HEAD0);
    nv_wr32(d, base + NV_PFSP_OFF_QUEUE_TAIL0, 0);
    nv_wr32(d, base + NV_PFSP_OFF_QUEUE_HEAD0, 0);
    nv_wr32(d, base + NV_PFSP_OFF_MSGQ_TAIL0, mh);
    nv_wr32(d, base + NV_PFSP_OFF_MSGQ_HEAD0, mh);
    uart_puts("[NV-GB202FSP] end qh=");
    uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_QUEUE_HEAD0));
    uart_puts(" qt=");
    uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_QUEUE_TAIL0));
    uart_puts(" mh=");
    uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_MSGQ_HEAD0));
    uart_puts(" mt=");
    uart_print_hex(nv_rd32(d, base + NV_PFSP_OFF_MSGQ_TAIL0));
    uart_puts("\n");
  }
  return 0;
}

#else /* the probe is x86-64 hardware code; other arches need the symbols */

#include <stdint.h>
#include "errno.h"

struct nv_dev;

int nv_fsp_channel_probe(struct nv_dev *d) {
  (void)d;
  return -ENODEV;
}
uint32_t nv_fsp_ememc_offset(uint32_t ememc) { return ememc; }
uint32_t nv_fsp_ememc_val(uint32_t offs, int aincw, int aincr) {
  (void)aincw; (void)aincr;
  return offs;
}
int nv_fsp_wr_verdict(uint32_t orig, uint32_t sent, uint32_t rb) {
  (void)orig; (void)sent;
  return rb ? 2 : 1;
}
const char *nv_fsp_wr_verdict_name(int v) {
  (void)v;
  return "?";
}
void nv_fsp_caps_packet(uint32_t out[3]) {
  out[0] = out[1] = out[2] = 0;
}
uint32_t nv_fsp_packet_tail(uint32_t dwords) { return dwords ? dwords - 1u : 0; }

#endif /* __x86_64__ */
