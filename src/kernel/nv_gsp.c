/* ============================================================================
 *  nv_gsp: the GSP firmware stage -- probe now, boot in stages.
 *
 *  On Turing and later every useful GPU function lives in GSP-RM, which runs on
 *  the GPU's own processor (a RISC-V core behind a falcon-shaped register
 *  front end on this generation).  Booting it is a chain of vendor-signed
 *  blobs: the host places the booter and the RM image in a write-protected
 *  region of VRAM, describes them in a 256-byte metadata block, starts the
 *  processor, and then talks to RM over shared-memory RPC queues.
 *
 *  THE CORRECTION THAT MATTERS (docs/nv-gsp-wpr-contract.md section 2): the host
 *  does NOT program the write-protected region on AD102/535.  The booter, run
 *  on SEC2, builds WPR2 from the 256-byte metadata block; the host only writes
 *  that block into system memory and hands over its address.  Every earlier plan
 *  here that said "lay out the region, then program it" was wrong in the first
 *  step, and the region registers are read-protected anyway (see the probe).
 *
 *  What this file does TODAY, and what it refuses to do:
 *
 *    nv_gsp_probe()   reads and logs the registers the boot path depends on
 *                     (WPR2 base/size, the falcon's extended control words,
 *                     the RISC-V status word).  Pure observation; safe.
 *    nv_gsp_boot()    the staged machine.  Each stage logs one row and either
 *                     completes or names itself as the stopping point.  A stage
 *                     whose register contract is not yet verified on real
 *                     hardware returns -ENOTSUP with a row saying so -- a
 *                     driver that guesses a write into the write-protected
 *                     region and then reports "GSP boot failed" has told you
 *                     nothing, and may have wedged the card for the next run.
 *
 *  The stages, in the order the measured contract says they happen
 *  (docs/nv-gsp-wpr-contract.md section 8, docs/nv-gsp-rpc-contract.md):
 *    1 firmware on disk: read + parse + hash      DONE on silicon
 *    2 halt SEC2 and the GSP                       DONE (falcon halt)
 *    3 build the radix3 tree for the RM image     DONE (nv_radix3_build, 9288 pages)
 *    4 write the 256-byte metadata block          block built + DMA-able via
 *                                                 nv_dma; SEC2 reads it, host
 *                                                 does not write it into VRAM
 *    5 DMA the booter into SEC2 IMEM+DMEM         DONE (SEC2 xfer; row per half)
 *    6 start SEC2, wait for MAILBOX0 == 0         attempted every boot
 *    7 metadata `verified` == 0xA0A0...           not reached (booter stops)
 *   7b RPC pre-boot: 72 then 73 queued, one      DONE (runs before the launch;
 *      doorbell (contract 4.1 rows 3-5)           rows say wr=2 pw=0)
 *    8 build the RPC block (nv_dma + nv_rpc)      block built + published; the
 *                                                 link waits for RM's header,
 *                                                 and the send/doorbell with it
 *    9 GSP_INIT_DONE over RPC                     not started
 * ========================================================================== */

#ifdef __x86_64__

#include "nv.h"
#include "nv_regs.h"
#include "nv_fw.h"
#include "nv_flcn.h"
#include "nv_dma.h"
#include "nv_rpc.h"
#include "nv_gsp_glue.h"
#include "nv_vram.h"
#include "frame.h"
#include "kmem.h"
#include "errno.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

/* Two hex digits.  uart_print_hex() is 16 digits wide, which turns an 8-byte
 * digest into a wall of zeros and makes the log hard to compare by eye. */
static void nv_hex_byte(uint8_t b) {
  static const char *d = "0123456789abcdef";
  char s[3] = {d[b >> 4], d[b & 0xF], 0};
  uart_puts(s);
}

/* ============================================================================
 *  stage 8 glue: nv_dma -> nv_rpc (src/include/nv_gsp_glue.h)
 *
 *  WHAT THIS REPLACES.  The RPC block used to be a plan and nothing else: the
 *  doorbell existed as a local define nobody wrote, and the only addresses the
 *  boot path handed to a device were bare frame_alloc_contig() physical
 *  addresses.  A GPU does not dereference those; it dereferences whatever its
 *  IOMMU context says they mean, so the two are the same number only while
 *  VT-d translation is off (nv_dma.h says the rest).  The block below is the
 *  only place the two facilities meet, and it takes the GPU-visible base from
 *  nv_dma_bus() and from nowhere else.
 *
 *  THE TWO PROPERTIES THE UNIT TIER PINS (nv_gsp_glue_test.c):
 *    G-02  PTE[0] of the built block == nv_dma_bus() of the mapping
 *    G-04  one nv_rpc_kick() per message -- one write, offset 0x110C00, value 0
 * ========================================================================== */

/* Byte-wise little-endian readers, the same rule the transport uses: the block
 * is written by a device, so nothing here may be a wide load at an offset the
 * compiler chose (skills/hobbyos-kernel-constraints rule 3). */
static uint32_t glue_ld32(const uint8_t *p) {
  const volatile uint8_t *b = p;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

/* Byte-wise little-endian writers, same rule: the block is read by a device, so
 * no wide store at a compiler-chosen offset.  Only our own words are ever
 * written here (never the status header: that one belongs to RM). */
static void glue_st32(uint8_t *p, uint32_t v) {
  volatile uint8_t *b = p;
  b[0] = (uint8_t)v;
  b[1] = (uint8_t)(v >> 8);
  b[2] = (uint8_t)(v >> 16);
  b[3] = (uint8_t)(v >> 24);
}

static uint64_t glue_ld64(const uint8_t *p) {
  const volatile uint8_t *b = p;
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
  return v;
}

int nv_gsp_rpc_build(struct nv_gsp_rpc *r) {
  if (!r) return -EINVAL;
  if (!r->block.phys || !r->block.pages) {
    uart_puts("[NV-RPC] stage=block FAIL no mapping: nv_dma_alloc must come "
              "first\n");
    return -EINVAL;
  }
  if ((uint64_t)r->block.pages * NV_DMA_PAGE != NV_RPC_BLOCK_SIZE ||
      r->block.len != NV_RPC_BLOCK_SIZE) {
    /* pages and len are the same number twice (PLAN.md section 6 rule 3): a
     * block whose mapping is not exactly 129 pages is a block whose last PTEs
     * point at somebody else's memory. */
    uart_puts("[NV-RPC] stage=block FAIL mapping is not the block: len=");
    uart_print_hex(r->block.len);
    uart_puts(" pages=");
    print_int((int)r->block.pages);
    uart_puts("\n");
    return -EINVAL;
  }

  /* THE SEAM.  Not r->block.phys, not a literal: the accessor, which answers
   * NV_DMA_BUS_INVALID while translation is live.  A build from `phys` here is
   * the bug the facility exists to prevent (a page table full of addresses the
   * GPU cannot resolve), so it fails here rather than at the firmware. */
  uint64_t bus = nv_dma_bus(&r->block);
  if (bus == NV_DMA_BUS_INVALID) {
    uart_puts("[NV-RPC] stage=block FAIL nv_dma_bus refused for phys=");
    uart_print_hex(r->block.phys);
    uart_puts(": no bus address exists, so no PTE table is written\n");
    return -ENOTSUP;
  }

  uint8_t *block = (uint8_t *)phys_to_virt(r->block.phys);
  int rc = nv_rpc_block_build(block, bus);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=block FAIL nv_rpc_block_build rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }

  rc = nv_rpc_queue_init(&r->q, block, bus, r->rx_stage);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=queue FAIL nv_rpc_queue_init rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  r->built = 1;

  /* Read the PTE back out of the bytes rather than trusting the call: the
   * claim is about what the firmware will walk, and only the block itself
   * answers that. */
  uint64_t pte0 = glue_ld64(block);
  uint64_t pte_last = glue_ld64(block + (uint64_t)(NV_RPC_PTE_COUNT - 1u) * 8u);
  uart_puts("[NV-RPC] stage=block bus=");
  uart_print_hex(bus);
  uart_puts(" phys=");
  uart_print_hex(r->block.phys);
  uart_puts(" pages=");
  print_int((int)r->block.pages);
  uart_puts(" pte0=");
  uart_print_hex(pte0);
  uart_puts(" pte128=");
  uart_print_hex(pte_last);
  uart_puts(" identity=");
  print_int(bus == r->block.phys ? 1 : 0);
  uart_puts("\n");
  if (pte0 != bus ||
      pte_last != bus + (uint64_t)(NV_RPC_PTE_COUNT - 1u) * NV_RPC_SLOT) {
    uart_puts("[NV-RPC] stage=block FAIL PTE table does not describe the "
              "mapping's bus address\n");
    r->built = 0;
    return -EIO;
  }
  return 0;
}

/* ============================================================================
 *  The publish side: GSP_ARGUMENTS_CACHED + the libos-args table + MAILBOX0/1
 *  (docs/nv-gsp-rpc-contract.md 1.5 and 1.6)
 *
 *  THE GAP THIS CLOSES.  Building the block is not telling firmware about it.
 *  RM does not scan memory: it reads a one-page argument structure, finds it
 *  through a table of named regions whose address the host put in two falcon
 *  mailbox words, and walks the 129 PTEs from mq.sharedMemPhysAddr.  Without
 *  this, a running RM reads a zero status header forever -- which is
 *  indistinguishable, in a log, from RM never having started at all.
 *
 *  NO PEER IS NEEDED.  All of it is host-side image construction plus two
 *  MMIO dwords, so it is checkable on a card where SEC2 has never reached
 *  INIT_DONE, which is the card we have.
 * ========================================================================== */

/* The two GSP falcon mailbox words.  Defined here rather than beside the other
 * GSP registers further down because the publish below is the first user. */
#define NV_GSP_MAILBOX0 (NV_FALCON_GSP_BASE + 0x040)
#define NV_GSP_MAILBOX1 (NV_FALCON_GSP_BASE + 0x044)

/* One id8 as its eight characters.  Printed straight out of the re-read page,
 * so the log shows the bytes the firmware will read rather than the string we
 * meant to write; a non-printable byte becomes '.', which is visibly not a
 * region name. */
static void nv_arg_put_id8(uint64_t id) {
  char s[9];
  for (int i = 0; i < 8; i++) {
    uint8_t c = (uint8_t)((id >> (8 * i)) & 0xFFu);
    s[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
  }
  s[8] = 0;
  uart_puts(s);
}

/* The argument page, the libos table, and the three log regions the table
 * names.  Taken and released together: a table that names regions nobody
 * allocated would point the firmware at someone else's memory. */
static int nv_gsp_args_alloc(struct nv_gsp_rpc *r) {
  int rc = nv_dma_alloc(NV_RPC_GSP_ARGS_PAGE, &r->args);
  if (rc != 0) {
    uart_puts("[NV-ARG] stage=alloc FAIL args page rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  rc = nv_dma_alloc(NV_RPC_GSP_ARGS_PAGE, &r->libos);
  if (rc != 0) {
    uart_puts("[NV-ARG] stage=alloc FAIL libos table rc=");
    print_int(rc);
    uart_puts("\n");
    nv_dma_free(&r->args);
    return rc;
  }
  rc = nv_dma_alloc((uint64_t)NV_RPC_LIBOS_LOG_PAGES * NV_DMA_PAGE, &r->logs);
  if (rc != 0) {
    uart_puts("[NV-ARG] stage=alloc FAIL log regions rc=");
    print_int(rc);
    uart_puts("\n");
    nv_dma_free(&r->args);
    nv_dma_free(&r->libos);
    return rc;
  }
  return 0;
}

int nv_gsp_rpc_publish(struct nv_gsp_rpc *r, struct nv_dev *d) {
  if (!r || !d) return -EINVAL;
  if (!r->built) {
    uart_puts("[NV-ARG] stage=publish FAIL no block: publishing an address "
              "nothing was built at would name a page table that does not "
              "exist\n");
    return -EINVAL;
  }
  if (!r->args.phys || !r->libos.phys || !r->logs.phys) {
    uart_puts("[NV-ARG] stage=publish FAIL no argument pages (nv_dma_alloc "
              "runs in setup)\n");
    return -EINVAL;
  }

  struct nv_rpc_geom g;
  if (nv_rpc_queue_geom(&g) != 0) return -EINVAL;

  /* THE ADDRESS RULE.  Every address below is an nv_dma_bus() answer, and
   * `phys` appears only as the CPU's way to reach a page it is writing.  The
   * page the firmware walks and the page the firmware is told about are then
   * the same number by construction, not by coincidence. */
  uint64_t block_bus = nv_dma_bus(&r->block);
  uint64_t args_bus = nv_dma_bus(&r->args);
  uint64_t libos_bus = nv_dma_bus(&r->libos);
  uint64_t logs_bus = nv_dma_bus(&r->logs);
  if (block_bus == NV_DMA_BUS_INVALID || args_bus == NV_DMA_BUS_INVALID ||
      libos_bus == NV_DMA_BUS_INVALID || logs_bus == NV_DMA_BUS_INVALID) {
    uart_puts("[NV-ARG] stage=publish FAIL nv_dma_bus refused one of the four "
              "mappings: no bus address exists, so no page is written and no "
              "mailbox is programmed\n");
    return -ENOTSUP;
  }
  if (block_bus != r->q.bus) {
    /* The PTEs were built from q.bus.  Publishing a different number would
     * tell firmware to walk a table that is not this block's. */
    uart_puts("[NV-ARG] stage=publish FAIL block bus=");
    uart_print_hex(block_bus);
    uart_puts(" q.bus=");
    uart_print_hex(r->q.bus);
    uart_puts(": the published address is not the one the PTEs were built "
              "from\n");
    return -EIO;
  }

  /* ---- 1. GSP_ARGUMENTS_CACHED ---------------------------------------- */
  uint8_t *args = (uint8_t *)phys_to_virt(r->args.phys);
  int rc = nv_rpc_args_build(args, &g, block_bus);
  if (rc != 0) {
    uart_puts("[NV-ARG] stage=args FAIL nv_rpc_args_build rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }

  /* The row is built from the page's OWN bytes (nv_rpc_args_read) and from the
   * block's OWN PTE[0], not from the variables that wrote them: a writer that
   * stored the wrong thing still returns 0. */
  struct nv_rpc_rmargs a;
  if (nv_rpc_args_read(args, &a) != 0) return -EIO;
  const uint8_t *block = (const uint8_t *)phys_to_virt(r->block.phys);
  uint64_t pte0 = glue_ld64(block);
  int field = nv_rpc_args_crosscheck(&g, block_bus, &a);

  uart_puts("[NV-ARG] stage=args bus=");
  uart_print_hex(args_bus);
  uart_puts(" phys=");
  uart_print_hex(r->args.phys);
  uart_puts(" magic=");
  uart_print_hex(a.magic);
  uart_puts(" size=");
  uart_print_hex(a.size);
  uart_puts("\n");

  uart_puts("[NV-ARG] stage=args mq sharedMemPhysAddr=");
  uart_print_hex(a.shared_mem_phys);
  uart_puts(" pte0=");
  uart_print_hex(pte0);
  uart_puts(" equal=");
  print_int(a.shared_mem_phys == pte0 ? 1 : 0);
  uart_puts("\n");

  uart_puts("[NV-ARG] stage=args mq cmd=");
  uart_print_hex(a.cmd_queue_off);
  uart_puts(" stat=");
  uart_print_hex(a.stat_queue_off);
  uart_puts(" hdr=");
  uart_print_hex(a.elem_hdr_size);
  uart_puts(" min=");
  uart_print_hex(a.elem_size_min);
  uart_puts(" max=");
  uart_print_hex(a.elem_size_max);
  uart_puts(" halign=");
  uart_print_hex(a.header_align);
  uart_puts(" ealign=");
  uart_print_hex(a.elem_align);
  uart_puts(" pte_count=");
  print_int((int)a.pte_count);
  uart_puts("\n");

  uart_puts("[NV-ARG] stage=args crosscheck=");
  uart_puts(field == NV_RPC_ARG_OK
                ? "OK (the page in memory describes the block this boot "
                  "built)\n"
                : nv_rpc_arg_field_name((enum nv_rpc_arg_field)field));
  if (field != NV_RPC_ARG_OK) {
    uart_puts(" FAIL: the argument page and the transport disagree\n");
    return -EIO;
  }

  /* ---- 2. the libos-args table ---------------------------------------- */
  /* Every field is set: these are the four regions the reference driver
   * publishes, in the reference order (contract 1.6).  The three log regions
   * are real 4 KiB pages because the contract leaves "zero-sized log region"
   * NOT ESTABLISHED and three pages is cheap; RMARGS is the argument page,
   * and its size is the whole page so the firmware can read the 0xC8 struct
   * starting at pa without a size it would have to trust. */
  struct nv_rpc_libos_region e[NV_RPC_LIBOS_MAX];
  const char *ids[NV_RPC_LIBOS_MAX] = {"LOGINIT", "LOGINTR", "LOGRM",
                                       "RMARGS"};
  for (int i = 0; i < (int)NV_RPC_LIBOS_MAX; i++) {
    e[i].id = ids[i];
    e[i].id8 = 0; /* build() packs `id` */
    e[i].kind = NV_RPC_LIBOS_KIND_CONTIG;
    e[i].loc = NV_RPC_LIBOS_LOC_SYSMEM;
    if (i < (int)NV_RPC_LIBOS_LOG_PAGES) {
      e[i].pa = logs_bus + (uint64_t)i * NV_DMA_PAGE;
      e[i].size = NV_DMA_PAGE;
    } else {
      e[i].pa = args_bus;
      e[i].size = NV_RPC_GSP_ARGS_PAGE;
    }
  }

  uint8_t *libos = (uint8_t *)phys_to_virt(r->libos.phys);
  rc = nv_rpc_libos_build(libos, e, NV_RPC_LIBOS_MAX);
  if (rc != 0) {
    uart_puts("[NV-ARG] stage=libos FAIL nv_rpc_libos_build rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }

  uart_puts("[NV-ARG] stage=libos bus=");
  uart_print_hex(libos_bus);
  uart_puts(" phys=");
  uart_print_hex(r->libos.phys);
  uart_puts(" entries=");
  print_int((int)NV_RPC_LIBOS_MAX);
  uart_puts("\n");
  for (uint32_t i = 0; i < NV_RPC_LIBOS_MAX; i++) {
    struct nv_rpc_libos_region b;
    if (nv_rpc_libos_read(libos, i, &b) != 0) return -EIO;
    uart_puts("[NV-ARG] stage=libos[");
    print_int((int)i);
    uart_puts("] id=");
    nv_arg_put_id8(b.id8);
    uart_puts(" pa=");
    uart_print_hex(b.pa);
    uart_puts(" size=");
    uart_print_hex(b.size);
    uart_puts(" kind=");
    print_int((int)b.kind);
    uart_puts(" loc=");
    print_int((int)b.loc);
    uart_puts("\n");
  }

  /* The fourth entry is the whole point of the page: it must name the argument
   * page's bus address, read back out of the table. */
  struct nv_rpc_libos_region rargs;
  if (nv_rpc_libos_read(libos, NV_RPC_LIBOS_MAX - 1u, &rargs) != 0) return -EIO;
  if (rargs.id8 != nv_rpc_id8("RMARGS") || rargs.pa != args_bus) {
    uart_puts("[NV-ARG] stage=libos FAIL RMARGS id=");
    nv_arg_put_id8(rargs.id8);
    uart_puts(" pa=");
    uart_print_hex(rargs.pa);
    uart_puts(" expected pa=");
    uart_print_hex(args_bus);
    uart_puts("\n");
    return -EIO;
  }

  /* ---- 3. MAILBOX0/1 name the table ----------------------------------- *
   * ORDERING.  0x110040 is FWSEC's error channel earlier in the boot
   * (docs/nv-devinit-contract.md section 3): this publish must be the last
   * writer of it in a boot.  Stage 8 runs after every SEC2/FWSEC attempt and
   * nothing runs after it here; a later stage that needs FWSEC again must
   * re-zero the word first.
   *
   * The readback is logged and NOT gated on.  The contract's MEASURED rows show
   * 0x00000000 in both reference dumps (pre- and post-init), so "the firmware
   * took my arguments" cannot be read out of these two words.  What this card
   * measures instead: after the write they read back exactly what was written
   * (arga: 0x20956B000).  That says the MMIO write landed -- a state fact about
   * a card whose falcon page is not fenced -- and says nothing about firmware.
   * Nothing may gate on either reading. */
  nv_wr32(d, NV_GSP_MAILBOX0, (uint32_t)(libos_bus & 0xFFFFFFFFu));
  nv_wr32(d, NV_GSP_MAILBOX1, (uint32_t)(libos_bus >> 32));
  uint32_t mb0 = nv_rd32(d, NV_GSP_MAILBOX0);
  uint32_t mb1 = nv_rd32(d, NV_GSP_MAILBOX1);
  uart_puts("[NV-ARG] stage=mailbox0 bus=");
  uart_print_hex(libos_bus);
  uart_puts(" lo=");
  uart_print_hex((uint64_t)(libos_bus & 0xFFFFFFFFu));
  uart_puts(" hi=");
  uart_print_hex(libos_bus >> 32);
  uart_puts(" readback=");
  uart_print_hex(((uint64_t)mb1 << 32) | mb0);
  uart_puts(" (a state fact, not a firmware ack: nothing gates on it)\n");

  /* ---- 4. the status header, still the peer's -------------------------- *
   * Publishing must not counterfeit RM's first write.  Eight words at
   * block + stat_off are what nv_rpc_link_validate() reads, so those eight are
   * what this row prints -- from the block's bytes, after everything above. */
  uint32_t sw[8];
  int zeros = 0;
  for (int i = 0; i < 8; i++) {
    sw[i] = glue_ld32(block + g.stat_off + (uint32_t)i * 4u);
    if (sw[i] == 0) zeros++;
  }
  uart_puts("[NV-ARG] stage=status bus=");
  uart_print_hex(block_bus);
  uart_puts(" off=");
  uart_print_hex(g.stat_off);
  uart_puts(" zero=");
  print_int(zeros);
  uart_puts("/8");
  for (int i = 0; i < 8; i++) {
    uart_puts(" w");
    print_int(i);
    uart_puts("=");
    uart_print_hex(sw[i]);
  }
  uart_puts(zeros == 8 ? " (publishing did not counterfeit the peer's first "
                         "write)\n"
                       : " (NONZERO: the peer wrote it, or we did -- that "
                         "difference matters)\n");

  r->published = 1;
  return 0;
}

int nv_gsp_rpc_setup(struct nv_gsp_rpc *r, uint8_t *rx_stage) {
  if (!r || !rx_stage) return -EINVAL;

  r->block.phys = 0;
  r->block.bus = 0;
  r->block.len = 0;
  r->block.pages = 0;
  r->args.phys = 0;
  r->args.bus = 0;
  r->args.len = 0;
  r->args.pages = 0;
  r->libos.phys = 0;
  r->libos.bus = 0;
  r->libos.len = 0;
  r->libos.pages = 0;
  r->logs.phys = 0;
  r->logs.bus = 0;
  r->logs.len = 0;
  r->logs.pages = 0;
  r->sends = 0;
  r->doorbells = 0;
  r->built = 0;
  r->published = 0;
  r->rx_stage = rx_stage; /* before build: queue_init copies it */

  int rc = nv_dma_alloc(NV_RPC_BLOCK_SIZE, &r->block);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=block FAIL nv_dma_alloc rc=");
    print_int(rc);
    uart_puts(rc == -ENOMEM ? " (no contiguous 129-page run: fragmentation, not "
                              "a wrong size)\n"
                            : "\n");
    return rc;
  }

  rc = nv_gsp_rpc_build(r);
  if (rc != 0) {
    /* No half-built transport: a block we cannot describe is frames somebody
     * else needs. */
    nv_dma_free(&r->block);
    return rc;
  }

  /* The publish side has to exist before stage 8 can tell anyone about the
   * block.  Its failure is setup's failure for the same reason: a transport
   * nobody can be told about is 129 frames doing nothing. */
  rc = nv_gsp_args_alloc(r);
  if (rc != 0) {
    nv_dma_free(&r->block);
    r->built = 0;
    r->q.block = 0;
    r->q.bus = 0;
    return rc;
  }
  return 0;
}

void nv_gsp_rpc_teardown(struct nv_gsp_rpc *r) {
  if (!r) return;
  nv_dma_free(&r->block);
  nv_dma_free(&r->args);
  nv_dma_free(&r->libos);
  nv_dma_free(&r->logs);
  r->q.block = 0;
  r->q.bus = 0;
  r->q.linked = 0;
  r->q.swapped = 0;
  r->built = 0;
  r->published = 0;
}

int nv_gsp_rpc_notify(struct nv_gsp_rpc *r, const uint8_t *stage,
                      const struct nv_rpc_hw *hw) {
  if (!r || !r->built || !stage || !hw) return -EINVAL;

  /* The length lives in the element's own RPC header (slot + 0x38), because
   * the staged bytes are the message: an argument that disagreed with them
   * would describe a different message than the one committed. */
  uint32_t rpc_len = glue_ld32(stage + NV_RPC_R_LENGTH);
  enum nv_rpc_reason why = NV_RPC_OK;
  uint32_t ec = 0;
  int rc = nv_rpc_tx_prepare(&r->q, rpc_len, &ec, &why);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=send FAIL prepare rc=");
    print_int(rc);
    uart_puts(" reason=");
    uart_puts(nv_rpc_reason_name(why));
    uart_puts("\n");
    return rc;
  }
  /* S4..S8: copies the slots and advances writePtr.  This does NOT notify --
   * that separation is T-09's and it is why a commit that fails cannot look
   * like a delivery. */
  rc = nv_rpc_tx_commit(&r->q, stage, hw);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=send FAIL commit rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  r->sends++;

  /* S9/S10: the ONE notification.  fence, then one write of 0 to 0x110C00. */
  rc = nv_rpc_kick(&r->q, hw);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=send FAIL kick rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  r->doorbells++;
  return 0;
}

/* One card, one GSP: the driver is a singleton (nv_dev()), and the block must
 * outlive every call that builds it, so it lives here rather than on a stack. */
static struct nv_gsp_rpc g_gsp_rpc;
static uint8_t g_rpc_rx_stage[NV_RPC_SLOT * NV_RPC_ELEM_MAX];

const struct nv_gsp_rpc *nv_gsp_rpc_state(void) { return &g_gsp_rpc; }

/* Stage 8 of the boot: publish the block the RM task will walk.  Three rows,
 * one per stage, and the notification is gated on the peer's own header --
 * which RM writes when its task starts, so on a card where RM is not running
 * this stage cannot ring the doorbell and says so instead.
 *
 * The contract's two bounded waits (4.1 rows 7 and 9) are 4 s each.  That is
 * what both reference implementations use (NVIDIA/mq.c GspStatusQueueInit, and
 * the independent one's r535_gsp_init), and RM's slowest measured init is
 * 105.8 ms, so 4 s is "the documented patience", not a guess. */
#define NV_RPC_LINK_TIMEOUT_US 4000000u
#define NV_RPC_INIT_TIMEOUT_US 4000000u

/* THE CANARY, and why it exists.  The card's acceptance turns on one
 * difference: the peer's status-header write pointer moving, and not our own
 * command-queue one.  With no peer there is no send that could advance either,
 * so this probe manufactures the comparison without touching the peer's word:
 * it writes a canary into OUR producer word (block + cmd_off + 0x10), reads the
 * peer's word (block + stat_off + 0x10) while the canary is there, and restores
 * our word to the value it had.  A peer word that did not follow the canary is
 * a MEASURED fact that the two are different storage, which is the first half
 * of the discriminator; the second half (the peer's word moving) still needs
 * RM.  Nothing is written at block+stat_off, ever: a canary there would
 * counterfeit the very evidence the link stage exists to read.
 *
 * The restore is read back.  If it does not take, the block is unusable and the
 * stage fails rather than leaving a writePtr no send ever wrote. */
static int rpc_writepointer_canary(struct nv_gsp_rpc *r) {
  if (!r || !r->built || !r->q.block) return -EINVAL;
  uint8_t *block = r->q.block;
  uint8_t *own = block + r->q.cmd_off + NV_RPC_Q_WRITE_PTR;
  const uint8_t *peer = block + r->q.stat_off + NV_RPC_Q_WRITE_PTR;

  uint32_t before = glue_ld32(own);
  uint32_t peer_before = glue_ld32(peer);
  /* A valid ring index far from the current one: if this ever leaked, the value
   * would still be a pointer rather than a wild word. */
  uint32_t canary = (before + 47u) % NV_RPC_MSG_COUNT;
  glue_st32(own, canary);
  uint32_t peer_mid = glue_ld32(peer);
  glue_st32(own, before);
  uint32_t after = glue_ld32(own);

  uart_puts("[NV-RPC] stage=canary own_off=");
  uart_print_hex(r->q.cmd_off + NV_RPC_Q_WRITE_PTR);
  uart_puts(" before=");
  print_int((int)before);
  uart_puts(" wrote=");
  print_int((int)canary);
  uart_puts(" restored=");
  print_int((int)after);
  uart_puts(" peer_off=");
  uart_print_hex(r->q.stat_off + NV_RPC_Q_WRITE_PTR);
  uart_puts(" peer_before=");
  print_int((int)peer_before);
  uart_puts(" peer_during=");
  print_int((int)peer_mid);
  if (after != before) {
    uart_puts(" verdict=RESTORE-FAILED (our writePtr is not what we built; the "
              "block is not usable)\n");
    return -EIO;
  }
  uart_puts(peer_mid == peer_before && peer_before == 0
                ? " verdict=SEPARATE-WORDS (the peer's word did not follow a "
                  "store to ours, and is still the zero we built)\n"
                : " verdict=PEER-WORD-NONZERO (it was already written: read it "
                  "before calling it ours)\n");
  return 0;
}

static int rpc_stage8_common(struct nv_gsp_rpc *r, struct nv_dev *d,
                             uint32_t link_timeout_us) {
  if (!r || !d) return -EINVAL;

  /* Bring-up may call the firmware stage more than once (probe, then boot).
   * The block is a 129-page allocation that must outlive every stage, so it is
   * taken once and reused: a second setup would leak the first run. */
  int rc = r->built ? 0 : nv_gsp_rpc_setup(r, g_rpc_rx_stage);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=block result=");
    print_int(rc);
    uart_puts(" (no block, so no link and no send)\n");
    return rc;
  }

  struct nv_rpc_hw hw;
  nv_rpc_hw_dev(d, &hw);

  /* Publish BEFORE looking for the peer.  The order is the contract's: the
   * firmware is told where the block is before anybody checks whether it has
   * arrived, and the check itself (the status header) is untouched by the
   * publish -- so a zero header after this point still means "RM has not
   * started", not "we scribbled on it".  No peer is needed: this is image
   * construction plus two mailbox dwords.
   *
   * If the pre-boot step already published (it runs before the launch and
   * writes MAILBOX0/1), do not write those two words a second time after the
   * processor started: MAILBOX0 is also FWSEC's error channel and the publish
   * is supposed to be the last writer of it in a boot. */
  int prc = 0;
  if (!r->published) {
    prc = nv_gsp_rpc_publish(r, d);
    if (prc != 0) {
      uart_puts("[NV-ARG] stage=publish result=");
      print_int(prc);
      uart_puts(" (the firmware cannot be told where the block is)\n");
    }
  }

  enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
  enum nv_rpc_reason why = NV_RPC_OK;
  /* One read first: on a boot where the booter failed there can be no peer,
   * and burning the contract's 4 s before saying so would make every such boot
   * slower for nothing.  Only an absent header (which a booter that returned
   * success can still turn into a real one) is worth polling. */
  int lrc = nv_rpc_link_apply(&r->q, &bad);
  if (lrc == -EAGAIN && link_timeout_us) {
    lrc = nv_rpc_link(&r->q, &hw, link_timeout_us, &why);
    if (lrc == -EINVAL) {
      /* Name the field, so the terminal case is not reported as "field=ok". */
      enum nv_rpc_link_field b2 = NV_RPC_LINK_OK;
      (void)nv_rpc_link_apply(&r->q, &b2);
      bad = b2;
    }
  }

  uart_puts("[NV-RPC] stage=link ");
  if (lrc == 0) {
    uart_puts("linked=1 (the peer published a usable status header) wait_us=");
    uart_print_hex(link_timeout_us);
    uart_puts(" doorbells=");
    print_int((int)r->doorbells);
    uart_puts("\n");
  } else if (lrc == -EAGAIN || lrc == -ETIMEDOUT) {
    /* The honest reading of a zeroed status header: the RM task has not
     * started, so nothing is listening and the doorbell is withheld rather than
     * written into a window a reset just fenced. */
    uart_puts("linked=0 peer=none (status header still zero: RM has not "
              "started; doorbell withheld, doorbells=");
    print_int((int)r->doorbells);
    uart_puts(" wait_us=");
    uart_print_hex(link_timeout_us);
    if (lrc == -ETIMEDOUT) {
      uart_puts(" reason=");
      uart_puts(nv_rpc_reason_name(why));
    }
    uart_puts(")\n");
  } else {
    uart_puts("linked=0 field=");
    uart_puts(nv_rpc_link_field_name(bad));
    uart_puts(" (header written but unusable; waiting will not fix it)\n");
  }
  nv_rpc_dump("link", &r->q);

  /* With no peer, the one comparison a card can still make: our producer word
   * and the peer's are different storage.  On a linked queue it is not needed
   * and is skipped (a canary in a live queue is a store RM could read), and it
   * is skipped whenever the caller said a peer is possible (link_timeout_us
   * != 0), so the probe can never race a peer that is starting. */
  if (lrc != 0 && prc == 0 && link_timeout_us == 0) {
    int crc = rpc_writepointer_canary(r);
    if (crc != 0) return crc;
  }

  /* A publish that failed is the more fundamental error: the firmware was
   * never told where the block is, so the link's -EAGAIN cannot become 0 on
   * this boot however long anyone waits. */
  return prc != 0 ? prc : lrc;
}

int nv_gsp_rpc_stage8_into(struct nv_gsp_rpc *r, struct nv_dev *d) {
  return rpc_stage8_common(r, d, 0);
}

int nv_gsp_rpc_stage8_wait(struct nv_dev *d, uint32_t link_timeout_us) {
  return rpc_stage8_common(&g_gsp_rpc, d, link_timeout_us);
}

int nv_gsp_rpc_stage8(struct nv_dev *d) { return rpc_stage8_common(&g_gsp_rpc, d, 0); }

/* Stage 9: the first round trip.  Nothing here runs unless stage 8 linked the
 * status queue, because every send goes through nv_rpc_roundtrip(), whose first
 * act is the same gate.  The two waits are the contract's own (4.1 rows 9 and
 * 10) and each failure names itself in one row. */
int nv_gsp_rpc_stage9_into(struct nv_gsp_rpc *r, struct nv_dev *d) {
  if (!r || !d) return -EINVAL;
  if (!r->q.linked) {
    uart_puts("[NV-RPC] stage=init skipped: linked=0 (no peer header to answer, "
              "so nothing was sent and no doorbell was rung; doorbells=");
    print_int((int)r->doorbells);
    uart_puts(")\n");
    return -EAGAIN;
  }

  struct nv_rpc_hw hw;
  nv_rpc_hw_dev(d, &hw);
  /* 0x1000 is the queue's element size: enough for INIT_DONE's header and for
   * the 0xD10-byte static-info reply, and small enough to be one frame's
   * worth of scratch. */
  static uint8_t msg[NV_RPC_SLOT];
  static uint8_t req[NV_RPC_SLOT];

  uint32_t result = NV_RPC_PENDING;
  enum nv_rpc_reason why = NV_RPC_OK;

  int n = nv_rpc_rx_wait(&r->q, &hw, NV_RPC_FN_INIT_DONE,
                         NV_RPC_INIT_TIMEOUT_US, msg, sizeof msg, &result, &why);
  uart_puts("[NV-RPC] stage=init_done fn=0x1001");
  if (n <= 0) {
    uart_puts(" FAIL rc=");
    print_int(n);
    uart_puts(" reason=");
    uart_puts(nv_rpc_reason_name(why));
    uart_puts(" wait_us=");
    uart_print_hex(NV_RPC_INIT_TIMEOUT_US);
    uart_puts(" (linked, but the init event never arrived: the link is up and "
              "the event path is not)\n");
    return n;
  }
  uart_puts(" len=");
  uart_print_hex((uint64_t)n);
  uart_puts(" result=");
  uart_print_hex(result);
  uart_puts(result == 0
                ? " (RM reports initialised)\n"
                : " (RM's own init verdict is nonzero: read it, do not retry)\n");
  if (result != 0) return -EIO;

  int ec = nv_rpc_static_info_request(req, r->q.tx_seq);
  if (ec <= 0) {
    uart_puts("[NV-RPC] stage=static_info FAIL build rc=");
    print_int(ec);
    uart_puts(" (the request did not frame; nothing was sent)\n");
    return ec ? ec : -EIO;
  }
  uart_puts("[NV-RPC] stage=static_info req fn=65 len=");
  uart_print_hex(NV_RPC_STATIC_INFO_LEN);
  uart_puts(" ec=");
  print_int(ec);
  uart_puts(" wp=");
  print_int((int)(glue_ld32(r->q.block + r->q.cmd_off + NV_RPC_Q_WRITE_PTR)));
  uart_puts(" doorbells_before=");
  print_int((int)r->doorbells);
  uart_puts("\n");

  n = nv_rpc_roundtrip(&r->q, &hw, req, NV_RPC_FN_GET_STATIC_INFO,
                       NV_RPC_INIT_TIMEOUT_US, msg, sizeof msg, &result, &why);
  nv_rpc_dump("send", &r->q);
  if (n <= 0) {
    uart_puts("[NV-RPC] stage=reply FAIL rc=");
    print_int(n);
    uart_puts(" reason=");
    uart_puts(nv_rpc_reason_name(why));
    uart_puts(" doorbells=");
    print_int((int)r->doorbells);
    uart_puts(" (the request was sent; no reply with fn=65 arrived inside the "
              "contract's wait -- do NOT resend, the peer may have consumed "
              "it)\n");
    return n;
  }

  /* The four payload words, RAW: the contract names hInternalClient/Device/
   * Subdevice as the proof, but it does not give their offsets inside
   * GspStaticConfigInfo, and a row that guessed them would be a claim.  The
   * shape is logged from the RPC header (function, length, result), which is
   * what "answered" means here. */
  uart_puts("[NV-RPC] stage=reply fn=65 len=");
  uart_print_hex((uint64_t)n);
  uart_puts(" result=");
  uart_print_hex(result);
  for (int i = 0; i < 4; i++) {
    uart_puts(" w");
    print_int(i);
    uart_puts("=");
    uart_print_hex(glue_ld32(msg + NV_RPC_H_PAYLOAD + (uint32_t)i * 4u));
  }
  uart_puts(result == 0
                ? " (one message answered: the first round trip)\n"
                : " (answered with a nonzero result: contract 3.3 names it)\n");
  return result == 0 ? 0 : -EIO;
}

int nv_gsp_rpc_stage9(struct nv_dev *d) {
  return nv_gsp_rpc_stage9_into(&g_gsp_rpc, d);
}

/* ============================================================================
 *  The pre-boot step: stage 72 and 73 BEFORE the processor starts
 *  (docs/nv-gsp-rpc-contract.md 4.1 rows 3-5)
 *
 *  WHAT WAS MISSING.  The transport could build, publish, link and send, but
 *  nothing ever put a message in the queue before the launch: RM would start
 *  with an empty command queue and no reason to init.  Both messages are
 *  "stuffed into the message queue before the GSP is booted" (the vendor
 *  comment the contract quotes), so they are written here, while the processor
 *  is stopped, and announced with ONE doorbell.
 *
 *  NEITHER IS WAITED ON.  There is no processor to answer; a reply would
 *  arrive later, interleaved with GSP_INIT_DONE and other events (contract
 *  3.5).  This function therefore has no clock and no poll.
 *
 *  THE ADDRESS RULE, again: nothing here spells phys.  The block's bus address
 *  came from nv_dma_bus() in nv_gsp_rpc_build(), and the only device-facing
 *  stores in this function are the one doorbell and (through the publish) the
 *  two mailbox words.
 * ========================================================================== */

/* One message's staging area, reused: each message is committed to the ring
 * before the next is built, so nothing outlives its commit.  The ceiling is
 * the transport's (16 slots), not this card's (both messages are one slot). */
static uint8_t g_pre_stage[NV_RPC_SLOT * NV_RPC_ELEM_MAX];

/* Compact hex, the way the rows in this file read: 0x2E0, not
 * 0x00000000000002E0. */
static void nv_pre_hex(uint32_t v) {
  static const char *d = "0123456789ABCDEF";
  char s[12];
  int n = 0, started = 0;
  s[n++] = '0';
  s[n++] = 'x';
  for (int sh = 28; sh >= 0; sh -= 4) {
    uint32_t nib = (v >> sh) & 0xFu;
    if (nib || started || sh == 0) {
      s[n++] = d[nib];
      started = 1;
    }
  }
  s[n] = 0;
  uart_puts(s);
}

/* Slot `idx` of the command ring, addressed the way the header says (entryOff,
 * then idx * msgSize) rather than by a literal 0x1000. */
static const uint8_t *nv_pre_slot(const struct nv_rpc_queue *q, uint32_t idx) {
  const uint8_t *cmdq = q->block + q->cmd_off;
  uint32_t entry_off = glue_ld32(cmdq + NV_RPC_Q_ENTRY_OFF);
  return cmdq + entry_off + (uint64_t)idx * NV_RPC_SLOT;
}

/* The whole-element fold over what is IN THE RING, across a wrap.  After
 * nv_rpc_elem_build() stores the fold, the accumulator's two halves are equal;
 * that is the "re-folds to 0" property, checked on the bytes the firmware will
 * hash rather than on the staging copy. */
static int nv_pre_fold_ok(const struct nv_rpc_queue *q, uint32_t idx,
                          uint32_t ec) {
  uint64_t a = 0;
  for (uint32_t k = 0; k < ec; k++) {
    uint32_t i = (idx + k) % q->msg_count;
    a ^= nv_rpc_cksum_acc(nv_pre_slot(q, i), NV_RPC_SLOT);
  }
  return (uint32_t)a == (uint32_t)(a >> 32);
}

/* One row per staged message, every field read back from the ring:
 *   [NV-PRE] stage=system_info fn=72 len=0x2E0 ec=1 wp_after=1 sum=OK */
static void nv_pre_row(const char *what, uint32_t fn,
                       const struct nv_rpc_queue *q, uint32_t idx) {
  const uint8_t *slot = nv_pre_slot(q, idx);
  uint32_t ec = glue_ld32(slot + NV_RPC_E_ELEMCOUNT);
  uint32_t wp = glue_ld32(q->block + q->cmd_off + NV_RPC_Q_WRITE_PTR);

  uart_puts("[NV-PRE] stage=");
  uart_puts(what);
  uart_puts(" fn=");
  print_int((int)fn);
  uart_puts(" len=");
  nv_pre_hex(glue_ld32(slot + NV_RPC_R_LENGTH));
  uart_puts(" ec=");
  print_int((int)ec);
  uart_puts(" wp_after=");
  print_int((int)wp);
  uart_puts(" sum=");
  uart_puts(nv_pre_fold_ok(q, idx, ec) ? "OK" : "BAD");
  uart_puts("\n");
}

int nv_gsp_rpc_preboot(struct nv_gsp_rpc *r, struct nv_dev *d) {
  if (!r || !d) return -EINVAL;

  if (r->preboot) {
    /* Staging twice would queue 72/73 again behind whatever RM has already
     * consumed, and ring a second doorbell for messages the peer did not ask
     * for.  Say so and change nothing. */
    uart_puts("[NV-PRE] stage=preboot already staged (wr=");
    print_int((int)glue_ld32(r->q.block + r->q.cmd_off + NV_RPC_Q_WRITE_PTR));
    uart_puts(" doorbells=");
    print_int((int)r->doorbells);
    uart_puts("): nothing queued, no doorbell\n");
    return 0;
  }

  int rc = r->built ? 0 : nv_gsp_rpc_setup(r, g_rpc_rx_stage);
  if (rc != 0) {
    uart_puts("[NV-PRE] stage=preboot FAIL no transport rc=");
    print_int(rc);
    uart_puts(" (nothing is staged: a queue nobody could build is not a queue)\n");
    return rc;
  }
  /* Publish BEFORE the first message: RM is told where the block is through
   * the argument page and the mailboxes (contract 1.5/1.6), and it reads the
   * command queue through that page.  A message in a block RM cannot find is
   * a message nobody consumes. */
  if (!r->published) {
    rc = nv_gsp_rpc_publish(r, d);
    if (rc != 0) {
      uart_puts("[NV-PRE] stage=preboot FAIL publish rc=");
      print_int(rc);
      uart_puts(" (no message is staged)\n");
      return rc;
    }
  }

  struct nv_rpc_hw hw;
  nv_rpc_hw_dev(d, &hw);

  /* ---- the payload, and what it does NOT claim ------------------------ */
  /* Every value below is either measured on this card or honest about being
   * unknown; none of them is a number picked to look plausible. */
  struct nv_rpc_system_info si;
  si.gpu_phys_addr = d->bar0_phys;      /* BAR0 base, 0x80000000 on this card */
  si.gpu_phys_fb_addr = d->bar1_phys;   /* BAR1 aperture base (above 4 GiB)   */
  si.gpu_phys_inst_addr = 0;
  si.nv_domain_bus_device_func =
      ((uint64_t)d->bus << 8) | ((uint64_t)d->slot << 3) | (uint64_t)d->func;
  si.max_user_va = 0;
  si.sys_timer_offset_ns = 0;           /* 0 is safe: timestamped logs only   */
  si.pci_config_mirror_base = 0;
  si.pci_config_mirror_size = 0;
  si.chipset = 0;                       /* tolerated; the vendor never sets it */

  uart_puts("[NV-PRE] si gpuPhysAddr=");
  uart_print_hex(si.gpu_phys_addr);
  uart_puts(" gpuPhysFbAddr=");
  uart_print_hex(si.gpu_phys_fb_addr);
  uart_puts(" nvDomainBusDeviceFunc=");
  uart_print_hex(si.nv_domain_bus_device_func);
  uart_puts(" Chipset=");
  print_int((int)si.chipset);
  uart_puts("\n");
  uart_puts("[NV-PRE] si gpuPhysInstAddr=0 NOT ESTABLISHED (no BAR2 on this "
            "card; BAR3 is 32 MiB and was not tried)\n");
  uart_puts("[NV-PRE] si maxUserVa=0 NOT ESTABLISHED (non-Linux host; the "
            "vendor driver always sets it)\n");
  uart_puts("[NV-PRE] si pciConfigMirrorBase=0 size=0 NOT ESTABLISHED (the pci "
            "layer exposes no MMCONFIG physical base)\n");

  /* Sequence numbers are per MESSAGE (contract 2.4); the doorbell is per
   * notification.  Two messages therefore consume two numbers and one kick. */
  uint32_t seq = r->q.tx_seq;

  /* ---- row 3: GSP_SET_SYSTEM_INFO (72) -------------------------------- */
  int ec = nv_rpc_system_info_build(g_pre_stage, &si, seq);
  if (ec < 0) {
    uart_puts("[NV-PRE] stage=system_info FAIL build rc=");
    print_int(ec);
    uart_puts("\n");
    return ec;
  }
  uint32_t len = glue_ld32(g_pre_stage + NV_RPC_R_LENGTH);
  enum nv_rpc_reason why = NV_RPC_OK;
  uint32_t ec_tx = 0;
  rc = nv_rpc_tx_prepare(&r->q, len, &ec_tx, &why);
  if (rc != 0) {
    uart_puts("[NV-PRE] stage=system_info FAIL prepare rc=");
    print_int(rc);
    uart_puts(" reason=");
    uart_puts(nv_rpc_reason_name(why));
    uart_puts("\n");
    return rc;
  }
  rc = nv_rpc_tx_commit(&r->q, g_pre_stage, &hw);
  if (rc != 0) {
    uart_puts("[NV-PRE] stage=system_info FAIL commit rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  r->sends++;
  uint32_t wp72 = glue_ld32(r->q.block + r->q.cmd_off + NV_RPC_Q_WRITE_PTR);
  nv_pre_row("system_info", NV_RPC_FN_SET_SYSTEM_INFO, &r->q, wp72 - ec_tx);
  /* The payload is one slot, so the ring copy has the same layout as the
   * staging copy and the checker can read it in place -- which is the point:
   * it checks what the firmware will read. */
  enum nv_rpc_si_field bad = nv_rpc_system_info_check(
      nv_pre_slot(&r->q, wp72 - ec_tx), &si);
  if (bad != NV_RPC_SI_F_OK) {
    uart_puts("[NV-PRE] stage=system_info FAIL verify=");
    uart_puts(nv_rpc_si_field_name(bad));
    uart_puts(" (a field is not where the contract's table puts it)\n");
    return -EIO;
  }

  /* ---- row 4: SET_REGISTRY (73) --------------------------------------- */
  /* WHICH KEYS ARE MANDATORY IS NOT ESTABLISHED (contract 5.2): neither the
   * vendor driver's list nor the independent implementation's documents a
   * minimum.  An empty table is well-formed, so that is what is sent -- and
   * the gap is named in the row rather than filled with invented keys, whose
   * only symptom would be a non-zero GSP_INIT_DONE result later. */
  ec = nv_rpc_registry_build(g_pre_stage, 0, 0, seq + 1u);
  if (ec < 0) {
    uart_puts("[NV-PRE] stage=registry FAIL build rc=");
    print_int(ec);
    uart_puts("\n");
    return ec;
  }
  len = glue_ld32(g_pre_stage + NV_RPC_R_LENGTH);
  rc = nv_rpc_tx_prepare(&r->q, len, &ec_tx, &why);
  if (rc != 0) {
    uart_puts("[NV-PRE] stage=registry FAIL prepare rc=");
    print_int(rc);
    uart_puts(" reason=");
    uart_puts(nv_rpc_reason_name(why));
    uart_puts("\n");
    return rc;
  }
  rc = nv_rpc_tx_commit(&r->q, g_pre_stage, &hw);
  if (rc != 0) {
    uart_puts("[NV-PRE] stage=registry FAIL commit rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  r->sends++;
  uint32_t wp73 = glue_ld32(r->q.block + r->q.cmd_off + NV_RPC_Q_WRITE_PTR);
  nv_pre_row("registry", NV_RPC_FN_SET_REGISTRY, &r->q, wp73 - ec_tx);

  /* The table's own two words, read out of the ring: a `size` that disagrees
   * with what was measured makes every offset in it a guess. */
  const uint8_t *slot73 = nv_pre_slot(&r->q, wp73 - ec_tx);
  uint32_t tbl = glue_ld32(slot73 + NV_RPC_R_PAYLOAD + NV_RPC_REG_O_SIZE);
  uint32_t num = glue_ld32(slot73 + NV_RPC_R_PAYLOAD + NV_RPC_REG_O_NUM);
  int tbl_ok = tbl == (uint32_t)nv_rpc_registry_table_size(0, 0) && num == 0;
  uart_puts("[NV-PRE] stage=registry keys=");
  print_int((int)num);
  uart_puts(" table=");
  nv_pre_hex(tbl);
  uart_puts(tbl_ok ? " (NOT ESTABLISHED which keys are mandatory: an empty "
                     "table is well-formed but may leave RM unbootable)\n"
                   : " FAIL table header does not match the packed table\n");
  if (!tbl_ok) return -EIO;

  /* ---- row 5: the one doorbell ---------------------------------------- */
  rc = nv_rpc_kick(&r->q, &hw);
  if (rc != 0) {
    uart_puts("[NV-PRE] stage=doorbell FAIL kick rc=");
    print_int(rc);
    uart_puts(" (the messages are in the ring and nobody was told)\n");
    return rc;
  }
  r->doorbells++;
  /* kick() advanced tx_seq by one notification; two MESSAGES were queued, and
   * the next send's seqNum has to follow the ring, not the doorbell. */
  r->q.tx_seq = seq + 2u;

  /* THE DISCRIMINATOR.  wr is our producer word (cmdq + 0x10); pw is the
   * peer's (statq + 0x10).  wr=2 with pw=0 says two messages are queued and
   * no processor has started -- which is exactly the state a pre-boot step is
   * supposed to leave behind.  The status header is never written by us. */
  uart_puts("[NV-PRE] stage=doorbell wr=");
  print_int((int)glue_ld32(r->q.block + r->q.cmd_off + NV_RPC_Q_WRITE_PTR));
  uart_puts(" pw=");
  print_int((int)glue_ld32(r->q.block + r->q.stat_off + NV_RPC_Q_WRITE_PTR));
  uart_puts(" doorbells=");
  print_int((int)r->doorbells);
  uart_puts("\n");

  r->preboot = 1;
  return 0;
}

/* PFB: the write-protected-region block.  MEASURED (docs/nv-gsp-reference.md)
 * and RE-DERIVED from sources (docs/nv-gsp-wpr-contract.md section 2.1):
 *
 *   0x1FA824 / 0x1FA828 are WPR2_ADDR_LO / WPR2_ADDR_HI, and they are a
 *   (start, END) pair in **4 KiB units** -- not low/high halves of an address,
 *   and not base/size.  The region's size is (HI - LO) * 4 KiB, if it were
 *   readable.
 *
 *   On this card they are NOT readable: 0x1FA7C8 bit 0 (READ_PROTECTION_LEVEL0)
 *   reads 1, and the words decode to 0x1FFFFE00/0 -- a start of 128 GiB minus
 *   2 MiB on a 24 GiB card.  A live 24 GiB card cannot be that, so the values
 *   are the protected-window answer, not state.  The reference driver running
 *   GSP-RM and rendering Vulkan reads the same words.
 *
 *   Therefore: these are a NAME for WPR2 and a NOT a state oracle, and the host
 *   must never write them -- on AD102/535 the SEC2 booter programs the region
 *   from the metadata block. */
#define NV_PFB_WPR2_ADDR_LO 0x1FA824
#define NV_PFB_WPR2_ADDR_HI 0x1FA828
#define NV_PFB_READ_PROTECTION 0x1FA7C8
#define NV_PFB_RP_LEVEL0 (1u << 0)
/* The pair that DOES move across init, identity unexplained (contract 2.4c). */
#define NV_PFB_MOVES_A 0x1FA804
#define NV_PFB_MOVES_B 0x1FA808
#define NV_PFB_MOVES_C 0x1FA82C
#define NV_PFB_MOVES_D 0x1FA830

/* GSP-specific words inside the falcon window (BAR0 0x110000 + offset).
 * NV_GSP_MAILBOX0/1 are defined beside nv_gsp_rpc_publish(), their first
 * user, so the publish does not depend on this block's position. */
#define NV_GSP_ENGINE_RESET (NV_FALCON_GSP_BASE + 0x3C0)
/* Was written down as a BAR0 word at 0x001668 -- wrong: 0x001668 reads the
 * unreadable-register answer in every state.  The live word is falcon-relative,
 * 0x111668 = BCR_CTRL, and 0x111 = VALID | RISCV | BRFETCH, i.e. "run the
 * RISC-V core, fetch from the boot ROM".  Measured 0x1 post-init, 0xBADF5620
 * (fenced) before it. */
#define NV_GSP_BCR_CTRL (NV_FALCON_GSP_BASE + 0x1668)
#define NV_GSP_BCR_VALID (1u << 0)
#define NV_GSP_BCR_RISCV (1u << 4)
#define NV_GSP_BCR_BRFETCH (1u << 8)
/* RISC-V core window inside the GSP block. */
#define NV_GSP_RISCV_BASE (NV_FALCON_GSP_BASE + 0x1000)
#define NV_GSP_RISCV_CPUCTL (NV_GSP_RISCV_BASE + 0x388)
/* The queue doorbell used to be defined here as NV_GSP_QUEUE_HEAD0 and was
 * never written: notification is nv_rpc_kick()'s job, and the offset has one
 * definition (NV_RPC_DOORBELL_OFF in nv_rpc.h).  Keeping a second copy in the
 * boot path is how a driver ends up with two doorbell writes and no way to say
 * which one the firmware saw. */

/* Stage numbers, mirrored into nv.gsp_stage so the ABI and the log agree. */
#define NV_GSP_STAGE_NONE 0
#define NV_GSP_STAGE_PROBED 1
#define NV_GSP_STAGE_HALTED 2
#define NV_GSP_STAGE_FW_LOADED 3
#define NV_GSP_STAGE_WPR_META 4
#define NV_GSP_STAGE_IMAGE_PLACED 5
#define NV_GSP_STAGE_BOOTER_LOADED 6
#define NV_GSP_STAGE_STARTED 7
#define NV_GSP_STAGE_RPC 8

/* Observation only: what the card says about its own firmware state.  Every
 * field here is a read; calling this twice in a row is harmless. */
int nv_gsp_probe(struct nv_dev *d) {
  if (!d) return -EINVAL;
  if (!d->falcon[NV_FALCON_IDX_GSP].present) {
    uart_puts("[NV-GSP] no gsp window: firmware stage not reachable\n");
    return -ENODEV;
  }
  uint32_t lo = nv_rd32(d, NV_PFB_WPR2_ADDR_LO);
  uint32_t hi = nv_rd32(d, NV_PFB_WPR2_ADDR_HI);
  uint32_t rp = nv_rd32(d, NV_PFB_READ_PROTECTION);
  uint32_t mv_a = nv_rd32(d, NV_PFB_MOVES_A);
  uint32_t mv_b = nv_rd32(d, NV_PFB_MOVES_B);
  uint32_t bcr = nv_rd32(d, NV_GSP_BCR_CTRL);
  uint32_t mb0 = nv_rd32(d, NV_GSP_MAILBOX0);
  uint32_t mb1 = nv_rd32(d, NV_GSP_MAILBOX1);
  uint32_t ucctl = nv_rd32(d, NV_GSP_RISCV_CPUCTL);
  uint32_t eng_reset = nv_rd32(d, NV_GSP_ENGINE_RESET);

  uart_puts("[NV-GSP] probe wpr2 lo=");
  uart_print_hex(lo);
  uart_puts(" hi=");
  uart_print_hex(hi);
  uart_puts(" readprot=");
  uart_print_hex(rp);
  uart_puts(" (LO/HI are a start/END pair in 4 KiB units)\n");

  /* Say what the words mean and what they cannot mean.  On this card they are
   * protected, and the decoded start lands outside a 24 GiB FB -- so the
   * honest report is "these are not state", not a verdict about the firmware. */
  uint64_t start_kib = (uint64_t)lo * 4u;
  uint64_t end_kib = (uint64_t)hi * 4u;
  uart_puts("[NV-GSP] probe wpr2 span_kib=");
  uart_print_hex(end_kib > start_kib ? end_kib - start_kib : 0);
  if ((rp & NV_PFB_RP_LEVEL0) && hi != 0) {
    uart_puts(" verdict=not-a-state-oracle (read-protected; decode would put the "
              "region above any real FB). The SEC2 booter builds WPR2; the host "
              "only supplies the metadata.\n");
  } else if (hi == 0) {
    uart_puts(" verdict=hi-is-zero (region not established); still: the booter "
              "builds it, the host never writes these.\n");
  } else {
    uart_puts(" verdict=span decodes inside FB, but the meaning of the pair is "
              "not established -- do not write these.\n");
  }

  uart_puts("[NV-GSP] probe moves a=");
  uart_print_hex(mv_a);
  uart_puts(" b=");
  uart_print_hex(mv_b);
  uart_puts(" (this pair changes across init; identity unexplained)\n");
  uart_puts("[NV-GSP] probe bcr_ctrl=");
  uart_print_hex(bcr);
  if (bcr == NV_REG_FENCED) {
    uart_puts(" (fenced)");
  } else {
    uart_puts((bcr & NV_GSP_BCR_VALID) ? " VALID" : " !valid");
    if (bcr & NV_GSP_BCR_RISCV) uart_puts("|RISCV");
    if (bcr & NV_GSP_BCR_BRFETCH) uart_puts("|BRFETCH");
  }
  uart_puts("\n[NV-GSP] probe mb0=");
  uart_print_hex(mb0);
  uart_puts(" mb1=");
  uart_print_hex(mb1);
  uart_puts(" riscv_cpuctl=");
  uart_print_hex(ucctl);
  uart_puts(" eng_reset=");
  uart_print_hex(eng_reset);
  uart_puts("\n");

  d->wpr2_lo = lo;
  d->wpr2_hi = hi;
  d->wpr_ctl_a = mv_a;
  d->wpr_ctl_b = mv_b;
  d->gsp_msg0 = mb0;
  d->gsp_msg1 = mb1;
  d->gsp_ucctl = ucctl;
  d->gsp_stage = NV_GSP_STAGE_PROBED;
  return 0;
}

/* Stage 2, the part that needs no register contract at all: read the firmware
 * from disk, parse it, and hash it.  This is pure file I/O -- the one stage in
 * the chain that cannot wedge the card, so it runs on every boot and its result
 * is checkable against a pinned digest.
 *
 * Names are 8.3 on purpose: the image is FAT16 and the native path resolver does
 * not read long-name entries, so deploy.sh stages the canonical NVIDIA filenames
 * under role names (BOOTER.BIN = booter_load, LOADER.BIN = bootloader,
 * GSPRM.BIN = the RM image). */
#define NV_FW_SAMPLE_BYTES (64u * 1024u)
/* Usable FB on this card, from the reference driver's own report (nvidia-smi
 * total 24564 MiB).  Reading it from the card's strap instead is a TODO: the
 * value is only a layout input, and a wrong one fails at the booter rather than
 * corrupting anything, so it is logged next to every computed address. */
#define NV_AD102_FB_SIZE 0x5FF400000ULL
#define NV_FW_BOOTER "BOOTER.BIN"
#define NV_FW_LOADER "LOADER.BIN"
#define NV_FW_RM "GSPRM.BIN"

static int nv_gsp_load_one(struct nv_dev *d, const char *name, int parse_hs,
                           uint32_t *out_size, uint8_t out_sha[8]) {
  struct nv_fw_blob b;
  int rc = nv_fw_load_blob(name, &b);
  uart_puts("[NV-GSP] fw ");
  uart_puts(name);
  if (rc != 0) {
    uart_puts(" load FAILED rc=");
    print_int(rc);
    uart_puts(rc == -ENOENT ? " (not on the image)\n"
                            : (rc == -ENOMEM ? " (frame pool too fragmented)\n"
                                             : "\n"));
    return rc;
  }
  *out_size = b.size;
  uart_puts(" bytes=");
  print_int((int)b.size); /* 38 MB fits an int; sizes are well under 2 GiB */
  uart_puts(" phys=");
  uart_print_hex(b.phys);

  if (parse_hs) {
    struct nv_fw_container c;
    struct nv_fw_hs hs;
    if ((rc = nv_fw_container_parse(b.buf, b.size, &c)) != 0) {
      uart_puts(" container FAILED rc=");
      print_int(rc);
      uart_puts("\n");
    } else if ((rc = nv_fw_hs_parse(&c, &hs)) != 0) {
      uart_puts(" hs FAILED rc=");
      print_int(rc);
      uart_puts("\n");
    } else {
      uart_puts(" container ok data_off=");
      uart_print_hex(c.data_off);
      uart_puts(" data_size=");
      uart_print_hex(c.data_size);
      uart_puts(" sig=");
      uart_print_hex(hs.sig_prod_size);
      uart_puts(" trailer=");
      uart_print_hex(c.trailing);
    }
  }
  /* Integrity, sized to the failure that actually happens.  A short read is
   * caught by head+tail hashes and the byte count; a full hash of the 38 MB RM
   * image costs 108 s of emulated CPU (measured: hash_us=108600896), which is
   * not a tax every boot should pay for a check that cannot fail differently
   * from the sampled form. */
  uint8_t sha[32];
  uint32_t t0 = nv_rd32(d, NV_PTIMER_TIME_0);
  const uint32_t kSample = 64 * 1024;
  uint32_t n = NV_FW_SAMPLE_BYTES; /* 64 KiB, or the whole file if smaller */
  n = b.size < n ? b.size : n;
  if (nv_fw_sha256(b.buf, n, sha) == 0) {
    uart_puts(" head=");
    for (uint32_t i = 0; i < 4; i++) {
      nv_hex_byte(sha[i]);
      out_sha[i] = sha[i];
    }
    if (b.size > kSample) {
      /* Tail: the other end of a truncated file. */
      if (nv_fw_sha256(b.buf + b.size - kSample, kSample, sha) == 0) {
        uart_puts(" tail=");
        for (uint32_t i = 0; i < 4; i++) {
          nv_hex_byte(sha[i]);
          out_sha[4 + i] = sha[i];
        }
      }
    }
    uint32_t t1 = nv_rd32(d, NV_PTIMER_TIME_0);
    uart_puts(" hash_us=");
    /* PTIMER runs at ~1 GHz, so a raw tick delta is microseconds: a coarse but
     * honest cost figure for hashing on this CPU. */
    print_int((int)(t1 - t0));
  }
  uart_puts("\n");
  nv_fw_release(&b);
  return 0;
}

/* ---- stage 5: hand the booter to SEC2 ----------------------------------
 *
 * SEC2, not the GSP, runs the booter: it builds WPR2 from the metadata block and
 * then verifies and starts the RM image.  Register offsets here are SEC2-window
 * relative (docs/nv-gsp-wpr-contract.md 4.2).
 *
 * The field positions are from the published falcon register definition, NOT
 * guessed: DMATRFCMD has SEC at bits 3:2, IMEM at 4, WRITE at 5, SIZE at 10:8,
 * CTXDMA at 14:12.  Our older encoder put a "port" at 12..14 and never set SEC,
 * which is a silent way to load a booter that will not authenticate. */
#define NV_SEC2_FBIF_TRANSCFG 0x840600 /* SEC2 window + 0x600, TRANSCFG[0] */
#define NV_LOCAL_TRANSCFG_COHERENT 0x5 /* TARGET=COHERENT_SYSMEM | MEM_TYPE=PHYSICAL */
#define NV_XFER_EXT_BASE 0x110
#define NV_XFER_EXT_BASE1 0x128
#define NV_XFER_FALCON_ADDR 0x114
#define NV_XFER_EXT_ADDR 0x11C
#define NV_XFER_CTRL 0x118
#define NV_XFER_STATUS 0x120
#define XF_IMEM (1u << 4)
#define XF_SEC(n) (((n) & 0x3) << 2)
#define XF_WRITE (1u << 5)
#define XF_SIZE(log2) (((log2) & 0x7) << 8)
#define XF_FULL (1u << 0) /* 0x118: the previous request is still queued */
#define XF_DONE (1u << 1) /* 0x118: the engine collected the request */
#define XF_STORE (1u << 5) /* 0x118: DIR=1 -- falcon memory -> system memory */
/* Should the engine's STORE direction be used to read IMEM back?
 *
 * MEASURED r2c: NOT YET.  A store request encoded the way the load is (same
 * registers, DIR bit set) leaves 0x118 bit 0 (FULL) stuck set, and the next DMA
 * request -- the FWSEC image load -- then times out on its first block
 * (`dma state pre-load 118=0x637`, `stage6b result=-110`).  So the encoding is
 * wrong in a way this driver does not understand yet, and using it costs a
 * deploy.  Kept, compiled out, with the evidence attached. */
#define NV_IMEM_STORE_PROBE 0
#define XF_IDLE (1u << 1) /* 0x120 bit 1: a DIFFERENT status word, not completion */
#define NV_SEC2_ADDR2 0x841000
#define NV_BROM_PARAADDR0 (NV_SEC2_ADDR2 + 0x210)
#define NV_BROM_ENGIDMASK (NV_SEC2_ADDR2 + 0x19C)
#define NV_BROM_CURR_UCODE_ID (NV_SEC2_ADDR2 + 0x198)
#define NV_BROM_MOD_SEL (NV_SEC2_ADDR2 + 0x180)

/* One 256-byte-block DMA.  `is_imem` selects the code path (and SEC pages);
 * `src_off` is the offset inside the image, which for IMEM requests is also the
 * address the block lands at. */
/* What the encoder actually asked for, so the F1 row can quote the parameter
 * the transfer received rather than a literal repeated at the call site. */
static uint32_t g_dma_dst_first, g_dma_dst_last, g_dma_nblk;

static int falcon_dma_block(struct nv_dev *d, uint32_t fbase,
                            uint64_t src_phys, uint32_t src_end, uint32_t dst,
                            int is_imem, int sec) {
  if (g_dma_nblk == 0) g_dma_dst_first = dst;
  g_dma_dst_last = dst;
  g_dma_nblk++;
  /* FULL (0x118 bit 0) means the previous request has not been collected yet.
   * 0x118 is the command register itself and reads back the engine's own state;
   * 0x120 is a DIFFERENT status word on this part (measured 0x10 while 0x118
   * read 0x2, in the same run).  Polling 0x120 for these bits is what made the
   * load report -ETIMEDOUT on the first block as soon as the window became
   * readable -- before that the fence word sent the code down its "cannot
   * watch, pace and continue" path instead. */
  for (int i = 0; i < 200000; i++) {
    if (!(nv_rd32(d, fbase + NV_XFER_CTRL) & XF_FULL)) break;
    if (i == 199999) return -110;
  }
  nv_wr32(d, fbase + NV_XFER_EXT_BASE,
          (uint32_t)(src_phys >> 8));
  nv_wr32(d, fbase + NV_XFER_EXT_BASE1,
          (uint32_t)((src_phys >> 40) & 0x1FF));
  nv_wr32(d, fbase + NV_XFER_FALCON_ADDR, dst);
  nv_wr32(d, fbase + NV_XFER_EXT_ADDR, src_end);
  /* The command word comes from the one place that encodes it (nv_flcn.c), so
   * the SIZE field cannot be "8 for 256 bytes" again: that masked to 0, which
   * asks the engine for a 4-byte transfer, and the engine accepts it silently. */
  uint32_t ctrl = nv_flcn_xfer_cmd(NV_FLCN_BLOCK, is_imem, sec);
  if (ctrl == 0xffffffffu) return -EINVAL;
  nv_wr32(d, fbase + NV_XFER_CTRL, ctrl);
  /* Idle means the request left the queue; the writes landed.  But the status
   * register is INSIDE the read-fenced span (measured), so on a fenced window
   * there is nothing to poll: the transfer still happens, we simply cannot watch
   * it.  Failing here would abort a transfer that is fine -- the lesson the
   * mailbox probe taught.  So: if the status reads the fence word, pace the
   * blocks with a short delay and let the final verdict judge the stage. */
  for (int i = 0; i < 200000; i++) {
    uint32_t st = nv_rd32(d, fbase + NV_XFER_CTRL);
    if (st & XF_DONE) return 0; /* bit 1: the request was collected */
    if (st == NV_REG_FENCED || st == 0xffffffffu) {
      /* Unobservable window: the transfer still happens, so pace it instead of
       * aborting -- but pace it by TIME (~10 us, a 256-byte sysmem read several
       * times over), not by 50 spins of nothing, which issues the next command
       * before the engine can have drained the last one. */
      uint64_t t0 = 0, now = 0;
      if (nv_timer_read(d, &t0) == 0) {
        for (int k = 0; k < 100000; k++) {
          if (nv_timer_read(d, &now) == 0 && now - t0 > 10000ULL) break;
        }
      } else {
        for (volatile int spin = 0; spin < 200000; spin++) {
        }
      }
      return 0;
    }
  }
  return -110;
}

/* ---- F2: load a block through the PIO port instead of the DMA -------------
 *
 * PLAN.md §2's F2.  The DMA path has four things that can each be wrong and
 * cannot be observed here (the plug is inside the read-fenced span: 0x110,
 * 0x114, 0x118, 0x11c and 0x120 all answer 0xBADF5620 in every state this
 * driver has measured).  The PIO ports are a different mechanism: one writing
 * index register, one data register, no request, no context, no status word to
 * poll -- the write completes or it does not, and the ucode's behaviour is the
 * judge.  This driver has already proved DMEM round-trips through port 0
 * (nv_selftest), and IMEM needs two extra things both stacks set: the SECRET
 * bit (28) on the index word for secure pages, and a CODE_VIRT_ADDR write per
 * block.
 *
 * No status polling: the index register's autoincrement makes the block
 * synchronous by construction, which is exactly why this is the experiment
 * that does not depend on a readable fence.
 */
static int falcon_pio_block(struct nv_dev *d, uint32_t fbase, const uint8_t *src,
                            uint32_t nbytes, uint32_t dst, int is_imem, int sec) {
  uint32_t idx_reg = fbase + (is_imem ? NV_FALCON_CODE_INDEX : NV_FALCON_DATA_INDEX0);
  uint32_t data_reg = fbase + (is_imem ? NV_FALCON_CODE : NV_FALCON_DATA0);
  /* One conversion, shared with the read path: a byte offset in, a word index
   * out, and the index shifted into the register's field (bits 15:2) rather
   * than written into bits 0..13, which is what addressed a quarter of the
   * offset before this round. */
  uint32_t widx = nv_flcn_idx_word(dst);
  if (widx == NV_FLCN_IDX_NONE) return -ERANGE;
  uint32_t init = (widx << 2) | NV_FALCON_IDX_WR_AUTOINC;
  if (is_imem && sec) init |= NV_FALCON_IDX_SECRET; /* a secure page */
  nv_wr32(d, idx_reg, init);
  if (is_imem) nv_wr32(d, fbase + NV_FALCON_CODE_VIRT_ADDR, 0);
  for (uint32_t w = 0; w + 3 < nbytes; w += 4) {
    uint32_t v = (uint32_t)src[w] | ((uint32_t)src[w + 1] << 8) |
                 ((uint32_t)src[w + 2] << 16) | ((uint32_t)src[w + 3] << 24);
    nv_wr32(d, data_reg, v);
  }
  return 0;
}

/* Bring SEC2 out of the fence.  Before anything initialises it, the whole
 * 0x840000 window reads 0xBADF5620 -- measured, and measured again on the
 * reference card post-init where it answers falcon-shaped values.  The poke is
 * a PMC engine enable; the bit is not named in the published PMC header, so two
 * candidates are tried in order and the log says which one worked (or that
 * neither did). */
static int sec2_unfence(struct nv_dev *d) {
  uint32_t ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
  if (ctl != NV_REG_FENCED && ctl != 0xffffffffu) {
    uart_puts("[NV-SEC2] window live ctl=");
    uart_print_hex(ctl);
    uart_puts("\n");
    return 0;
  }
  /* THE FENCE EXPERIMENT (docs/nv-pmu-acr-contract.md 6 step 3).
   *
   * Two writes and two reads: pulse the engine-reset word, then see whether the
   * window stops answering the inaccessibility signature.  This is the single
   * most informative test on this card, because it separates "the fence is
   * reset state, so the pulse is enough" from "the fence is privilege state, so
   * FWSEC must run first" -- and those two answers lead to completely different
   * amounts of work.
   *
   * PMC_ENABLE is NOT part of this any more: a full single-bit sweep of it
   * lifted nothing (measured), and the register reads one bit on this part. */
  uart_puts("[NV-SEC2] pmc_enable=");
  uart_print_hex(nv_rd32(d, NV_PMC_ENABLE));
  uart_puts(" (not the gate: a 32-bit sweep lifted nothing)\n");

  /* Read-only dump of the FWSEC status block: whether it is even readable
   * pre-boot is itself unknown, and the answer is free. */
  uart_puts("[NV-SEC2] block1400:");
  for (uint32_t off = 0x1400; off < 0x1440; off += 4) {
    uart_puts(" ");
    uart_print_hex(nv_rd32(d, off));
  }
  uart_puts("\n");

  /* The pulse.  0x3C0 has no published name on this part; a wrong write there
   * resets a live engine, which is exactly why it is done only when the window
   * is fenced (nothing can be running behind a fence we cannot read). */
  nv_wr32(d, 0x8403C0, 1);
  for (volatile int spin = 0; spin < 200; spin++) {
  }
  nv_wr32(d, 0x8403C0, 0);
  ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
  uint32_t addr2 = nv_rd32(d, NV_SEC2_ADDR2);
  uart_puts("[NV-SEC2] after pulse ctl=");
  uart_print_hex(ctl);
  uart_puts(" addr2=");
  uart_print_hex(addr2);
  if (ctl != NV_REG_FENCED && ctl != 0xffffffffu) {
    uart_puts(" LIVE -> the fence was reset state\n");
    return 0;
  }
  uart_puts(" still fenced -> the fence is privilege state, so FWSEC must run on "
            "the GSP falcon first\n");
  return -ENODEV;
}

/* ---- the accessibility map -----------------------------------------------
 *
 * Which offsets in an engine window answer, and which return the
 * inaccessibility signature?  This is read-only and it is the difference
 * between "0x600 is fenced" and "everything above the IO page is fenced":
 * a 64-character row per window, one character per 64-byte block, sampled at
 * the block's first dword.  'F' = the fence word, '-' = all ones (nothing
 * there), '.' = a real read.
 *
 * The boundary matters because it says what devinit has to unlock, and whether
 * the unlock is page-scoped or region-scoped. */
static void nv_map_window(struct nv_dev *d, uint32_t base, const char *name) {
  char row[65];
  for (int i = 0; i < 64; i++) {
    uint32_t v = nv_rd32(d, base + (uint32_t)i * 64);
    row[i] = v == NV_REG_FENCED ? 'F' : (v == 0xffffffffu ? '-' : '.');
  }
  row[64] = 0;
  uart_puts("[NV-MAP] ");
  uart_puts(name);
  uart_puts(" ");
  uart_puts(row);
  uart_puts("\n");
}

/* ---- is the fence read-side only? ---------------------------------------
 *
 * docs/nv-devinit-contract.md section 6: every read outside the readable half of
 * the falcon page returns the inaccessibility signature, so "wrote X, read back
 * the fence word" cannot tell a dropped write from a landed one.  MAILBOX0
 * (0x110040) reads 0 in BOTH reference dumps -- fenced or not -- which makes it
 * the one word that can answer the question: write a sentinel, read it, restore.
 *
 * Restoring is not optional: 0x110040 is the FWSEC verdict word, and a leftover
 * sentinel would make a later FWSEC run report failure for the wrong reason.
 *
 * Also kills the previous hypothesis: on Turing+ devinit is PMU ucode launched
 * by FWSEC, not host-replayed golden tables, and no dword in this card's ROM
 * even contains a falcon-window address.  So there is nothing to replay. */
int nv_gsp_write_lands(struct nv_dev *d) {
  uint32_t a = nv_rd32(d, NV_FALCON_GSP_BASE + 0x040);
  uint32_t b = nv_rd32(d, NV_FALCON_GSP_BASE + 0x044);
  nv_wr32(d, NV_FALCON_GSP_BASE + 0x040, 0xA5A51234u);
  uint32_t r1 = nv_rd32(d, NV_FALCON_GSP_BASE + 0x040);
  nv_wr32(d, NV_FALCON_GSP_BASE + 0x044, 0x5A5A4321u);
  uint32_t r2 = nv_rd32(d, NV_FALCON_GSP_BASE + 0x044);
  nv_wr32(d, NV_FALCON_GSP_BASE + 0x040, 0);
  nv_wr32(d, NV_FALCON_GSP_BASE + 0x044, 0);
  uint32_t c = nv_rd32(d, NV_FALCON_GSP_BASE + 0x040);

  uart_puts("[NV-PROBE] write-lands baseline=");
  uart_print_hex(a);
  uart_puts("/");
  uart_print_hex(b);
  uart_puts(" wrote 0xa5a51234 -> ");
  uart_print_hex(r1);
  uart_puts(" wrote 0x5a5a4321 -> ");
  uart_print_hex(r2);
  uart_puts(" restored=");
  uart_print_hex(c);
  if (r1 == 0xA5A51234u && r2 == 0x5A5A4321u) {
    uart_puts("\n[NV-PROBE] verdict=WRITES LAND: the fence is read-side only, so "
              "FWSEC can be launched without any unfencing step\n");
    return 1;
  }
  if (r1 == 0 && a == 0) {
    uart_puts("\n[NV-PROBE] verdict=WRITE DROPPED: the window is genuinely "
              "write-fenced; next test is a cold VM start without an FLR\n");
    return 0;
  }
  uart_puts("\n[NV-PROBE] verdict=UNEXPECTED (neither a landing nor a zero): do "
            "not build on this register\n");
  return -1;
}

void nv_gsp_map_windows(struct nv_dev *d) {
  if (!d) return;
  uart_puts("[NV-MAP] one char per 64B block, F=fence word -=all ones .=a read\n");
  nv_map_window(d, NV_FALCON_GSP_BASE, "gsp 0x110000");
  nv_map_window(d, NV_FALCON_SEC2_BASE, "sec2 0x840000");
  nv_map_window(d, NV_FALCON_PMU_BASE, "pmu 0x10A000");
}

/* ---- observability the FWSEC experiment queue needs (PLAN.md section 2) ---
 *
 * F1-F5 are all "read a register and decide", and the fence makes that
 * dangerous: a register inside the fenced span answers 0xBADF5620 whether it
 * holds 0, a real value, or nothing at all (PLAN.md section 6 rule 1).  So
 * every register an experiment turns on is *established readable in the same
 * run* -- write a sentinel, read it back, put the register back -- and the raw
 * values are always logged next to the Boolean.
 */

/* Write `sentinel`, read it back, then set the register to `restore`.
 * Returns 1 when the sentinel survived the round trip: the register is
 * readable and writable right now.  `restore` is supplied by the caller
 * instead of the pre-write value because on a fenced window the pre-write
 * value is the fence word itself, and writing that back would be a poison
 * write. */
static int nv_probe_word(struct nv_dev *d, uint32_t off, uint32_t sentinel,
                         uint32_t restore) {
  uint32_t before = nv_rd32(d, off);
  nv_wr32(d, off, sentinel);
  uint32_t back = nv_rd32(d, off);
  nv_wr32(d, off, restore);
  uint32_t after = nv_rd32(d, off);
  uart_puts("[NV-PROBE] @");
  uart_print_hex(off);
  uart_puts(" before=");
  uart_print_hex(before);
  uart_puts(" wrote=");
  uart_print_hex(sentinel);
  uart_puts(" back=");
  uart_print_hex(back);
  uart_puts(" restore=");
  uart_print_hex(after);
  uart_puts(back == sentinel ? " READABLE+WRITABLE\n"
                             : " NOT-READ-WRITE (fenced, or writes dropped)\n");
  return back == sentinel;
}

/* ---- what is in the falcon's memory, and how we know --------------------
 *
 * Round 1 ended with three words read back through the DMEM port that did not
 * match the staged buffer, and no way to separate three explanations: the load
 * did not land, the read path addresses something else, or the comparison
 * looked in the wrong place.  A row per probe answers that -- it prints the
 * word the port answered, the word the staged image holds at the same offset,
 * and (when they differ) the offset in the image where the answered value does
 * appear.  That last field is what names a unit error: if the port answers the
 * image's word at `off>>2`, the index was fed words where bytes belong; if it
 * answers the word at `off<<2`, the destination was.
 */
struct nv_landing {
  int probes;      /* addresses that answered with data */
  int match;       /* ... that hold exactly the staged word */
  int displaced;   /* ... whose value is in the image at some other offset */
  int failed;      /* ... that did not answer at all */
};

static int nv_img_word(const uint8_t *img, uint32_t len, uint32_t off,
                       uint32_t *out) {
  if (!img || off + 4u > len) return 0;
  *out = (uint32_t)img[off] | ((uint32_t)img[off + 1] << 8) |
         ((uint32_t)img[off + 2] << 16) | ((uint32_t)img[off + 3] << 24);
  return 1;
}

static void nv_landing_row(struct nv_dev *d, const struct nv_falcon *f,
                           int is_imem, uint32_t off, const uint8_t *img,
                           uint32_t img_len, struct nv_landing *st) {
  uint32_t got = 0, want = 0;
  int rc = is_imem ? nv_falcon_imem_read_ex(d, f, off, &got, 1, 1)
                   : nv_falcon_dmem_read(d, f, off, &got, 1);
  uart_puts(is_imem ? "[NV-LAND] imem @" : "[NV-LAND] dmem @");
  uart_print_hex(off);
  uart_puts(" got=");
  uart_print_hex(got);
  if (rc != 0) {
    uart_puts(" READ-FAILED rc=");
    print_int(rc);
    uart_puts("\n");
    st->failed++;
    return;
  }
  st->probes++;
  if (!nv_img_word(img, img_len, off, &want)) {
    uart_puts(" (past the staged image: nothing to compare against)\n");
    st->probes--;
    return;
  }
  uart_puts(" want=");
  uart_print_hex(want);
  if (got == want) {
    uart_puts(" MATCH\n");
    st->match++;
    return;
  }
  /* Not what was staged there.  Where in the image does this value live?
   * Candidates first (the two unit errors), then a full search, because a
   * value that is in the image at an unrelated offset is a different finding
   * from one that is in it nowhere. */
  uint32_t q = 0, t4 = 0;
  int is_q = nv_img_word(img, img_len, off >> 2, &q) && q == got;
  int is_t4 = (off <= (img_len >> 2) * 4u) &&
              nv_img_word(img, img_len, off << 2, &t4) && t4 == got;
  uint32_t found = nv_flcn_find_word(img, img_len, got);
  uart_puts(" DIFF");
  if (is_q) uart_puts("|read-at-off>>2 (byte offset fed to a word field)");
  if (is_t4) uart_puts("|read-at-off<<2 (word offset fed to a byte field)");
  uart_puts("|value-in-image-at:");
  if (found == NV_FLCN_IDX_NONE) {
    uart_puts(" NOWHERE (this word is not in the staged image at all)\n");
  } else {
    uart_print_hex(found);
    uart_puts("\n");
  }
  st->displaced++;
}

static void nv_landing_verdict(const char *what, const struct nv_landing *st) {
  uart_puts("[NV-LAND] ");
  uart_puts(what);
  uart_puts(" probes=");
  print_int(st->probes);
  uart_puts(" match=");
  print_int(st->match);
  uart_puts(" displaced=");
  print_int(st->displaced);
  uart_puts(" failed=");
  print_int(st->failed);
  if (st->probes > 0 && st->match == st->probes)
    uart_puts(" verdict=LANDED (every probed word is the staged word)\n");
  else if (st->probes > 0 && st->displaced == st->probes - st->match &&
           st->match == 0)
    uart_puts(" verdict=NOT-THE-STAGED-WORDS (the image is in there, but not "
              "where the load asked for it: read the DIFF rows for the unit)\n");
  else if (st->probes == 0)
    uart_puts(" verdict=NO-ANSWER (no probe returned a comparable word)\n");
  else
    uart_puts(" verdict=MIXED (some words are the staged ones, some are not: "
              "a partial or interleaved load)\n");
}

/* ---- reading the code port, and reading IMEM through the engine --------
 *
 * The PIO code port answered 0xDEAD5EC1 at every IMEM address in run r2a --
 * seven addresses, one value, a word that is in no staged image.  That is a
 * signature, not data (the same class as 0xBADF5620 on the MMIO side), so
 * "the code landed" cannot be asked through the port.  Two other ways exist
 * and both are cheap:
 *
 *   1. the code port with the SECRET bit clear, and with CODE_VIRT_ADDR
 *      written first, which is the one difference between this read and the
 *      write path that is known to land;
 *   2. the XFER engine in its STORE direction: falcon memory -> system memory.
 *      A different path to the same bytes, and the only way to compare what
 *      IMEM holds against what was staged.
 */
static void nv_imem_read_row(struct nv_dev *d, struct nv_falcon *f, uint32_t off,
                             const uint8_t *img, uint32_t img_len,
                             int sec, int virt, struct nv_landing *st) {
  uint32_t got = 0, want = 0;
  if (virt) nv_wr32(d, f->base + NV_FALCON_CODE_VIRT_ADDR, 0);
  int rc = nv_falcon_imem_read_ex(d, f, off, &got, 1, sec);
  uart_puts("[NV-IMEM] @");
  uart_print_hex(off);
  uart_puts(" sec=");
  print_int(sec);
  uart_puts(" virt=");
  print_int(virt);
  uart_puts(" got=");
  uart_print_hex(got);
  if (rc != 0) {
    uart_puts(" READ-FAILED rc=");
    print_int(rc);
    uart_puts("\n");
    st->failed++;
    return;
  }
  if (!nv_img_word(img, img_len, off, &want)) {
    uart_puts(" (past the staged image)\n");
    return;
  }
  uart_puts(" want=");
  uart_print_hex(want);
  st->probes++;
  if (got == want) {
    uart_puts(" MATCH\n");
    st->match++;
  } else if (got == NV_REG_IMEM_MARKER) {
    uart_puts(" MASKED (0xdead5ec1: the port's answer for a secure page, not "
              "data)\n");
    st->failed++;
  } else {
    uart_puts(" DIFF (value-in-image-at:");
    uint32_t found = nv_flcn_find_word(img, img_len, got);
    if (found == NV_FLCN_IDX_NONE)
      uart_puts(" NOWHERE)\n");
    else {
      uart_print_hex(found);
      uart_puts(")\n");
    }
    st->displaced++;
  }
}

#if NV_IMEM_STORE_PROBE
/* falcon memory -> system memory, one 256-byte block, through the XFER engine.
 * Same four registers as the load, with DIR (0x118 bit 5) set.  Returns 0 when
 * the engine reported the request collected, -ETIMEDOUT otherwise; the caller
 * still compares the buffer, because "collected" is not "copied". */
static int falcon_dma_store_block(struct nv_dev *d, uint32_t fbase,
                                  uint64_t dst_phys, uint32_t dst_off,
                                  uint32_t src_falcon, int is_imem, int sec) {
  for (int i = 0; i < 200000; i++) {
    if (!(nv_rd32(d, fbase + NV_XFER_CTRL) & XF_FULL)) break;
    if (i == 199999) return -110;
  }
  nv_wr32(d, fbase + NV_XFER_EXT_BASE, (uint32_t)(dst_phys >> 8));
  nv_wr32(d, fbase + NV_XFER_EXT_BASE1, (uint32_t)((dst_phys >> 40) & 0x1FF));
  nv_wr32(d, fbase + NV_XFER_FALCON_ADDR, src_falcon);
  nv_wr32(d, fbase + NV_XFER_EXT_ADDR, dst_off);
  uint32_t ctrl = nv_flcn_xfer_cmd(NV_FLCN_BLOCK, is_imem, sec);
  if (ctrl == 0xffffffffu) return -EINVAL;
  nv_wr32(d, fbase + NV_XFER_CTRL, ctrl | XF_STORE);
  for (int i = 0; i < 200000; i++) {
    if (nv_rd32(d, fbase + NV_XFER_CTRL) & XF_DONE) return 0;
  }
  return -110;
}

/* Copy one IMEM block back through the engine and compare it with the staged
 * bytes.  One row per block; `*matched` counts fully-matching blocks so the
 * caller can print one verdict. */
static void nv_imem_store_row(struct nv_dev *d, uint32_t fbase,
                              const uint8_t *img, uint32_t img_len, uint32_t off,
                              uint64_t scratch_phys, int *matched, int *tried) {
  uint8_t *bb = (uint8_t *)phys_to_virt(scratch_phys);
  for (uint32_t i = 0; i < NV_FLCN_BLOCK; i++) bb[i] = 0xA5; /* prefill: an
    * engine that copies nothing leaves this, and "prefill with something whose
    * zero would look like success" is the rule this is applying */
  int rc = falcon_dma_store_block(d, fbase, scratch_phys, 0, off, 1, 1);
  uint32_t ctl = nv_rd32(d, fbase + NV_XFER_CTRL);
  int bad = -1;
  uint32_t got = 0, want = 0;
  if (off + NV_FLCN_BLOCK <= img_len) {
    for (uint32_t i = 0; i < NV_FLCN_BLOCK; i += 4) {
      uint32_t g = (uint32_t)bb[i] | ((uint32_t)bb[i + 1] << 8) |
                   ((uint32_t)bb[i + 2] << 16) | ((uint32_t)bb[i + 3] << 24);
      uint32_t w = (uint32_t)img[off + i] | ((uint32_t)img[off + i + 1] << 8) |
                   ((uint32_t)img[off + i + 2] << 16) |
                   ((uint32_t)img[off + i + 3] << 24);
      if (g != w && bad < 0) {
        bad = (int)i;
        got = g;
        want = w;
      }
    }
  }
  uart_puts("[NV-IMEM] store @");
  uart_print_hex(off);
  uart_puts(" rc=");
  print_int(rc);
  uart_puts(" 118=");
  uart_print_hex(ctl);
  (*tried)++;
  if (rc != 0) {
    uart_puts(" ENGINE DID NOT COLLECT (no copy to compare)\n");
    return;
  }
  if (bad < 0) {
    uart_puts(" MATCH (256 bytes copied back are the staged bytes)\n");
    (*matched)++;
    return;
  }
  uart_puts(" DIFF first@+0x");
  uart_print_hex((uint32_t)bad);
  uart_puts(" got=");
  uart_print_hex(got);
  uart_puts(" want=");
  uart_print_hex(want);
  /* What the copied block IS, if it is a staged block at another offset: the
   * unit error this would expose is a destination interpreted in words
   * (block at dst lands at 4*dst) or in quarters (at dst/4). */
  {
    uint32_t cand[2] = {off >> 2, off << 2};
    const char *name[2] = {"off>>2", "off<<2"};
    int named = 0;
    for (int i = 0; i < 2; i++) {
      if (cand[i] + NV_FLCN_BLOCK > img_len) continue;
      int same = 1;
      for (uint32_t k = 0; k < NV_FLCN_BLOCK; k++) {
        if (bb[k] != img[cand[i] + k]) {
          same = 0;
          break;
        }
      }
      if (same) {
        uart_puts(" |block==");
        uart_puts(name[i]);
        uart_puts(" (the destination is being read in the wrong unit)");
        named = 1;
      }
    }
    if (!named) {
      /* Where does the block's first word appear at all? */
      uint32_t found = nv_flcn_find_word(img, img_len, got);
      uart_puts(" |block-head-in-image-at:");
      if (found == NV_FLCN_IDX_NONE)
        uart_puts(" NOWHERE");
      else
        uart_print_hex(found);
    }
  }
  uart_puts("\n");
}

#endif /* NV_IMEM_STORE_PROBE */

/* ---- the 0x1400 status block as a read-only control ---------------------
 *
 * "Was 0x1438 written, or was the region cleared?" is answered by the
 * neighbours: snapshot the block before the launch and after it, and compare.
 * Nothing is written to registers whose meaning is unknown, and a change in any
 * other word is itself a finding (it names what the ucode touched).
 */
#define NV_SEC_STATUS_BLOCK 0x1400u
#define NV_SEC_STATUS_WORDS 16u

static void nv_snapshot_block(struct nv_dev *d,
                              uint32_t out[NV_SEC_STATUS_WORDS]) {
  for (uint32_t i = 0; i < NV_SEC_STATUS_WORDS; i++)
    out[i] = nv_rd32(d, NV_SEC_STATUS_BLOCK + i * 4u);
}

/* Count words that differ, fill `first` with the first differing index. */
static int nv_compare_block(const uint32_t *a, const uint32_t *b, int *first) {
  int n = 0;
  if (first) *first = -1;
  for (uint32_t i = 0; i < NV_SEC_STATUS_WORDS; i++) {
    if (a[i] != b[i]) {
      if (n == 0 && first) *first = (int)i;
      n++;
    }
  }
  return n;
}

/* ---- non-blocking PIO, for sampling inside the launch window -----------
 *
 * nv_falcon_dmem_read() waits for the xfer queue to drain first, and once the
 * window fences that wait costs a full timeout per call -- 20 M spins, measured
 * as the reason run r2b's sampling row could only say "no change" over 300 ms
 * of wall clock.  This is the same register pair without the wait, so a sampler
 * can ask "what does this word say right now" and get an answer or a failure
 * without stalling the observation it is trying to make.
 */
static int nv_pio_rd1_nb(struct nv_dev *d, uint32_t fbase, int is_imem,
                         uint32_t addr, uint32_t *out) {
  uint32_t idx = nv_flcn_idx_word(addr);
  if (idx == NV_FLCN_IDX_NONE) return -ERANGE;
  uint32_t idx_reg = fbase + (is_imem ? NV_FALCON_CODE_INDEX : NV_FALCON_DATA_INDEX0);
  uint32_t data_reg = fbase + (is_imem ? NV_FALCON_CODE : NV_FALCON_DATA0);
  nv_wr32(d, idx_reg, (idx << 2) | (is_imem ? NV_FALCON_IDX_SECRET : 0u) |
                          NV_FALCON_IDX_RD_AUTOINC);
  *out = nv_rd32(d, data_reg);
  return 0;
}

static int nv_pio_wr1_nb(struct nv_dev *d, uint32_t fbase, int is_imem,
                         uint32_t addr, uint32_t val) {
  uint32_t idx = nv_flcn_idx_word(addr);
  if (idx == NV_FLCN_IDX_NONE) return -ERANGE;
  uint32_t idx_reg = fbase + (is_imem ? NV_FALCON_CODE_INDEX : NV_FALCON_DATA_INDEX0);
  uint32_t data_reg = fbase + (is_imem ? NV_FALCON_CODE : NV_FALCON_DATA0);
  nv_wr32(d, idx_reg, (idx << 2) | (is_imem ? NV_FALCON_IDX_SECRET : 0u) |
                          NV_FALCON_IDX_WR_AUTOINC);
  nv_wr32(d, data_reg, val);
  return 0;
}

/* ---- the engine's STORE direction ---------------------------------------
 *
 * The load direction is proven (the image lands, DMEM compares word for word
 * through the port).  The reverse direction is the only channel that survives
 * the read fence -- and the one attempt that was ever made left the xfer queue
 * FULL and cost the next load its first block (r2c).  One attempt cannot say
 * which field was wrong, so this is a matrix judged by EFFECT: the landing
 * buffer is prefilled with a value that is not in the image, the request goes
 * out, and the buffer itself is polled.  The buffer is host memory, so the
 * judgement does not depend on any register inside the fence.
 *
 * Three things make a negative here mean something:
 *   - the prefill value is checked against the staged bytes (byte-wise, any
 *     alignment): a prefill that is also data cannot tell "copied" from
 *     "survived", and this row says whether that happened;
 *   - a LOAD_CONTROL with DIR clear runs first with the same address registers.
 *     If it moves bytes and no store variant does, the finding is about the
 *     direction bit, not about the driver being unable to drive the engine;
 *   - each variant gets its own block, so a landing that lags one request
 *     cannot be read as the next variant's success.
 */
#define NV_STORE_PREFILL 0xA6A6A6A6u

struct nv_store_res {
  int queue_full_at_entry; /* a previous request was already wedged */
  int issued;
  int changed;      /* the landing buffer changed: the engine wrote it */
  uint64_t changed_ns;
  int probes, match;
  uint32_t first_bad; /* byte offset inside the block, 0xffffffff = none */
  uint32_t bad_got, bad_want;
  uint32_t ctl_after, stat_after;
};

static uint32_t nv_ld_word(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* One request.  `want`/`want_len`/`want_off` describe the bytes the block is
 * expected to hold; passing want = 0 skips the comparison (the buffer-change
 * count is still the oracle).  `swap` writes the sysmem offset to 0x114 and the
 * falcon address to 0x11C, i.e. the hypothesis that the two side registers swap
 * roles with the direction; it is a separate axis from the command word because
 * no encoding of 0x118 can express it. */
static void nv_store_block(struct nv_dev *d, uint32_t fbase, uint64_t landing,
                           uint32_t falcon_off, const uint8_t *want,
                           uint32_t want_len, uint32_t want_off, int is_imem,
                           int sec, int variant, int swap, uint64_t deadline_ns,
                           struct nv_store_res *r) {
  for (uint32_t i = 0; i < sizeof *r; i++) ((uint8_t *)r)[i] = 0;
  r->first_bad = 0xffffffffu;
  uint8_t *bb = (uint8_t *)phys_to_virt(landing);
  for (uint32_t i = 0; i < NV_FLCN_BLOCK; i++) bb[i] = 0xA6;
  uint32_t cmd = nv_flcn_store_cmd(NV_FLCN_BLOCK, is_imem, sec, variant);
  /* A queue that is already FULL cannot take this request; say so rather than
   * waiting on it, and do not issue a command that would be queued behind a
   * request the engine never collected. */
  int gone = 0;
  for (int i = 0; i < 200000; i++) {
    if (!(nv_rd32(d, fbase + NV_XFER_CTRL) & XF_FULL)) {
      gone = 1;
      break;
    }
  }
  r->queue_full_at_entry = !gone;
  if (!gone || cmd == 0xffffffffu) {
    r->ctl_after = nv_rd32(d, fbase + NV_XFER_CTRL);
    r->stat_after = nv_rd32(d, fbase + NV_XFER_STATUS);
    return;
  }
  nv_wr32(d, fbase + NV_XFER_EXT_BASE, (uint32_t)(landing >> 8));
  nv_wr32(d, fbase + NV_XFER_EXT_BASE1, (uint32_t)((landing >> 40) & 0x1FF));
  uint32_t off_11c = nv_flcn_store_off(0, variant);
  nv_wr32(d, fbase + NV_XFER_FALCON_ADDR, swap ? off_11c : falcon_off);
  nv_wr32(d, fbase + NV_XFER_EXT_ADDR, swap ? falcon_off : off_11c);
  nv_wr32(d, fbase + NV_XFER_CTRL, cmd);
  r->issued = 1;
  uint64_t t0 = 0, now = 0;
  int have_t = nv_timer_read(d, &t0) == 0;
  for (uint32_t i = 0; i < 8000000u; i++) {
    if (bb[0] != 0xA6 || bb[1] != 0xA6 || bb[2] != 0xA6 || bb[3] != 0xA6) {
      r->changed = 1;
      if (have_t && nv_timer_read(d, &now) == 0) r->changed_ns = now - t0;
      break;
    }
    if ((i & 0x3f) == 0 && have_t && nv_timer_read(d, &now) == 0 &&
        now - t0 > deadline_ns)
      break;
  }
  r->ctl_after = nv_rd32(d, fbase + NV_XFER_CTRL);
  r->stat_after = nv_rd32(d, fbase + NV_XFER_STATUS);
  if (!want) return;
  for (uint32_t i = 0; i + 4u <= NV_FLCN_BLOCK; i += 4) {
    if (want_off + i + 4u > want_len) break;
    uint32_t got = nv_ld_word(bb + i);
    uint32_t w = nv_ld_word(want + want_off + i);
    r->probes++;
    if (got == w) {
      r->match++;
    } else if (r->first_bad == 0xffffffffu) {
      r->first_bad = i;
      r->bad_got = got;
      r->bad_want = w;
    }
  }
}

static void nv_store_row(struct nv_dev *d, const char *tag, int variant,
                         int is_imem, int sec, uint32_t falcon_off, int swap,
                         const struct nv_store_res *r) {
  (void)d; /* the row is a function of the result only */
  uart_puts("[NV-STORE] ");
  uart_puts(tag);
  uart_puts(" v=");
  uart_puts(nv_flcn_store_name(variant));
  uart_puts(swap ? " sides=SWAPPED" : " sides=contract");
  uart_puts(" imem=");
  print_int(is_imem);
  uart_puts(" sec=");
  print_int(sec);
  uart_puts(" @falcon=0x");
  uart_print_hex(falcon_off);
  uart_puts(" cmd=0x");
  uart_print_hex(nv_flcn_store_cmd(NV_FLCN_BLOCK, is_imem, sec, variant));
  uart_puts(" 11c=0x");
  uart_print_hex(nv_flcn_store_off(0, variant));
  uart_puts(" -> changed=");
  print_int(r->changed);
  if (r->changed) {
    uart_puts(" at +");
    uart_print_hex(r->changed_ns);
    uart_puts(" ns");
  }
  uart_puts(" match=");
  print_int(r->match);
  uart_puts("/");
  print_int(r->probes);
  if (r->first_bad != 0xffffffffu) {
    uart_puts(" first_bad=+0x");
    uart_print_hex(r->first_bad);
    uart_puts(" got=");
    uart_print_hex(r->bad_got);
    uart_puts(" want=");
    uart_print_hex(r->bad_want);
  }
  uart_puts(" 118=");
  uart_print_hex(r->ctl_after);
  uart_puts(" 120=");
  uart_print_hex(r->stat_after);
  uart_puts(" pending_s=");
  print_int((int)NV_FALCON_XFER_STORES_PENDING(r->stat_after));
  uart_puts(" pending_l=");
  print_int((int)NV_FALCON_XFER_LOADS_PENDING(r->stat_after));
  uart_puts(" verdict=");
  if (r->queue_full_at_entry)
    uart_puts("QUEUE-WEDGED-BEFORE-THIS-OP (an earlier request was still FULL, "
              "so this variant was never issued)\n");
  else if (!r->issued)
    uart_puts("NOT-ISSUED (the encoder refused the word)\n");
  else if (!r->changed)
    uart_puts("NO-EFFECT (the landing buffer still holds the prefill: nothing "
              "wrote host memory)\n");
  else if (r->probes && r->match == r->probes)
    uart_puts("STORE-WORKS (the bytes the engine wrote to host memory are the "
              "block asked for, byte for byte)\n");
  else
    uart_puts("STORE-WROTE-OTHER-BYTES (host memory changed, but to something "
              "other than the requested block: first_bad names the word)\n");
}

/* Run the matrix.  Returns the first variant that got bytes into host memory,
 * or -1.  `landing` is a 4 KiB contiguous frame; each probe uses its own
 * 256-byte slot so a late landing cannot be read as the next variant's success.
 *
 * The store queue is FOUR slots deep on this part (measured: pending_s counted
 * 1,2,3,4 across four requests and none retired), so the probe list is capped at
 * four and every row that finds a slot already full says so instead of blaming
 * its own encoding. */
struct nv_store_probe {
  int variant;
  int swap;
  int is_imem;
  int sec;
  uint32_t falcon_off;
};

static int nv_store_matrix(struct nv_dev *d, uint32_t fbase, uint64_t landing,
                           const uint8_t *img, uint32_t img_len,
                           uint32_t dmem_off, uint32_t dmem_len,
                           const char *when, const struct nv_store_probe *pr,
                           int nprobe) {
  struct nv_store_res r;
  uart_puts("[NV-STORE] ");
  uart_puts(when);
  uart_puts(" prefill=0x");
  uart_print_hex(NV_STORE_PREFILL);
  uart_puts(" collisions-in-image=");
  print_int(nv_flcn_prefill_collides(img, img_len, NV_STORE_PREFILL));
  uart_puts(" entry 118=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_CTRL));
  uart_puts(" 120=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_STATUS));
  uart_puts(" (a prefill that is also in the image would make 'unchanged' and "
            "'copied' the same observation)\n");

  /* Control first: same address registers, DIR clear.  It must leave falcon
   * memory holding the prefill; a PIO read of the destination answers it.  This
   * is the row that separates "the direction does not work" from "the engine is
   * not taking anything at all right now" -- which after START is a real
   * possibility, because the window is fenced and every read here may be the
   * fence word. */
  {
    const uint32_t kCtlOff = 0x1800; /* past the 0xD80 DMEM image */
    uint8_t *bb = (uint8_t *)phys_to_virt(landing + 4096u - NV_FLCN_BLOCK);
    for (uint32_t i = 0; i < NV_FLCN_BLOCK; i++) bb[i] = 0xA6;
    uint32_t ctl_cmd = nv_flcn_store_cmd(NV_FLCN_BLOCK, 0, 0,
                                        NV_FLCN_STORE_V_LOAD_CONTROL);
    uint32_t before = 0;
    (void)nv_pio_rd1_nb(d, fbase, 0, kCtlOff, &before);
    nv_wr32(d, fbase + NV_XFER_EXT_BASE,
            (uint32_t)((landing + 4096u - NV_FLCN_BLOCK) >> 8));
    nv_wr32(d, fbase + NV_XFER_EXT_BASE1,
            (uint32_t)(((landing + 4096u - NV_FLCN_BLOCK) >> 40) & 0x1FF));
    nv_wr32(d, fbase + NV_XFER_FALCON_ADDR, kCtlOff);
    nv_wr32(d, fbase + NV_XFER_EXT_ADDR, 0);
    nv_wr32(d, fbase + NV_XFER_CTRL, ctl_cmd);
    uint32_t after = 0;
    int answered = 0;
    for (int i = 0; i < 200; i++) { /* bounded: ~1 ms of pacing */
      if (nv_pio_rd1_nb(d, fbase, 0, kCtlOff, &after) == 0 &&
          after == NV_STORE_PREFILL) {
        answered = 1;
        break;
      }
      uint64_t tw = 0;
      if (nv_timer_read(d, &tw) == 0) {
        uint64_t t1 = 0;
        while (nv_timer_read(d, &t1) == 0 && t1 - tw < 50000ULL) {
        }
      }
    }
    uart_puts("[NV-STORE] control ");
    uart_puts(when);
    uart_puts(" v=LOAD_CONTROL cmd=0x");
    uart_print_hex(ctl_cmd);
    uart_puts(" @falcon=0x");
    uart_print_hex(kCtlOff);
    uart_puts(" was=");
    uart_print_hex(before);
    uart_puts(" now=");
    uart_print_hex(after);
    uart_puts(" 118=");
    uart_print_hex(nv_rd32(d, fbase + NV_XFER_CTRL));
    uart_puts(answered
                  ? " verdict=ENGINE-DRIVES-THESE-ADDRESSES (DIR clear moved the "
                    "prefill into falcon memory, so a store's silence below is "
                    "about the direction, not about the addresses)\n"
                  : " verdict=CONTROL-FAILED (DIR clear did not move the bytes "
                    "either, so no store row below says anything about its own "
                    "encoding -- this is the window's state, not the "
                    "store's)\n");
    if (!answered) return -1;
  }

  int winner = -1;
  for (int i = 0; i < nprobe && i < 4; i++) {
    /* Every probe gets its own slot, even when the falcon offset repeats. */
    uint32_t slot = (uint32_t)i * NV_FLCN_BLOCK;
    uint32_t img_ref = pr[i].is_imem ? 0u : dmem_off;
    uint32_t img_len_ref = pr[i].is_imem ? dmem_off : dmem_len;
    nv_store_block(d, fbase, landing + slot, pr[i].falcon_off, img + img_ref,
                   img_len_ref, pr[i].falcon_off, pr[i].is_imem, pr[i].sec,
                   pr[i].variant, pr[i].swap, 20000000ULL, &r);
    nv_store_row(d, when, pr[i].variant, pr[i].is_imem, pr[i].sec,
                 pr[i].falcon_off, pr[i].swap, &r);
    if (r.changed && r.probes && r.match == r.probes && winner < 0)
      winner = pr[i].variant;
    if (r.queue_full_at_entry && i + 1 < nprobe)
      uart_puts("[NV-STORE] the queue is FULL: the rows below were not issued, "
                "so they carry no information about their own encoding\n");
  }
  return winner;
}

/* ---- the launch, watched across the whole budget ------------------------
 *
 * Round 2 answered "did it run?" with a blocking wait for one word: the poll
 * broke when 0x1438 changed, and everything the core did between START and that
 * write was never sampled.  The fence closed ~3.5 us after START and the verdict
 * word arrived at ~104 ms, so a blocking poll has a 104 ms blind spot by
 * construction.
 *
 * This is a non-blocking sweep instead: four words sampled every iteration, the
 * verdict word every iteration, a timestamp taken only when something changes
 * (a timer read per sample would cost more than the sample), and no early exit
 * for any reason -- not for the halt bit, not for the verdict write -- so the
 * window's behaviour over the whole 2 s is one continuous observation.
 *
 *   0x110100 UC_CTRL     the core's own control word (STOPPED bit 4)
 *   0x110114 DMATRFMOFFS the DMA destination echo, inside the fenced block
 *   0x111668 BCR_CTRL    the RISC-V/falcon core select
 *   0x110FF0 PC          where the core is, if the window is open at all
 *
 * DMEM is sampled through the port only while one of those reads is a real
 * value: poking the data port while the window is fenced would be asking a
 * closed door 125,000 times a second, and the reopen is visible in the register
 * poll anyway.
 */
#define NV_SWEEP_REGS 4
#define NV_SWEEP_EV_MAX 40
#define NV_SWEEP_DM_MAX 8

struct nv_sweep_ev {
  uint64_t ns;
  uint32_t reg;
  uint32_t from, to;
};

struct nv_sweep_dm {
  uint64_t ns;
  uint32_t addr, from, to;
};

struct nv_sweep_state {
  struct nv_sweep_ev ev[NV_SWEEP_EV_MAX];
  struct nv_sweep_dm dm[NV_SWEEP_DM_MAX];
  int nev, ndm, dropped;
  uint64_t iters;
  uint64_t first_fence_ns[NV_SWEEP_REGS];
  uint64_t last_real_ns[NV_SWEEP_REGS];
  uint64_t first_real_ns[NV_SWEEP_REGS];
  int seen_fence[NV_SWEEP_REGS], seen_real[NV_SWEEP_REGS];
  int reopened;
  uint64_t reopened_ns;
  uint64_t dm_samples, dm_fenced, dm_changes;
  uint64_t end_ns;
  uint32_t dm_last[4];
  int dm_seeded;
};
static struct nv_sweep_state g_sw; /* file scope: the kernel stack is small */

struct nv_launch_obs {
  uint32_t st;
  uint32_t ctl_at_change, pc_at_change, bcr_at_change, dma_at_change,
      mb_at_change;
  uint64_t changed_ns;
  int changed;
  int halted;
  uint32_t ctl_last;
};

static void nv_sw_ev(uint64_t ns, uint32_t reg, uint32_t from, uint32_t to) {
  if (g_sw.nev < NV_SWEEP_EV_MAX) {
    g_sw.ev[g_sw.nev].ns = ns;
    g_sw.ev[g_sw.nev].reg = reg;
    g_sw.ev[g_sw.nev].from = from;
    g_sw.ev[g_sw.nev].to = to;
    g_sw.nev++;
  } else {
    g_sw.dropped++;
  }
}

static void nv_sw_dm(uint64_t ns, uint32_t addr, uint32_t from, uint32_t to) {
  g_sw.dm_changes++;
  if (g_sw.ndm < NV_SWEEP_DM_MAX) {
    g_sw.dm[g_sw.ndm].ns = ns;
    g_sw.dm[g_sw.ndm].addr = addr;
    g_sw.dm[g_sw.ndm].from = from;
    g_sw.dm[g_sw.ndm].to = to;
    g_sw.ndm++;
  } else {
    g_sw.dropped++;
  }
}

/* The instant the verdict word was written: take everything that describes it
 * before printing anything, because a row costs several microseconds of UART
 * and this is the one moment the whole round exists to look at.  Two channels,
 * in the order that survives the fence: the PIO port (answers only while the
 * window is open) and the engine's store direction (answers -- if it works --
 * regardless, because its landing buffer is host memory). */
static void nv_verdict_instant(struct nv_dev *d, uint32_t fbase, uint64_t landing,
                               const uint8_t *img, uint32_t img_len,
                               uint32_t dmem_off, uint32_t dmem_len,
                               int store_variant) {
  static const uint32_t pio_addr[4] = {0x000, 0x004, 0x0B0C, 0x0D40};
  uint32_t pio_val[4] = {0, 0, 0, 0};
  int pio_rc[4] = {0, 0, 0, 0};
  struct nv_store_res st_res;
  const uint32_t st_off = 0xB00;   /* the appif/DMEMMAPPER + command buffer */
  /* The most-promising remaining candidate: DIR with the two side registers
   * swapped, on the block that holds the FRTS command. */
  const int st_var = NV_FLCN_STORE_V_PLAIN;
  int do_store = (store_variant >= 0);
  (void)img_len;
  for (int i = 0; i < 4; i++)
    pio_rc[i] = nv_pio_rd1_nb(d, fbase, 0, pio_addr[i], &pio_val[i]);
  if (do_store)
    nv_store_block(d, fbase, landing, st_off, img + dmem_off, dmem_len, st_off,
                   0, 0, st_var, 1, 2000000ULL, &st_res);
  uart_puts("[NV-SWEEP] at-verdict port:");
  for (int i = 0; i < 4; i++) {
    uint32_t want = 0;
    int have_want = pio_addr[i] + 4u <= dmem_len &&
                    nv_img_word(img + dmem_off, dmem_len, pio_addr[i], &want);
    uart_puts(" @");
    uart_print_hex(pio_addr[i]);
    uart_puts("=");
    uart_print_hex(pio_val[i]);
    if (pio_rc[i] != 0)
      uart_puts("(READ-FAILED)");
    else if (nv_flcn_read_class(pio_val[i]) == NV_FLCN_RD_FENCED)
      uart_puts("(FENCED)");
    else if (nv_flcn_read_class(pio_val[i]) == NV_FLCN_RD_SEC_MARKER)
      uart_puts("(SEC-MARKER)");
    else if (have_want && pio_val[i] == want)
      uart_puts("(unchanged)");
    else if (have_want)
      uart_puts("(CHANGED)");
  }
  uart_puts("\n");
  if (do_store) {
    nv_store_row(d, "at-verdict", st_var, 0, 0, st_off, 1, &st_res);
  } else {
    uart_puts("[NV-SWEEP] at-verdict store: NOT AVAILABLE (no store variant "
              "worked in this run, so DMEM at this instant is unobservable)\n");
  }
}

static void nv_launch_sweep(struct nv_dev *d, uint32_t fbase, uint32_t stale,
                            const char *tag, uint64_t budget_ns,
                            struct nv_launch_obs *o, uint64_t landing,
                            const uint8_t *img, uint32_t img_len,
                            uint32_t dmem_off, uint32_t dmem_len,
                            int store_variant) {
  static const char *const nm[NV_SWEEP_REGS] = {"ctl", "dma114", "bcr", "pc"};
  uint32_t addr[NV_SWEEP_REGS];
  addr[0] = fbase + NV_FALCON_UC_CTRL;
  addr[1] = fbase + NV_XFER_FALCON_ADDR;
  addr[2] = NV_GSP_BCR_CTRL;
  addr[3] = fbase + NV_FALCON_PC;
  static const uint32_t dm_addr[4] = {0x000, 0x800, 0x0B0C, 0x0D40};
  static const uint64_t marks[10] = {1000ULL,    10000ULL,   100000ULL,
                                     1000000ULL, 3500000ULL, 10000000ULL,
                                     100000000ULL, 104000000ULL, 1000000000ULL,
                                     2000000000ULL};
  static const char *const mark_name[10] = {"1us",  "10us",  "100us", "1ms",
                                            "3.5ms", "10ms",  "100ms", "104ms",
                                            "1s",    "2s"};

  for (uint32_t i = 0; i < sizeof g_sw; i++) ((uint8_t *)&g_sw)[i] = 0;
  o->st = stale;
  o->changed = 0;
  o->halted = 0;
  o->ctl_last = 0;

  uint64_t t0 = 0, now = 0;
  int have_t = nv_timer_read(d, &t0) == 0;
  uart_puts("[NV-SWEEP] ");
  uart_puts(tag);
  uart_puts(" start budget_ns=");
  uart_print_hex(budget_ns);
  uart_puts(" timer=");
  print_int(have_t);
  uart_puts(" store-snapshot=");
  uart_puts(store_variant >= 0 ? "on(SWAPPED/DIR, block 0xB00)" : "off");
  uart_puts("\n");

  int seeded = 0, mi = 0;
  uint32_t lastv[NV_SWEEP_REGS] = {0, 0, 0, 0};
  int window_open = 0;
  uint64_t waited = 0;
  for (;;) {
    g_sw.iters++;
    for (int k = 0; k < NV_SWEEP_REGS; k++) {
      uint32_t v = nv_rd32(d, addr[k]);
      int cls = nv_flcn_read_class(v);
      if (k == 0) {
        window_open = (cls == NV_FLCN_RD_REAL) ? 1 : 0;
      }
      if (cls == NV_FLCN_RD_REAL) {
        if (k == 0) o->ctl_last = v;
        if (v & (k == 0 ? NV_FALCON_UC_CTRL_STOPPED : 0u)) o->halted = 1;
      }
      if (!seeded) {
        lastv[k] = v;
        if (cls == NV_FLCN_RD_REAL) {
          g_sw.seen_real[k] = 1;
          g_sw.first_real_ns[k] = 0;
          g_sw.last_real_ns[k] = 0;
        } else if (cls == NV_FLCN_RD_FENCED) {
          g_sw.seen_fence[k] = 1;
          g_sw.first_fence_ns[k] = 0;
        }
        continue;
      }
      if (v == lastv[k]) continue;
      uint64_t ts = 0;
      if (have_t && nv_timer_read(d, &ts) == 0) ts -= t0;
      nv_sw_ev(ts, (uint32_t)k, lastv[k], v);
      if (cls == NV_FLCN_RD_FENCED) {
        if (!g_sw.seen_fence[k]) {
          g_sw.seen_fence[k] = 1;
          g_sw.first_fence_ns[k] = ts;
        }
      } else if (cls == NV_FLCN_RD_REAL) {
        if (!g_sw.seen_real[k]) g_sw.first_real_ns[k] = ts;
        g_sw.seen_real[k] = 1;
        g_sw.last_real_ns[k] = ts;
        if (g_sw.seen_fence[k] && !g_sw.reopened) {
          g_sw.reopened = 1;
          g_sw.reopened_ns = ts;
        }
      }
      lastv[k] = v;
    }
    seeded = 1;

    /* The verdict word, outside the fenced span, every iteration. */
    o->st = nv_rd32(d, 0x1438);
    if (!o->changed && (o->st >> 16) != (stale >> 16)) {
      uint64_t ts = 0;
      if (have_t && nv_timer_read(d, &ts) == 0) ts -= t0;
      o->changed = 1;
      o->changed_ns = ts;
      o->ctl_at_change = nv_rd32(d, fbase + NV_FALCON_UC_CTRL);
      o->dma_at_change = nv_rd32(d, fbase + NV_XFER_FALCON_ADDR);
      o->bcr_at_change = nv_rd32(d, NV_GSP_BCR_CTRL);
      o->pc_at_change = nv_rd32(d, fbase + NV_FALCON_PC);
      o->mb_at_change = nv_rd32(d, fbase + NV_FALCON_MAILBOX0_FLCN);
      uart_puts("[NV-SWEEP] ");
      uart_puts(tag);
      uart_puts(" verdict-write at +");
      uart_print_hex(ts);
      uart_puts(" ns\n");
      nv_verdict_instant(d, fbase, landing, img, img_len, dmem_off, dmem_len,
                         store_variant);
    }

    /* DMEM, only while the window is answering. */
    if (window_open && (g_sw.iters & 0x0f) == 0) {
      int k = (int)((g_sw.iters >> 4) & 3u);
      uint32_t v = 0;
      if (nv_pio_rd1_nb(d, fbase, 0, dm_addr[k], &v) == 0) {
        g_sw.dm_samples++;
        if (nv_flcn_read_class(v) == NV_FLCN_RD_FENCED) {
          g_sw.dm_fenced++;
        } else if (!g_sw.dm_seeded) {
          g_sw.dm_last[k] = v;
        } else if (v != g_sw.dm_last[k]) {
          uint64_t ts = 0;
          if (have_t && nv_timer_read(d, &ts) == 0) ts -= t0;
          nv_sw_dm(ts, dm_addr[k], g_sw.dm_last[k], v);
          g_sw.dm_last[k] = v;
        }
        g_sw.dm_seeded = 1;
      }
    }

    if ((g_sw.iters & 0x3f) == 0 && have_t && nv_timer_read(d, &now) == 0) {
      waited = now - t0;
      if (mi < 10 && waited >= marks[mi]) {
        uart_puts("[NV-SWEEP] ");
        uart_puts(tag);
        uart_puts(" at ");
        uart_puts(mark_name[mi]);
        uart_puts(" ctl=");
        uart_print_hex(lastv[0]);
        uart_puts(" dma114=");
        uart_print_hex(lastv[1]);
        uart_puts(" bcr=");
        uart_print_hex(lastv[2]);
        uart_puts(" pc=");
        uart_print_hex(lastv[3]);
        uart_puts(" 1438=");
        uart_print_hex(o->st);
        uart_puts("\n");
        mi++;
      }
      if (waited > budget_ns) break;
    }
  }
  g_sw.end_ns = waited;

  uart_puts("[NV-SWEEP] ");
  uart_puts(tag);
  uart_puts(" done iters=");
  uart_print_hex(g_sw.iters);
  uart_puts(" end_ns=");
  uart_print_hex(g_sw.end_ns);
  uart_puts(" events=");
  print_int(g_sw.nev);
  uart_puts(" dropped=");
  print_int(g_sw.dropped);
  uart_puts("\n");
  for (int i = 0; i < g_sw.nev; i++) {
    uart_puts("[NV-SWEEP] ");
    uart_puts(tag);
    uart_puts(" ev +");
    uart_print_hex(g_sw.ev[i].ns);
    uart_puts(" ns ");
    uart_puts(nm[g_sw.ev[i].reg]);
    uart_puts(" ");
    uart_print_hex(g_sw.ev[i].from);
    uart_puts(" -> ");
    uart_print_hex(g_sw.ev[i].to);
    uart_puts(nv_flcn_read_class(g_sw.ev[i].to) == NV_FLCN_RD_FENCED
                  ? " (FENCED)\n"
                  : (nv_flcn_read_class(g_sw.ev[i].from) == NV_FLCN_RD_FENCED
                         ? " (REAL: out of the fence)\n"
                         : "\n"));
  }
  for (int i = 0; i < g_sw.ndm; i++) {
    uart_puts("[NV-SWEEP] ");
    uart_puts(tag);
    uart_puts(" dmemchg +");
    uart_print_hex(g_sw.dm[i].ns);
    uart_puts(" ns @");
    uart_print_hex(g_sw.dm[i].addr);
    uart_puts(" ");
    uart_print_hex(g_sw.dm[i].from);
    uart_puts(" -> ");
    uart_print_hex(g_sw.dm[i].to);
    uart_puts("\n");
  }
  for (int k = 0; k < NV_SWEEP_REGS; k++) {
    uart_puts("[NV-SWEEP] ");
    uart_puts(tag);
    uart_puts(" window ");
    uart_puts(nm[k]);
    uart_puts(" real=");
    print_int(g_sw.seen_real[k]);
    if (g_sw.seen_real[k]) {
      uart_puts(" last_real=+");
      uart_print_hex(g_sw.last_real_ns[k]);
    }
    uart_puts(" fenced=");
    print_int(g_sw.seen_fence[k]);
    if (g_sw.seen_fence[k]) {
      uart_puts(" first_fence=+");
      uart_print_hex(g_sw.first_fence_ns[k]);
    }
    uart_puts("\n");
  }
  uart_puts("[NV-SWEEP] ");
  uart_puts(tag);
  uart_puts(" dmem samples=");
  uart_print_hex(g_sw.dm_samples);
  uart_puts(" fenced=");
  uart_print_hex(g_sw.dm_fenced);
  uart_puts(" changes=");
  uart_print_hex(g_sw.dm_changes);
  uart_puts("\n");
  uart_puts("[NV-SWEEP] ");
  uart_puts(tag);
  uart_puts(" verdict=");
  if (g_sw.reopened) {
    uart_puts("WINDOW-REOPENED at +");
    uart_print_hex(g_sw.reopened_ns);
    uart_puts(" ns after being fenced (the fence is state, not a timer: what "
              "the core produced is visible from that instant on)\n");
  } else if (g_sw.dropped) {
    uart_puts("INCONCLUSIVE (event log overflowed: re-read the raw rows)\n");
  } else if (!g_sw.seen_fence[0] && !g_sw.seen_fence[2]) {
    uart_puts("WINDOW-NEVER-FENCED (every sample of ctl and bcr was a real "
              "value: the 3.5 us fence of round 2 did not reproduce)\n");
  } else {
    uart_puts("WINDOW-CLOSED-AND-STAYED-CLOSED (first fence word at +");
    uart_print_hex(g_sw.first_fence_ns[0] ? g_sw.first_fence_ns[0]
                                          : g_sw.first_fence_ns[2]);
    uart_puts(" ns -- +0 means it was ALREADY fenced at the first sample, i.e. "
              "within one iteration of START, which is not the same as 'the "
              "fence arrived at 0 ns' -- and no sample of any of the four words "
              "was a real value again inside the whole budget: DMEM is not "
              "visible through the port for the rest of the run)\n");
  }
}


/* ---- is IMEM readable, and is the code in it? --------------------------
 *
 * Two questions, and they had been conflated.  Every IMEM read in runs r2a/r2b
 * answered 0xDEAD5EC1 -- through the PIO port AND through the engine's store
 * direction -- which is consistent with two very different worlds:
 *
 *   (a) reads of IMEM are refused, and the marker is the refusal.  Then the
 *       code's presence cannot be read at all, and only effects judge it;
 *   (b) the marker is what IMEM *contains* (a reset fill of the secure memory),
 *       and the load did not overwrite it.  Then the code did not land.
 *
 * Writing a known pattern through the port and reading it back -- first through
 * the same port, then THROUGH THE ENGINE -- separates them: if the engine
 * copies the pattern out, reads work, so whatever else reads as the marker is
 * content.  The pattern is left in place on purpose: the load runs afterwards
 * and must overwrite it, which is a second, independent statement about the
 * load.
 */
static void nv_imem_write_probe(struct nv_dev *d, struct nv_falcon *f,
                                uint32_t off, uint32_t pat, uint64_t scratch_phys,
                                int *pio_ok, int *eng_ok) {
  uint32_t back = 0, eng = 0;
  int wrc = nv_pio_wr1_nb(d, f->base, 1, off, pat);
  int rrc = nv_pio_rd1_nb(d, f->base, 1, off, &back);
  uart_puts("[NV-IMEMW] @");
  uart_print_hex(off);
  uart_puts(" wrote=");
  uart_print_hex(pat);
  uart_puts(" wrc=");
  print_int(wrc);
  uart_puts(" pio_back=");
  uart_print_hex(back);
  (*pio_ok) = (rrc == 0 && back == pat) ? 1 : 0;
#if NV_IMEM_STORE_PROBE
  /* The engine's view of the same word. */
  uint8_t *bb = (uint8_t *)phys_to_virt(scratch_phys);
  for (uint32_t i = 0; i < NV_FLCN_BLOCK; i++) bb[i] = 0xA6;
  int s = falcon_dma_store_block(d, f->base, scratch_phys, 0, off, 1, 1);
  if (s == 0) {
    eng = (uint32_t)bb[0] | ((uint32_t)bb[1] << 8) | ((uint32_t)bb[2] << 16) |
          ((uint32_t)bb[3] << 24);
  }
  uart_puts(" store_rc=");
  print_int(s);
  uart_puts(" eng_back=");
  uart_print_hex(eng);
  (*eng_ok) = (s == 0 && eng == pat) ? 1 : 0;
#else
  (void)scratch_phys;
  (void)eng;
  uart_puts(" store_probe=disabled(see NV_IMEM_STORE_PROBE)");
  (*eng_ok) = 0;
#endif
  if (*pio_ok)
    uart_puts(" PORT-READS-BACK (a pattern written through the port reads back, "
              "so IMEM content IS visible this way and the marker elsewhere is "
              "content, not a refusal)\n");
  else
    uart_puts(" PORT-CANNOT-CONFIRM (a pattern written through the port does not "
              "read back: the code port refuses IMEM, so no read through it says "
              "anything about what the code memory holds)\n");
}

/* The five words that describe the last DMA request this host programmed.
 * 0x110 is the system-memory base (>>8), 0x114 the falcon-memory side
 * (DMATRFFBOFFS: the *destination*), 0x118 the command, 0x11C the image-side
 * offset, 0x120 the status.  One row, so a report can quote it verbatim. */
static void nv_xfer_state(struct nv_dev *d, uint32_t fbase, const char *when) {
  uart_puts("[NV-FWSEC] dma state ");
  uart_puts(when);
  uart_puts(" 110=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_EXT_BASE));
  uart_puts(" 114=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_FALCON_ADDR));
  uart_puts(" 118=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_CTRL));
  uart_puts(" 11c=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_EXT_ADDR));
  uart_puts(" 120=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_STATUS));
  uart_puts("\n");
}

/* ---- work package B: the fuse check -------------------------------------
 *
 * PLAN.md section 2 item B / docs/nv-pmu-acr-contract.md section 1.6 item 10.
 * Every falcon image is signed with a key whose version is fused into the
 * part; 0x8241C0 + (UcodeId-1)*4 holds that version for the ucode id the
 * descriptor names.  If the descriptor's SignatureVersions has no version in
 * common with the fuse, the Boot ROM rejects the image and NO amount of
 * transport or parameter fixing can help -- which is exactly the branch this
 * check exists to separate.  Read-only: eight dwords.
 */
#define NV_PFUSED_FW_VERSION0 0x8241C0u

static void nv_fuse_check(struct nv_dev *d) {
  /* The block holds one dword per ucode id, ids 1..N.  The descriptor's own id
   * can be above 8 (this card's FWSEC is id 9), so the dump is 16 dwords wide
   * AND the register the verdict uses is read explicitly: an index computed
   * against the dump's width would silently read past it, which is exactly the
   * "0 means no version" mistake this check exists to avoid. */
  uint32_t raw[16];
  int nfence = 0;
  uint32_t id = d->ucode_id ? d->ucode_id : 9;
  for (uint32_t i = 0; i < 16; i++) {
    raw[i] = nv_rd32(d, NV_PFUSED_FW_VERSION0 + i * 4);
    if (raw[i] == NV_REG_FENCED || raw[i] == 0xffffffffu) nfence++;
  }
  uart_puts("[NV-FUSE] reg 0x8241c0..1ff:");
  for (uint32_t i = 0; i < 16; i++) {
    uart_puts(" ");
    uart_print_hex(raw[i]);
  }
  uart_puts(" fenced_words=");
  print_int(nfence);
  uart_puts("\n");

  uint32_t fuse_addr = NV_PFUSED_FW_VERSION0 + (id - 1u) * 4u;
  uint32_t fuse = nv_rd32(d, fuse_addr);
  uint32_t vers = d->ucode_sigvers;
  uint32_t idx = nv_fwsec_sig_index(fuse, vers, d->ucode_sigcount);

  uart_puts("[NV-FUSE] ucode=");
  print_int((int)id);
  uart_puts(" fuse@");
  uart_print_hex(fuse_addr);
  uart_puts("=");
  uart_print_hex(fuse);
  uart_puts(" sigvers=");
  uart_print_hex(vers);
  uart_puts(" sigcount=");
  print_int((int)d->ucode_sigcount);
  uart_puts(" and=");
  uart_print_hex(fuse & vers);
  uart_puts(" sig_index=");
  if (idx == NV_FWSEC_SIG_NONE) {
    uart_puts("none\n");
  } else {
    print_int((int)idx);
    uart_puts("\n");
  }

  if (fuse == NV_REG_FENCED || fuse == 0xffffffffu) {
    uart_puts("[NV-FUSE] verdict=INCONCLUSIVE: the fuse word reads the "
              "inaccessibility signature, so a zero here is unreadable, not a "
              "verdict (PLAN.md section 6 rule 1)\n");
    return;
  }
  if (fuse == 0) {
    uart_puts("[NV-FUSE] verdict=NO-FUSED-VERSION for this ucode id: the "
              "descriptor cannot authenticate on this part\n");
    return;
  }
  if (idx == NV_FWSEC_SIG_NONE) {
    uart_puts("[NV-FUSE] verdict=IMAGE-INVALID-FOR-FUSES: fuse says version 0x");
    uart_print_hex(fuse);
    uart_puts(" and the descriptor offers only sigvers 0x");
    uart_print_hex(vers);
    uart_puts("; no signature the host can patch will verify\n");
    return;
  }
  uart_puts("[NV-FUSE] verdict=COMPATIBLE (fuse and descriptor share a "
            "version): the image is valid for this part's fuses, so a failure "
            "is transport or ucode, not fuses. The signature to copy into DMEM "
            "is index ");
  print_int((int)idx);
  uart_puts(" (of ");
  print_int((int)d->ucode_sigcount);
  uart_puts(")\n");
  (void)raw;
}

/* ---- stage 6a: reset the GSP falcon the way FWSEC expects ----------------
 *
 * docs/nv-pmu-acr-contract.md section 3.3 steps 1-3, which are required before
 * any FWSEC ucode can be handed over and need no ucode to run.  Each step has a
 * read-back, so this stage can be judged on its own:
 *   step 1  clear 0x048 bits 0-1, 0x014 = all-ones, pulse the engine-reset word,
 *           then wait for 0x0F4 bit 31 (reset completed) and bit 12 (memory
 *           scrubbing finished).
 *   step 2  0x624 bit 7 = ALLOW_PHYS_NO_CTX, 0x10C = 0: the DMA may address
 *           system memory without a context.
 *   step 3  0x600 = 5: the DMA port is routed to non-coherent physical sysmem.
 */
#define NV_GSP_U 0x110000u
int nv_gsp_fwsec_reset(struct nv_dev *d) {
  if (!d) return -EINVAL;
  if (!d->falcon[NV_FALCON_IDX_GSP].present) return -ENODEV;

  /* MEASURED, and it changes the plan: applying the documented reset to an
   * unprivileged host FENCES the GSP window (ctl goes 0x10 -> 0xBADF5620) and
   * 0x0F4 bit 31 never sets.  The engine was already STOPPED before we touched
   * it, so the reset was not needed for the property that matters -- never load
   * a running core -- and attempting it costs the window.  So: skip the
   * destructive half when the core is already stopped, and say so. */
  /* The reset is REQUIRED for STARTCPU to be accepted -- measured: without it the
   * core answers START with "still halted".  It was previously skipped because
   * applying it made the window read as fenced, but that fence is read-side only
   * (measured), so the reset was probably working all along and the driver was
   * misreading the result.  Do it unconditionally, and wait with a delay rather
   * than a poll, because 0x0F4 (the reset/scrub status) is inside the fenced
   * span and cannot be read. */
  uint32_t ctl0 = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL);
  uart_puts("[NV-FWSEC] gsp ctl before reset=");
  uart_print_hex(ctl0);
  uart_puts(" (reset is done unconditionally: the status word it is judged by is "
            "unreadable here)\n");
  /* Is 0x114 readable *before* the destructive half?  F1 judges the DMA by this
   * register, so whether the reset costs the read path has to be measured in
   * the same run rather than remembered from another boot. */
  (void)nv_probe_word(d, NV_GSP_U + NV_XFER_FALCON_ADDR, 0x00C0FFEEu, 0);
  uint32_t v48 = nv_rd32(d, NV_GSP_U + 0x048);
  nv_wr32(d, NV_GSP_U + 0x048, v48 & ~3u);
  nv_wr32(d, NV_GSP_U + 0x014, 0xffffffffu);
  nv_wr32(d, NV_GSP_U + 0x3C0, 1);
  for (volatile int spin = 0; spin < 2000; spin++) {
  }
  nv_wr32(d, NV_GSP_U + 0x3C0, 0);
  /* Scrubbing: bounded, ~20 ms spec, given generous margin under a VM. */
  for (volatile int spin = 0; spin < 2000000; spin++) {
  }

  /* Bounded: 150 us for the reset to complete, 20 ms for scrubbing.  The loop
   * counts are generous because a VM's TSC pacing is not the card's.
   *
   * HONESTY (docs/nv-fwsec-launch-contract.md §6): 0x0F4 sits inside the
   * read-fenced span, so on this card bit 31 and bit 12 are often the fence
   * word rather than state.  The previous version initialised both Booleans to
   * 1 ("already stopped") and so reported `reset done=1 scrubbed=1` without
   * ever having seen the bits -- a pass nobody observed.  Now the bits are
   * reported as observed and the launch is deliberately NOT gated on them: the
   * contract's rule is "do not gate any decision on a read of a fenced block",
   * and the launch's own verdict channels (0x1438 hi16, 0x840100) are the
   * gates. */
  uint32_t hwcfg2 = 0;
  int bit31 = 0, bit12 = 0;
  for (int i = 0; i < 200000; i++) {
    hwcfg2 = nv_rd32(d, NV_GSP_U + 0x0F4);
    bit31 = (hwcfg2 & (1u << 31)) ? 1 : 0;
    bit12 = (hwcfg2 & (1u << 12)) ? 1 : 0;
    if (bit31 && !bit12) break;
  }
  uint32_t ctl = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL);
  uart_puts("[NV-FWSEC] reset observed bit31=");
  print_int(bit31);
  uart_puts(" bit12=");
  print_int(bit12);
  uart_puts(" hwcfg2=");
  uart_print_hex(hwcfg2);
  uart_puts(" ctl=");
  uart_print_hex(ctl);
  if (hwcfg2 == NV_REG_FENCED || hwcfg2 == 0xffffffffu)
    uart_puts(" verdict=UNOBSERVABLE (fence word): 0x0f4 is inside the read-fenced "
              "span, so the reset's own handshake cannot be read here -- the "
              "launch proceeds and is judged by 0x1438/0x840100 instead\n");
  else
    uart_puts(" verdict=READABLE (these are real bits, not the fence word)\n");
  /* After the reset: is 0x114 still a register we can judge a DMA by? */
  (void)nv_probe_word(d, NV_GSP_U + NV_XFER_FALCON_ADDR, 0x00C0FFEEu, 0);
  nv_xfer_state(d, NV_GSP_U, "after-reset");

  /* ---- the two state fixups the reference applies at exactly this point ---
   *
   * Both are INFERRED-from-SOURCE and both were missing here, so they are one
   * change with one question attached: does putting the falcon in the state
   * the reference leaves it in let STARTCPU reach the core?
   *
   *   FALCON_RM (0x084) <- PMC_BOOT_0.  Every falcon enable in the reference
   *   writes it at the end of the reset, before the first load.
   *
   *   The RISC-V core select cleared.  Measured: 0x111668 is unreadable BEFORE
   *   the reset (0xBADF5620) and reads 0x110 -- RISCV|BRFETCH, VALID clear --
   *   after the launch attempt, so the reset is where it becomes observable.
   *   The reference clears the select bit before a falcon (non-RISC-V) load and
   *   waits for bit 0; a START issued while the core is selected as RISC-V need
   *   not reach the falcon core at all.
   */
  nv_wr32(d, NV_GSP_U + 0x084, nv_rd32(d, 0x000000));
  uart_puts("[NV-FWSEC] falcon_rm(0x084) <- pmc_boot0=0x");
  uart_print_hex(nv_rd32(d, 0x000000));
  uart_puts("\n");
  {
    uint32_t bcr = nv_rd32(d, NV_GSP_BCR_CTRL);
    if (bcr == NV_REG_FENCED || bcr == 0xffffffffu) {
      uart_puts("[NV-FWSEC] f3 core-select 0x111668 unreadable after reset (");
      uart_print_hex(bcr);
      uart_puts("): cannot clear the RISC-V selection\n");
    } else if (bcr & NV_GSP_BCR_RISCV) {
      nv_wr32(d, NV_GSP_BCR_CTRL, 0);
      uint32_t b = bcr;
      for (int i = 0; i < 100000; i++) {
        b = nv_rd32(d, NV_GSP_BCR_CTRL);
        if (b & NV_GSP_BCR_VALID) break;
      }
      uart_puts("[NV-FWSEC] f3 core-select cleared: 0x111668 ");
      uart_print_hex(bcr);
      uart_puts(" -> ");
      uart_print_hex(b);
      uart_puts((b & NV_GSP_BCR_VALID)
                    ? " (VALID: the falcon core is now the selected core)\n"
                    : " (VALID never set: the clear did not take)\n");
    } else {
      uart_puts("[NV-FWSEC] f3 core-select already falcon: 0x111668=");
      uart_print_hex(bcr);
      uart_puts("\n");
    }
  }
  (void)nv_probe_word(d, NV_GSP_U + 0x084, 0x5A5A5A5Au, nv_rd32(d, 0x000000));

  uint32_t v624 = nv_rd32(d, NV_GSP_U + 0x624);
  nv_wr32(d, NV_GSP_U + 0x624, v624 | (1u << 7));
  nv_wr32(d, NV_GSP_U + 0x10C, 0);
  nv_wr32(d, NV_GSP_U + 0x600, 0x5);
  uart_puts("[NV-FWSEC] dma cfg 624=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x624));
  uart_puts(" 10c=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x10C));
  uart_puts(" 600=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x600));
  uart_puts("\n");
  return 0;
}

/* The metadata block's `verified` field, read and written byte-wise (it is a
 * wire structure the firmware also touches).  Defined above the launch so the
 * BAR1 read-back of the VRAM copy can use the same offset. */
#define NV_WPR_OFF_VERIFIED 0xF8u
#define NV_WPR_VERIFIED_WORD 0xA0A0A0A0A0A0A0A0ULL

/* ---- stage 6b: launch FWSEC ---------------------------------------------
 *
 * docs/nv-pmu-acr-contract.md 3.3 steps 4-11, now that the measurement says the
 * falcon page accepts writes.  The ucode comes out of the card's own VBIOS
 * (nv_vbios_probe located it), is staged into contiguous frames, and is handed
 * over by DMA: IMEM as secret pages, DMEM as normal pages.
 *
 * Two variants are attempted, in order, because one field layout is not
 * established: (A) boot with no DMEM descriptor -- the shape a plain falcon
 * image takes; (B) write the 44-byte bl-dmem descriptor first.  Each fails
 * cleanly into MAILBOX0, so trying both costs one boot and answers the question.
 * The descriptor byte layout below is INFERRED from the field list in the
 * contract; if variant B is the one that works, that inference gets a note.
 *
 * Round 4 adds the VRAM channel: the FRTS region (1 MiB at frts_off) is
 * prefilled through BAR1 before the launch and scanned again after it.  BAR1 is
 * a PFB decode of VRAM, so the falcon's read fence does not cover it, and the
 * prefill makes "FWSEC wrote nothing" an observation instead of an assumption
 * (src/kernel/nv_vram.c).
 */
int nv_gsp_fwsec_launch(struct nv_dev *d, uint32_t frts_off, uint32_t frts_size,
                        uint64_t wpr_start) {
  if (!d) return -EINVAL;
  if (!d->ucode_desc || !d->ucode_imem || !d->ucode_dmem) {
    uart_puts("[NV-FWSEC] no ucode located: nothing to launch\n");
    return -ENOENT;
  }
  /* ---- the VRAM channel, before anything touches the falcon -------------
   *
   * This is the one path the launch cannot fence.  It also comes first in the
   * run so its answer exists whether the launch below succeeds, fails or
   * wedges: a region read after a failed launch means nothing unless the same
   * region was the host's own pattern before it.
   *
   * `frts_off` arrives in 4 KiB pages (the command's unit), so the region is
   * frts_off*4096 for frts_size*4096 bytes -- 0x5FE300000 for 0x100000 here.
   */
  struct nv_vram_win vram;
  struct nv_vram_scan vscan_pre;
  int vram_ready = 0;
  const uint32_t kVramSentinel = 0xA5A5A5A5u;
  uint32_t vram_len = frts_size * 4096u;
  /* One region of 1 MiB is the contract's value; a page count that would make
   * the window larger than a few MiB is not a region this run should write
   * into, so it is refused rather than scaled. */
  if (frts_size > 0x400u) vram_len = 0;
  for (uint32_t i = 0; i < (uint32_t)sizeof vram; i++) ((uint8_t *)&vram)[i] = 0;
  nv_vram_scan_reset(&vscan_pre);
  if (frts_off && vram_len) {
    int vrc = nv_vram_prefill(d, &vram, (uint64_t)frts_off * 4096u, vram_len,
                              kVramSentinel, &vscan_pre);
    vram_ready = (vrc == 0);
    uart_puts("[NV-BAR1] channel=");
    uart_puts(vram_ready ? "TRUSTWORTHY" : "BLIND");
    uart_puts(" (prefill read back through the aperture; only TRUSTWORTHY can "
              "report a firmware write)\n");
    /* The same three questions asked before START, so the change of behaviour
     * is one observation with one instrument rather than two code paths. */
    (void)nv_vram_behaviour(d, (uint64_t)frts_off * 4096u, "frts-pre");
    (void)nv_vram_behaviour(d, (uint64_t)frts_off * 4096u + vram_len - 0x1000u,
                            "frts-last-pre");
    (void)nv_vram_behaviour(d, (uint64_t)frts_off * 4096u + vram_len,
                            "above-pre");
    (void)nv_vram_behaviour(d, 0x5F0000000ULL, "scratch-pre");
  } else {
    uart_puts("[NV-BAR1] skipped: no FRTS region in this layout\n");
  }

  uint32_t total = d->ucode_imem + d->ucode_dmem;
  uint32_t pages = (total + FRAME_SIZE - 1) / FRAME_SIZE;
  uint64_t phys = frame_alloc_contig((int)pages);
  if (!phys) return -ENOMEM;
  uint64_t imem_scratch = 0; /* allocated below; freed at `out` */
  uint64_t store_scratch = 0; /* the store direction's landing frame */
  uint8_t *buf = (uint8_t *)phys_to_virt(phys);
  int rc = nv_vbios_read(d, d->ucode_payload, buf, total);
  if (rc != 0) {
    for (uint32_t i = 0; i < pages; i++) frame_free(phys + (uint64_t)i * FRAME_SIZE);
    return rc;
  }
  /* The boot ROM looks for the PKC signature at DMEM offset 0xB24 (the
   * descriptor's own PKCDataOffset).  Both stacks copy 384 bytes there before
   * the DMA; without it the ROM rejects the image and the core never runs. */
  uint8_t sig[384];
  /* Which of the descriptor's 384-byte signatures the part's fused key version
   * selects.  Copying the wrong one is rejected by the Boot ROM with no
   * host-visible reason, so this is derived (nv_fwsec_sig_index) instead of
   * being assumed to be the first one.  Measured on this card: fuse at
   * 0x8241e0 = 1, SignatureVersions 0x3, SignatureCount 2 -> index 1. */
  {
    uint32_t id = d->ucode_id ? d->ucode_id : 9;
    uint32_t fuse = nv_rd32(d, NV_PFUSED_FW_VERSION0 + (id - 1u) * 4u);
    uint32_t idx = nv_fwsec_sig_index(fuse, d->ucode_sigvers, d->ucode_sigcount);
    uart_puts("[NV-FWSEC] sig fuse=");
    uart_print_hex(fuse);
    uart_puts(" selects index=");
    if (idx == NV_FWSEC_SIG_NONE) {
      uart_puts("none (no version the fuse and the descriptor share): the image "
                "cannot authenticate on this part\n");
      rc = -EINVAL;
      goto out;
    }
    print_int((int)idx);
    uart_puts("\n");
    uint32_t sig_off = d->ucode_desc + 0x2C + idx * (uint32_t)sizeof sig;
    if (sig_off + (uint32_t)sizeof sig > d->ucode_payload) {
      uart_puts("[NV-FWSEC] signature index runs past the descriptor block: "
                "refusing to read inside the payload\n");
      rc = -EINVAL;
      goto out;
    }
    if (nv_vbios_read(d, sig_off, sig, sizeof sig) != 0) {
      uart_puts("[NV-FWSEC] PKC signature read FAILED: the ROM aperture did not "
                "answer\n");
      rc = -EIO;
      goto out;
    }
    for (uint32_t i = 0; i < sizeof sig; i++)
      buf[d->ucode_imem + 0xB24 + i] = sig[i];
    uart_puts("[NV-FWSEC] PKC signature patched at DMEM 0xB24 from desc+0x");
    uart_print_hex(idx * (uint32_t)sizeof sig + 0x2C);
    uart_puts(" head=");
    for (uint32_t i = 0; i < 4; i++) nv_hex_byte(sig[i]);
    uart_puts("\n");
  }
  /* ---- the FRTS command ------------------------------------------------
   * FWSEC's payload is a small OS; the host hands it exactly one command
   * through the DMEMMAPPER application interface in the DMEM image, and the
   * region it must carve out is FRTS's whole purpose.  Without this patch the
   * ucode wakes up with init_cmd = 0 and produces no WPR2 -- which looks
   * identical to "the core never ran" from every host-visible register.
   * Offsets and field order: docs/nv-pmu-acr-contract.md §3.2, measured
   * against this card's own DMEM image (appif 0x1C, DMEMMAPPER 0xAE0,
   * command buffer 0xD40, init_cmd 0xB0C). */
  {
    uint8_t *dmem = buf + d->ucode_imem;
    uint32_t cmd_off = nv_fwsec_cmd_buffer(dmem, d->ucode_dmem, d->ucode_iface);
    uart_puts("[NV-FWSEC] frts patch dmem_iface=0x");
    uart_print_hex(d->ucode_iface);
    uart_puts(" cmd_buf=0x");
    uart_print_hex(cmd_off);
    uart_puts(" region_pages=");
    uart_print_hex(frts_off);
    uart_puts("/");
    uart_print_hex(frts_size);
    uart_puts(" type=2\n");
    int prc = nv_fwsec_patch_frts(dmem, d->ucode_dmem, d->ucode_iface, frts_off,
                                  frts_size);
    uart_puts("[NV-FWSEC] frts result=");
    print_int(prc);
    if (prc == 0) {
      /* Read the staged image back: the evidence is the bytes the DMA will
       * copy, not the arguments this function was given. */
      uint32_t init_cmd = 0, w[11];
      for (uint32_t i = 0; i < 11; i++) w[i] = 0;
      int rrc = nv_fwsec_read_cmd(dmem, d->ucode_dmem, d->ucode_iface, &init_cmd,
                                  w, 11);
      uart_puts(" readback=");
      print_int(rrc);
      uart_puts(" init_cmd=0x");
      uart_print_hex(init_cmd);
      uart_puts("\n[NV-FWSEC] frts cmd dwords:");
      for (uint32_t i = 0; i < 11; i++) {
        uart_puts(" ");
        uart_print_hex(w[i]);
      }
      uart_puts("\n");

      /* ---- the verdict channel's own negative control -------------------
       *
       * "hi16 became 0, so FWSEC reported no error" is an inference, and it is
       * the inference the whole launch verdict rests on.  It has never been
       * tested against a run that MUST fail: if an invalid command still
       * produces the same 0x00005A5A at the same time, the word is written by
       * the ucode's exit path and carries no status at all -- and this round's
       * "success" reading collapses.
       *
       * The injected fault is the DMEMMAPPER command selector: init_cmd 0x15
       * (FRTS) becomes 0x99, an id no handler in that dispatcher can claim.
       * Everything else in the image, the launch and the timings stay identical
       * to run r3c, so the comparison is one variable.
       *
       * The DMEMMAPPER block is located by its own "DMAP" signature rather than
       * by a remembered offset, and the row prints where it found it.
       */
      {
        /* RESULT (r3d, evidence/nv/r3d-fault-injected-init-cmd.log): the fault
         * landed (init_cmd 0x15 -> 0x99, read back through nv_fwsec_read_cmd)
         * and the answer is decisive -- the core halted at +27,623,488 ns
         * (27.6 ms, three times earlier than a normal launch) and 0x1438 kept
         * its sentinel: NOTHING was written.  So the 0x00005A5A write at
         * ~105 ms is conditional on the FRTS command being accepted, which is
         * what makes it evidence; and a rejected command produces no error code
         * at all, so the channel cannot report one.  Off again: this is a
         * one-run control, and a fault-injected boot must never be the default
         * shape of a run. */
        const int kInjectFault = 0;
        if (kInjectFault) {
          uint32_t dmap = nv_flcn_find_word(dmem, d->ucode_dmem, 0x50414D44u);
          if (dmap == NV_FLCN_IDX_NONE || dmap + 0x30 > d->ucode_dmem) {
            uart_puts("[NV-FWSEC] CONTROL: no DMAP block found in the staged "
                      "DMEM, so the fault was not injected (this run is a "
                      "repeat of the previous one)\n");
          } else {
            uint8_t *p = dmem + dmap + 0x2C; /* init_cmd */
            uint32_t was = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            p[0] = 0x99;
            p[1] = 0;
            p[2] = 0;
            p[3] = 0;
            uint32_t now = 0;
            int rrc2 = nv_fwsec_read_cmd(dmem, d->ucode_dmem, d->ucode_iface,
                                        &now, w, 11);
            uart_puts("[NV-FWSEC] CONTROL: DMAP at dmem+0x");
            uart_print_hex(dmap);
            uart_puts(" init_cmd 0x");
            uart_print_hex(was);
            uart_puts(" -> 0x");
            uart_print_hex(now);
            uart_puts(" (rrc=");
            print_int(rrc2);
            uart_puts(") FAULT INJECTED: if the verdict word still reads "
                      "0x00005A5A at the same +105 ms, hi16 carries no status "
                      "and the 'no error' reading is unfounded\n");
          }
        }
      }
    } else {
      uart_puts(prc == -ERANGE ? " (command buffer too small)" : " (not the image the descriptor described)");
      uart_puts("\n");
      rc = prc;
      goto out;
    }
  }
  uart_puts("[NV-FWSEC] ucode staged phys=");
  uart_print_hex(phys);
  uart_puts(" dmem_image=+0x");
  uart_print_hex(d->ucode_imem);
  uart_puts(" block=256\n");

  /* ---- is the DMEM port a read path at all? -----------------------------
   *
   * Every conclusion below about the falcon's memory is a PIO read through
   * DATA_INDEX0/DATA0, and that read had never been proved on this card: round
   * 1's read-back disagreed with the staged buffer in all three words it
   * examined.  A disagreement is only evidence once the instrument is known to
   * work, so: write a pattern at the addresses the read-back uses (plus one
   * past the image, where nothing was loaded), read each back through the same
   * register pair, and leave zeros -- this runs BEFORE the load, so it cannot
   * damage the image, and a zero is what the load writes over anyway.
   *
   * The probe can fail, and the row says which way it went.  If it fails, the
   * load's read-back below proves nothing and the verdict says so.
   */
  struct nv_falcon *gspf = &d->falcon[NV_FALCON_IDX_GSP];
  int dmem_read_trustworthy = 0;
  /* One scratch frame serves both IMEM paths: the write probe below and the
   * engine copies in the landing map.  Allocated here because the write probe
   * has to run before the load. */
  imem_scratch = frame_alloc_contig(1);
  store_scratch = frame_alloc_contig(1);
  {
    /* IMEM: is it writable/readable at all, and by which path?
     *
     * MEASURED r2c/r2d: the answer is "no, and the asking costs the load".  A
     * pattern written through the code port does not read back (the port answers
     * 0xDEAD5EC1), and in both runs where this probe ran the *next* DMA request
     * -- the FWSEC image load -- timed out on its first block (r2c: the queue was
     * left FULL, 0x118=0x637; r2d: 0x118 read 0x2 and never DONE).  Writing the
     * code port is therefore not inert: it leaves the engine unable to collect a
     * DMA request.  The probe is off; the read-only port rows below (harmless in
     * r2a/r2b, where the load succeeded) report what can be reported. */
    const int kImemWriteProbe = 0;
    if (!kImemWriteProbe) {
      uart_puts("[NV-IMEMW] write probe OFF: measured (r2c, r2d) that a "
                "code-port write stops the engine collecting the next DMA "
                "request, and the port cannot read the pattern back anyway\n");
    } else if (!imem_scratch) {
      uart_puts("[NV-IMEMW] skipped: no contiguous scratch frame\n");
    } else {
      int pio_ok = 0, eng_ok = 0;
      nv_imem_write_probe(d, gspf, 0x000, 0x5EC2C0DEu, imem_scratch, &pio_ok,
                          &eng_ok);
      nv_imem_write_probe(d, gspf, 0x1000, 0x5EC2C1DEu, imem_scratch, &pio_ok,
                          &eng_ok);
    }
  }
  {
    /* How much DMEM does the engine say it has?  The third probe lands past the
     * image on purpose (to show the port works where nothing was loaded), and
     * if the engine's own size word says otherwise the probe would be asking
     * for memory that does not exist -- which would look like a broken read
     * path and be wrong. */
    uint32_t caps = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CAPS);
    uint32_t dmem_bytes = (uint32_t)NV_FALCON_CAPS_DATA_SIZE(caps) * 256u;
    uart_puts("[NV-DMEM] caps 108=");
    uart_print_hex(caps);
    uart_puts(" data_bytes=");
    uart_print_hex(dmem_bytes);
    uart_puts(" (UC_CAPS.DATA_SIZE in 256-byte units)\n");
    static const uint32_t paddr[3] = {0xB0C, 0xD40, 0x1000};
    static const uint32_t ppat[3] = {0x5EC2B0Cu, 0x5EC2D40u, 0x5EC21000u};
    int ok = 0, tried = 0;
    for (int i = 0; i < 3; i++) {
      if (dmem_bytes && paddr[i] + 4u > dmem_bytes) {
        uart_puts("[NV-DMEM] path selftest @");
        uart_print_hex(paddr[i]);
        uart_puts(" SKIPPED (past the engine's own DATA_SIZE)\n");
        continue;
      }
      uint32_t seen = 0;
      int r = nv_falcon_dmem_probe(d, gspf, paddr[i], ppat[i], 0, &seen);
      uart_puts("[NV-DMEM] path selftest @");
      uart_print_hex(paddr[i]);
      uart_puts(" wrote=");
      uart_print_hex(ppat[i]);
      uart_puts(" back=");
      uart_print_hex(seen);
      if (r == 1) {
        uart_puts(" ROUNDTRIP\n");
        ok++;
        tried++;
      } else if (r == 0) {
        uart_puts(" DIFFERS (the port answered something other than what was "
                  "written: every read below is suspect)\n");
        tried++;
      } else {
        uart_puts(" REFUSED rc=");
        print_int(r);
        uart_puts("\n");
      }
    }
    uart_puts("[NV-DMEM] path verdict=");
    if (tried == 0) {
      uart_puts("NO-PROBE (the port refused every address: the window is not "
                "the engine this driver thinks)\n");
    } else if (ok == tried) {
      dmem_read_trustworthy = 1;
      uart_puts("READ-PATH-TRUSTWORTHY (write then read at the same byte "
                "offset returns the written word)\n");
    } else {
      uart_puts("READ-PATH-BROKEN (at least one address did not round-trip): a "
                "load read-back through this port cannot be believed\n");
    }
  }

  /* F1 precondition: is the register the experiment reads a register in THIS
   * run?  (PLAN.md §6 rule 1 -- a masked read is not evidence.) */
  nv_xfer_state(d, NV_GSP_U, "pre-load");
  (void)nv_probe_word(d, NV_GSP_U + NV_XFER_FALCON_ADDR, 0x00C0FFEEu, 0);

  /* 256-byte blocks into the GSP this time -- the previous version targeted
   * SEC2 for every transfer, so the ucode never reached the engine it was
   * meant for.  DMEM's tail is padded up to a block boundary. */
  uint32_t nblk = 0, dst_first = 0, dst_last = 0;
  g_dma_nblk = 0;
  /* ---- F2 (PLAN.md §2): load the image by PIO, not by DMA -----------------
   * One constant decides which transport runs, because which one runs *is* the
   * experiment: the DMA path's status registers are all inside the read-fenced
   * span (measured twice), so "the request was accepted" cannot be observed
   * there at all, while the PIO port has no request to accept.  Set to 0 to
   * run the identical block sequence through the DMA engine. */
  const int kFwsecPio = 0;
  uart_puts("[NV-FWSEC] f2 transport=");
  uart_puts(kFwsecPio ? "PIO (port 0: 0x180/0x184 IMEM with SECRET, 0x1c0/0x1c4 "
                        "DMEM; no DMA request, no context, no status word)\n"
                      : "DMA (256-byte requests)\n");

  if (kFwsecPio) {
    for (uint32_t off = 0; off < d->ucode_imem; off += NV_FLCN_BLOCK) {
      uint32_t n = d->ucode_imem - off;
      if (n > NV_FLCN_BLOCK) n = NV_FLCN_BLOCK;
      rc = falcon_pio_block(d, NV_GSP_U, buf + off, n, off, 1, 1);
      if (rc != 0) goto out;
      g_dma_nblk++;
    }
    uart_puts("[NV-FWSEC] f2 imem pio blocks=");
    print_int((int)g_dma_nblk);
    uart_puts(" bytes=");
    uart_print_hex(d->ucode_imem);
    uart_puts("\n");
    for (uint32_t off = 0; off < d->ucode_dmem; off += NV_FLCN_BLOCK) {
      uint32_t n = d->ucode_dmem - off;
      if (n > NV_FLCN_BLOCK) n = NV_FLCN_BLOCK;
      rc = falcon_pio_block(d, NV_GSP_U, buf + d->ucode_imem + off, n, off, 0, 0);
      if (rc != 0) goto out;
      g_dma_nblk++;
    }
    /* The DMA-side oracle (0x114) is not programmed by this path, so the F1
     * row below would be meaningless: say so instead of printing it. */
    nblk = g_dma_nblk;
    uart_puts("[NV-FWSEC] f1 not applicable this run: the PIO path never writes "
              "0x114, so the IMEM destination cannot be read back even in "
              "principle\n");
  } else {
    /* ---- the ablation (round 2, run r2f) --------------------------------
     *
     * "The status word went to 0 and the core halted" is only evidence that
     * FWSEC ran if the same thing does NOT happen without the code.  IMEM
     * content cannot be read back on this part (the code port answers
     * 0xDEAD5EC1 for every address, and a port write stops the DMA engine), so
     * the load's IMEM half has never been verified.  Set this to 0 to launch
     * with DMEM only: the core then executes whatever IMEM holds.  If 0x1438
     * still goes to 0 and the core still halts, the observation was never about
     * the code; if it does not, the code is what produced it.
     *
     * RESULT (r2f, evidence/nv/r2f-ablation-no-imem.log): with IMEM empty START
     * is refused -- ctl reads 0x10 at 1 us and at 2 s -- and nothing happens:
     * 0x1438 keeps its sentinel, the 0x1400 block does not change, the counter
     * at 0x1418 does not advance.  The signature therefore belongs to the loaded
     * image.  Restored to 1; the ablation is a one-run instrument.
     */
    const int kFwsecLoadImem = 1;
    if (!kFwsecLoadImem) {
      uart_puts("[NV-FWSEC] ABLATION: IMEM is NOT loaded this run (DMEM only): "
                "the status/halt signature is being tested against its own "
                "absence\n");
    }
    for (uint32_t off = 0; kFwsecLoadImem && off < d->ucode_imem;
         off += NV_FLCN_BLOCK) {
      rc = falcon_dma_block(d, NV_GSP_U, phys, off, off, 1, 1);
      if (rc != 0) goto out;
    }
    /* ---- F1 (PLAN.md §2): did the image land in the GSP's own memory? -----
     * Read after the IMEM load and before the DMEM load, which is what the
     * experiment specifies.  0x114 is the falcon-memory side of the transfer,
     * so with 0xF200 bytes in 256-byte blocks the last destination this host
     * asks for is 0xF200-0x100 = 0xF100.  The value the encoder actually
     * passed is logged next to it, and the readability probe above says
     * whether the register can answer at all -- on this card it cannot, which
     * is itself the result: the plan's F1 oracle (0x114) is inside the
     * read-fenced span. */
    if (!kFwsecLoadImem) {
      uart_puts("[NV-FWSEC] f1 not applicable: no IMEM request was issued this "
                "run\n");
      goto after_f1;
    }
    uart_puts("[NV-FWSEC] f1 requested imem dst first=");
    uart_print_hex(g_dma_dst_first);
    uart_puts(" last=");
    uart_print_hex(g_dma_dst_last);
    uart_puts(" blocks=");
    print_int((int)g_dma_nblk);
    uart_puts(" (a landing image leaves 0x114=0xF100)\n");
    nv_xfer_state(d, NV_GSP_U, "post-imem");
    {
      uint32_t a114 = nv_rd32(d, NV_GSP_U + NV_XFER_FALCON_ADDR);
      uint32_t want =
          d->ucode_imem >= NV_FLCN_BLOCK ? d->ucode_imem - NV_FLCN_BLOCK : 0;
      if (a114 == want) {
        uart_puts("[NV-FWSEC] f1 verdict=ECHO 0x114=0xF100 is the host's own "
                  "last destination read back: it says the window is writable "
                  "and the encoder's dst advanced, NOT that the engine honoured "
                  "it.  The landing map below is the oracle\n");
      } else if (a114 == NV_REG_FENCED || a114 == 0xffffffffu) {
        uart_puts("[NV-FWSEC] f1 verdict=MASKED 0x114 reads the inaccessibility "
                  "signature, so this register cannot answer on this card; the "
                  "destination the encoder used is the row above\n");
      } else if (a114 == 0) {
        uart_puts("[NV-FWSEC] f1 verdict=NEGATIVE 0x114=0: every block was "
                  "addressed at falcon offset 0, so the image did not land\n");
      } else {
        uart_puts("[NV-FWSEC] f1 verdict=UNEXPECTED 0x114=");
        uart_print_hex(a114);
        uart_puts(": neither the top of IMEM nor 0 -- judge by the probe "
                  "above\n");
      }
    }
  after_f1:
    for (uint32_t off = 0; off < d->ucode_dmem; off += NV_FLCN_BLOCK) {
      rc = falcon_dma_block(d, NV_GSP_U, phys, d->ucode_imem + off, off, 0, 0);
      if (rc != 0) goto out;
    }
    nblk = g_dma_nblk;
    dst_first = g_dma_dst_first;
    dst_last = g_dma_dst_last;
  }
  uart_puts("[NV-FWSEC] GSP IMEM+DMEM loaded\n");
  uart_puts("[NV-FWSEC] blocks total=");
  print_int((int)nblk);
  uart_puts(" dst first=");
  uart_print_hex(dst_first);
  uart_puts(" last=");
  uart_print_hex(dst_last);
  uart_puts(" last-cmd=");
  uart_print_hex(nv_flcn_xfer_cmd(NV_FLCN_BLOCK, 0, 0));
  uart_puts("\n");

  /* Boot-ROM parameters: the values this image needs, not the booter's. */
  /* ---- did the image land?  Read it back, and say WHERE it is -------------
   *
   * F1's indirect oracle (0x114) is an echo of the host's own write; this is
   * what the falcon's memory actually holds.  Two maps, because the two halves
   * of the image take different paths and a unit error in one need not affect
   * the other: DMEM is non-secure (data port), IMEM is read with the SECRET
   * bit set the same way it was written (code port).
   *
   * Every row prints the word the port answered, the word the staged buffer
   * holds at that offset, and -- on a difference -- whether the answered value
   * is the staged word from `off>>2` or `off<<2`, and where in the image it
   * appears at all.  That is the difference between "MISMATCH", which cost
   * round 1 its conclusion, and "the read answered the image's word for
   * offset/4", which names the defect.
   *
   * Gated on 0x120 being readable: the reads wait for the xfer queue to drain,
   * and on a fenced status register that wait is 20 M iterations of nothing.
   */
  {
    uint32_t st120 = nv_rd32(d, NV_GSP_U + NV_XFER_STATUS);
    if (st120 == NV_REG_FENCED || st120 == 0xffffffffu) {
      uart_puts("[NV-LAND] readback skipped: 0x120 reads ");
      uart_print_hex(st120);
      uart_puts(" (fenced), so the xfer queue cannot be drained and a PIO read "
                "would be a masked read\n");
    } else {
      const uint8_t *dmem_img = buf + d->ucode_imem;
      struct nv_landing dm = {0, 0, 0, 0}, im = {0, 0, 0, 0};
      static const uint32_t dm_probe[] = {0x000, 0x100, 0x400, 0x800,
                                          0xAE0, 0xB0C, 0xB24, 0xD40, 0xD00};
      static const uint32_t im_probe[] = {0x000, 0x1000, 0x8000};
      for (uint32_t i = 0; i < sizeof dm_probe / sizeof dm_probe[0]; i++)
        nv_landing_row(d, gspf, 0, dm_probe[i], dmem_img, d->ucode_dmem, &dm);
      nv_landing_verdict("dmem", &dm);

      /* The code port, three ways.  r2a answered 0xDEAD5EC1 for every address
       * with SECRET set; if the plain page (SECRET clear) or the CODE_VIRT_ADDR
       * pre-write changes that, IMEM becomes verifiable through the port. */
      for (uint32_t i = 0; i < sizeof im_probe / sizeof im_probe[0]; i++)
        nv_imem_read_row(d, gspf, im_probe[i], buf, d->ucode_imem, 1, 0, &im);
      for (uint32_t i = 0; i < sizeof im_probe / sizeof im_probe[0]; i++)
        nv_imem_read_row(d, gspf, im_probe[i], buf, d->ucode_imem, 1, 1, &im);
      for (uint32_t i = 0; i < sizeof im_probe / sizeof im_probe[0]; i++)
        nv_imem_read_row(d, gspf, im_probe[i], buf, d->ucode_imem, 0, 0, &im);
      nv_landing_verdict("imem-port", &im);

      /* And the engine's store direction: falcon memory -> system memory.  This
       * is the only path to IMEM content that is not the refused port.
       *
       * 0x4000 is in the list because of the unit question: if the destination
       * of a *code* DMA were interpreted as words, the block staged for IMEM
       * 0x1000 would sit at 0x4000, and this row would say so by name. */
      int imem_ok = 0;
      {
#if NV_IMEM_STORE_PROBE
        int sm = 0, st = 0;
        if (!imem_scratch) {
          uart_puts("[NV-IMEM] store-block skipped: no contiguous frame\n");
        } else {
          static const uint32_t st_probe[] = {0x000, 0x1000, 0x4000};
          for (uint32_t i = 0; i < sizeof st_probe / sizeof st_probe[0]; i++)
            nv_imem_store_row(d, NV_GSP_U, buf, d->ucode_imem, st_probe[i],
                              imem_scratch, &sm, &st);
        }
#endif
        /* The port's own view of the block the write probe planted: if the
         * pattern is gone, the load wrote that block; if it is still there, the
         * load never did. */
        {
          uint32_t pb = 0, want0 = 0;
          int prc = nv_pio_rd1_nb(d, NV_GSP_U, 1, 0x000, &pb);
          int have_want0 = nv_img_word(buf, d->ucode_imem, 0x000, &want0);
          uart_puts("[NV-IMEM] port @0x000 after load rc=");
          print_int(prc);
          uart_puts(" got=");
          uart_print_hex(pb);
          uart_puts(" staged=");
          uart_print_hex(have_want0 ? want0 : 0);
          if (pb == 0x5EC2C0DEu)
            uart_puts(" STILL-THE-PROBE-PATTERN (the code load did not write "
                      "this IMEM block)\n");
          else if (have_want0 && pb == want0)
            uart_puts(" THE-STAGED-WORD (the load landed and the port can read "
                      "it back)\n");
          else if (pb == NV_REG_IMEM_MARKER)
            uart_puts(" THE-MARKER (the port refuses this read: says nothing "
                      "about the content -- the engine rows are the evidence)\n");
          else
            uart_puts(" SOME OTHER VALUE (a third state: report it as raw)\n");
        }
#if NV_IMEM_STORE_PROBE
        uart_puts("[NV-IMEM] verdict=");
        if (st == 0)
          uart_puts("NO-STORE-PATH (the engine never took a store request)\n");
        else if (sm == st) {
          imem_ok = 1;
          uart_puts("IMEM VERIFIED VIA THE ENGINE (the bytes the engine copies "
                    "back are the staged bytes: the code landed)\n");
        } else
          uart_puts("IMEM COPY DISAGREES WITH THE STAGED BYTES (the rows above "
                    "name the first word)\n");
#endif
      }
      /* A port read that matched would be just as good as the engine copy. */
      if (im.probes > 0 && im.match == im.probes) imem_ok = 1;

      uart_puts("[NV-LAND] verdict=");
      if (!dmem_read_trustworthy) {
        uart_puts("UNTRUSTWORTHY (the DMEM port failed its own write/read "
                  "round-trip in this run, so no read-back row above is "
                  "evidence)\n");
      } else if (dm.match == dm.probes && dm.probes > 0 && imem_ok) {
        uart_puts("IMAGE IS IN THE FALCON'S MEMORY (DMEM word for word through a "
                  "port proved in this run; IMEM either read back or copied back "
                  "byte for byte)\n");
      } else if (dm.match == dm.probes && dm.probes > 0) {
        uart_puts("DMEM IS THE IMAGE, IMEM IS NOT VERIFIED (the code port answers "
                  "0xDEAD5EC1 at every address and the engine's store direction "
                  "is the other channel -- see the [NV-STORE] rows; the load's "
                  "IMEM half rests on the DMA's effect only)\n");
      } else {
        uart_puts("IMAGE IS NOT WHERE IT WAS STAGED FOR (see the DIFF rows: the "
                  "answered values say which unit is wrong)\n");
      }
    }
  }

  /* ---- the store channel, AFTER the launch -------------------------------
   *
   * MEASURED in r3a: a store request is accepted (0x120's stores-pending count
   * went 1,2,3,4 and the queue never went FULL) and never retires -- and with
   * those four slots held, the core never wrote its verdict word at all.  So the
   * matrix is not free before START: it costs the launch.  It runs after the
   * sweep now, where a stuck request can only cost itself.
   */
  int store_variant = -1;

  /* Is a write into the BROM parameter window landing?  That block is the one
   * place the launch contract lists as "never sampled" (its D4), and if writes
   * there are dropped then the Boot ROM verifies nothing and the core stops in
   * its own ROM with no host-visible reason.  The probe writes MOD_SEL's real
   * value first, reads it back, and leaves it in place -- no restore of a
   * possibly-poisoned word. */
  (void)nv_probe_word(d, 0x111180, 0x5A5A5A5Au, 1);
  nv_wr32(d, 0x111210, 0xB24); /* PARAADDR = PKCDataOffset */
  nv_wr32(d, 0x11119C, 0x400); /* ENGIDMASK */
  nv_wr32(d, 0x111198, d->ucode_id ? d->ucode_id : 9);
  nv_wr32(d, 0x111180, 1); /* MOD_SEL = RSA3K */
  nv_wr32(d, NV_GSP_U + 0x104, 0);
  uart_puts("[NV-FWSEC] pre-start 100=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL));
  uart_puts(" 104=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_UC_ENTRY));
  uart_puts(" 118=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_XFER_CTRL));
  uart_puts(" 120=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_XFER_STATUS));
  uart_puts("\n");
  uart_puts("[NV-FWSEC] brom para=0xB24 engid=0x400 ucode=");
  print_int((int)(d->ucode_id ? d->ucode_id : 9));
  uart_puts("\n");

  /* ---- the launch, with a verdict channel that can be judged -------------
   *
   * 0x1438 bits 31:16 is FWSEC's status channel, and a zero there has two
   * readings one sentinel cannot separate: "the ucode wrote 0, i.e. success"
   * and "the core cleared the word on its way down".  Round 1 saw
   * 0xBEEF0000 -> 0 exactly once, after START, while the core still read as
   * running, and could not say which.  Four changes make it decidable:
   *
   *   - the sentinel changes, so no earlier run's leftover can be mistaken for
   *     this run's prefill (0x5EC2 is round 2's marker);
   *   - the sentinel is read BACK after the prefill: a word that will not hold
   *     a sentinel cannot report a write either;
   *   - an unrelated word gets its own sentinel as a control.  MAILBOX0 sits
   *     in the falcon window and nothing in the image writes it (the vendor
   *     passes NULL mailboxes for FWSEC), so if BOTH words clear it is a
   *     region clear and the zero carries no verdict -- and if only 0x1438
   *     clears, the write was to this word;
   *   - the transition is timestamped and correlated with the core state, the
   *     PC and 0x111668, instead of being read once at the end.
   */
  const uint32_t kStale = 0x5EC25A5Au; /* round-2 sentinel: hi16 = 0x5EC2,
                                        * low half non-zero so a write that
                                        * touches only bits 31:16 (which would
                                        * leave 0x00005A5A) is distinguishable
                                        * from a full-word write of 0 */
  nv_wr32(d, 0x1438, kStale);
  /* MAILBOX0 is FWSEC's error channel with 0 meaning "no error"
   * (docs/nv-devinit-contract.md section 3: 0x040 <- 0, then 0x040 == 0 is part
   * of completion).  It is therefore NOT usable as a sentinel control: a
   * non-zero value there is indistinguishable from an error code.  Round 2's
   * first run paid for that lesson; the control is now a read-only snapshot of
   * the whole 0x1400 block, taken before and after, which answers "region
   * clear?" without writing into any register whose meaning is unknown. */
  nv_wr32(d, NV_GSP_U + NV_FALCON_MAILBOX0_FLCN, 0);
  uint32_t pre_1438 = nv_rd32(d, 0x1438);
  uint32_t pre_mb0 = nv_rd32(d, NV_GSP_U + NV_FALCON_MAILBOX0_FLCN);
  uint32_t bcr_pre = nv_rd32(d, NV_GSP_BCR_CTRL);
  uint32_t blk_pre[NV_SEC_STATUS_WORDS];
  nv_snapshot_block(d, blk_pre);
  uart_puts("[NV-FWSEC] guard 1438=");
  uart_print_hex(pre_1438);
  uart_puts("/");
  uart_print_hex(kStale);
  uart_puts(" mb0=");
  uart_print_hex(pre_mb0);
  uart_puts(" bcr_before=");
  uart_print_hex(bcr_pre);
  uart_puts(pre_1438 == kStale
                ? " SENTINEL-STICKS (a no-write is distinguishable from a write)\n"
                : " NOT-STICKY (a word that will not hold a sentinel cannot "
                  "report a write either)\n");

  /* Did the core change its own memory?  A core that never executed cannot; a
   * core that executed anything leaves a stack, a scratch word or a result
   * behind.  Sixteen words of DMEM read now, read again after the wait: an
   * oracle that is about the effect of running, not about a status register.
   *
   * And because the window re-fenced within ~1 ms of START in run r2a, the
   * "after" read alone can only ever say UNOBSERVABLE.  So four words spread
   * across DMEM (two of them past the 0xD80-byte image, where a running ucode's
   * stack or bss would be) are also sampled in the wait loop itself, from the
   * first microsecond after START until the port stops answering. */
  uint32_t dm_pre[16];
  int dm_pre_ok = (nv_falcon_dmem_read(d, gspf, 0x000, dm_pre, 16) == 0);
  /* ---- bracket the fence with the same instrument ------------------------
   *
   * Every earlier round inferred "the window was open until START, then
   * fenced" from two different code paths (a PIO read-back before START, a
   * sampler after it).  Running the identical sweep for 60 ms before START
   * makes the closing edge one observation with one timestamp convention: the
   * last sample that answered a real value, and the first that did not.
   *
   * r3c also spent 80 ms here doing nothing, as the control for run r3a (whose
   * launch never wrote its verdict after issuing four engine store requests
   * between the load and START): the verdict still arrived at +105,788,608 ns,
   * so the pause is exonerated and r3a's silent launch is attributable to the
   * queued store requests.  The pause is not kept -- its question is answered
   * and 140 ms of dead time per boot buys nothing.
   */
  {
    struct nv_launch_obs pre;
    nv_launch_sweep(d, NV_GSP_U, kStale, "pre-start", 60000000ULL, &pre,
                    store_scratch, buf, total, d->ucode_imem, d->ucode_dmem, -1);
    uart_puts("[NV-SWEEP] pre-start guard 1438=");
    uart_print_hex(pre.st);
    uart_puts(pre.st == kStale ? " (sentinel intact before START)\n"
                               : " (NOT the sentinel before START)\n");
  }

  /* START accepted, or refused?  Two samples a decade apart in time: 0x10 at
   * both is a refused start (nothing ran); clear at 1 us and 0x10 later is a
   * core that ran and died (docs/nv-fwsec-launch-contract.md step 13). */
  uint32_t ctl_pre = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL);
  uint32_t pc_pre = nv_rd32(d, NV_GSP_U + NV_FALCON_PC);
  nv_wr32(d, NV_GSP_U + NV_FALCON_UC_CTRL, NV_FALCON_UC_CTRL_START);
  for (volatile int spin = 0; spin < 400; spin++) {
  }
  uint32_t ctl_1us = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL);

  /* The blocking wait is gone.  Round 2's poll broke the moment 0x1438
   * changed, which is 104 ms after START -- so everything the core did in
   * between, including whether the window ever re-opened, was never sampled.
   * nv_launch_sweep() runs the whole 2 s without an early exit and timestamps
   * every change in the four words that describe the core and the window. */
  struct nv_launch_obs obs;
  nv_launch_sweep(d, NV_GSP_U, kStale, "post-start", 2000000000ULL, &obs,
                  store_scratch, buf, total, d->ucode_imem, d->ucode_dmem,
                  store_scratch ? 1 : 0);
  uint32_t st = obs.st;
  uint32_t ctl_10ms = obs.ctl_last;
  int halted = obs.halted, changed = obs.changed;
  uint64_t changed_ns = obs.changed_ns;
  uint32_t ctl_at_change = obs.ctl_at_change, pc_at_change = obs.pc_at_change,
           bcr_at_change = obs.bcr_at_change, mb_at_change = obs.mb_at_change;
  uart_puts("[NV-FWSEC] halt wait done halted=");
  print_int(halted);
  uart_puts(" sentinel_changed=");
  print_int(changed);
  uart_puts("\n");
  if (changed) {
    uart_puts("[NV-FWSEC] sentinel 0x5EC20000 -> ");
    uart_print_hex(st);
    uart_puts(" at +");
    uart_print_hex(changed_ns);
    uart_puts(" ns ctl=");
    uart_print_hex(ctl_at_change);
    uart_puts(" pc=");
    uart_print_hex(pc_at_change);
    uart_puts(" bcr=");
    uart_print_hex(bcr_at_change);
    uart_puts(" mb0=");
    uart_print_hex(mb_at_change);
    uart_puts("\n");
  }

  /* ---- what FWSEC left in VRAM, read on the channel the fence misses ----
   *
   * Run r4a's first shape of this block reported "every word of the 1 MiB
   * changed, therefore FWSEC wrote 1 MiB", and the log itself refutes that:
   * the head dump read 0xBAD0AC0A at offset 0 and the next dump, 32 reads
   * later, read 0xBAD0AC2A at the *same* offset.  An address that answers a
   * different value on every access is a counter, not memory (PLAN.md section
   * 6, rule 1: a read that cannot be negative is not evidence).
   *
   * So every post-launch conclusion now starts with nv_vram_behaviour(), which
   * asks the same address three questions: eight reads in a row (memory answers
   * the same value), two addresses alternately (a shared counter means the
   * address is ignored), and a write read back (a page that will not hold a
   * value cannot report a firmware write either).  Only a window that answers
   * memory gets a scan.
   *
   * The probes cover four offsets on purpose, so the boundary of whatever
   * happened is measured and not assumed: the FRTS region itself, the bootbin
   * page inside the same WPR region, the metadata copy page at the WPR base,
   * and a scratch page 134 MiB below the WPR that no firmware is told about.
   */
  if (vram.p && vram_len) {
    uint64_t frts64 = (uint64_t)frts_off * 4096u;
    /* The four boundary offsets, so the extent of the poison is measured and
     * not assumed: the last 4 KiB page below the region, the first page of it,
     * its last page, and the first page above it.  A poison that starts exactly
     * at frtsOffset and ends exactly at frtsOffset+frtsSize is the region the
     * FRTS command was handed, which is what makes the next run's control
     * (fault-injected init_cmd) worth a deploy. */
    int mem_below = nv_vram_behaviour(d, frts64 - 0x1000u, "below-frts-post");
    int mem_frts = nv_vram_behaviour(d, frts64, "frts-post");
    int mem_frts_last = nv_vram_behaviour(d, frts64 + vram_len - 0x1000u,
                                          "frts-last-post");
    uint64_t meta_off = wpr_start ? wpr_start : 0x5F8700000ULL;
    int mem_above = (frts64 + vram_len == meta_off)
                        ? -1
                        : nv_vram_behaviour(d, frts64 + vram_len,
                                            "above-frts-post");
    int mem_boot = nv_vram_behaviour(d, 0x5FE2F8000ULL, "bootbin-post");
    int mem_meta = nv_vram_behaviour(d, meta_off, "wpmeta-post");
    int mem_scratch = nv_vram_behaviour(d, 0x5F0000000ULL, "scratch-post");

    if (mem_frts == 1) {
      struct nv_vram_scan post;
      int src = nv_vram_scan_window(&vram, 0, vram_len / 4u, kVramSentinel,
                                    &post);
      int eff = nv_vram_judge(&vscan_pre, &post);
      uart_puts("[NV-BAR1] frts post-scan rc=");
      print_int(src);
      uart_puts(" words=");
      uart_print_hex(post.words);
      uart_puts(" untouched=");
      uart_print_hex(post.sentinel);
      uart_puts(" changed=");
      uart_print_hex(post.other);
      uart_puts(" zero=");
      uart_print_hex(post.zero);
      uart_puts(" sum=");
      uart_print_hex(post.sum);
      uart_puts(" (pre sum=");
      uart_print_hex(vscan_pre.sum);
      uart_puts(")\n");
      uart_puts("[NV-BAR1] frts verdict=");
      uart_puts(nv_vram_effect_name(eff));
      uart_puts(eff == NV_VRAM_EFFECT_NOTHING
                    ? " (every word is still the host's pattern: FWSEC wrote "
                      "nothing into the region in this run)\n"
                    : (eff == NV_VRAM_EFFECT_WROTE
                           ? " (words the host prefilled are something else now: "
                             "see the first-change row)\n"
                           : " (see the raw counts: no verdict read into them)\n"));
      if (post.other) {
        uart_puts("[NV-BAR1] first change @frts+0x");
        uart_print_hex(post.first_other_off);
        uart_puts(" = ");
        uart_print_hex(post.first_other_val);
        uart_puts("\n");
      }
    } else if (mem_frts == 2) {
      uart_puts("[NV-BAR1] frts post-scan skipped: reads are stable but a write "
                "does not read back, so 'changed' and 'a constant poison' look "
                "identical at this offset (verdict=AMBIGUOUS, no claim)\n");
    } else {
      uart_puts("[NV-BAR1] frts post-scan skipped: the region answers an access "
                "counter after START, so no word in it is a readable datum "
                "(verdict=FENCED-ON-BAR1, poison 0xBAD0AC0A + n)\n");
    }

    /* The metadata copy the booter leaves at gspFwWprStart.  Its magic and its
     * `verified` word are the only VRAM-side trace of the booter -- i.e. of the
     * privilege drop having happened -- and SEC2 writes them, so absence this
     * run is expected and presence would be news.  Read only when the page
     * answers memory: zeros from a poisoned page are not "no copy". */
    if (wpr_start && mem_meta == 1) {
      struct nv_vram_win mw;
      if (nv_vram_open(d, wpr_start, 256, &mw) == 0) {
        uint64_t magic = ((uint64_t)nv_vram_rd32(&mw, 4u) << 32) |
                         (uint64_t)nv_vram_rd32(&mw, 0u);
        uint32_t ver_lo = nv_vram_rd32(&mw, NV_WPR_OFF_VERIFIED);
        uint32_t ver_hi = nv_vram_rd32(&mw, NV_WPR_OFF_VERIFIED + 4);
        uint32_t nz = 0;
        for (uint32_t i = 0; i < 64; i++)
          if (nv_vram_rd32(&mw, i * 4u)) nz++;
        uart_puts("[NV-BAR1] wpmeta @");
        uart_print_hex(wpr_start);
        uart_puts(" magic=");
        uart_print_hex(magic);
        uart_puts(" verified_hi=");
        uart_print_hex(ver_hi);
        uart_puts("_lo=");
        uart_print_hex(ver_lo);
        uart_puts(" nonzero_words=");
        uart_print_hex(nz);
        if (magic == 0xDC3AAE21371A60B3ULL && ver_hi == 0xA0A0A0A0u &&
            ver_lo == 0xA0A0A0A0u)
          uart_puts(" verdict=BOOTER ACCEPTED THE META (the privilege drop "
                    "happened: SEC2 ran)\n");
        else if (magic == 0xDC3AAE21371A60B3ULL)
          uart_puts(" verdict=META COPY PRESENT, verified NOT SET (something "
                    "copied the block; no booter acceptance)\n");
        else
          uart_puts(" verdict=NO META COPY AT gspFwWprStart (absent magic: "
                    "consistent with the booter never running, which the SEC2 "
                    "rows already report)\n");
        nv_vram_close(&mw);
      }
    } else if (wpr_start) {
      uart_puts("[NV-BAR1] wpmeta read skipped: the page does not answer as "
                "memory after START\n");
    }

    /* One summary row, so the shape of the fence is one line in the report:
     * which of the four offsets still answered memory after START. */
    uart_puts("[NV-BAR1] post-start map below=");
    uart_puts(mem_below == 1 ? "MEMORY" : (mem_below == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" frts-first=");
    uart_puts(mem_frts == 1 ? "MEMORY" : (mem_frts == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" frts-last=");
    uart_puts(mem_frts_last == 1 ? "MEMORY"
                                 : (mem_frts_last == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" above=");
    uart_puts(mem_above < 0 ? "same-page"
                            : (mem_above == 1 ? "MEMORY"
                                              : (mem_above == 2 ? "AMBIGUOUS"
                                                                : "POISON")));
    uart_puts(" bootbin=");
    uart_puts(mem_boot == 1 ? "MEMORY" : (mem_boot == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" wpmeta=");
    uart_puts(mem_meta == 1 ? "MEMORY" : (mem_meta == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" scratch=");
    uart_puts(mem_scratch == 1 ? "MEMORY"
                               : (mem_scratch == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" halt_ns=");
    uart_print_hex(changed_ns);
    uart_puts(" halted=");
    print_int(halted);
    uart_puts("\n");

    /* Page granularity at both edges.  'M' = the page answers as memory, 'P' =
     * it answers a sequence, 'n' = neither.  Nine pages at each edge, four
     * outside and four inside plus the boundary page, so "the fence covers this
     * 1 MiB and nothing else" is a count and not a claim. */
    {
      char pm[10];
      if (nv_vram_page_map(d, frts64 - 4u * 0x1000u, 9, pm) == 0) {
        uart_puts("[NV-BAR1] edge lo pages ");
        uart_print_hex(frts64 - 4u * 0x1000u);
        uart_puts("..+0x9000: ");
        uart_puts(pm);
        uart_puts("\n");
      }
      if (nv_vram_page_map(d, frts64 + vram_len - 5u * 0x1000u, 9, pm) == 0) {
        uart_puts("[NV-BAR1] edge hi pages ");
        uart_print_hex(frts64 + vram_len - 5u * 0x1000u);
        uart_puts("..+0x9000: ");
        uart_puts(pm);
        uart_puts("\n");
      }
    }
    nv_vram_close(&vram);
  } else {
    uart_puts("[NV-BAR1] post-scan skipped: the VRAM channel is not open\n");
  }

  /* ---- the store matrix, now that the launch has said its piece ---------- */
  {
    static const struct nv_store_probe probes[3] = {
        /* The field-swap hypothesis: sysmem offset to 0x114, falcon address to
         * 0x11C.  r3a's four encodings all queued and never retired, and no
         * encoding of 0x118 can express "the sides swap with the direction". */
        {NV_FLCN_STORE_V_PLAIN, 1, 0, 0, 0x000},
        /* Two contexts that were never tried: 1 and 4.  ctx 5 pointed at a
         * TRANSCFG slot this driver never configured; 1 and 4 are the other
         * plausible coherent targets. */
        {NV_FLCN_STORE_V_CTX1, 0, 0, 0, 0x400},
        {NV_FLCN_STORE_V_CTX4, 0, 0, 0, 0x800},
    };
    if (store_scratch) {
      store_variant = nv_store_matrix(d, NV_GSP_U, store_scratch, buf, total,
                                      d->ucode_imem, d->ucode_dmem, "post",
                                      probes, 3);
      uart_puts("[NV-STORE] post winner=");
      uart_puts(store_variant >= 0
                    ? nv_flcn_store_name(store_variant)
                    : "none (no store variant put bytes in host memory)");
      uart_puts("\n");
    } else {
      uart_puts("[NV-STORE] post skipped: no contiguous landing frame\n");
    }
  }

  uart_puts("[NV-FWSEC] after start ctl_1us=");
  uart_print_hex(ctl_1us);
  uart_puts(" ctl_late=");
  uart_print_hex(ctl_10ms);
  uart_puts(" ctl_before=");
  uart_print_hex(ctl_pre);
  uart_puts(" pc_before=");
  uart_print_hex(pc_pre);
  uart_puts("\n[NV-FWSEC] ctl reading: 0x10 at both samples = START refused; "
            "clear then 0x10 = ran and died; clear at both = the START trigger "
            "was consumed and the core is running (or the read is masked, which "
            "0x1438's behaviour in the same window argues against)\n");

  /* ---- the effect oracle: did the core touch its own memory? ------------ */
  {
    uint32_t dm_post[16];
    int post_ok = 1;
    for (uint32_t i = 0; i < 16; i++)
      if (nv_pio_rd1_nb(d, NV_GSP_U, 0, i * 4u, &dm_post[i]) != 0) post_ok = 0;
    uint32_t first = 0xffffffffu, got = 0, was = 0;
    int diffs = 0;
    if (dm_pre_ok && post_ok) {
      for (uint32_t i = 0; i < 16; i++) {
        if (dm_pre[i] != dm_post[i]) {
          if (!diffs) {
            first = i * 4u;
            was = dm_pre[i];
            got = dm_post[i];
          }
          diffs++;
        }
      }
    }
    uart_puts("[NV-FWSEC] dmem-effect pre_ok=");
    print_int(dm_pre_ok);
    uart_puts(" post_ok=");
    print_int(post_ok);
    uart_puts(" changed_words=");
    print_int(diffs);
    if (diffs) {
      uart_puts(" first@0x");
      uart_print_hex(first);
      uart_puts(" was=");
      uart_print_hex(was);
      uart_puts(" now=");
      uart_print_hex(got);
    }
    if (!dm_pre_ok || !post_ok)
      uart_puts(" verdict=UNOBSERVABLE (the port did not answer; this is not "
                "'unchanged')\n");
    else if (dm_post[0] == NV_REG_FENCED)
      uart_puts(" verdict=UNOBSERVABLE (the window re-fenced during the launch: "
                "the 'after' read is the fence word)\n");
    else if (diffs)
      uart_puts(" verdict=DMEM CHANGED (the core executed: it wrote memory the "
                "host loaded and never touched again)\n");
    else
      uart_puts(" verdict=DMEM UNCHANGED (nothing executed in this window, or "
                "the ucode touched none of it)\n");
  }

  /* Every word a later experiment would want, taken in the same run as the
   * start: mailbox pair, entry point, and the two secondary oracles. */
  uart_puts("[NV-FWSEC] oracles 040=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_MAILBOX0_FLCN));
  uart_puts(" 044=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_MAILBOX1_FLCN));
  uart_puts(" 104=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_UC_ENTRY));
  uart_puts(" 240=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_SEC_MODE));
  uart_puts(" 148=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x148));
  uart_puts(" 14c=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x14C));
  uart_puts("\n");
  /* If the core is still running, ask it where: TRACEPC (SCTL 0x240, index
   * 0x148, frame 0x14c).  On a fenced window these read the fence word and the
   * row says so; on a live one they name the loop the core is sitting in, which
   * is the only remaining host-side view of a core that will not halt. */
  if (!halted) {
    uint32_t sctl = nv_rd32(d, NV_GSP_U + NV_FALCON_SEC_MODE);
    uint32_t tidx = nv_rd32(d, NV_GSP_U + 0x148);
    uart_puts("[NV-FWSEC] tracepc sctl=");
    uart_print_hex(sctl);
    uart_puts(" tidx=");
    uart_print_hex(tidx);
    uint32_t nr = (tidx >> 16) & 0xffu;
    if (nr > 8u) nr = 8u;
    for (uint32_t sp = 0; sp < nr; sp++) {
      nv_wr32(d, NV_GSP_U + 0x148, sp);
      uart_puts(" f");
      print_int((int)sp);
      uart_puts("=");
      uart_print_hex(nv_rd32(d, NV_GSP_U + 0x14C));
    }
    uart_puts("\n");
  }

  /* F3 (PLAN.md §2): does the GSP report itself in RISC-V mode?  0x111 is
   * VALID|RISCV|BRFETCH and is only written after a successful FWSEC, so a
   * pre-launch 0xBADF5620 here and 0x111 later is the whole evidence chain. */
  {
    uint32_t bcr = nv_rd32(d, NV_GSP_BCR_CTRL);
    uart_puts("[NV-FWSEC] f3 bcr_ctrl(0x111668)=");
    uart_print_hex(bcr);
    if (bcr == NV_REG_FENCED)
      uart_puts(" verdict=fenced (unreadable: no RISC-V evidence either way)\n");
    else if ((bcr & NV_GSP_BCR_VALID) && (bcr & NV_GSP_BCR_RISCV))
      uart_puts(" verdict=RISCV-MODE (VALID|RISCV set: the switch was taken and "
                "the core got that far)\n");
    else
      uart_puts((bcr & NV_GSP_BCR_VALID)
                    ? " verdict=core-select set but RISC-V clear: the switch was "
                      "not taken\n"
                    : " verdict=NOT-SWITCHED (VALID clear; the RISCV/BRFETCH bits "
                      "being set is a mode selection, not the switch -- 0x111 is "
                      "the whole value a taken switch reads back as)\n");
  }
  /* F4 (PLAN.md §2): the secondary oracle -- FWSEC's own job was to drop the
   * privilege level so SEC2 answers.  The pass condition is "not the fence
   * word", never a particular value. */
  {
    uint32_t s100 = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
    uint32_t s000 = nv_rd32(d, NV_FALCON_SEC2_BASE);
    uart_puts("[NV-FWSEC] f4 sec2 0x840100=");
    uart_print_hex(s100);
    uart_puts(" 0x840000=");
    uart_print_hex(s000);
    if (s100 == NV_REG_FENCED)
      uart_puts(" verdict=STILL FENCED (FWSEC's job was not done)\n");
    else
      uart_puts(" verdict=UNFENCED (the privilege level dropped: FWSEC ran)\n");
  }
  /* ---- classify the verdict word ----------------------------------------
   *
   * Two questions, both answered by measurement rather than by a code value:
   *   (1) was 0x1438 written, or was the region cleared?  The 0x1400 block
   *       snapshot taken before the launch is the control -- if a neighbour
   *       changed too, it was a clear, and the zero carries no verdict.
   *   (2) did the ucode do its work?  SEC2 unfencing, the halt, and the DMEM
   *       sample rows are the effect oracles; this row prints them as they are,
   *       including the case where they are unobservable.
   */
  {
    uint32_t mb0_end = nv_rd32(d, NV_GSP_U + NV_FALCON_MAILBOX0_FLCN);
    uint32_t sec2 = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
    uint32_t blk_post[NV_SEC_STATUS_WORDS];
    nv_snapshot_block(d, blk_post);
    int first_diff = -1;
    int ndiff = nv_compare_block(blk_pre, blk_post, &first_diff);
    int wrote_specific = 0;
    uart_puts("[NV-FWSEC] status1438=");
    uart_print_hex(st);
    uart_puts(" mb0=");
    uart_print_hex(mb0_end);
    uart_puts(" sec2_ctl=");
    uart_print_hex(sec2);
    uart_puts("\n[NV-FWSEC] block1400 after:");
    for (uint32_t i = 0; i < NV_SEC_STATUS_WORDS; i++) {
      uart_puts(" ");
      uart_print_hex(blk_post[i]);
    }
    uart_puts(" changed_words=");
    print_int(ndiff);
    if (ndiff) {
      uart_puts(" first@0x");
      uart_print_hex(NV_SEC_STATUS_BLOCK + (uint32_t)first_diff * 4u);
      uart_puts(" was=");
      uart_print_hex(blk_pre[first_diff]);
    }
    uart_puts("\n");

    if (st == kStale) {
      uart_puts("[NV-FWSEC] verdict=NEVER WRITTEN (the 0x5ec25a5a sentinel is "
                "still there: nothing wrote this word, so there is no code to "
                "read)\n");
    } else if ((st >> 16) != 0) {
      uart_puts("[NV-FWSEC] verdict=ERROR CODE WRITTEN (hi16=");
      uart_print_hex(st >> 16);
      uart_puts(": the ucode reported a failure; the code values are not "
                "published)\n");
    } else if (st == 0x00005A5Au) {
      uart_puts("[NV-FWSEC] verdict=HI16 WRITTEN, LOW HALF KEPT (0x5a5a "
                "survived: the writer set bits 31:16 to 0 and did not clear the "
                "word -- a status write, not a clear)\n");
      wrote_specific = 1;
    } else if (st == 0) {
      int only_1438 = (ndiff == 1 && first_diff == 14); /* 0x1400 + 14*4 */
      if (only_1438) {
        uart_puts("[NV-FWSEC] verdict=FULL-WORD WRITE TO 0x1438 (the value went "
                  "to 0 and no other word of the 0x1400 block changed: a "
                  "targeted write, not a region clear).  A success code of 0 and "
                  "a deliberate clear of that word are the same value; the effect "
                  "oracles are what separates them\n");
        wrote_specific = 1;
      } else if (ndiff == 0) {
        uart_puts("[NV-FWSEC] verdict=INCONSISTENT (0x1438 now reads 0 but the "
                  "block matches the pre-launch snapshot word for word: re-read "
                  "before believing either)\n");
      } else {
        uart_puts("[NV-FWSEC] verdict=MORE THAN ONE WORD OF 0x1400 CHANGED (the "
                  "zero arrived with other writes in the same block, so 'a clear "
                  "of that word' and 'the ucode writing a status' are not "
                  "separated by this row: see changed_words above)\n");
        wrote_specific = 1; /* a targeted write is still a write; the row says which */
      }
    } else {
      uart_puts("[NV-FWSEC] verdict=UNEXPECTED VALUE (neither the sentinel, an "
                "error code, nor 0: judge by the raw rows)\n");
    }
    uart_puts(sec2 != NV_REG_FENCED ? "[NV-FWSEC] SEC2=UNFENCED (privilege "
                                      "dropped: FWSEC did its job)\n"
                                    : "[NV-FWSEC] SEC2=still fenced\n");
    rc = wrote_specific ? 0 : -EPERM;
  }
out:
  if (imem_scratch) frame_free(imem_scratch);
  if (store_scratch) frame_free(store_scratch);
  for (uint32_t i = 0; i < pages; i++) frame_free(phys + (uint64_t)i * FRAME_SIZE);
  return rc;
}

/* `force` = run the attempt even when the SEC2 window refuses to be read. */
int nv_gsp_sec2_start_booter_ex(struct nv_dev *d, const uint8_t *booter,
                                uint32_t len, uint64_t meta_phys, int force);

static uint64_t nv_meta_get64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

/* A little-endian dword out of a firmware image (never a wide load: the buffer
 * the loader hands back is bytes, and this kernel parses wire data byte-wise). */
static uint32_t nv_img_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void nv_meta_put64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)v;
    v >>= 8;
  }
}

int nv_gsp_sec2_start_booter(struct nv_dev *d, const uint8_t *booter,
                             uint32_t len, uint64_t meta_phys) {
  return nv_gsp_sec2_start_booter_ex(d, booter, len, meta_phys, 0);
}

/* ---- the booter's VRAM receipt, before and after the attempt ------------
 *
 * Round 4 turned BAR1 into an instrument for FWSEC: prefill a region the
 * firmware would write into, then count what changed -- and judge the channel
 * itself first, because a page that answers an access counter would report
 * "1 MiB changed" for anything.  The booter needs the same instrument on TWO
 * regions, and a third reading that costs nothing:
 *
 *   bootbin  [boot_bin_offset, +0x8000): where the booter DMAs the bootloader's
 *            data region (meta.sysmem_addr_bootloader, size 0x8000).  Prefilled
 *            with the host's pattern and, after the attempt, compared word for
 *            word against those same file bytes -- a fingerprint, not a count.
 *   wpmeta   [gspFwWprStart, +0x100): where the booter leaves its copy of the
 *            256-byte block.  Prefilled, so "still my pattern" is an
 *            observation and a magic+verified read is a find.
 *   behaviour  before and after for both pages.  They answer memory before the
 *            attempt (round 4 measured exactly that); if they answer a poison
 *            afterwards, the WPR2 extent went from FRTS's 1 MiB to the whole
 *            WPR, and the only actor that programs WPR2 is the booter.
 *
 * None of this reads a falcon register, so the SEC2 fence cannot hide any of it.
 */
#define NV_BOOTBIN_LEN 0x8000u
#define NV_WPMETA_LEN 0x100u

struct nv_booter_vram {
  struct nv_vram_win boot;
  struct nv_vram_win meta;
  struct nv_vram_scan boot_pre;
  struct nv_vram_scan meta_pre;
  int boot_ok;
  int meta_ok;
  uint64_t boot_off, meta_off;
};

static int nv_booter_vram_prefill(struct nv_dev *d, struct nv_booter_vram *s,
                                  uint64_t boot_off, uint64_t meta_off) {
  const uint32_t kS = 0xA5A5A5A5u;
  for (uint32_t i = 0; i < (uint32_t)sizeof *s; i++) ((uint8_t *)s)[i] = 0;
  s->boot_off = boot_off;
  s->meta_off = meta_off;
  int bb = nv_vram_behaviour(d, boot_off, "bootbin-pre");
  int mb = nv_vram_behaviour(d, meta_off, "wpmeta-pre");

  if (nv_vram_open(d, boot_off, NV_BOOTBIN_LEN, &s->boot) == 0) {
    nv_vram_fill(&s->boot, 0, NV_BOOTBIN_LEN / 4u, kS);
    nv_vram_scan_window(&s->boot, 0, NV_BOOTBIN_LEN / 4u, kS, &s->boot_pre);
    s->boot_ok = (s->boot_pre.sentinel == s->boot_pre.words &&
                  s->boot_pre.other == 0);
  }
  if (nv_vram_open(d, meta_off, NV_WPMETA_LEN, &s->meta) == 0) {
    nv_vram_fill(&s->meta, 0, NV_WPMETA_LEN / 4u, kS);
    nv_vram_scan_window(&s->meta, 0, NV_WPMETA_LEN / 4u, kS, &s->meta_pre);
    s->meta_ok = (s->meta_pre.sentinel == s->meta_pre.words &&
                  s->meta_pre.other == 0);
  }
  uart_puts("[NV-SEC2] bootbin prefill off=");
  uart_print_hex(boot_off);
  uart_puts(" len=");
  uart_print_hex(NV_BOOTBIN_LEN);
  uart_puts(" behaviour=");
  uart_puts(bb == 1 ? "MEMORY" : (bb == 2 ? "AMBIGUOUS" : "POISON"));
  uart_puts(" words=");
  uart_print_hex(s->boot_pre.words);
  uart_puts(" stuck=");
  uart_print_hex(s->boot_pre.sentinel);
  uart_puts(" changed=");
  uart_print_hex(s->boot_pre.other);
  uart_puts(" wpmeta_prefill behaviour=");
  uart_puts(mb == 1 ? "MEMORY" : (mb == 2 ? "AMBIGUOUS" : "POISON"));
  uart_puts(" stuck=");
  uart_print_hex(s->meta_pre.sentinel);
  uart_puts("/");
  uart_print_hex(s->meta_pre.words);
  uart_puts((s->boot_ok && s->meta_ok)
                ? " verdict=INSTRUMENT-ARMED (a later non-sentinel word is the "
                  "booter's write, and 'none' is an observation)\n"
                : " verdict=NOT-ARMED (a prefill did not stick: that region "
                  "cannot report a write in this run)\n");
  /* Extent, before: 10 pages starting one below the region, so the map after
   * the attempt has a control page on the left. */
  {
    char pm[12];
    if (nv_vram_page_map(d, boot_off - 0x1000u, 10, pm) == 0) {
      uart_puts("[NV-SEC2] bootbin pages before ");
      uart_print_hex(boot_off - 0x1000u);
      uart_puts("..+0xA000: ");
      uart_puts(pm);
      uart_puts("\n");
    }
  }
  return (s->boot_ok && s->meta_ok) ? 0 : -EIO;
}

static void nv_booter_vram_post(struct nv_dev *d, struct nv_booter_vram *s,
                                const uint8_t *expect, uint32_t expect_len) {
  const uint32_t kS = 0xA5A5A5A5u;
  int bb = nv_vram_behaviour(d, s->boot_off, "bootbin-post");
  if (bb == 1 && s->boot.p) {
    struct nv_vram_scan post;
    if (nv_vram_scan_window(&s->boot, 0, NV_BOOTBIN_LEN / 4u, kS, &post) == 0) {
      uint32_t mw = 0, bad_off = 0, bad_val = 0;
      int mrc = -EINVAL;
      if (expect && expect_len >= NV_BOOTBIN_LEN)
        mrc = nv_vram_match(&s->boot, 0, expect, NV_BOOTBIN_LEN, &mw, &bad_off,
                            &bad_val);
      int v = nv_bootbin_verdict(&s->boot_pre, &post, mw);
      uart_puts("[NV-SEC2] bootbin post words=");
      uart_print_hex(post.words);
      uart_puts(" untouched=");
      uart_print_hex(post.sentinel);
      uart_puts(" changed=");
      uart_print_hex(post.other);
      uart_puts(" zero=");
      uart_print_hex(post.zero);
      uart_puts(" match_file=");
      uart_print_hex(mw);
      uart_puts("/");
      uart_print_hex(post.words);
      uart_puts(" match_rc=");
      print_int(mrc);
      uart_puts(" verdict=");
      uart_puts(nv_bootbin_verdict_name(v));
      uart_puts(v == NV_BOOTBIN_MATCHED
                    ? " (the bootloader's own bytes are in VRAM: the booter "
                      "ran far enough to copy it)\n"
                    : (v == NV_BOOTBIN_UNTOUCHED
                           ? " (every word is still the host's pattern: no "
                             "booter DMA into this region in this run)\n"
                           : (v == NV_BOOTBIN_BLIND
                                  ? " (the prefill did not survive: no "
                                    "verdict either way)\n"
                                  : " (the region changed, but not to the "
                                    "bootloader's bytes: raw counts above)\n")));
      if (v == NV_BOOTBIN_OTHER && mrc == 0) {
        uart_puts("[NV-SEC2] bootbin first mismatch @");
        uart_print_hex(bad_off);
        uart_puts(" reads=");
        uart_print_hex(bad_val);
        uart_puts("\n");
      }
    }
  } else {
    uart_puts("[NV-SEC2] bootbin post: the page ");
    uart_puts(bb == 2 ? "reads stable but will not hold a value"
                      : "answers a poison (not memory)");
    uart_puts(" after the attempt, having answered MEMORY before it: "
              "verdict=CARVED (the host lost this page, which is what a WPR2 "
              "over the whole WPR looks like from BAR1 -- only the booter "
              "programs it)\n");
  }

  /* The metadata copy the booter leaves at gspFwWprStart. */
  {
    int mb = nv_vram_behaviour(d, s->meta_off, "wpmeta-post");
    if (mb == 1 && s->meta.p) {
      uint64_t magic = ((uint64_t)nv_vram_rd32(&s->meta, 4u) << 32) |
                       (uint64_t)nv_vram_rd32(&s->meta, 0u);
      uint32_t nz = 0, sent = 0;
      for (uint32_t i = 0; i < NV_WPMETA_LEN / 4u; i++) {
        uint32_t w = nv_vram_rd32(&s->meta, i * 4u);
        if (w) nz++;
        if (w == kS) sent++;
      }
      uint32_t vlo = nv_vram_rd32(&s->meta, NV_WPR_OFF_VERIFIED);
      uint32_t vhi = nv_vram_rd32(&s->meta, NV_WPR_OFF_VERIFIED + 4u);
      uart_puts("[NV-SEC2] wpmeta post off=");
      uart_print_hex(s->meta_off);
      uart_puts(" magic=");
      uart_print_hex(magic);
      uart_puts(" verified_hi=");
      uart_print_hex(vhi);
      uart_puts("_lo=");
      uart_print_hex(vlo);
      uart_puts(" nonzero=");
      uart_print_hex(nz);
      uart_puts(" sentinel=");
      uart_print_hex(sent);
      uart_puts("/");
      uart_print_hex(NV_WPMETA_LEN / 4u);
      if (magic == 0xDC3AAE21371A60B3ULL)
        uart_puts(" verdict=META COPY PRESENT (the booter DMA'd the block into "
                  "VRAM; verified word above is its acceptance)\n");
      else if (sent == NV_WPMETA_LEN / 4u)
        uart_puts(" verdict=STILL THE HOST'S PATTERN (no meta copy: the booter "
                  "did not reach this step)\n");
      else
        uart_puts(" verdict=NEITHER (the page holds something else: report it "
                  "as raw)\n");
    } else {
      uart_puts("[NV-SEC2] wpmeta post: ");
      uart_puts(mb == 2 ? "AMBIGUOUS" : "POISON");
      uart_puts(" after the attempt, MEMORY before it: verdict=CARVED (same "
                "reading as the bootbin page)\n");
    }
  }

  {
    char pm[12];
    if (nv_vram_page_map(d, s->boot_off - 0x1000u, 10, pm) == 0) {
      uart_puts("[NV-SEC2] bootbin pages after  ");
      uart_print_hex(s->boot_off - 0x1000u);
      uart_puts("..+0xA000: ");
      uart_puts(pm);
      uart_puts("  (M=memory P=poison; the map before the attempt is the "
                "control)\n");
    }
  }
}

/* ---- SEC2's RISC-V core select ----------------------------------------
 *
 * Round 1's F3 fixed the GSP path with exactly this write: 0x111668 (the RISC-V
 * BCR control, GSP-base + 0x1668) reads 0x110 -- RISCV|BRFETCH, VALID clear --
 * and "a START issued while the core is selected as RISC-V need not reach the
 * falcon core at all".  Clearing it is what made FWSEC runnable.
 *
 * SEC2 carries the same block at the same offset inside its own window
 * (0x840000 + 0x1668).  The SEC2 RISC-V base is INFERRED by symmetry in
 * docs/nv-gsp-wpr-contract.md 4.2; this row is the measurement of that
 * inference, and both outcomes are worth a line:
 *   readable + RISCV set -> clear it, and the SEC2 window may answer (which
 *     turns the booter from inferred into observed)
 *   fenced either side    -> the same write round 1 proved is still issued, and
 *     the attempt proceeds; the row says the window stayed blind.
 */
static void sec2_core_select(struct nv_dev *d) {
  const uint32_t kBcr = NV_FALCON_SEC2_BASE + 0x1668;
  uint32_t bcr = nv_rd32(d, kBcr);
  uart_puts("[NV-SEC2] core-select @");
  uart_print_hex(kBcr);
  uart_puts("=");
  uart_print_hex(bcr);
  if (bcr == NV_REG_FENCED || bcr == 0xffffffffu) {
    nv_wr32(d, kBcr, 0);
    for (int i = 0; i < 100000; i++) {
      bcr = nv_rd32(d, kBcr);
      if (bcr & NV_GSP_BCR_VALID) break;
    }
    uint32_t ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
    uart_puts(" fenced before and after the clear (now=");
    uart_print_hex(bcr);
    uart_puts(") verdict=CLEAR-UNOBSERVABLE (the write is the one round 1 "
              "proved on the GSP); ctl after=");
    uart_print_hex(ctl);
    uart_puts(ctl == NV_REG_FENCED ? " still-fenced\n" : " READABLE\n");
    return;
  }
  if (bcr & NV_GSP_BCR_RISCV) {
    nv_wr32(d, kBcr, 0);
    uint32_t b2 = bcr;
    for (int i = 0; i < 100000; i++) {
      b2 = nv_rd32(d, kBcr);
      if (b2 & NV_GSP_BCR_VALID) break;
    }
    uart_puts(" RISCV-SELECTED -> cleared: ");
    uart_print_hex(b2);
    uart_puts((b2 & NV_GSP_BCR_VALID)
                  ? " verdict=VALID (the falcon core is now the selected "
                    "core)\n"
                  : " verdict=VALID-NEVER-SET (the clear did not take)\n");
  } else {
    uart_puts(" already falcon-selected: no clear needed\n");
  }
  {
    uint32_t ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
    uart_puts("[NV-SEC2] ctl after core-select=");
    uart_print_hex(ctl);
    uart_puts(ctl == NV_REG_FENCED
                  ? " (the fence stands, though the select word answered)\n"
                  : " verdict=READABLE (the SEC2 window answers)\n");
  }
}

/* ---- one SEC2 attempt, end to end -------------------------------------
 *
 * Everything from "put the signature in" to "did the metadata move", in one
 * place so the same code can run twice with one variable between the runs: the
 * 384 signature bytes.  That is the only choice this driver makes that the
 * container does not state (it carries two slots and no SignatureVersions
 * dword), and the fuse rule that picks between them is inherited from FWSEC --
 * where the descriptor *did* carry the mask.  Rather than spend a deploy per
 * guess, attempt A uses the fuse-derived slot and attempt B the other one, in
 * one boot, with identical image and identical transport.  If A works B never
 * runs; if neither works the slot is ruled out in one run.
 *
 * DMEM is loaded by PIO here, for both attempts, so that the transport cannot
 * differ between them: the index register plus the data register, one word at a
 * time, no queue and no status word (round 2 measured this path on this card).
 */
struct sec2_img {
  uint8_t *file;      /* the whole booter file, host virtual (patched in place) */
  uint32_t imem_file; /* file offset of the IMEM image (the landing map's base) */
  uint32_t imem_len;  /* IMEM image size (needed to reload after a scrub) */
  uint32_t dmem_file; /* file offset of the DMEM image */
  uint32_t dmem_len;
  uint32_t sig_off;   /* file offset of the production-signature block */
  uint32_t sig_slot;  /* bytes per slot */
  uint32_t sig_idx;   /* slot the fuse rule selects (the launch path's patch) */
  uint32_t patch_off; /* DMEM-image offset the signature goes to */
  uint32_t bootvec;
  uint32_t ucode_id;
  uint64_t meta_phys;
  uint8_t *meta;      /* host virtual; the verified word lives inside */
};

/* ---- register trace: does the START write reach the core? ---------------
 *
 * Round 6 measured START -> STOPPED at 2.3 us and found transitions=0: ctl
 * carried STOPPED (bit 4, RO) before the write and in every one of ~2.7 M
 * samples after it.  That says the core never left STOPPED.  It does not say
 * why, and the three causes lead to different next moves:
 *
 *   refused   the falcon's control logic discards the trigger because the core
 *             is held (RISC-V selected, engine still in reset).  Fix: the
 *             select/reset sequence, not the image.
 *   dropped   the write never reaches the register file (dead window or a write
 *             gate).  Fix: another door (the 0x130 alias) or another engine.
 *   consumed  the bit latched and was taken: the core started and died inside
 *             the ROM before the first read.  Fix: the image, or the ROM's own
 *             rejection channel -- the only one of the three that puts a boot
 *             ROM rejection on the table.
 *
 * A read-back alone cannot decide: no bit of ctl is sticky (the trigger
 * self-clears when taken and is discarded when not), so "wrote 0x2, read 0x10"
 * proves nothing by itself.  Two things make the rows mean something:
 *
 *   1. the same window is proved writable first, with registers that ARE
 *      sticky: UC_ENTRY (0x104) and FALCON_RM (0x084) written with a value that
 *      cannot appear there by themselves, read back, then restored;
 *   2. the trigger is judged by what the core DID (STOPPED clearing, DMEM
 *      changing, the booter's own verdict word), never by the bit alone.
 *
 * Every row carries ctl, the core-select word (0x841668) and the two FBIF
 * words the DMA configuration lives in (0x840624 ALLOW_PHYS_NO_CTX, 0x840600
 * TRANSCFG[0]), so a select or a context that differs between boots shows up
 * in the same row as the trigger.  UC_CTRL_ALIAS (0x130) is carried too: it
 * mirrors the trigger latch, which is what distinguishes "the bit never landed"
 * from "the bit is being held".
 */
#define NV_SEC2_BCR 0x841668u
#define NV_SEC2_FBIF_CFG624 (NV_FALCON_SEC2_BASE + 0x624)
#define NV_SEC2_UC_CTRL_ALIAS (NV_FALCON_SEC2_BASE + 0x130)

/* One row of the trace.  `ctl`/`ctl_known` let a caller print the value it read
 * immediately after the trigger as that row's ctl, so the row nearest the write
 * is the read nearest the write rather than the next one. */
static void sec2_row(struct nv_dev *d, const char *tag, const char *phase,
                     uint64_t t0, int have_t, uint32_t ctl, int ctl_known) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  uint64_t now = 0;
  int have_now = have_t && nv_timer_read(d, &now) == 0;
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" ");
  uart_puts(phase);
  uart_puts(" dt_ns=");
  uart_print_hex(have_now ? (now - t0) : 0u);
  uart_puts(" ctl=");
  uart_print_hex(ctl_known ? ctl : nv_rd32(d, b + NV_FALCON_UC_CTRL));
  uart_puts(" bcr=");
  uart_print_hex(nv_rd32(d, NV_SEC2_BCR));
  uart_puts(" tcfg624=");
  uart_print_hex(nv_rd32(d, NV_SEC2_FBIF_CFG624));
  uart_puts(" tcfg600=");
  uart_print_hex(nv_rd32(d, b + 0x600));
  uart_puts(" ent=");
  uart_print_hex(nv_rd32(d, b + NV_FALCON_UC_ENTRY));
  uart_puts(" st4c=");
  uart_print_hex(nv_rd32(d, b + 0x04C));
  uart_puts(" hw0f4=");
  uart_print_hex(nv_rd32(d, b + 0x0F4));
  uart_puts(" rm84=");
  uart_print_hex(nv_rd32(d, b + 0x084));
  uart_puts(" sec240=");
  uart_print_hex(nv_rd32(d, b + 0x240));
  uart_puts(" alias130=");
  uart_print_hex(nv_rd32(d, NV_SEC2_UC_CTRL_ALIAS));
  uart_puts(" pc=");
  uart_print_hex(nv_rd32(d, b + NV_FALCON_PC));
  uart_puts(" mb0=");
  uart_print_hex(nv_rd32(d, b + 0x040));
  uart_puts("\n");
}

/* Is this window's register file accepting host writes at all?  Two registers
 * whose value can only come from us, written and read back, then restored.  A
 * positive row here is what makes a later "the START bit never appeared" a
 * statement about the control logic instead of about a dead window. */
static void sec2_write_land_control(struct nv_dev *d, const char *tag) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  const uint32_t kA = 0xA5C33C5Au, kB = 0x5A3CC3A5u;
  uint32_t e_pre = nv_rd32(d, b + NV_FALCON_UC_ENTRY);
  nv_wr32(d, b + NV_FALCON_UC_ENTRY, kA);
  uint32_t e_back = nv_rd32(d, b + NV_FALCON_UC_ENTRY);
  nv_wr32(d, b + NV_FALCON_UC_ENTRY, e_pre);
  uint32_t r_pre = nv_rd32(d, b + 0x084);
  nv_wr32(d, b + 0x084, kB);
  uint32_t r_back = nv_rd32(d, b + 0x084);
  nv_wr32(d, b + 0x084, r_pre);
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" write-lands ent_pre=");
  uart_print_hex(e_pre);
  uart_puts(" wrote=");
  uart_print_hex(kA);
  uart_puts(" ent_back=");
  uart_print_hex(e_back);
  uart_puts(" rm_pre=");
  uart_print_hex(r_pre);
  uart_puts(" wrote=");
  uart_print_hex(kB);
  uart_puts(" rm_back=");
  uart_print_hex(r_back);
  int landed = (e_back == kA) + (r_back == kB);
  uart_puts(landed == 2
                ? " verdict=WRITE-PATH-LIVE (two non-trigger registers took a "
                  "value that could not appear there by themselves and read it "
                  "back: the SEC2 register file accepts host writes, so a START "
                  "that never appears is the control logic's decision, not a "
                  "dead bus)\n"
                : (landed ? " verdict=PARTIAL (one of the two restored: report "
                            "which, the other register may be write-gated)\n"
                          : " verdict=WRITE-PATH-DEAD (neither register took the "
                            "value: no claim about START is meaningful until "
                            "this is explained)\n"));
}

/* ---- raw register block dump ------------------------------------------- *
 * Read `nwords` at `off` and print them eight per row.  Read-only.  Used on
 * the blocks the fence leaves partly readable (0x000..0x05C, 0x0F0..0x134,
 * 0x200..0x24C, 0xFC0..0xFFC and the RISC-V block at 0x1650..0x169C) so that a
 * value which changed across the halt is nameable by address rather than by
 * prose.  A fenced word prints 0xBADF5620 like any other read -- the rows say
 * which words answered, which is itself the measurement. */
static void sec2_reg_block(struct nv_dev *d, const char *tag, const char *phase,
                           uint32_t off, uint32_t nwords) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  for (uint32_t i = 0; i < nwords; i += 8) {
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" ");
    uart_puts(phase);
    uart_puts("@");
    uart_print_hex(b + off + i * 4);
    for (uint32_t k = 0; k < 8 && i + k < nwords; k++) {
      uart_puts(" ");
      uart_print_hex(nv_rd32(d, b + off + (i + k) * 4));
    }
    uart_puts("\n");
  }
}

/* The five blocks probed around the halt, in one call.  0x04C is
 * STATUS_BUSY, 0x048 ACCESS_EN, 0x240 SEC_MODE (0=NS 1=LS 2=HS), 0x0F4/0x0FC
 * are the two words that answered while the rest of the page was fenced,
 * 0xFC0..0xFFC holds SP/PC/HWCFG2 (the header calls 0xFFC a riscv/hwcfg
 * alias window on v5), and 0x1650..0x169C is the RISC-V block round 7 saw
 * answering while the core select stood at 0x110. */
static void sec2_page_probe(struct nv_dev *d, const char *tag,
                            const char *phase) {
  sec2_reg_block(d, tag, phase, 0x000, 0x60);  /* INTR..0x05C, incl 0x04C */
  sec2_reg_block(d, tag, phase, 0x0F0, 0x48);  /* 0x0F0..0x134 */
  sec2_reg_block(d, tag, phase, 0x200, 0x50);  /* debug port + SEC_MODE */
  sec2_reg_block(d, tag, phase, 0xFC0, 0x40);  /* SP/PC/HWCFG2 */
  sec2_reg_block(d, tag, phase, 0x1650, 0x50); /* the RISC-V block */
}

/* ---- sample the live status registers across the halt -------------------
 *
 * The trace below reads ctl only, and prints a line between the first read and
 * the burst, so its first post-START sample after the write is ~6 ms later
 * (one UART line at 115200).  That leaves the halt interval anywhere between
 * 2.3 us and 6 ms and says nothing about what the status registers held while
 * the core ran.  This burst takes `SEC2_AUX_N` samples with NO output in
 * between -- ctl, STATUS_BUSY (0x04C), 0x030, 0x034, ACCESS_EN (0x048) and
 * SEC_MODE (0x240) -- then one read of the RISC-V block, then a ctl-only sweep
 * to find the first sample that saw STOPPED again, and only then prints.  Every
 * printed value therefore predates the printing.
 */
#define SEC2_AUX_N 32
static void sec2_aux_burst(struct nv_dev *d, const char *tag, uint64_t t0,
                           int have_t) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  uint32_t a_ctl[SEC2_AUX_N], a_4c[SEC2_AUX_N], a_30[SEC2_AUX_N],
      a_34[SEC2_AUX_N], a_48[SEC2_AUX_N], a_240[SEC2_AUX_N];
  uint64_t a_ts[SEC2_AUX_N];
  for (int i = 0; i < SEC2_AUX_N; i++) {
    uint64_t t = 0;
    a_ts[i] = (have_t && nv_timer_read(d, &t) == 0) ? (t - t0) : 0;
    a_ctl[i] = nv_rd32(d, b + NV_FALCON_UC_CTRL);
    a_4c[i] = nv_rd32(d, b + NV_FALCON_STATUS_BUSY);
    a_30[i] = nv_rd32(d, b + 0x030);
    a_34[i] = nv_rd32(d, b + 0x034);
    a_48[i] = nv_rd32(d, b + NV_FALCON_ACCESS_EN);
    a_240[i] = nv_rd32(d, b + NV_FALCON_SEC_MODE);
  }

  /* One read of the RISC-V block, still before any output. */
  uint32_t rv[20];
  for (uint32_t i = 0; i < 20; i++)
    rv[i] = nv_rd32(d, b + 0x1650 + i * 4);

  /* ctl-only sweep: one read per poll, a ptimer read only when the value
   * changed, so the first STOPPED sample is timestamped to within a poll. */
  uint32_t last = a_ctl[SEC2_AUX_N - 1];
  uint32_t changes = 0, runs = (last & NV_FALCON_UC_CTRL_STOPPED) ? 0 : 1;
  int saw_run = runs;
  uint64_t stop_ns = 0, last_change_ns = 0, polls = 0;
  for (int i = 0; i < 40000; i++) {
    uint32_t v = nv_rd32(d, b + NV_FALCON_UC_CTRL);
    polls++;
    if (v != last) {
      uint64_t n = 0;
      if (have_t && nv_timer_read(d, &n) == 0) {
        last_change_ns = n - t0;
        if (!(v & NV_FALCON_UC_CTRL_STOPPED)) {
          runs++;
          saw_run = 1;
        } else if (saw_run && stop_ns == 0) {
          stop_ns = n - t0;
        }
      }
      changes++;
      last = v;
    }
  }

  int printed = 0;
  for (int i = 0; i < SEC2_AUX_N; i++) {
    int show = (i < 2) || (i >= SEC2_AUX_N - 2) ||
               (i > 0 && a_ctl[i] != a_ctl[i - 1]) ||
               (i > 0 && a_4c[i] != a_4c[i - 1]);
    if (!show) continue;
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" aux dt_ns=");
    uart_print_hex((uint32_t)a_ts[i]);
    uart_puts(" ctl=");
    uart_print_hex(a_ctl[i]);
    uart_puts(" st4c=");
    uart_print_hex(a_4c[i]);
    uart_puts(" r030=");
    uart_print_hex(a_30[i]);
    uart_puts(" r034=");
    uart_print_hex(a_34[i]);
    uart_puts(" acc48=");
    uart_print_hex(a_48[i]);
    uart_puts(" sec240=");
    uart_print_hex(a_240[i]);
    uart_puts("\n");
    printed++;
  }
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" aux-summary samples=");
  uart_print_hex(SEC2_AUX_N);
  uart_puts(" printed=");
  uart_print_hex((uint32_t)printed);
  uart_puts(" span_ns=");
  uart_print_hex((uint32_t)a_ts[SEC2_AUX_N - 1]);
  uart_puts(" sweep_polls=");
  uart_print_hex((uint32_t)polls);
  uart_puts(" sweep_changes=");
  uart_print_hex(changes);
  uart_puts(" stop_seen_ns=");
  uart_print_hex((uint32_t)stop_ns);
  uart_puts(" last_change_ns=");
  uart_print_hex((uint32_t)last_change_ns);
  uart_puts(" final_ctl=");
  uart_print_hex(last);
  uart_puts(" (stop_seen_ns=0 and final STOPPED means the core was already "
            "stopped when the sweep began)\n");
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" aux-riscv@0x");
  uart_print_hex(b + 0x1650);
  for (uint32_t i = 0; i < 20; i++) {
    uart_puts(" ");
    uart_print_hex(rv[i]);
  }
  uart_puts("\n");
}

/* ---- timeline: every change of the status words, timestamped -------------
 *
 * The burst above is 32 samples deep (~160 us) and then the ctl-only sweep
 * only watches ctl.  This takes the same words as the burst plus INTR (0x008),
 * UNK094 (0x094), 0x0D0 and 0x0DC, polls for ~40 ms, and records every sample
 * where any of them changed -- so the halt, the INTR.EXIT latch (falcon_intr
 * bit 4), the SEC_MODE flip and the two unknown latches all get one timestamp
 * each, on the same clock.  No output inside the loop.
 */
#define SEC2_TL_N 40
static void sec2_aux_timeline(struct nv_dev *d, const char *tag, uint64_t t0,
                              int have_t) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  uint32_t r_ns[SEC2_TL_N], r_ctl[SEC2_TL_N], r_intr[SEC2_TL_N],
      r_busy[SEC2_TL_N], r_sec[SEC2_TL_N], r_u94[SEC2_TL_N],
      r_d0[SEC2_TL_N], r_dc[SEC2_TL_N];
  int n = 0;
  uint32_t p_ctl = 0xffffffffu, p_intr = 0xffffffffu, p_busy = 0xffffffffu,
           p_sec = 0xffffffffu, p_u94 = 0xffffffffu;
  uint64_t first_ns = 0, last_ns = 0;
  uint32_t polls = 0;
  for (int i = 0; i < 6000; i++) {
    uint64_t t = 0;
    int ht = have_t && nv_timer_read(d, &t) == 0;
    uint32_t ctl = nv_rd32(d, b + NV_FALCON_UC_CTRL);
    uint32_t intr = nv_rd32(d, b + NV_FALCON_INTR);
    uint32_t busy = nv_rd32(d, b + NV_FALCON_STATUS_BUSY);
    uint32_t sec = nv_rd32(d, b + NV_FALCON_SEC_MODE);
    uint32_t u94 = nv_rd32(d, b + 0x094);
    uint32_t d0 = nv_rd32(d, b + 0x0D0);
    uint32_t dc = nv_rd32(d, b + 0x0DC);
    uint64_t ns = ht ? (t - t0) : 0;
    polls++;
    if (i == 0) first_ns = ns;
    last_ns = ns;
    if (i == 0 || ctl != p_ctl || intr != p_intr || busy != p_busy ||
        sec != p_sec || u94 != p_u94) {
      if (n < SEC2_TL_N) {
        r_ns[n] = (uint32_t)ns;
        r_ctl[n] = ctl;
        r_intr[n] = intr;
        r_busy[n] = busy;
        r_sec[n] = sec;
        r_u94[n] = u94;
        r_d0[n] = d0;
        r_dc[n] = dc;
        n++;
      }
      p_ctl = ctl;
      p_intr = intr;
      p_busy = busy;
      p_sec = sec;
      p_u94 = u94;
    }
  }
  for (int i = 0; i < n; i++) {
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" tl dt_ns=");
    uart_print_hex(r_ns[i]);
    uart_puts(" ctl=");
    uart_print_hex(r_ctl[i]);
    uart_puts(" intr=");
    uart_print_hex(r_intr[i]);
    uart_puts(" st4c=");
    uart_print_hex(r_busy[i]);
    uart_puts(" sec240=");
    uart_print_hex(r_sec[i]);
    uart_puts(" u94=");
    uart_print_hex(r_u94[i]);
    uart_puts(" d0=");
    uart_print_hex(r_d0[i]);
    uart_puts(" dc=");
    uart_print_hex(r_dc[i]);
    uart_puts("\n");
  }
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" tl-summary polls=");
  uart_print_hex(polls);
  uart_puts(" rows=");
  uart_print_hex((uint32_t)n);
  uart_puts(" first_ns=");
  uart_print_hex((uint32_t)first_ns);
  uart_puts(" last_ns=");
  uart_print_hex((uint32_t)last_ns);
  uart_puts("\n");
}

/* Fire START once and watch what the window and the core do with it.
 *
 * The first read after the write is taken as the row's ctl, then a burst of
 * back-to-back reads bounds how long a transient bit could have been visible,
 * then rows at 10 ms and 100 ms.  The write is bracketed by two ptimer reads
 * so the gap between the write and the first look at the register is measured
 * (at_ns is the interval from the write returning to the read returning), not
 * inferred from a round-trip cost measured elsewhere.  Returns 1 when the core
 * reacted in any sample (STOPPED cleared, or bit 1 was ever observable). */
static int sec2_start_trace(struct nv_dev *d, const char *tag,
                            uint64_t *start_ns) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  uint64_t t0 = 0;
  int have_t = nv_timer_read(d, &t0) == 0;
  if (start_ns) *start_ns = have_t ? t0 : 0;

  /* The cost of one ctl read through this window, in this boot: any interval
   * at or below it is a bound, and the rows say so. */
  uint64_t rc0 = 0, rc1 = 0, read_cost_ns = 0;
  {
    uint32_t sink = 0;
    int have_c = nv_timer_read(d, &rc0) == 0;
    for (int i = 0; i < 32; i++) sink ^= nv_rd32(d, b + NV_FALCON_UC_CTRL);
    (void)sink;
    if (have_c && nv_timer_read(d, &rc1) == 0) read_cost_ns = (rc1 - rc0) / 32u;
  }

  sec2_row(d, tag, "pre", 0, 0, 0, 0);

  uint64_t tw = 0, tf = 0;
  int have_tw = have_t && nv_timer_read(d, &tw) == 0;
  nv_wr32(d, b + NV_FALCON_UC_CTRL, NV_FALCON_UC_CTRL_START);
  uint32_t ctl_first = nv_rd32(d, b + NV_FALCON_UC_CTRL);
  int have_tf = have_t && nv_timer_read(d, &tf) == 0;

  int bit1_seen = (ctl_first & NV_FALCON_UC_CTRL_START) ? 1 : 0;
  int ran_seen = !(ctl_first & NV_FALCON_UC_CTRL_STOPPED);

  /* No output until the timeline, the burst, the RISC-V read and the ctl
   * sweep are done: the rows then describe real time after the trigger instead
   * of time plus one UART line between each sample. */
  sec2_aux_timeline(d, tag, t0, have_t);
  sec2_aux_burst(d, tag, t0, have_t);

  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" post_first ctl=");
  uart_print_hex(ctl_first);
  uart_puts(" wr_to_read_ns=");
  uart_print_hex((have_tw && have_tf) ? (tf - tw) : 0u);
  uart_puts(" since_t0_ns=");
  uart_print_hex(have_tf ? (tf - t0) : 0u);
  uart_puts(" read_cost_ns=");
  uart_print_hex(read_cost_ns);
  uart_puts(" alias130=");
  uart_print_hex(nv_rd32(d, NV_SEC2_UC_CTRL_ALIAS));
  uart_puts(" bit1=");
  print_int(bit1_seen);
  uart_puts(" running=");
  print_int(ran_seen);
  uart_puts("\n");

  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" burst");
  for (int i = 0; i < 12; i++) {
    uint32_t v = nv_rd32(d, b + NV_FALCON_UC_CTRL);
    uint64_t ts = 0;
    int have_s = nv_timer_read(d, &ts) == 0;
    if (v & NV_FALCON_UC_CTRL_START) bit1_seen = 1;
    if (!(v & NV_FALCON_UC_CTRL_STOPPED)) ran_seen = 1;
    uart_puts(" ");
    uart_print_hex(have_s ? (ts - t0) : 0u);
    uart_puts(":");
    uart_print_hex(v);
  }
  uart_puts("\n");

  sec2_row(d, tag, "post", t0, have_t, 0, 0);

  /* 10 ms and 100 ms rows, plus a transition count over the whole span, so
   * "already stopped, and still stopped" is a measurement rather than one
   * sample's opinion. */
  uint32_t last = ctl_first;
  uint64_t polls = 0, run_samples = 0, changes = 0;
  int did10 = 0, did100 = 0;
  for (int i = 0; i < 4000000; i++) {
    uint32_t v = nv_rd32(d, b + NV_FALCON_UC_CTRL);
    polls++;
    if (!(v & NV_FALCON_UC_CTRL_STOPPED)) {
      run_samples++;
      ran_seen = 1;
    }
    if (v & NV_FALCON_UC_CTRL_START) bit1_seen = 1;
    if (v != last) {
      changes++;
      last = v;
    }
    if ((i & 0x1f) == 0 && have_t) {
      uint64_t n = 0;
      if (nv_timer_read(d, &n) == 0) {
        if (!did10 && n - t0 >= 10000000ULL) {
          did10 = 1;
          sec2_row(d, tag, "at10ms", t0, have_t, 0, 0);
        }
        if (!did100 && n - t0 >= 100000000ULL) {
          did100 = 1;
          sec2_row(d, tag, "at100ms", t0, have_t, 0, 0);
          break;
        }
      }
    }
  }
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" summary bit1_ever=");
  print_int(bit1_seen);
  uart_puts(" running_ever=");
  print_int(ran_seen);
  uart_puts(" polls=");
  uart_print_hex(polls);
  uart_puts(" running_samples=");
  uart_print_hex(run_samples);
  uart_puts(" ctl_changes=");
  uart_print_hex(changes);
  uart_puts(bit1_seen
                ? " verdict=START-BIT-WAS-VISIBLE (the register held bit 1 at "
                  "least once after the write: the write landed and the bit was "
                  "NOT taken -- a pending trigger the core never executed)\n"
                : (ran_seen ? " verdict=START-TOOK (bit 1 self-cleared because the "
                              "core left STOPPED: the trigger reached the core)\n"
                            : " verdict=START-NEVER-VISIBLE (bit 1 was 0 in every "
                              "sample and the core never left STOPPED: the write "
                              "was either discarded by the control logic or taken "
                              "and refused within the first read; the write-lands "
                              "row above is what separates those)\n"));
  return bit1_seen || ran_seen;
}

/* ---- the reset, and the two things it changes ---------------------------
 *
 * The GSP's own path applies a full falcon reset before its load and records
 * that the reset is REQUIRED for STARTCPU to be accepted ("without it the core
 * answers START with 'still halted'").  SEC2 has never received it: the halt
 * step only issues UC_CTRL's reset bits, and only when ctl is unreadable or not
 * already stopped -- which on this card is never, because STOPPED is always
 * set.  So the GSP sequence runs here: 0x048 &= ~3, 0x014 = all ones, pulse
 * 0x3c0, then the ~20 ms scrub.
 */
static void sec2_reset_apply(struct nv_dev *d, const char *tag, uint64_t t0,
                             int have_t) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  uint32_t v48 = nv_rd32(d, b + 0x048);
  nv_wr32(d, b + 0x048, v48 & ~3u);
  nv_wr32(d, b + 0x014, 0xffffffffu);
  nv_wr32(d, b + 0x3C0, 1);
  for (volatile int spin = 0; spin < 2000; spin++) {
  }
  nv_wr32(d, b + 0x3C0, 0);
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" reset-issued 48was=");
  uart_print_hex(v48);
  uart_puts(" 48now=");
  uart_print_hex(nv_rd32(d, b + 0x048));
  uart_puts(" 014now=");
  uart_print_hex(nv_rd32(d, b + 0x014));
  uart_puts(" (GSP sequence: 0x048 &= ~3, 0x014 = all ones, pulse 0x3c0)\n");
  if (have_t) {
    uint64_t n = 0;
    for (int i = 0; i < 200000; i++) {
      if (nv_timer_read(d, &n) == 0 && n - t0 > 20000000ULL) break;
    }
  } else {
    for (volatile int spin = 0; spin < 2000000; spin++) {
    }
  }
}

/* ---- build-time switches for the launch-path bisect ---------------------
 * SEC2_IMEM_CONTRACT_LAYOUT: 1 = os_code+app at IMEM 0 with BOOTVEC 0x100,
 *   0 = app alone at IMEM 0 with BOOTVEC 0.
 * SEC2_BOOTVEC_FORCE: non-zero forces 0x104 to that value after the layout.
 * SEC2_DMEM_PIO: 1 = IMEM through the SEC=1 DMA path and DMEM through PIO,
 *   0 = both through DMA.  Isolates which half of the transport makes START
 *   be refused (measured: PIO both -> accepted; DMA both -> refused).
 */
#define SEC2_IMEM_CONTRACT_LAYOUT 0
#define SEC2_BOOTVEC_FORCE 0u
#define SEC2_DMEM_PIO 0
#define SEC2_SIG_SLOT_FORCE 0xFFu
#define SEC2_IMEM_SEC 1u

/* Reload the image through the DMA engine, the transport the reference uses
 * and the only one that can carry the SEC bit for a 256-byte IMEM block.
 *
 * Why this is not the same as the PIO reload: the booter's 0x7600 app-code
 * bytes are ciphertext (AES-128-ECB keyed by this engine's fused ucode key,
 * measured in an earlier round).  A DMA block marked IMEM=1, SEC=1 is the
 * engine's secure write path; the PIO port's secret index bit marks the page
 * secret but is not documented to run the same decrypt on the way in.  If the
 * decrypt is what makes the code executable, a PIO launch runs ciphertext and
 * the core loops in whatever the bytes decode to -- which is exactly the s1/s2
 * observation (running forever, no verdict, no halt).  The two transports are
 * told apart in the row, and the PIO fallback stays for a DMA queue that will
 * not drain. */
static int sec2_reload_dma(struct nv_dev *d, const struct sec2_img *im,
                           uint64_t src_phys, const char *tag) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  int rc = 0;
  uint32_t off = 0;
  (void)src_phys;

  /* The source offsets the engine is given must be 256-byte aligned.  Measured
   * in s5/s6: a DMEM transfer requested from the container's file offset
   * 0x7A78 landed the bytes from 0x7A00 (0x7A78 & ~0xFF) in DMEM -- the low
   * eight bits of the external address are not honoured on this path.  The
   * container's images sit at 0x478 and 0x7A78, so every DMA block this driver
   * has ever issued was 0x78 bytes out of place.  Stage both images into one
   * page-aligned buffer at 256-byte-aligned offsets and DMA from there only. */
  uint32_t ilen = im->imem_len, dlen = im->dmem_len;
  uint32_t dm_off = (ilen + 0xFFu) & ~0xFFu;
  uint64_t need = ((uint64_t)dm_off + (uint64_t)dlen + (NV_DMA_PAGE - 1)) &
                  ~(NV_DMA_PAGE - 1);
  struct nv_dma_map st;
  rc = nv_dma_alloc(need, &st);
  int via_pio = 0;
  uint64_t bus = 0;
  if (rc == 0) {
    uint8_t *sbuf = (uint8_t *)phys_to_virt(st.phys);
    for (uint32_t i = 0; i < ilen; i++) sbuf[i] = im->file[im->imem_file + i];
    for (uint32_t i = 0; i < dlen; i++)
      sbuf[dm_off + i] = im->file[im->dmem_file + i];
    bus = nv_dma_bus(&st);
    if (bus == NV_DMA_BUS_INVALID) {
      nv_dma_free(&st);
      rc = -ENOTSUP;
    }
  }
  if (rc == 0) {
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" stage buf len=");
    uart_print_hex((uint32_t)need);
    uart_puts(" imem@0 dmem@0x");
    uart_print_hex(dm_off);
    uart_puts(" bus=");
    uart_print_hex((uint32_t)(bus >> 32));
    uart_puts(":");
    uart_print_hex((uint32_t)bus);
    uart_puts("\n");
    for (off = 0; off < ilen; off += 256) {
      rc = falcon_dma_block(d, b, bus, off, off, 1, (int)SEC2_IMEM_SEC);
      if (rc != 0) break;
    }
  }
  /* The IMEM transfer is the SEC=1 one.  Row here, before DMEM is touched, so
   * a state change caused by the secure write path is visible on its own. */
  sec2_row(d, tag, "after-imem-load", 0, 0, 0, 0);
  int dm_by_pio = 0;
  if (rc == 0) {
#if SEC2_DMEM_PIO
    dm_by_pio = 1;
    rc = falcon_pio_block(d, b, im->file + im->dmem_file, im->dmem_len, 0, 0, 0);
#else
    for (off = 0; off < dlen; off += 256) {
      rc = falcon_dma_block(d, b, bus, dm_off + off, off, 0, 0);
      if (rc != 0) break;
    }
#endif
  }
  sec2_row(d, tag, "after-dmem-load", 0, 0, 0, 0);
  if (rc != 0) {
    via_pio = 1;
    rc = falcon_pio_block(d, b, im->file + im->imem_file, ilen, 0, 1, 1);
    if (rc == 0)
      rc = falcon_pio_block(d, b, im->file + im->dmem_file, dlen, 0, 0, 0);
  }
  if (need) nv_dma_free(&st);
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" reload-dma imem=");
  uart_print_hex(ilen);
  uart_puts(" dmem=");
  uart_print_hex(dlen);
  uart_puts(" dmem_img@0x");
  uart_print_hex(dm_off);
  uart_puts(" rc=");
  print_int(rc);
  uart_puts(" dmem_transport=");
  uart_puts(dm_by_pio ? "PIO" : "DMA");
  uart_puts(" transport=");
  uart_puts(via_pio ? "PIO (DMA failed)\n" : "DMA (256-byte-aligned staging buffer)\n");
  return rc;
}

/* The state fixups the GSP path applies at exactly this point in its load:
 * FALCON_RM <- PMC_BOOT_0, 0x624 bit 7 (ALLOW_PHYS_NO_CTX), 0x10C = 0, and the
 * context word in 0x600.  After a reset all four are back at their reset
 * values (measured on this card: 0x624 0x190 -> 0x110, 0x600 0x5 -> 0x110,
 * 0x084 pmc_boot0 -> 0), so a comparison of "reset only" against "reset plus
 * the state the GSP leaves behind" is a real bisect and not a re-run. */
static void sec2_state_fixups(struct nv_dev *d, const char *tag) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  uint32_t v624 = nv_rd32(d, NV_SEC2_FBIF_CFG624);
  nv_wr32(d, NV_SEC2_FBIF_CFG624, v624 | (1u << 7));
  nv_wr32(d, b + 0x10C, 0);
  nv_wr32(d, b + 0x600, NV_LOCAL_TRANSCFG_COHERENT);
  nv_wr32(d, b + 0x084, nv_rd32(d, 0x000000));
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" state-fixups 624=");
  uart_print_hex(v624);
  uart_puts("->");
  uart_print_hex(nv_rd32(d, NV_SEC2_FBIF_CFG624));
  uart_puts(" 600=");
  uart_print_hex(nv_rd32(d, b + 0x600));
  uart_puts(" 10c=");
  uart_print_hex(nv_rd32(d, b + 0x10C));
  uart_puts(" rm84=");
  uart_print_hex(nv_rd32(d, b + 0x084));
  uart_puts(" (the GSP's own pre-load state)\n");
}

/* Clear the core select AFTER the reset, and report what the reset put there.
 * Measured: the reset re-arms 0x1668 to 0x110 (RISCV|BRFETCH) and the falcon
 * page goes back to the fence word until the select is cleared again. */
static void sec2_select_clear(struct nv_dev *d, const char *tag) {
  uint32_t bcr = nv_rd32(d, NV_SEC2_BCR);
  if (bcr == NV_REG_FENCED || bcr == 0xffffffffu) {
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" bcr unreadable=");
    uart_print_hex(bcr);
    uart_puts(" (the clear is still issued)\n");
    nv_wr32(d, NV_SEC2_BCR, 0);
    return;
  }
  if (!(bcr & NV_GSP_BCR_RISCV)) {
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" bcr=");
    uart_print_hex(bcr);
    uart_puts(" already-falcon-selected: no clear needed\n");
    return;
  }
  nv_wr32(d, NV_SEC2_BCR, 0);
  uint32_t b2 = bcr;
  for (int i = 0; i < 100000; i++) {
    b2 = nv_rd32(d, NV_SEC2_BCR);
    if (b2 & NV_GSP_BCR_VALID) break;
  }
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" bcr-after-reset=");
  uart_print_hex(bcr);
  uart_puts(" RE-ARMED-RISCV -> cleared: ");
  uart_print_hex(b2);
  uart_puts((b2 & NV_GSP_BCR_VALID)
                ? " verdict=VALID (falcon core selected again)\n"
                : " verdict=VALID-NEVER-SET\n");
}

/* What else lives in the select block?  Read-only, 0x1600..0x169c, taken while
 * the RISC-V core is still the selected one -- i.e. the state the part comes up
 * in after a real reset and the state this driver has never looked at. */
static void sec2_select_block_dump(struct nv_dev *d, const char *tag) {
  for (uint32_t off = 0x1600; off < 0x16A0; off += 0x20) {
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" selblk@");
    uart_print_hex(NV_FALCON_SEC2_BASE + off);
    for (uint32_t i = 0; i < 8; i++) {
      uart_puts(" ");
      uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + off + i * 4));
    }
    uart_puts("\n");
  }
}

static void sec2_reload(struct nv_dev *d, const struct sec2_img *im,
                        const char *tag) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  int rc = falcon_pio_block(d, b, im->file + im->imem_file, im->imem_len, 0, 1, 1);
  if (rc == 0)
    rc = falcon_pio_block(d, b, im->file + im->dmem_file, im->dmem_len, 0, 0, 0);
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" reload imem=");
  uart_print_hex(im->imem_len);
  uart_puts(" dmem=");
  uart_print_hex(im->dmem_len);
  uart_puts(" rc=");
  print_int(rc);
  uart_puts(rc == 0 ? " verdict=RELOADED (after the reset, not before it)\n"
                    : " verdict=RELOAD-FAILED (report the port, no start claim "
                      "from this variant)\n");
}

/* The BROM parameters, the mailbox sentinel and the verdict word, exactly as
 * the two signature attempts do them. */
static void sec2_arm(struct nv_dev *d, const struct sec2_img *im) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  nv_wr32(d, NV_BROM_PARAADDR0, im->patch_off);
  nv_wr32(d, NV_BROM_ENGIDMASK, 1);
  nv_wr32(d, NV_BROM_CURR_UCODE_ID, im->ucode_id);
  nv_wr32(d, NV_BROM_MOD_SEL, 1);
  nv_wr32(d, b + NV_FALCON_UC_ENTRY, im->bootvec);
  nv_wr32(d, b + 0x040, (uint32_t)im->meta_phys);
  nv_wr32(d, b + 0x044, (uint32_t)(im->meta_phys >> 32));
  if (im->meta) nv_meta_put64(im->meta + NV_WPR_OFF_VERIFIED, 0x5EC2A0A0ULL);
}

static void sec2_verdict_row(const struct sec2_img *im, const char *tag) {
  uint64_t v = im->meta ? nv_meta_get64(im->meta + NV_WPR_OFF_VERIFIED) : 0;
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" verified=");
  uart_print_hex(v);
  uart_puts(v == NV_WPR_VERIFIED_WORD
                ? " verdict=RAN (the booter wrote its own acknowledgement)\n"
                : " verdict=NOWRITE (the metadata kept its sentinel: the boot "
                  "ROM did not run, or ran and rejected before its verdict)\n");
}

/* Poll the booter's own verdict channel -- the metadata `verified` word, which
 * lives in host memory, so no register read is involved -- for longer than the
 * booter's budget.  The reference's own halt for this step is ~105.8 ms, and
 * round 7 read the word at +100 ms on a core that was still running: that says
 * nothing.  Rows are emitted at +300 ms and +600 ms from the START (the same
 * t0 the trace used), and the closing row carries ctl, pc and both mailbox
 * words, so "still running, no verdict", "STOPPED without a verdict" (a boot
 * ROM rejection) and "verdict written" are three different rows instead of one
 * sentinel read at an arbitrary time.  Returns 1 when the booter wrote
 * NV_WPR_VERIFIED_WORD. */
static int sec2_verdict_wait(struct nv_dev *d, const struct sec2_img *im,
                             const char *tag, uint64_t t0, int have_t) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  static const uint64_t kAt[2] = {300000000ULL, 600000000ULL};
  if (!im->meta) {
    sec2_verdict_row(im, tag);
    return 0;
  }
  uint64_t v = 0, now = 0, polls = 0;
  int done = 0;
  for (uint64_t i = 0; i < 200000000ULL; i++) {
    v = nv_meta_get64(im->meta + NV_WPR_OFF_VERIFIED);
    polls++;
    if (v == NV_WPR_VERIFIED_WORD) break;
    if ((i & 0xff) == 0) {
      if (!have_t) {
        if (i >= 12000000ULL) break; /* no timer: fixed iteration budget */
      } else if (nv_timer_read(d, &now) == 0) {
        while (done < 2 && now - t0 >= kAt[done]) {
          uint32_t c = nv_rd32(d, b + NV_FALCON_UC_CTRL);
          uart_puts("[NV-SEC2T] ");
          uart_puts(tag);
          uart_puts(" verdict@");
          uart_print_hex((uint32_t)(kAt[done] / 1000000ULL));
          uart_puts("ms verified=");
          uart_print_hex(v);
          uart_puts(" ctl=");
          uart_print_hex(c);
          uart_puts(" halted=");
          print_int((c & NV_FALCON_UC_CTRL_STOPPED) ? 1 : 0);
          uart_puts(" pc=");
          uart_print_hex(nv_rd32(d, b + NV_FALCON_PC));
          uart_puts(" mb0=");
          uart_print_hex(nv_rd32(d, b + 0x040));
          uart_puts("\n");
          done++;
        }
        if (done >= 2) break;
      }
    }
  }
  {
    uint32_t c = nv_rd32(d, b + NV_FALCON_UC_CTRL);
    uint64_t el = 0;
    if (have_t && nv_timer_read(d, &now) == 0) el = now - t0;
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" verdict-wait ms=");
    uart_print_hex((uint32_t)(el / 1000000ULL));
    uart_puts(" verified=");
    uart_print_hex(v);
    uart_puts(" ctl=");
    uart_print_hex(c);
    uart_puts(" halted=");
    print_int((c & NV_FALCON_UC_CTRL_STOPPED) ? 1 : 0);
    uart_puts(" pc=");
    uart_print_hex(nv_rd32(d, b + NV_FALCON_PC));
    uart_puts(" alias130=");
    uart_print_hex(nv_rd32(d, NV_SEC2_UC_CTRL_ALIAS));
    uart_puts(" mb0=");
    uart_print_hex(nv_rd32(d, b + 0x040));
    uart_puts(" mb1=");
    uart_print_hex(nv_rd32(d, b + 0x044));
    uart_puts(" polls=");
    uart_print_hex((uint32_t)polls);
    uart_puts(v == NV_WPR_VERIFIED_WORD
                  ? " verdict=RAN (the booter wrote its acknowledgement into "
                    "the metadata block: WPR2 is built)\n"
                  : ((c & NV_FALCON_UC_CTRL_STOPPED)
                         ? " verdict=NOWRITE-HALTED (the core is STOPPED and "
                           "the metadata still holds the sentinel: a rejection, "
                           "not a slow boot)\n"
                         : " verdict=NOWRITE-RUNNING (the core is still running "
                           "past the booter's budget and has written no "
                           "verdict)\n"));
  }
  return v == NV_WPR_VERIFIED_WORD;
}

/* ---- variant R: the reset, and nothing else -----------------------------
 *
 * Runs after both signature attempts, as the last thing this driver does to the
 * card in the boot, so it cannot disturb their verdicts.  It answers three
 * questions in one deploy: does a real falcon reset re-arm the core select,
 * does it change what START does, and (if the trigger latches) is the latch the
 * same state a second door -- the 0x130 alias -- also sees.
 */
static void sec2_reset_variant(struct nv_dev *d, const struct sec2_img *im) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  const char *tag = "R";
  uint64_t t0 = 0;
  int have_t = nv_timer_read(d, &t0) == 0;

  sec2_row(d, tag, "before-reset", 0, 0, 0, 0);
  sec2_reset_apply(d, tag, t0, have_t);
  sec2_row(d, tag, "after-reset", t0, have_t, 0, 0);
  sec2_select_block_dump(d, tag);
  sec2_select_clear(d, tag);
  sec2_row(d, tag, "after-bcr", t0, have_t, 0, 0);
  sec2_reload(d, im, tag);
  sec2_arm(d, im);
  (void)sec2_start_trace(d, tag, 0);
  sec2_verdict_row(im, tag);

  /* A second door to the same latch: UC_CTRL_ALIAS.  The latch is cleared
   * first (write 0 to ctl) so the alias write is the only thing that could set
   * it -- with bit 1 already set from the trace above, a no-op would read as
   * "no effect" for the wrong reason (measured in the first cut of this
   * variant: ctl 0x12 -> 0x12). */
  {
    nv_wr32(d, b + NV_FALCON_UC_CTRL, 0);
    uint32_t ctl_clear = nv_rd32(d, b + NV_FALCON_UC_CTRL);
    uint32_t al_clear = nv_rd32(d, NV_SEC2_UC_CTRL_ALIAS);
    nv_wr32(d, NV_SEC2_UC_CTRL_ALIAS, NV_FALCON_UC_CTRL_START);
    uint32_t ctl_post = nv_rd32(d, b + NV_FALCON_UC_CTRL);
    uint32_t al_post = nv_rd32(d, NV_SEC2_UC_CTRL_ALIAS);
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" alias-door ctl-cleared=");
    uart_print_hex(ctl_clear);
    uart_puts(" alias-cleared=");
    uart_print_hex(al_clear);
    uart_puts(" after-alias-start ctl=");
    uart_print_hex(ctl_post);
    uart_puts(" alias=");
    uart_print_hex(al_post);
    uart_puts((ctl_post & NV_FALCON_UC_CTRL_START) ||
                      (al_post & NV_FALCON_UC_CTRL_START)
                  ? " verdict=ALIAS-SETS-THE-TRIGGER (a second register reaches "
                    "the same latch)\n"
                  : " verdict=ALIAS-NO-EFFECT (the alias write did not set the "
                    "trigger that 0x840100 sets)\n");
  }

  /* ---- last: does ctl respond to ANY write? ----------------------------
   *
   * Destructive on purpose (the reset bits scrub memories) and therefore last:
   * one write per bit, read back, with the window's own write path already
   * proved live above.  A bit that sets and stays set is a latch the control
   * logic keeps; a bit that never appears is one it clears. */
  {
    static const uint32_t kBits[6] = {1u, 2u, 4u, 8u, 0x10u, 0x40u};
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" ctl-write-sweep");
    for (int i = 0; i < 6; i++) {
      nv_wr32(d, b + NV_FALCON_UC_CTRL, kBits[i]);
      uart_puts(" w=");
      uart_print_hex(kBits[i]);
      uart_puts("->");
      uart_print_hex(nv_rd32(d, b + NV_FALCON_UC_CTRL));
    }
    uart_puts(" (one write per bit, read back; 0x10 is STOPPED, RO)\n");
  }
}

/* ---- variant S: reset, then the state the GSP leaves behind -------------
 *
 * R says what a bare reset does.  S adds back the four state fixups the GSP
 * path applies at the same point (0x624 bit 7, 0x10C = 0, 0x600 = 5,
 * FALCON_RM <- PMC_BOOT_0) and repeats the identical START.  The difference
 * between the two is then attributable to those writes and nothing else: same
 * image, same transport, same entry, same mailbox, same boot.
 *
 * This is the launch path the campaign runs, not a probe: it patches the
 * signature into the DMEM image itself (a launch that omitted it would fail
 * authentication for a reason of our own making), polls the booter's verdict
 * for longer than its budget, and returns 1 only when the metadata block says
 * the booter ran.  R and A/B stay in the boot as controls -- same image, same
 * transport, and their rows are the ones that must still read ctl=0x10 with
 * bit 1 gone.
 */
static void sec2_dmem_diff(struct nv_dev *d, const struct sec2_img *im,
                           const char *tag);

/* Stop the running core and read where it stopped.
 *
 * While SEC2 runs, the whole falcon page answers the fence word (measured in S:
 * bcr/tcfg624/ent/pc all 0xBADF5620), so PC is unobservable exactly when it
 * matters most.  The documented any->stopped transition is a write of the
 * three reset bits; issuing it after the verdict window and then clearing the
 * re-armed select (round 7: a reset puts 0x110 RISCV|BRFETCH there) makes the
 * page readable again.  The row then carries the PC the core stopped on, and a
 * DMEM diff after the halt says whether it wrote anything at all.  If the core
 * does not stop, the row says so -- no stop claim is made from a timeout. */
static void sec2_halt_probe(struct nv_dev *d, const struct sec2_img *im,
                            const char *tag) {
  const uint32_t b = NV_FALCON_SEC2_BASE;
  uint64_t t0 = 0, t1 = 0;
  int have_t = nv_timer_read(d, &t0) == 0;
  nv_wr32(d, b + NV_FALCON_UC_CTRL,
          NV_FALCON_UC_CTRL_RESET_TLB | NV_FALCON_UC_CTRL_RESET_UNK2 |
              NV_FALCON_UC_CTRL_RESET_UNK3);
  uint32_t c = nv_rd32(d, b + NV_FALCON_UC_CTRL);
  int stopped = (c & NV_FALCON_UC_CTRL_STOPPED) ? 1 : 0;
  for (int i = 0; i < 4000000 && !stopped; i++) {
    c = nv_rd32(d, b + NV_FALCON_UC_CTRL);
    stopped = (c & NV_FALCON_UC_CTRL_STOPPED) ? 1 : 0;
    if (!stopped && have_t && (i & 0x3ff) == 0) {
      uint64_t n = 0;
      if (nv_timer_read(d, &n) == 0 && n - t0 > 100000000ULL) break;
    }
  }
  uint64_t el = 0;
  if (have_t && nv_timer_read(d, &t1) == 0) el = t1 - t0;
  uint32_t pc_at_stop = nv_rd32(d, b + NV_FALCON_PC);
  uart_puts("[NV-SEC2T] ");
  uart_puts(tag);
  uart_puts(" halt-probe ms=");
  uart_print_hex((uint32_t)(el / 1000000ULL));
  uart_puts(" ctl=");
  uart_print_hex(c);
  uart_puts(" stopped=");
  print_int(stopped);
  uart_puts(" pc_at_stop=");
  uart_print_hex(pc_at_stop);
  uart_puts(" bcr=");
  uart_print_hex(nv_rd32(d, NV_SEC2_BCR));
  uart_puts(stopped ? " verdict=STOPPED (PC and the DMEM diff below are the "
                     "state the core left)\n"
                   : " verdict=STILL-RUNNING after the reset bits: no PC "
                     "claim from this boot\n");
  /* The stop re-arms the select to RISC-V (round 7 measured 0x110); clear it
   * so the page answers real registers again, then take the full row and the
   * block dump -- the dump is what says which words the halt left readable. */
  sec2_select_clear(d, tag);
  sec2_row(d, tag, "post-halt", t1, have_t, 0, 0);
  sec2_page_probe(d, tag, "pg-ctlreset");
  sec2_dmem_diff(d, im, tag);
}

static int sec2_reset_fixup_variant(struct nv_dev *d,
                                    const struct sec2_img *im) {
  const char *tag = "S";
  uint64_t t0 = 0;
  int have_t = nv_timer_read(d, &t0) == 0;

  /* R's one-write-per-bit sweep leaves ctl holding its last value (0x50); start
   * from a cleared control word so this variant's "before" row is clean. */
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL, 0);
  sec2_row(d, tag, "before-reset", 0, 0, 0, 0);
  sec2_reset_apply(d, tag, t0, have_t);
  /* Clear the select FIRST, then apply the fixups: while the RISC-V core is
   * selected the falcon page answers the fence word, so a read-modify-write of
   * 0x624 issued before the clear writes the fence word back (measured in the
   * first cut of this variant: 0x624 read 0xBADF5620 and the write did not
   * land at all). */
  sec2_select_clear(d, tag);
  sec2_state_fixups(d, tag);
  sec2_row(d, tag, "after-bcr", t0, have_t, 0, 0);
  /* The signature the boot ROM verifies goes into the DMEM image before the
   * reload, exactly as sec2_attempt patches it.  S reloads the whole image
   * after the reset scrub, so without this line the launch path would carry
   * whatever bytes the container's patch site held -- and a rejection would be
   * about our own missing patch. */
  {
    uint8_t *dmem = im->file + im->dmem_file;
    uint32_t slot = (SEC2_SIG_SLOT_FORCE < 0x80u) ? SEC2_SIG_SLOT_FORCE
                                                  : im->sig_idx;
    const uint8_t *sig = im->file + im->sig_off + slot * im->sig_slot;
    for (uint32_t i = 0; i < im->sig_slot; i++)
      dmem[im->patch_off + i] = sig[i];
    uart_puts("[NV-SEC2T] ");
    uart_puts(tag);
    uart_puts(" sig-patched slot=");
    print_int((int)slot);
    uart_puts(" bytes=");
    uart_print_hex(im->sig_slot);
    uart_puts(" dmem+0x");
    uart_print_hex(im->patch_off);
    uart_puts("\n");
  }
  sec2_reload_dma(d, im, d->booter_phys, tag);
  sec2_arm(d, im);
  /* Did the load path itself touch DMEM?  The DMA block marked SEC=1 is the
   * engine's secure write path; if it uses DMEM as decrypt scratch, the
   * "DMEM changed" rows after START would be about the transport, not the
   * core.  This diff is taken before START for exactly that reason. */
  sec2_dmem_diff(d, im, "S-pre");
  /* The same blocks the halt will be judged on, before the trigger: the
   * baseline that makes "0x240 reads 0x3002 after the launch" a change rather
   * than a constant. */
  sec2_page_probe(d, tag, "pg-pre");
  uint64_t st = 0;
  (void)sec2_start_trace(d, tag, &st);
  int verified = sec2_verdict_wait(d, im, tag, st, st != 0);
  /* While the core runs, the DMEM port answers nothing useful (s1 measured one
   * read error and no diff) -- so when the verdict did not arrive, stop it and
   * take the state it left: PC, and a DMEM diff that the port can actually
   * serve once the core is no longer running. */
  sec2_dmem_diff(d, im, tag);
  sec2_page_probe(d, tag, "pg-post");
  if (!verified) {
    sec2_halt_probe(d, im, tag);
    /* The three UC_CTRL reset bits did not lift the fence (round 8).  A full
     * falcon reset is the other door the GSP path uses; run it here and dump
     * the same blocks again, so "the fence never lifted" and "the halt state
     * was gone after a real reset" are separate rows.  S is the last variant in
     * the boot, so the scrub cannot disturb anything measured above it. */
    sec2_reset_apply(d, tag, 0, 0);
    sec2_select_clear(d, tag);
    sec2_row(d, tag, "after-full-reset", 0, 0, 0, 0);
    sec2_page_probe(d, tag, "pg-fullreset");
  }
  return verified;
}

/* The build-time switches for the launch-path bisect live at the top of the
 * SEC2 section (SEC2_IMEM_CONTRACT_LAYOUT, SEC2_BOOTVEC_FORCE, SEC2_DMEM_PIO),
 * above the first function that reads them. */

static int sec2_attempt(struct nv_dev *d, const struct sec2_img *im,
                        uint32_t sig_idx, const char *tag);
static void sec2_dmem_diff(struct nv_dev *d, const struct sec2_img *im,
                           const char *tag);

static int sec2_attempt(struct nv_dev *d, const struct sec2_img *im,
                        uint32_t sig_idx, const char *tag) {
  uint8_t *dmem = im->file + im->dmem_file;
  const uint8_t *sig = im->file + im->sig_off + sig_idx * im->sig_slot;

  for (uint32_t i = 0; i < im->sig_slot; i++)
    dmem[im->patch_off + i] = sig[i];
  uart_puts("[NV-SEC2] attempt=");
  uart_puts(tag);
  uart_puts(" sig_slot=");
  print_int((int)sig_idx);
  uart_puts(" head=");
  for (uint32_t i = 0; i < 8; i++) nv_hex_byte(sig[i]);
  uart_puts(" dmem+0x");
  uart_print_hex(im->patch_off);
  uart_puts("\n");

  int rc = falcon_pio_block(d, NV_FALCON_SEC2_BASE, dmem, im->dmem_len, 0, 0, 0);
  if (rc != 0) {
    uart_puts("[NV-SEC2] attempt=");
    uart_puts(tag);
    uart_puts(" DMEM PIO FAILED rc=");
    print_int(rc);
    uart_puts("\n");
    return -1;
  }

  /* ---- landing map: does the port read back the bytes we just wrote? -----
   *
   * Round 2's lesson, applied to SEC2.  The port and the staged buffer are two
   * implementations of the same bytes; a displaced, truncated or silently
   * rewritten image is invisible unless they are compared word for word.  Pure
   * reads through the same port the load used, so this cannot change what the
   * core will see -- and the DMEM probe at +0x10 is also the check that the
   * signature patch landed where the boot ROM looks.
   */
  {
    static const uint32_t kOff[8] = {0x0,    0x10,   0x100,  0x1000,
                                     0x2000, 0x4000, 0x5F00, 0x5FF0};
    struct nv_falcon sf = d->falcon[NV_FALCON_IDX_SEC2];
    if (!sf.base) sf.base = NV_FALCON_SEC2_BASE;
    /* `present` was set by the probe when this window answered the fence word.
     * It answers real registers now (r5b cleared the core select), and flcn_pio
     * refuses to touch a falcon it thinks is absent -- that flag, not the
     * hardware, is what made r5e's landing map return 8/8 read errors. */
    sf.present = 1;
    int match = 0, bad = -1, rerr = 0;
    uint32_t bad_off = 0, bad_got = 0, bad_want = 0;
    for (uint32_t i = 0; i < 8; i++) {
      uint32_t w = 0;
      if (nv_falcon_dmem_read(d, &sf, kOff[i], &w, 1) != 0) {
        rerr++;
        continue;
      }
      const uint8_t *p = dmem + kOff[i];
      uint32_t want = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      if (w == want)
        match++;
      else if (bad < 0) {
        bad = (int)i;
        bad_off = kOff[i];
        bad_got = w;
        bad_want = want;
      }
    }
    uart_puts("[NV-SEC2] attempt=");
    uart_puts(tag);
    uart_puts(" dmem landing match=");
    print_int(match);
    uart_puts("/8 read_errors=");
    print_int(rerr);
    if (bad >= 0) {
      uart_puts(" first_mismatch@");
      uart_print_hex(bad_off);
      uart_puts(" got=");
      uart_print_hex(bad_got);
      uart_puts(" want=");
      uart_print_hex(bad_want);
    }
    uart_puts(rerr == 8
                  ? " verdict=DMEM-UNREADABLE (no probe answered: no claim "
                    "either way -- this is not evidence the image is absent)\n"
                  : (match == 8
                         ? " verdict=LANDED (the port reads the staged image back "
                           "word for word, signature patch included)\n"
                         : " verdict=NOT-LANDED (the image the core sees is not "
                           "the image we staged; raw counts above)\n"));
    /* IMEM with the same SECRET bit the load used.  A marker word is an
     * unreadable page, not a mismatch, and is reported as such. */
    uint32_t iunread = 0, imatch = 0;
    int ihave_bad = 0;
    uint32_t ibad_off = 0, ibad_got = 0;
    for (uint32_t i = 0; i < 4; i++) {
      static const uint32_t kI[4] = {0x0, 0x100, 0x4000, 0x7500};
      uint32_t w = 0;
      if (nv_falcon_imem_read_ex(d, &sf, kI[i], &w, 1, 1) != 0) {
        iunread++;
        continue;
      }
      const uint8_t *p = im->file + im->imem_file + kI[i];
      uint32_t want = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      if (w == want)
        imatch++;
      else if (!ihave_bad) {
        ihave_bad = 1;
        ibad_off = kI[i];
        ibad_got = w;
      }
    }
    uart_puts("[NV-SEC2] attempt=");
    uart_puts(tag);
    uart_puts(" imem landing match=");
    print_int((int)imatch);
    uart_puts("/4 read_errors=");
    print_int(iunread);
    if (ihave_bad) {
      uart_puts(" first_mismatch@");
      uart_print_hex(ibad_off);
      uart_puts(" got=");
      uart_print_hex(ibad_got);
    }
    uart_puts(iunread == 4 && imatch == 0
                  ? " verdict=IMEM-UNREADABLE (all four probes refused)\n"
                  : (imatch == 4
                         ? " verdict=IMEM-LANDED\n"
                         : (ihave_bad && ibad_got == 0xDEAD5EC1u
                                ? " verdict=IMEM-SECURE-MARKER (0xDEAD5EC1 is "
                                  "the code port's answer for a secure page -- "
                                  "the page is present and secure, its bytes are "
                                  "not readable, so this says nothing about the "
                                  "content: the boot ROM is the only reader)\n"
                                : " verdict=IMEM-MISMATCH (the code the core "
                                  "would fetch is not the code we staged)\n")));
  }

  /* Boot ROM parameters: where the signature is, which engine and ucode id it
   * must cover, and which algorithm. */
  nv_wr32(d, NV_BROM_PARAADDR0, im->patch_off);
  nv_wr32(d, NV_BROM_ENGIDMASK, 1);
  nv_wr32(d, NV_BROM_CURR_UCODE_ID, im->ucode_id);
  nv_wr32(d, NV_BROM_MOD_SEL, 1);
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_ENTRY, im->bootvec);
  uart_puts("[NV-SEC2] attempt=");
  uart_puts(tag);
  uart_puts(" brom para=");
  uart_print_hex(nv_rd32(d, NV_BROM_PARAADDR0));
  uart_puts(" engid=");
  uart_print_hex(nv_rd32(d, NV_BROM_ENGIDMASK));
  uart_puts(" ucode=");
  uart_print_hex(nv_rd32(d, NV_BROM_CURR_UCODE_ID));
  uart_puts(" modsel=");
  uart_print_hex(nv_rd32(d, NV_BROM_MOD_SEL));
  uart_puts(" bootvec=");
  uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_ENTRY));
  uart_puts("\n");

  /* MAILBOX0/1 carry the metadata block's address -- the booter's only
   * argument, and the word it overwrites with its return code (0 = success).
   * That makes the argument its own sentinel: a mailbox still holding the
   * address is "no code written", which is a different fact from "code 0". */
  nv_wr32(d, NV_FALCON_SEC2_BASE + 0x040, (uint32_t)im->meta_phys);
  nv_wr32(d, NV_FALCON_SEC2_BASE + 0x044, (uint32_t)(im->meta_phys >> 32));

  /* Does this window take writes, and what does the core do with START?
   * Both rows are needed: the first is what makes the second mean something. */
  sec2_write_land_control(d, tag);
  int core_reacted = sec2_start_trace(d, tag, 0);
  (void)core_reacted;
  uint32_t ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
  uint32_t mb0 = nv_rd32(d, NV_FALCON_SEC2_BASE + 0x040);
  int halted = (ctl & NV_FALCON_UC_CTRL_STOPPED) ? 1 : 0;

  /* The verdict that needs no register: did the booter write its own
   * acknowledgement into the metadata block?  Polled for a bounded time,
   * because the booter builds WPR2 before it answers. */
  int verified = 0;
  uint64_t v = 0;
  if (im->meta) {
    uint64_t t0 = 0, now = 0;
    int have_t = nv_timer_read(d, &t0) == 0;
    for (int i = 0; i < 200000; i++) {
      v = nv_meta_get64(im->meta + NV_WPR_OFF_VERIFIED);
      if (v == NV_WPR_VERIFIED_WORD) break;
      if (have_t && (i & 0x3f) == 0 && nv_timer_read(d, &now) == 0 &&
          now - t0 > 300000000ULL)
        break; /* 300 ms: the reference's own budget for this step */
    }
    if (v == NV_WPR_VERIFIED_WORD) verified = 1;
  }
  uart_puts("[NV-SEC2] attempt=");
  uart_puts(tag);
  uart_puts(" start ctl=");
  uart_print_hex(ctl);
  uart_puts(" halted=");
  print_int(halted);
  uart_puts(" mailbox0=");
  uart_print_hex(mb0);
  uart_puts(" mailbox1=");
  uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + 0x044));
  uart_puts(" verified=");
  uart_print_hex(v);
  uart_puts(" pc_after=");
  uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_PC));
  if (verified) {
    uart_puts(" verdict=RAN (0xA0A0... in the metadata: the block was accepted "
              "and WPR2 is built)\n");
  } else if (v == 0x5EC2A0A0ULL) {
    uart_puts(" verdict=NOTHING-WRITTEN");
    if (mb0 == (uint32_t)im->meta_phys)
      uart_puts(" (the mailbox still holds the argument: the booter wrote no "
                "return code either, which is what a boot ROM rejection looks "
                "like from this side)\n");
    else
      uart_puts(" (mailbox0 changed to a return code: authentication or layout "
                "rejection, code above)\n");
  } else {
    uart_puts(" verdict=THIRD-STATE (neither sentinel nor 0xA0A0: report raw)\n");
  }
  (void)halted;
  /* What did the core leave in its own data image?  A rejection that writes a
   * status word is visible here, and so is silence. */
  sec2_dmem_diff(d, im, tag);
  return verified ? 1 : 0;
}

/* ---- what the core left in DMEM ---------------------------------------
 *
 * A boot ROM that rejects an image does not have to be silent: several falcon
 * ROMs leave a status word in DMEM.  With the port answering (r5b cleared the
 * core select; `present` is forced because the probe set it while the window
 * was still fenced) the whole DMEM image can be compared against the bytes we
 * staged, and every word the core changed is visible.
 *
 * Read-only, and it runs after the core has halted, so it cannot influence the
 * attempt it describes.
 */
static void sec2_dmem_diff(struct nv_dev *d, const struct sec2_img *im,
                           const char *tag) {
  struct nv_falcon sf = d->falcon[NV_FALCON_IDX_SEC2];
  if (!sf.base) sf.base = NV_FALCON_SEC2_BASE;
  sf.present = 1;
  const uint8_t *img = im->file + im->dmem_file;
  uint32_t words = im->dmem_len / 4u;
  uint32_t chunk[64];
  uint32_t changed = 0, first_off = 0, first_got = 0, first_want = 0;
  int have_first = 0, rerr = 0;
  uint32_t ncap = 0, cap_off[6], cap_got[6], cap_want[6];
  for (uint32_t done = 0; done < words; done += 64) {
    uint32_t n = words - done;
    if (n > 64) n = 64;
    if (nv_falcon_dmem_read(d, &sf, done * 4u, chunk, (int)n) != 0) {
      rerr++;
      break;
    }
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t *p = img + (done + i) * 4u;
      uint32_t want = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      if (chunk[i] != want) {
        changed++;
        if (!have_first) {
          have_first = 1;
          first_off = (done + i) * 4u;
          first_got = chunk[i];
          first_want = want;
        }
        if (ncap < 6) {
          cap_off[ncap] = (done + i) * 4u;
          cap_got[ncap] = chunk[i];
          cap_want[ncap] = want;
          ncap++;
        }
      }
    }
  }
  uart_puts("[NV-SEC2] attempt=");
  uart_puts(tag);
  uart_puts(" dmem after changed=");
  uart_print_hex(changed);
  uart_puts("/");
  uart_print_hex(words);
  uart_puts(" read_errors=");
  print_int(rerr);
  if (have_first) {
    uart_puts(" first@");
    uart_print_hex(first_off);
    uart_puts(" got=");
    uart_print_hex(first_got);
    uart_puts(" staged=");
    uart_print_hex(first_want);
  }
  uart_puts(rerr ? " verdict=PARTIAL-READ (report raw)\n"
                 : (changed == 0
                        ? " verdict=DMEM UNTOUCHED (the core wrote nothing back "
                          "into the data image: no status word, no trace)\n"
                        : " verdict=DMEM CHANGED (the core wrote into its own "
                          "data image: the row above names where and what)\n"));
  /* What it wrote, and where: up to six changed words, then the head of DMEM
   * as it stands.  A boot ROM or ucode that leaves a status block usually
   * leaves it at a fixed low offset; a stack is at the top.  Without this a
   * count of 258 changed words cannot distinguish either. */
  for (uint32_t i = 0; i < ncap; i++) {
    uart_puts("[NV-SEC2] attempt=");
    uart_puts(tag);
    uart_puts(" dmem-chg@");
    uart_print_hex(cap_off[i]);
    uart_puts(" got=");
    uart_print_hex(cap_got[i]);
    uart_puts(" staged=");
    uart_print_hex(cap_want[i]);
    uart_puts("\n");
  }
  if (rerr == 0) {
    uint32_t head[16];
    if (nv_falcon_dmem_read(d, &sf, 0, head, 16) == 0) {
      uart_puts("[NV-SEC2] attempt=");
      uart_puts(tag);
      uart_puts(" dmem-head");
      for (uint32_t i = 0; i < 16; i++) {
        uart_puts(" ");
        uart_print_hex(head[i]);
      }
      uart_puts("\n");
    }
  }
}

/* `force` runs the attempt even when the SEC2 window refuses to be read.
 *
 * Why that is worth a deploy: the fence is READ-side only (measured on the GSP
 * window: every write lands, every read answers the fence word), and the
 * booter's own verdict channel -- the metadata block's `verified` word -- lives
 * in host memory, so the question "did the booter run?" does not need the
 * fenced window at all.  The window's refusal therefore stops the *observation*
 * of the attempt, not the attempt.
 *
 * The assumption being spent: that SEC2's window is like the GSP's in this
 * respect.  It is measured on the GSP and INFERRED here; if the booter does not
 * run, one reading is that SEC2's fence also drops writes.
 *
 * And because `verified` == 0 is also what an untouched block reads, it is
 * pre-filled with a sentinel before the start and logged after: a value that
 * came back is evidence, a value nobody wrote is not. */
int nv_gsp_sec2_start_booter_ex(struct nv_dev *d, const uint8_t *booter,
                                uint32_t len, uint64_t meta_phys, int force) {
  if (!d || !booter || len < 0x7700 + 0x6000) return -EINVAL;
  if (meta_phys == 0) return -EINVAL;

  uint8_t *meta = (uint8_t *)phys_to_virt(meta_phys);
  uint64_t ver_before = meta ? nv_meta_get64(meta + NV_WPR_OFF_VERIFIED) : 0;

  int rc = sec2_unfence(d);
  if (rc != 0 && !force) return rc;
  if (rc != 0) {
    uart_puts("[NV-SEC2] FORCED: the fence refused the read path.  The fence is "
              "read-side only (measured on the GSP window) and the booter's "
              "verdict channel -- the metadata's verified word -- is in host "
              "memory, so the attempt is made and judged there\n");
  }
  /* Pre-fill the verdict word: 0 is also "nobody wrote here". */
  if (meta) {
    nv_meta_put64(meta + NV_WPR_OFF_VERIFIED, 0x5EC2A0A0ULL);
    uart_puts("[NV-SEC2] meta verified prefill: was=");
    uart_print_hex(ver_before);
    uart_puts(" now=");
    uart_print_hex(nv_meta_get64(meta + NV_WPR_OFF_VERIFIED));
    uart_puts(" (sentinel 0x5ec2a0a0; the booter must overwrite it with "
              "0xa0a0...)\n");
  }

  /* Halt it: never load a core that is running.  On a fenced window the
   * control word cannot be read, so the reset is issued and the launch is not
   * gated on a value the window refuses to show. */
  uint32_t ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
  int ctl_readable = !(ctl == NV_REG_FENCED || ctl == 0xffffffffu);
  if (!ctl_readable) {
    nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL,
            NV_FALCON_UC_CTRL_RESET_TLB | NV_FALCON_UC_CTRL_RESET_UNK2 |
                NV_FALCON_UC_CTRL_RESET_UNK3);
    uart_puts("[NV-SEC2] halt unobservable (ctl reads the fence word): reset "
              "issued, launch NOT gated on it\n");
  } else {
    if (!(ctl & NV_FALCON_UC_CTRL_STOPPED)) {
      nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL,
              NV_FALCON_UC_CTRL_RESET_TLB | NV_FALCON_UC_CTRL_RESET_UNK2 |
                  NV_FALCON_UC_CTRL_RESET_UNK3);
      for (int i = 0; i < 100000; i++) {
        ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
        if (ctl & NV_FALCON_UC_CTRL_STOPPED) break;
      }
    }
    uart_puts("[NV-SEC2] halt ctl=");
    uart_print_hex(ctl);
    if (!(ctl & NV_FALCON_UC_CTRL_STOPPED)) {
      /* A readable window that will not report STOPPED is a new observation,
       * not a reason to throw the attempt away: the reset above was issued, and
       * skipping the load because a handshake nobody has measured on this part
       * did not answer would spend the deploy on the handshake.  The row says
       * which happened, and the verdict channels below need no ctl at all. */
      uart_puts(" NOT-STOPPED after the reset: the reset was issued and the "
                "load proceeds (the row above records it; the verdict channels "
                "do not depend on ctl)\n");
    } else {
      uart_puts("\n");
    }
  }

  /* ---- FALCON_RM (0x084) <- PMC_BOOT_0 --------------------------------
   *
   * The GSP path writes this at the end of its reset and says why: "every falcon
   * enable in the reference writes it, before the first load".  SEC2 never got
   * it, which is the one state fixup of the GSP path this window was still
   * missing (the other, the core select, is cleared just below).  One write, the
   * same value the GSP carries, and the row prints both sides.
   */
  {
    uint32_t rm = nv_rd32(d, 0x000000);
    uint32_t was = nv_rd32(d, NV_FALCON_SEC2_BASE + 0x084);
    nv_wr32(d, NV_FALCON_SEC2_BASE + 0x084, rm);
    uart_puts("[NV-SEC2] falcon_rm(0x084) was=");
    uart_print_hex(was);
    uart_puts(" <- pmc_boot0=");
    uart_print_hex(rm);
    uart_puts(" now=");
    uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + 0x084));
    uart_puts("\n");
  }

  /* ---- SEC2's RISC-V core select, cleared exactly where the GSP's was ----
   *
   * The GSP measurement (round 1, F3) is that 0x111668 is unreadable *before*
   * the reset and reads 0x110 -- RISCV|BRFETCH -- once the reset has run.  So
   * this step belongs after the reset, not before it: the same order the GSP
   * path uses, and the reason the read may answer here at all. */
  sec2_core_select(d);

  /* The DMA reads system memory coherently and physically. */
  nv_wr32(d, NV_SEC2_FBIF_TRANSCFG, NV_LOCAL_TRANSCFG_COHERENT);
  uart_puts("[NV-SEC2] transcfg=");
  uart_print_hex(nv_rd32(d, NV_SEC2_FBIF_TRANSCFG));
  uart_puts("\n");

  /* ---- where the image actually is, and which signature it needs --------
   *
   * Three defects lived in the twelve lines that used to be here, and each one
   * alone makes the attempt meaningless:
   *
   *  1. The load header's offsets are relative to the container's DATA REGION
   *     (nv_fw.h: os_code(0,0x100), os_data(0x7700,0x6000), app_code(0x100,
   *     0x7600) -- and those sizes sum to data_size exactly, which is what
   *     settles the reading).  They were used as FILE offsets, so every block
   *     fetched 0x378 bytes (data_off) ahead of the image it meant: the first
   *     IMEM block came out of the signature block, and the DMEM read stopped
   *     0x378 bytes short of the end of the file.  Rows below print the
   *     arithmetic and the check that settles it (the two regions must tile
   *     the file and land exactly on its end).
   *  2. The DMA's destination never advanced (`dst` was 0 for every block), so
   *     all 118 IMEM blocks landed on IMEM offset 0 and the core would have run
   *     the last 256 bytes of the image.  Round 1 fixed exactly this on the
   *     FWSEC path and the booter path never got the fix.
   *  3. The production signature was copied from slot 0.  The container carries
   *     `num_sig` slots of `sig_prod_size / num_sig` bytes each (measured: 2 at
   *     file offset 0x3C, 0x180 bytes each -- two *different* images), and the
   *     boot ROM picks the one the part's fuse selects.  Round 1 found the same
   *     defect on FWSEC ("index 0 was patched, the fuse selects 1").
   *
   * Everything below is derived from the file and the fuse register, and each
   * derivation prints the numbers it used.
   */
  uint64_t base = d->booter_phys;
  struct nv_fw_container bc;
  struct nv_fw_hs bhs;
  for (uint32_t i = 0; i < (uint32_t)sizeof bc; i++) ((uint8_t *)&bc)[i] = 0;
  for (uint32_t i = 0; i < (uint32_t)sizeof bhs; i++) ((uint8_t *)&bhs)[i] = 0;
  int cparsed = (nv_fw_container_parse(booter, len, &bc) == 0 &&
                 nv_fw_hs_parse(&bc, &bhs) == 0);
  uint32_t imem_file = 0x100, imem_len = 0x7600;
  uint32_t dmem_file = 0x7700, dmem_len = 0x6000;
  uint32_t bootvec = 0x100;
  if (cparsed) {
#if SEC2_IMEM_CONTRACT_LAYOUT
    /* IMEM = os_code + app, both from the container's data region, at IMEM 0;
     * BOOTVEC = app[0].code_off (0x100), per the contract's measured row. */
    imem_file = bc.data_off + bhs.os.code_off;
    imem_len = bhs.num_apps ? (bhs.app[0].code_off + bhs.app[0].code_size)
                            : 0x7700u;
    dmem_file = bc.data_off + bhs.os.data_off;
    dmem_len = bhs.os.data_size;
    bootvec = bhs.num_apps ? bhs.app[0].code_off : 0x100u;
    uart_puts("[NV-SEC2] layout=os_code+app@imem0 contract entry=0x100\n");
#else
    /* IMEM is the APP code alone, at IMEM 0, with the entry at 0: the contract's
     * own reading ("0x7600 bytes from image offset 0x100") and the value FWSEC's
     * working launch uses for the entry (0x104 = 0, with its code at IMEM 0).
     * r5c/r5d put os_code + app at IMEM 0 and set the entry to app[0].code_off
     * (0x100) -- the same bytes, 0x100 out of place relative to the entry, and
     * the boot ROM has no way to tell us which one it wanted. */
    imem_file = bc.data_off + (bhs.num_apps ? bhs.app[0].code_off : 0x100u);
    imem_len = bhs.num_apps ? bhs.app[0].code_size : 0x7600u;
    dmem_file = bc.data_off + bhs.os.data_off;
    dmem_len = bhs.os.data_size;
    bootvec = 0;
    uart_puts("[NV-SEC2] layout=app@imem0 entry=0\n");
#endif
#if SEC2_BOOTVEC_FORCE
    bootvec = SEC2_BOOTVEC_FORCE;
    uart_puts("[NV-SEC2] bootvec forced by this build = ");
    uart_print_hex(bootvec);
    uart_puts("\n");
#endif
    uart_puts("[NV-SEC2] image data_off=");
    uart_print_hex(bc.data_off);
    uart_puts(" imem@file=");
    uart_print_hex(imem_file);
    uart_puts(" len=");
    uart_print_hex(imem_len);
    uart_puts(" dmem@file=");
    uart_print_hex(dmem_file);
    uart_puts(" len=");
    uart_print_hex(dmem_len);
    uart_puts(" bootvec=");
    uart_print_hex(bootvec);
    uart_puts(" ends@=");
    uart_print_hex(dmem_file + dmem_len);
    uart_puts(" file_size=");
    uart_print_hex(len);
    uart_puts((dmem_file + dmem_len == len && imem_file + imem_len == dmem_file)
                  ? " verdict=TILES-THE-FILE (IMEM runs into the DMEM image and "
                    "the image ends exactly at the end of the file)\n"
                  : " verdict=DOES-NOT-TILE (the offsets below do not line up "
                    "with the file: read them as raw)\n");
  } else {
    uart_puts("[NV-SEC2] container parse FAILED: falling back to the literal "
              "0x100/0x7600 and 0x7700/0x6000 offsets, which are correct only "
              "if data_off happens to be 0 -- the attempt is still made so the "
              "rows exist\n");
  }

  /* The patch site: the descriptor names a file offset whose dword is where
   * the signature goes, inside the image.  Measured 0x7710 for a DMEM image at
   * 0x7700.  Derived rather than literal so a firmware change cannot silently
   * move it, and checked against the DMEM image's own extent. */
  uint32_t patch_off = 0x10;
  uint32_t patch_site = 0;
  if (cparsed && bhs.patch_loc) {
    patch_site = nv_img_rd32(booter + bhs.patch_loc);
    if (patch_site > bhs.os.data_off &&
        patch_site - bhs.os.data_off + 0x180u <= dmem_len)
      patch_off = patch_site - bhs.os.data_off;
  }

  /* Which 384-byte slot the boot ROM will accept.  The fuse word is a bitmask
   * of versions this part supports (one dword per ucode id, measured 0x1 for
   * every id on this card); the descriptor's slot count is what the container
   * declares.  The container carries no SignatureVersions mask of its own, so
   * "the slots offer versions 0..num_sig-1" is INFERRED from the count and from
   * the FWSEC path, where the same rule with a 0x3 mask chose slot 1 and the
   * image then ran.  Both fingerprints are logged, so a wrong choice is
   * visible in the next run's row rather than inferred from a failure. */
  uint32_t ucode_id = 3; /* BROM_CURR_UCODE_ID is written from this below, so the
                          * fuse lookup and the register carry one value */
  uint32_t sig_slot = 0x180, sig_idx = 0, sig_fuse = 0, sig_vers = 0;
  if (cparsed && bhs.num_sig) {
    sig_slot = bhs.sig_prod_size / bhs.num_sig;
    sig_vers = (bhs.num_sig >= 32u) ? 0xFFFFFFFFu : ((1u << bhs.num_sig) - 1u);
  }
  sig_fuse = nv_rd32(d, NV_PFUSED_FW_VERSION0 + (ucode_id - 1u) * 4u);
  {
    uint32_t idx = nv_fwsec_sig_index(sig_fuse, sig_vers, bhs.num_sig);
    if (idx == NV_FWSEC_SIG_NONE) {
      uart_puts("[NV-SEC2] sig fuse=");
      uart_print_hex(sig_fuse);
      uart_puts(" slots=");
      print_int((int)bhs.num_sig);
      uart_puts(" -> NO SELECTABLE SLOT (fuse and slot count do not "
                "overlap): using slot 0, and a rejection here is the fuse "
                "rather than the transport\n");
    } else {
      sig_idx = idx;
    }
  }

  /* The signature choice, printed once: the helper patches and logs per
   * attempt, but the derivation belongs in the log before either runs. */
  uart_puts("[NV-SEC2] sig block@");
  uart_print_hex((cparsed && bhs.sig_prod_off) ? bhs.sig_prod_off : 0x3Cu);
  uart_puts(" slots=");
  print_int((int)bhs.num_sig);
  uart_puts(" bytes=");
  uart_print_hex(sig_slot);
  uart_puts(" fuse@");
  uart_print_hex(NV_PFUSED_FW_VERSION0 + (ucode_id - 1u) * 4u);
  uart_puts("=");
  uart_print_hex(sig_fuse);
  uart_puts(" patchsite=");
  uart_print_hex(patch_site);
  uart_puts(" -> dmem+0x");
  uart_print_hex(patch_off);
  uart_puts(" fuse_rule_slot=");
  print_int((int)sig_idx);
  uart_puts("\n");

  /* ---- the DMA engine's configuration, the same three writes the GSP path
   * makes before its load (nv_gsp_fwsec_reset): 0x624 bit 7, 0x10C = 0, and the
   * context word in 0x600.  r5b's SEC2 load stalled at block 19 with 0x118
   * reading 0x617 (bit 0, "a command is still pending") and 0x114/0x11C still
   * holding block 18's values -- i.e. block 19 was never written, the queue
   * stopped draining.  The GSP's 0x118 after a *successful* 242-block load reads
   * 0x616, one bit short of identical, so the engine is in the state this card
   * reaches when the load works, minus the configuration those writes provide.
   */
  {
    uint32_t v624 = nv_rd32(d, NV_FALCON_SEC2_BASE + 0x624);
    nv_wr32(d, NV_FALCON_SEC2_BASE + 0x624, v624 | (1u << 7));
    nv_wr32(d, NV_FALCON_SEC2_BASE + 0x10C, 0);
    nv_wr32(d, NV_FALCON_SEC2_BASE + 0x600, NV_LOCAL_TRANSCFG_COHERENT);
    uart_puts("[NV-SEC2] dma cfg 624=");
    uart_print_hex(v624);
    uart_puts(" -> ");
    uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + 0x624));
    uart_puts(" 10c=");
    uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + 0x10C));
    uart_puts(" 600=");
    uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + 0x600));
    uart_puts(" (the GSP path's own pre-load writes)\n");
  }

  /* IMEM then DMEM, 256-byte blocks, IMEM as secret pages, the destination
   * advancing with the source -- the defect round 1 fixed on the GSP.
   *
   * If the DMA queue stalls, the same image goes in through the PIO port
   * instead: one index write, one data register, no request and no status word
   * to poll (round 2 measured that path on this card, and F2's whole point was
   * that PIO needs no observable engine state).  The row says which transport
   * actually fed the core, because "it ran" and "how it was loaded" are
   * different facts and only one of them is about the booter. */
  int via_pio = 0;
  rc = 0;
  {
    uint32_t off = 0;
    for (; off < imem_len; off += 256) {
      rc = falcon_dma_block(d, NV_FALCON_SEC2_BASE, base, imem_file + off, off,
                            1, 1);
      if (rc != 0) break;
    }
    if (rc != 0) {
      uart_puts("[NV-SEC2] IMEM dma FAILED at off=");
      uart_print_hex(off);
      uart_puts(" rc=");
      print_int(rc);
      uart_puts(" dma state 110=");
      uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + NV_XFER_EXT_BASE));
      uart_puts(" 114=");
      uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + NV_XFER_FALCON_ADDR));
      uart_puts(" 118=");
      uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + NV_XFER_CTRL));
      uart_puts(" 11c=");
      uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + NV_XFER_EXT_ADDR));
      uart_puts(" 120=");
      uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + NV_XFER_STATUS));
      uart_puts("\n");
    }
  }
  if (rc == 0) {
    for (uint32_t off = 0; off < dmem_len; off += 256) {
      rc = falcon_dma_block(d, NV_FALCON_SEC2_BASE, base, dmem_file + off, off,
                            0, 0);
      if (rc != 0) {
        uart_puts("[NV-SEC2] DMEM dma FAILED at off=");
        uart_print_hex(off);
        uart_puts(" rc=");
        print_int(rc);
        uart_puts("\n");
        break;
      }
    }
  }
  if (rc != 0) {
    uart_puts("[NV-SEC2] DMA did not finish the load: falling back to PIO for "
              "the whole image (index + data, no queue to stall)\n");
    via_pio = 1;
    rc = falcon_pio_block(d, NV_FALCON_SEC2_BASE, booter + imem_file, imem_len,
                          0, 1, 1);
    if (rc == 0)
      rc = falcon_pio_block(d, NV_FALCON_SEC2_BASE, booter + dmem_file,
                            dmem_len, 0, 0, 0);
    if (rc != 0) {
      uart_puts("[NV-SEC2] PIO load FAILED rc=");
      print_int(rc);
      uart_puts(" (index register out of range, or the port refused)\n");
      return rc;
    }
  }
  uart_puts("[NV-SEC2] IMEM loaded ");
  uart_print_hex(imem_len);
  uart_puts(" bytes from file+");
  uart_print_hex(imem_file);
  uart_puts(" + DMEM ");
  uart_print_hex(dmem_len);
  uart_puts(" from file+");
  uart_print_hex(dmem_file);
  uart_puts(" transport=");
  uart_puts(via_pio ? "PIO\n" : "DMA (destination advances, IMEM SEC pages)\n");

  /* ---- the attempts -----------------------------------------------------
   *
   * A: the slot the fuse rule picks.  B: the other slot, and only if A left the
   * metadata untouched -- one boot, one variable, and if neither works the
   * signature slot is ruled out rather than queued for another deploy. */
  struct sec2_img sim;
  sim.file = (uint8_t *)booter;
  sim.imem_file = imem_file;
  sim.imem_len = imem_len;
  sim.dmem_file = dmem_file;
  sim.dmem_len = dmem_len;
  sim.sig_off = (cparsed && bhs.sig_prod_off) ? bhs.sig_prod_off : 0x3Cu;
  sim.sig_slot = sig_slot;
  sim.sig_idx = sig_idx;
  sim.patch_off = patch_off;
  sim.bootvec = bootvec;
  sim.ucode_id = ucode_id;
  sim.meta_phys = meta_phys;
  sim.meta = meta;

  int verified = sec2_attempt(d, &sim, sig_idx, "A");
  if (verified == 0 && bhs.num_sig > 1) {
    uart_puts("[NV-SEC2] attempt A left the metadata at its sentinel.  The "
              "signature slot is the one thing this driver chooses that the "
              "container does not state (two slots, no SignatureVersions dword), "
              "and the fuse rule used above is inherited from FWSEC, whose "
              "descriptor carries that mask.  Trying the other slot in this "
              "same boot: same image, same IMEM, same transport, only the 384 "
              "signature bytes differ\n");
    /* A clean start: halt, then let the core come back STOPPED. */
    nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL,
            NV_FALCON_UC_CTRL_RESET_TLB | NV_FALCON_UC_CTRL_RESET_UNK2 |
                NV_FALCON_UC_CTRL_RESET_UNK3);
    uint32_t c2 = 0;
    for (int i = 0; i < 100000; i++) {
      c2 = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
      if (c2 & NV_FALCON_UC_CTRL_STOPPED) break;
    }
    uart_puts("[NV-SEC2] re-halt before B: ctl=");
    uart_print_hex(c2);
    uart_puts("\n");
    verified = sec2_attempt(d, &sim, (sig_idx + 1u) % bhs.num_sig, "B");
  }
  if (verified) return 0;
  /* Both signature attempts were refused at the trigger, so spend the rest of
   * this boot on the one cause neither of them touched: the core may be held
   * without the full falcon reset the GSP path applies (the halt step here only
   * issues UC_CTRL's reset bits, and only when ctl is unreadable or not
   * already stopped -- on this card it is always already stopped). */
  sec2_reset_variant(d, &sim);
  if (sec2_reset_fixup_variant(d, &sim)) {
    uart_puts("[NV-SEC2] the reset+fixups launch path (S) returned the "
              "booter's own verdict word: stage 5 is successful, and the S "
              "rows above are the launch record\n");
    return 0;
  }
  uart_puts("[NV-SEC2] neither signature slot produced a verdict, and the "
            "reset+fixups launch path did not either: the metadata "
            "block was not written, so the booter did not reach it.  With the "
            "window readable, the transport validated and every host write "
            "echoing back, what is left is the image the boot ROM sees (the "
            "signature's coverage, the ucode id/engine mask, or the IMEM layout "
            "and entry point)\n");
  return -EPERM;
}

int nv_gsp_firmware_check(struct nv_dev *d) {
  if (!d) return -EINVAL;
  int rc = 0;
  /* The VBIOS first: FWSEC's ucode lives there, and the probe is read-only. */
  (void)nv_vbios_probe(d);
  /* The booter must stay resident: SEC2 DMAs out of these frames, and patches
   * the signature into them before the transfer. */
  struct nv_fw_blob booter;
  int bldrc = nv_fw_load_blob(NV_FW_BOOTER, &booter);
  if (bldrc == 0) {
    struct nv_fw_container bc;
    struct nv_fw_hs bhs;
    if (nv_fw_container_parse(booter.buf, booter.size, &bc) == 0 &&
        nv_fw_hs_parse(&bc, &bhs) == 0) {
      uart_puts("[NV-GSP] fw BOOTER.BIN bytes=");
      print_int((int)booter.size);
      uart_puts(" phys=");
      uart_print_hex(booter.phys);
      uart_puts(" container ok data_off=");
      uart_print_hex(bc.data_off);
      uart_puts(" data_size=");
      uart_print_hex(bc.data_size);
      uart_puts(" sig=");
      uart_print_hex(bhs.sig_prod_size);
      uart_puts("\n");
      d->booter_phys = booter.phys;
      d->booter_size = booter.size;
    } else {
      uart_puts("[NV-GSP] fw BOOTER.BIN parse FAILED\n");
    }
  } else {
    uart_puts("[NV-GSP] fw BOOTER.BIN load FAILED rc=");
    print_int(bldrc);
    uart_puts("\n");
  }
  uint32_t lsize = 0;
  uint8_t lhash[8] = {0};
  (void)nv_gsp_load_one(d, NV_FW_LOADER, 0, &lsize, lhash);
  /* The bootloader stays resident: the metadata block points at its data region
   * (0x8000 bytes at file offset 0x6C), so freeing it before the block is built
   * would describe a buffer somebody else now owns. */
  struct nv_fw_blob loader;
  int ldr_rc = nv_fw_load_blob(NV_FW_LOADER, &loader);
  /* The RM image is not loaded as a blob and handed over: the booter reads it
   * through a radix3 tree, and the tree must describe the .fwimage SECTION, not
   * the whole ELF (the meta wants 0x2448000 bytes, the file is larger).  So:
   * load, parse the ELF, then build the tree over the section.  Pure memory
   * work -- no MMIO -- which is why it runs on every boot and reports numbers a
   * reviewer can check against the file. */
  struct nv_fw_blob rm;
  int lrc = nv_fw_load_blob(NV_FW_RM, &rm);
  uart_puts("[NV-GSP] fw ");
  uart_puts(NV_FW_RM);
  if (lrc != 0) {
    uart_puts(" load FAILED rc=");
    print_int(lrc);
    uart_puts("\n");
    return rc;
  }
  uart_puts(" bytes=");
  print_int((int)rm.size);
  uart_puts(" phys=");
  uart_print_hex(rm.phys);

  /* Integrity before anything mutates the buffer: head and tail of the file,
   * the same check the small blobs get. */
  {
    uint8_t sha[32];
    uint32_t n = rm.size < NV_FW_SAMPLE_BYTES ? rm.size : NV_FW_SAMPLE_BYTES;
    if (nv_fw_sha256(rm.buf, n, sha) == 0) {
      uart_puts(" head=");
      for (uint32_t i = 0; i < 4; i++) nv_hex_byte(sha[i]);
      if (rm.size > NV_FW_SAMPLE_BYTES &&
          nv_fw_sha256(rm.buf + rm.size - NV_FW_SAMPLE_BYTES,
                       NV_FW_SAMPLE_BYTES, sha) == 0) {
        uart_puts(" tail=");
        for (uint32_t i = 0; i < 4; i++) nv_hex_byte(sha[i]);
      }
    }
  }

  struct nv_fw_elf elf;
  if (nv_fw_elf_sections(rm.buf, rm.size, &elf) != 0) {
    uart_puts(" elf FAILED (not a relocatable image)\n");
    nv_fw_release(&rm);
    return rc;
  }
  uart_puts(" fwimage off=");
  uart_print_hex(elf.fwimage.off);
  uart_puts(" size=");
  uart_print_hex(elf.fwimage.size);
  uart_puts(" sigs=");
  print_int((int)elf.nsig);
  /* Where the signature sections actually live: the metadata block needs the
   * system-physical address of the .fwsignature_<engine> bytes, and whether they
   * sit before or after .fwimage decides whether the slide above destroyed them. */
  for (uint32_t i = 0; i < elf.nsig && i < NV_FW_SIG_MAX; i++) {
    uart_puts("\n[NV-GSP] sig[");
    print_int((int)i);
    uart_puts("] ");
    uart_puts(elf.sig[i].name);
    uart_puts(" off=");
    uart_print_hex(elf.sig[i].off);
    uart_puts(" size=");
    uart_print_hex(elf.sig[i].size);
  }

  /* The tree describes 4 KiB pages, so the image must START on a page. On this
   * firmware .fwimage sits at file offset 0x40 -- inside the ELF header page --
   * and pointing the tree at rm.phys would hand the booter 0x40 bytes of ELF
   * header in front of the image it expects.
   *
   * Fix without a second 38 MB buffer: slide the section down inside the frames
   * we already own (a backwards byte move, so overlapping is safe). The blob's
   * base is page-aligned by construction, so afterwards [phys, phys+size) is the
   * image, exactly page-aligned, and nothing was allocated twice. */
  if (elf.fwimage.off != 0) {
    uart_puts("\n[NV-GSP] radix3 slide .fwimage down by ");
    uart_print_hex(elf.fwimage.off);
    uart_puts(" inside its own frames (no second 38 MB buffer)\n");
    nv_fw_slide_down(rm.buf, rm.size, elf.fwimage.off, elf.fwimage.size);
  }
  uint64_t img_phys = rm.phys;
  /* ---- stage 4: assemble the metadata block -----------------------------
   * Pure memory work again: compute the VRAM layout, fill the 256-byte block,
   * encode it, and decode it back as a self-check.  Nothing is written to the
   * card here -- the block has to be DMA-able for that, and the BAR1 window
   * must cover the region first. */
  struct nv_wpr_layout_in lin;
  struct nv_wpr_layout lay;
  /* Policy: the region starts far enough below the top to leave the reserves
   * and the image; 128 MiB of headroom is the documented heap floor plus slack.
   * The exact start is what the booter will program, so it is logged loudly. */
  lin.fb_size = NV_AD102_FB_SIZE;
  lin.image_size = elf.fwimage.size;
  lin.wpr_start = (lin.fb_size - 0x6D00000ULL) & ~(NV_WPR_START_ALIGN - 1);
  int lrc2 = nv_wpr_layout(&lin, &lay);
  uart_puts("\n[NV-GSP] layout ");
  if (lrc2 != 0) {
    uart_puts("FAILED rc=");
    print_int(lrc2);
    uart_puts("\n");
  } else {
    uart_puts("start=");
    uart_print_hex(lay.wpr_start);
    uart_puts(" end=");
    uart_print_hex(lay.wpr_end);
    uart_puts(" heap=");
    uart_print_hex(lay.heap_size);
    uart_puts("\n[NV-GSP] layout image=");
    uart_print_hex(lay.image_offset);
    uart_puts(" bootbin=");
    uart_print_hex(lay.boot_bin_offset);
    uart_puts(" frts=");
    uart_print_hex(lay.frts_offset);
    uart_puts(" (every field matches the contract's AD102 table)\n");
  }

  uint64_t root = 0;
  uint32_t tpages = 0;
  int trc = nv_radix3_build(img_phys, elf.fwimage.size, &root, &tpages);
  uart_puts("\n[NV-GSP] radix3 ");
  if (trc != 0) {
    uart_puts("build FAILED rc=");
    print_int(trc);
    uart_puts(trc == -ENOMEM ? " (no contiguous run for the tree)\n" : "\n");
  } else {
    uart_puts("root=");
    uart_print_hex(root);
    uart_puts(" pages=");
    print_int((int)tpages);
    uart_puts(" maps ");
    print_int((int)((elf.fwimage.size + NV_RADIX3_PAGE - 1) / NV_RADIX3_PAGE));
    uart_puts(" image pages at ");
    uart_print_hex(img_phys);
    uart_puts(" (stage 3)\n");
    if (lrc2 == 0) {
      struct nv_wpr_meta_in m, back;
      for (uint32_t i = 0; i < (uint32_t)sizeof m; i++)
        ((uint8_t *)&m)[i] = 0;
      m.sysmem_addr_radix3_elf = root;
      m.size_radix3_elf = elf.fwimage.size;
      /* The bootloader's data region: 0x8000 bytes at file offset 0x6C. */
      m.sysmem_addr_bootloader = loader.phys + 0x6C;
      m.size_bootloader = 0x8000;
      m.bootloader_code_offset = 0x4800;
      m.bootloader_data_offset = 0x800;
      /* The ad10x signature section.  It sits AFTER .fwimage in the file, so the
       * slide that page-aligned the image did not move it: its bytes are still at
       * their original file offset.  Found by name, because picking the wrong
       * section fails authentication with no host-visible reason. */
      uint32_t sig_off = 0, sig_size = 0;
      for (uint32_t i = 0; i < elf.nsig && i < NV_FW_SIG_MAX; i++) {
        const char *n = elf.sig[i].name;
        /* "ad10x" substring check without string.h. */
        for (uint32_t k = 0; n[k]; k++) {
          if (n[k] == 'a' && n[k + 1] == 'd' && n[k + 2] == '1' &&
              n[k + 3] == '0' && n[k + 4] == 'x') {
            sig_off = elf.sig[i].off;
            sig_size = elf.sig[i].size;
            break;
          }
        }
        if (sig_off) break;
      }
      m.sysmem_addr_signature = sig_off ? (img_phys + sig_off) : 0;
      m.size_signature = sig_size;
      uart_puts("[NV-GSP] meta sig ");
      uart_puts(sig_off ? "ad10x at " : "NOT FOUND (authentication will fail) ");
      uart_print_hex(m.sysmem_addr_signature);
      uart_puts("\n");
      m.gsp_fw_rsvd_start = lay.non_wpr_heap_offset;
      m.non_wpr_heap_offset = lay.non_wpr_heap_offset;
      m.non_wpr_heap_size = lay.non_wpr_heap_size;
      m.gsp_fw_wpr_start = lay.wpr_start;
      m.gsp_fw_heap_offset = lay.heap_offset;
      m.gsp_fw_heap_size = lay.heap_size;
      m.gsp_fw_offset = lay.image_offset;
      m.boot_bin_offset = lay.boot_bin_offset;
      m.frts_offset = lay.frts_offset;
      m.frts_size = lay.frts_size;
      m.gsp_fw_wpr_end = lay.wpr_end;
      m.fb_size = lin.fb_size;
      m.vga_workspace_offset = lay.wpr_end;
      m.vga_workspace_size = lin.fb_size - lay.wpr_end;
      m.verified = 0;

      /* The block has to live where the GPU can read it: one contiguous frame
       * from nv_dma (one page; the block is 256 bytes), encoded in place.
       * `mbuf` is that frame, not a stack copy, and the address that reaches
       * SEC2 is nv_dma_bus() -- never the physical address, which equals it
       * only while VT-d translation is off (src/include/nv_dma.h). */
      int erc = nv_dma_alloc(NV_DMA_PAGE, &d->gsp_meta);
      uint8_t *mbuf = 0;
      int drc = erc;
      if (erc == 0) {
        mbuf = (uint8_t *)phys_to_virt(d->gsp_meta.phys);
        drc = nv_fw_wpr_meta_encode(&m, mbuf);
        /* Decode what is in the frames, not what we meant to write. */
        if (drc == 0) drc = nv_fw_wpr_meta_decode(mbuf, &back);
      }
      uart_puts("[NV-GSP] meta ");
      if (erc != 0 || drc != 0) {
        uart_puts("encode/decode FAILED rc=");
        print_int(erc ? erc : drc);
        uart_puts("\n");
      } else {
        /* A round-trip is not a proof the firmware likes it, but it does prove
         * the block is self-consistent: every field survives the wire format. */
        uart_puts("256B magic ok wpr=");
        uart_print_hex(back.gsp_fw_wpr_start);
        uart_puts("..");
        uart_print_hex(back.gsp_fw_wpr_end);
        uart_puts(" radix3=");
        uart_print_hex(back.sysmem_addr_radix3_elf);
        uart_puts(" elf=");
        uart_print_hex(back.size_radix3_elf);
        uart_puts(" verified=");
        print_int(nv_fw_wpr_meta_verified(&back));
        uart_puts(" block_phys=");
        uart_print_hex(d->gsp_meta.phys);
        uart_puts(" block_bus=");
        uart_print_hex(nv_dma_bus(&d->gsp_meta));
        uart_puts("\n");
      }
      d->radix3_root = root;
      d->radix3_pages = tpages;
    }
  }

  /* ---- stage 5: start the booter on SEC2 --------------------------------
   * Everything above this line is memory work.  This is the first stage that
   * writes to the card, and it is the one the metadata block was built for.
   * The trees, the image and the block all stay resident until it returns. */
  /* Whether the booter itself reported success.  Stage 8 uses it for one
   * decision only (how long to wait for RM's status header): a booter that
   * failed cannot produce a peer, so it gets one read instead of the 4 s. */
  int booter_ok = 0;
  nv_gsp_map_windows(d);
  (void)nv_gsp_write_lands(d);
  /* Work package B (the fuse check) runs here: read-only, and it decides
   * whether the image can authenticate at all on this part before a launch is
   * worth interpreting. */
  if (d->ucode_desc) nv_fuse_check(d);
  /* Stage 6a runs before stage 5 in the canonical order, and needs no ucode:
   * reset the GSP and route its DMA at system memory. */
  if (bldrc == 0) {
    int frc = nv_gsp_fwsec_reset(d);
    if (frc == 0 && lay.frts_offset) {
      int lrc3 = nv_gsp_fwsec_launch(d, (uint32_t)(lay.frts_offset >> 12),
                                     (uint32_t)(lay.frts_size >> 12),
                                     lay.wpr_start);
      uart_puts("[NV-GSP] stage6b (fwsec) result=");
      print_int(lrc3);
      uart_puts("\n");
    }
    uart_puts("[NV-GSP] stage6a (gsp reset) result=");
    print_int(frc);
    uart_puts("\n");
  }
  /* ---- the RPC pre-boot step, BEFORE the processor starts ----------------
   * Contract 4.1 rows 3-5: publish the argument page (row 2), pre-queue
   * GSP_SET_SYSTEM_INFO (72) and SET_REGISTRY (73), then ring the doorbell
   * once.  This is the first thing that ever puts work in RM's command queue;
   * without it a successful launch would leave RM with nothing to do.
   *
   * It sits here, after the GSP reset and the FWSEC attempt (so nothing later
   * in the boot writes MAILBOX0 again and clobbers the argument page's
   * address) and before the launch, because "pre-boot" is the whole point:
   * the messages must be in the queue while the processor is not running.
   * Nothing here waits -- there is no processor to answer yet. */
  if (bldrc == 0 && lrc2 == 0) {
    int prc2 = nv_gsp_rpc_preboot(&g_gsp_rpc, d);
    uart_puts("[NV-GSP] stage4b (rpc pre-boot 72/73) result=");
    print_int(prc2);
    uart_puts(prc2 == 0 ? " (two messages queued, one doorbell)\n"
                        : " (see the [NV-PRE] rows above)\n");
  }

  if (bldrc == 0 && lrc2 == 0 && d->gsp_meta.pages != 0) {
    /* The booter's only argument is the metadata block's address, and the
     * address it must receive is the one the GPU uses.  nv_dma_bus() answers
     * NV_DMA_BUS_INVALID when it cannot honestly name one, and passing that
     * to SEC2 would send it to 0xFFFFFFFFFFFFFxxx -- so the stage is skipped
     * with a row naming the reason instead. */
    uint64_t meta_bus = nv_dma_bus(&d->gsp_meta);
    if (meta_bus == NV_DMA_BUS_INVALID) {
      uart_puts("[NV-GSP] stage5 skipped: nv_dma_bus refused the metadata "
                "block (translation is live), so there is no address to give "
                "SEC2\n");
    } else {
      /* ---- the one attempt the plan has been holding back ----------------
       *
       * Round 4 made "FWSEC ran first" provable without a status register: the
       * 1 MiB FRTS region leaves the host's view (0xBAD0AC34, +1 per read)
       * exactly when the FRTS command was accepted, and the halt time says
       * which command ran.  That was the sequencing rule's gate, so the attempt
       * is made: BAR1 prefill of the two regions the booter would write, the
       * booter, then the read-back.  `force=1` because the SEC2 window's fence
       * is read-side only -- it stops the observation, not the attempt, and
       * every verdict below comes from host memory or BAR1, neither of which
       * the fence touches.
       */
      struct nv_booter_vram bv;
      uint64_t boot_off = lay.boot_bin_offset ? lay.boot_bin_offset
                                              : 0x5FE2F8000ULL;
      uint64_t wpmeta_off = lay.wpr_start ? lay.wpr_start : 0x5F8700000ULL;
      (void)nv_booter_vram_prefill(d, &bv, boot_off, wpmeta_off);
      int src2 = nv_gsp_sec2_start_booter_ex(d, booter.buf, booter.size,
                                             meta_bus, 1);
      booter_ok = (src2 == 0);
      uart_puts("[NV-GSP] stage5 result=");
      print_int(src2);
      uart_puts(src2 == 0 ? " (booter returned success)\n"
                          : " (no success reported; the [NV-SEC2] rows are the "
                            "evidence)\n");
      /* The bootloader's data region is what the booter copies into
       * boot_bin_offset (meta.sysmem_addr_bootloader = loader.phys + 0x6C,
       * size 0x8000), so it is also the fingerprint to compare against. */
      const uint8_t *expect = 0;
      uint32_t expect_len = 0;
      if (ldr_rc == 0 && loader.buf && loader.size >= 0x6C + NV_BOOTBIN_LEN) {
        expect = loader.buf + 0x6C;
        expect_len = NV_BOOTBIN_LEN;
      }
      nv_booter_vram_post(d, &bv, expect, expect_len);
      /* The SEC2 window, word by word, after the attempt.  If the core-select
       * clear made it answer, these are the numbers that say how far the booter
       * got: 0x118's done bit, 0x114's last destination, MAILBOX0's return code
       * (0 = success) and 0x840100's privilege bit.  If they are the fence word,
       * the row says the window stayed blind and nothing else is claimed. */
      {
        static const uint32_t kWin[] = {0x840100, 0x840104, 0x840110, 0x840114,
                                        0x840118, 0x84011C, 0x840120, 0x840128,
                                        0x840040, 0x840044, 0x841668};
        uart_puts("[NV-SEC2] window after:");
        for (uint32_t i = 0; i < (uint32_t)(sizeof kWin / sizeof kWin[0]); i++) {
          uart_puts(" ");
          uart_print_hex(kWin[i]);
          uart_puts("=");
          uart_print_hex(nv_rd32(d, kWin[i]));
        }
        uart_puts(nv_rd32(d, 0x840100) == NV_REG_FENCED
                      ? " verdict=STILL-BLIND\n"
                      : " verdict=OBSERVABLE\n");
      }
      uint32_t sec2_priv = nv_rd32(d, 0x840100);
      uart_puts("[NV-SEC2] summary meta_verified=0x");
      uart_print_hex(nv_meta_get64(
          (const uint8_t *)phys_to_virt(d->gsp_meta.phys) + NV_WPR_OFF_VERIFIED));
      uart_puts(" sec2_0x840100=");
      uart_print_hex(sec2_priv);
      uart_puts(sec2_priv == NV_REG_FENCED
                    ? " (SEC2 0x840100 still reads the fence word: no separate "
                      "privilege-drop oracle on this channel)\n"
                    : " (SEC2 0x840100 is readable: the privilege drop is "
                      "visible here)\n");
    }
  }

  /* Stage 8: the RPC transport.  This is memory work plus one read of the
   * peer's header (nv_gsp_rpc_stage8 logs its own [NV-RPC] rows), so it runs
   * even when the booter did not: publishing a block nobody has walked is
   * cheap, and the row that says "peer=none" is the honest state of a boot
   * that has not reached RM.
   *
   * `booter_ok` decides how long the peer's header is waited for, and nothing
   * else: a booter that returned success can still have RM appear, so that boot
   * gets the contract's 4 s; a booter that did not cannot, so it gets one read
   * and does not pay 4 s for an absent peer. */
  if (lrc2 == 0) {
    int s8 = nv_gsp_rpc_stage8_wait(d, booter_ok ? NV_RPC_LINK_TIMEOUT_US : 0u);
    /* Stage 9 is called even when the link failed, because it gates itself: on
     * an unlinked queue its one row is the statement that nothing was sent and
     * no doorbell was rung.  When the link is up it is the first round trip. */
    (void)s8;
    (void)nv_gsp_rpc_stage9(d);
  }

  if (d->radix3_root) {
    nv_radix3_release(d->radix3_root, d->radix3_pages);
    d->radix3_root = 0;
  }
  nv_fw_release(&rm);
  if (ldr_rc == 0) nv_fw_release(&loader);
  if (bldrc == 0) nv_fw_release(&booter);
  return rc;
}

/* The staged boot.  Deliberately stops at the first stage whose contract is
 * not verified, and says which contract is missing, so the next session's work
 * is named by the log rather than rediscovered. */
int nv_gsp_boot(struct nv_dev *d) {
  if (!d) return -EINVAL;
  if (d->gsp_stage == NV_GSP_STAGE_NONE && nv_gsp_probe(d) != 0) return -ENODEV;

  struct nv_falcon *f = &d->falcon[NV_FALCON_IDX_GSP];

  /* Stage 1: halt.  The reset triggers leave the processor STOPPED, which is
   * the documented "any -> stopped" transition; a processor we do not own is
   * not a processor we may load. */
  int rc = nv_falcon_halt(d, f);
  if (rc != 0) {
    uart_puts("[NV-GSP] stage1 halt FAILED rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  uint32_t ctl = nv_rd32(d, f->base + NV_FALCON_UC_CTRL);
  uart_puts("[NV-GSP] stage1 halted uc_ctrl=");
  uart_print_hex(ctl);
  uart_puts("\n");
  d->gsp_stage = NV_GSP_STAGE_HALTED;

  /* Stage 2..N are gated on firmware on disk and on register contracts that
   * have not been verified against a known-good driver yet.  Reporting the gate
   * is the honest outcome; pretending to try it is not. */
  /* Stage 2 is pure file I/O and runs on every boot (see nv_gsp_firmware_check);
   * here we only record that it is not the blocker any more. */
  uart_puts("[NV-GSP] stage2 firmware: read+parsed at bring-up (see fw rows)\n");
  /* The remaining stages are named by the contracts rather than by a guess:
   * radix3 tree -> metadata block -> booter DMA on SEC2 -> MAILBOX0 -> meta
   * `verified` -> RPC queues -> GSP_INIT_DONE.  Each has a read-back that
   * proves it, and none of them may be written before its predecessor's
   * read-back passes. */
  uart_puts("[NV-GSP] stage3+ not started: next is the radix3 tree, then the "
            "metadata block (codec ready), then the SEC2 booter "
            "(docs/nv-gsp-wpr-contract.md section 8)\n");
  return -ENOTSUP;
}

#endif /* __x86_64__ */
