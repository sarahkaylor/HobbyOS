#ifndef NV_RPC_H
#define NV_RPC_H

/* ============================================================================
 *  nv_rpc: the GSP-RPC shared-memory transport, as bytes.
 *
 *  Contract: docs/nv-rpc-implementation-spec.md (the whole document; section
 *  numbers below refer to it).  Nothing here is copied from a driver: the
 *  field table, the offsets and the checksum rule were written from the
 *  format, and the golden vectors in the unit tier are computed from the rule.
 *
 *  Two halves, one file each:
 *
 *    nv_rpc.c        pure.  Reads and writes THE BLOCK and nothing else: no
 *                    MMIO, no clock, no sleep (spec 5.1).  The only two things
 *                    it cannot do itself are `mb()` and the doorbell, and both
 *                    arrive through struct nv_rpc_hw (spec 5.2) so the unit
 *                    tier can count them instead of assuming them.
 *    nv_rpc_test.c   the spec's cases T-01..T-24 against synthetic queues.
 *
 *  DEPENDENCY DECISION (fixed by PLAN.md 3 WP-C): this branch does not call
 *  the DMA facility.  nv_rpc_block_build() takes `block_bus` and builds every
 *  PTE from it; nv_dma supplies that value at the call site later.  There is
 *  no `phys` in this interface on purpose -- a PTE must hold the address the
 *  GPU uses, and today that equals the CPU physical address only because VT-d
 *  translation is off (spec 9 risk 1).
 *
 *  NO STRUCT OVERLAY.  Spec 2: read and write through explicit offsets, never
 *  by casting a struct onto the bytes.  Every accessor below is byte-wise
 *  little-endian, which is also what AArch64 EL1 requires of wire structures
 *  (Device memory forbids unaligned wide loads).
 * ========================================================================== */

#include <stdint.h>

/* ---- geometry (spec 1, 2.6) -------------------------------------------- */

#define NV_RPC_PTE_COUNT 129u
#define NV_RPC_PTE_BYTES 0x1000u  /* one page holds all 129 x u64 */
#define NV_RPC_CMD_OFF 0x1000u    /* command queue, block-relative */
#define NV_RPC_STAT_OFF 0x41000u  /* status queue, block-relative */
#define NV_RPC_QUEUE_BYTES 0x40000u /* one queue: header page + 63 slots */
#define NV_RPC_BLOCK_SIZE 0x81000u  /* 129 x 0x1000 */

#define NV_RPC_SLOT 0x1000u     /* slot size (msgSize) */
#define NV_RPC_ELEM_HDR 0x30u   /* 48: the element header, up to the RPC */
#define NV_RPC_RPC_HDR 0x20u    /* 32: the RPC header, up to the payload */
#define NV_RPC_ELEM_MAX 16u     /* GSP_MSG_QUEUE_ELEMENT_SIZE_MAX */
#define NV_RPC_LEN_MAX 65488u   /* 0x10000 - 0x30: RPC header + payload */
#define NV_RPC_PAYLOAD_MAX 65456u /* 65488 - 32 (INFERRED) */
#define NV_RPC_SPLIT_CHUNK 65456u /* per-message payload in a split (spec 4.5) */

#define NV_RPC_MSG_COUNT 63u /* slots in the ring, from our header */
#define NV_RPC_USABLE 62u    /* one slot is always left empty */

/* ---- wire constants (spec 2.4, 2.6) ------------------------------------ */

#define NV_RPC_SIG 0x43505256u     /* "VRPC" in memory order */
#define NV_RPC_VERSION 0x03000000u /* header_version: major 3, minor 0 */
#define NV_RPC_PENDING 0xFFFFFFFFu /* rpc_result / rpc_result_private sentinel */
#define NV_RPC_FN_CONTINUATION 71u /* nvkm/rpcfn.h */
#define NV_RPC_FN_GET_STATIC_INFO 65u
#define NV_RPC_FN_SET_SYSTEM_INFO 72u
#define NV_RPC_FN_SET_REGISTRY 73u
#define NV_RPC_FN_RM_CONTROL 76u
#define NV_RPC_EVENT_BASE 0x1000u /* function >= this is an event */
#define NV_RPC_FN_INIT_DONE 0x1001u
/* "any non-event message is the reply" for nv_rpc_queue.want_fn.  No opcode
 * comes close to this value, so it cannot collide with a real function. */
#define NV_RPC_WANT_ANY 0xFFFFFFFFu

/* Doorbell: BAR0 NV_PGSP_QUEUE_HEAD(0) = NV_FALCON_GSP_BASE + 0xC00, written
 * with 0.  SOURCE (spec 2.6); the word reads 0 in every state, so it is a
 * notification and never a status (spec 4.3 S10). */
#define NV_RPC_DOORBELL_OFF 0x110C00u
#define NV_RPC_DOORBELL_QUEUE 0u

#define NV_RPC_FLAGS_SWAP_RX 1u /* MSGQ_FLAGS_SWAP_RX, bit 0 */
#define NV_RPC_FLAGS_RESERVED 0xFFFFFFFEu /* DECISION: reject a peer with these */

/* ---- offsets: queue header page (spec 2.1), queue-relative ------------ */

#define NV_RPC_Q_VERSION 0x00u
#define NV_RPC_Q_SIZE 0x04u
#define NV_RPC_Q_MSG_SIZE 0x08u
#define NV_RPC_Q_MSG_COUNT 0x0Cu
#define NV_RPC_Q_WRITE_PTR 0x10u
#define NV_RPC_Q_FLAGS 0x14u
#define NV_RPC_Q_RX_HDR_OFF 0x18u
#define NV_RPC_Q_ENTRY_OFF 0x1Cu
#define NV_RPC_Q_READ_PTR 0x20u
#define NV_RPC_Q_USED_BYTES 0x24u /* 36 bytes are defined, the rest is zero */

/* ---- offsets: element + RPC header, SLOT-relative ---------------------
 *
 * The RPC sits at 0x30, so its own offsets are the element's plus 0x30:
 * RPC off 0x00 == slot off 0x30.  One numbering for the whole slot keeps the
 * accessors in one place and makes the "RPC offset 8144" arithmetic in T-11
 * the same arithmetic the code does. */

#define NV_RPC_E_AUTHTAG 0x00u  /* 16 bytes, zero without CC */
#define NV_RPC_E_AAD 0x10u      /* 16 bytes, zero without CC */
#define NV_RPC_E_CKSUM 0x20u    /* u32: chosen so a whole-slot fold == 0 */
#define NV_RPC_E_SEQ 0x24u      /* u32: per queue, per MESSAGE */
#define NV_RPC_E_ELEMCOUNT 0x28u /* u32: slots this message occupies */
#define NV_RPC_E_PAD 0x2Cu      /* u32: 0, and inside the hashed range */

#define NV_RPC_R_HDRVER 0x30u
#define NV_RPC_R_SIG 0x34u
#define NV_RPC_R_LENGTH 0x38u /* total RPC bytes: 32 + payload */
#define NV_RPC_R_FUNCTION 0x3Cu
#define NV_RPC_R_RESULT 0x40u
#define NV_RPC_R_RESULT_PRIV 0x44u
#define NV_RPC_R_SEQUENCE 0x48u
#define NV_RPC_R_SPARE 0x4Cu
#define NV_RPC_R_PAYLOAD 0x50u

/* The same header RPC-RELATIVE, which is what nv_rpc_rx_take() hands back (it
 * copies the RPC stream from slot + 0x30).  Two numberings on purpose: the
 * slot one is for building, this one is for parsing a received message. */
#define NV_RPC_H_HDRVER 0x00u
#define NV_RPC_H_SIG 0x04u
#define NV_RPC_H_LENGTH 0x08u
#define NV_RPC_H_FUNCTION 0x0Cu
#define NV_RPC_H_RESULT 0x10u
#define NV_RPC_H_RESULT_PRIV 0x14u
#define NV_RPC_H_SEQUENCE 0x18u
#define NV_RPC_H_SPARE 0x1Cu
#define NV_RPC_H_PAYLOAD 0x20u

/* ---- result codes (spec 5.3, "Result codes") --------------------------
 *
 * The coarse errno comes from src/include/errno.h (which has no EPROTO and no
 * EBADMSG); the fine reason rides out of every call as an enum so a serial
 * log names the failure without a symbol table. */

enum nv_rpc_reason {
  NV_RPC_OK = 0,
  NV_RPC_NOTHING,      /* -EAGAIN: nothing to read yet */
  NV_RPC_NOT_LINKED,   /* -EAGAIN: peer header not written yet */
  NV_RPC_SHORT_PUBLISH,/* -EAGAIN: avail < elemCount */
  NV_RPC_NO_SPACE,     /* -ENOSPC: ec > free */
  NV_RPC_TOO_BIG,      /* -EMSGSIZE: rpc_len > 65488 (or out buffer too small) */
  NV_RPC_PEER_GEOM,    /* -EINVAL: peer header / pointer unusable */
  NV_RPC_BIG_ELEM,     /* -EINVAL: elemCount > 16 */
  NV_RPC_BAD_EC,       /* -EINVAL: elemCount == 0 */
  NV_RPC_CHECKSUM,     /* -EIO: whole-slot fold != 0 after the retries */
  NV_RPC_SEQ_GAP,      /* -EIO: seqNum > rx_seq */
  NV_RPC_SEQ_STALE,    /* not an error: seqNum < rx_seq, consumed and dropped */
  NV_RPC_BAD_SIG,      /* -EIO: signature */
  NV_RPC_BAD_LEN,      /* -EIO: length outside [32, ec*4096 - 48] */
  NV_RPC_EVENT,        /* 0: dispatched as an event (function >= 0x1000) */
  NV_RPC_UNKNOWN_FN,   /* 0: dispatched and dropped */
  NV_RPC_CONTINUATION, /* 0: function 71, appended to the assembly */
  NV_RPC_BUSY,         /* -EBUSY: one request already outstanding (4.1 P4) */
};

const char *nv_rpc_reason_name(enum nv_rpc_reason r);

/* ---- the hardware seam (spec 5.2): three function pointers -------------
 *
 * Real: doorbell -> nv_wr32(d, NV_RPC_DOORBELL_OFF, 0); fence ->
 * arch_memory_barrier(); now_us -> timer_get_us() (nv_rpc_hw_dev() below
 * builds exactly that, x86_64 only, like the rest of the driver).
 * Unit tier: each pointer counts, so "the doorbell was never rung" and
 * "the fence came before the notification" are assertions, not assumptions.
 *
 * Every call that may need mb() takes `const struct nv_rpc_hw *hw`, which may
 * be NULL when the caller does not want the ordering (spec 5.3's signature
 * table abbreviates this argument; the fence has to come from somewhere and
 * spec 5.1 forbids the pure half from reaching the CPU directly). */

struct nv_rpc_hw {
  void (*doorbell)(void *ctx, uint32_t queue_idx); /* MMIO, value 0 */
  void (*fence)(void *ctx);                        /* arch_memory_barrier */
  uint64_t (*now_us)(void *ctx);                   /* timer_get_us, or fake */
  void *ctx;
};

/* ---- geometry and the block (spec 1, 3) ------------------------------- */

struct nv_rpc_geom {
  uint32_t pte_count;
  uint32_t cmd_off;
  uint32_t stat_off;
  uint32_t block_size;
  uint32_t msg_size;
  uint32_t msg_count;
};

/* The RMARGS view of the SAME geometry (docs/nv-gsp-rpc-contract.md 1.5).
 * Field names are the contract's, because nv_rpc_args_crosscheck() reports the
 * first disagreement BY NAME.
 *
 * magic and size are part of the view, not decoration: they are the two words
 * that make the page a GSP_ARGUMENTS_CACHED block rather than a page of
 * plausible-looking geometry, so the cross-check reads them too. */
struct nv_rpc_rmargs {
  uint32_t magic;           /* +0x00 GSP_ARGUMENTS_MAGIC_VALUE */
  uint16_t size;            /* +0x04 sizeof(GSP_ARGUMENTS_CACHED) = 0xC8 */
  uint64_t shared_mem_phys; /* +0x18 mq.sharedMemPhysAddr = PTE[0] */
  uint64_t cmd_queue_off;   /* +0x20 */
  uint64_t stat_queue_off;  /* +0x28 */
  uint32_t elem_hdr_size;   /* +0x30 */
  uint32_t elem_size_min;   /* +0x34 */
  uint32_t elem_size_max;   /* +0x38 */
  uint32_t header_align;    /* +0x3C (log2 bytes) */
  uint32_t elem_align;      /* +0x40 (log2 bytes) */
  uint32_t pte_count;       /* +0x44 */
};

enum nv_rpc_arg_field {
  NV_RPC_ARG_OK = 0,
  NV_RPC_ARG_SHARED_MEM,  /* sharedMemPhysAddr */
  NV_RPC_ARG_PTE_COUNT,   /* pageTableEntryCount */
  NV_RPC_ARG_CMD_OFF,     /* cmdQueueOffset */
  NV_RPC_ARG_STAT_OFF,    /* statQueueOffset */
  NV_RPC_ARG_HDR_SIZE,    /* queueElementHdrSize */
  NV_RPC_ARG_ELEM_MIN,    /* queueElementSizeMin */
  NV_RPC_ARG_ELEM_MAX,    /* queueElementSizeMax */
  NV_RPC_ARG_HDR_ALIGN,   /* queueHeaderAlign */
  NV_RPC_ARG_ELEM_ALIGN,  /* queueElementAlign */
  NV_RPC_ARG_MAGIC,       /* +0x00: this is not a GSP_ARGUMENTS page */
  NV_RPC_ARG_SIZE,        /* +0x04: the struct size the firmware validates */
};

const char *nv_rpc_arg_field_name(enum nv_rpc_arg_field f);

/* Fill the geometry the whole transport is built from, and re-check the
 * invariants (spec 3).  Returns 0 or -EINVAL. */
int nv_rpc_queue_geom(struct nv_rpc_geom *g);

/* Zero 129 pages, fill 129 PTEs from block_bus, publish the COMMAND queue's
 * producer header only.  The status header is left zeroed on purpose: the
 * firmware's write of it is the boot's first proof of life (spec 3 step 4).
 * Returns 0, -EINVAL on a NULL or misaligned block_bus. */
int nv_rpc_block_build(uint8_t *block, uint64_t block_bus);

/* 0 when the two descriptions agree, else the enum naming the first field that
 * disagrees (spec 3 step 5). */
int nv_rpc_args_crosscheck(const struct nv_rpc_geom *g, uint64_t block_bus,
                           const struct nv_rpc_rmargs *a);

/* ---- publishing: GSP_ARGUMENTS_CACHED and the libos-args table ---------
 *
 * The block above is what RM walks.  These two pages are how RM is TOLD it
 * exists (docs/nv-gsp-rpc-contract.md 1.5 and 1.6): a one-page argument block
 * holding the block's GPU address and the geometry, and a one-page table of
 * named regions whose fourth entry points at that argument block.  The table's
 * address goes into the two GSP falcon mailbox words, and that is the entire
 * handshake -- the firmware does not scan memory.
 *
 * Both builders are PURE: bytes in, bytes out, no MMIO, no allocation, no
 * clock.  The address they are handed is the GPU-visible one, which the caller
 * obtains only through nv_dma_bus(); nothing in this file can tell whether it
 * is a bus address or a CPU physical address, which is exactly why the caller
 * must not substitute one for the other.
 * ---------------------------------------------------------------------- */

/* The 535 build of the structure (contract 1.5 field table).  0xC8 is what
 * goes in the size field; the allocation is a full page either way.
 *
 * NAMING: NV_RPC_GSP_ARGS_* is a WIRE value (what the firmware reads),
 * NV_RPC_ARG_* is a FIELD ID (what the cross-check reports).  Two namespaces
 * on purpose -- a single NV_RPC_ARG_MAGIC for both would make
 * nv_rpc_arg_field_name(NV_RPC_ARG_MAGIC) ask for the string at index
 * 0x20505347, and a preprocessor macro silently wins over the enum. */
#define NV_RPC_GSP_ARGS_MAGIC 0x20505347u /* bytes 47 53 50 20 = "GSP " */
#define NV_RPC_GSP_ARGS_SIZE 0xC8u
#define NV_RPC_GSP_ARGS_PAGE 0x1000u

/* Page-relative offsets.  Named once so the writer, the reader and the log row
 * cannot drift apart; the two halves of this file both use these. */
#define NV_RPC_ARG_O_MAGIC 0x00u
#define NV_RPC_ARG_O_SIZE 0x04u
#define NV_RPC_ARG_O_FLAGS 0x08u
#define NV_RPC_ARG_O_MQ_FLAGS 0x10u
#define NV_RPC_ARG_O_MQ_SHARED 0x18u
#define NV_RPC_ARG_O_MQ_CMD 0x20u
#define NV_RPC_ARG_O_MQ_STAT 0x28u
#define NV_RPC_ARG_O_MQ_HDR_SIZE 0x30u
#define NV_RPC_ARG_O_MQ_ELEM_MIN 0x34u
#define NV_RPC_ARG_O_MQ_ELEM_MAX 0x38u
#define NV_RPC_ARG_O_MQ_HDR_ALIGN 0x3Cu
#define NV_RPC_ARG_O_MQ_ELEM_ALIGN 0x40u
#define NV_RPC_ARG_O_MQ_PTE_COUNT 0x44u
#define NV_RPC_ARG_O_SR_OLD_LEVEL 0x50u
#define NV_RPC_ARG_O_SR_FLAGS 0x54u
#define NV_RPC_ARG_O_GPU_INSTANCE 0x64u

/* The geometry constants the transport fixes (contract 1.5, +0x30..+0x44).
 * They live here rather than in the builder because the cross-check compares
 * them and the row prints them; one definition, three readers. */
#define NV_RPC_ARG_ELEM_MAX 0x10000u /* queueElementSizeMax = 16 slots */
#define NV_RPC_ARG_HDR_ALIGN 4u      /* queueHeaderAlign: log2(16) */
#define NV_RPC_ARG_ELEM_ALIGN 12u    /* queueElementAlign: log2(4096) */

/* Write one GSP_ARGUMENTS_CACHED into `page` (all NV_RPC_ARG_PAGE bytes are
 * zeroed first, so the reserved tail is zero by construction) describing the
 * transport `g` that was built at `block_bus`.
 *   -EINVAL  NULL, a `g` that is not this transport's geometry, or a
 *            block_bus that is zero or not page aligned (a PTE[0] the
 *            firmware would walk to nowhere).
 *   -EIO     the page does not read back what was written to it.
 * Nothing outside this page is touched. */
int nv_rpc_args_build(uint8_t *page, const struct nv_rpc_geom *g,
                      uint64_t block_bus);

/* Read the page's OWN bytes back into the cross-check's view (contract 1.5).
 * A builder that returns 0 proves only that it was called; this is how a boot
 * row shows what is actually in memory.  Returns 0, or -EINVAL on NULL. */
int nv_rpc_args_read(const uint8_t *page, struct nv_rpc_rmargs *a);

/* ---- the libos-args table (contract 1.6) ------------------------------ */

#define NV_RPC_LIBOS_ENTRY 0x20u /* 32 bytes: id8, pa, size, kind, loc */
#define NV_RPC_LIBOS_MAX 4u      /* the reference driver publishes four */
#define NV_RPC_LIBOS_ID_BYTES 8u
/* Three 4 KiB log regions (LOGINIT, LOGINTR, LOGRM).  The contract leaves
 * "does the firmware tolerate a zero-sized log region" NOT ESTABLISHED and
 * says providing real ones is cheap; three pages is cheap. */
#define NV_RPC_LIBOS_LOG_PAGES 3u

#define NV_RPC_LIBOS_O_ID8 0x00u
#define NV_RPC_LIBOS_O_PA 0x08u
#define NV_RPC_LIBOS_O_SIZE 0x10u
#define NV_RPC_LIBOS_O_KIND 0x18u
#define NV_RPC_LIBOS_O_LOC 0x19u

#define NV_RPC_LIBOS_KIND_CONTIG 1u /* kind = 1: one contiguous region */
#define NV_RPC_LIBOS_LOC_SYSMEM 1u  /* loc  = 1: system memory          */

struct nv_rpc_libos_region {
  const char *id; /* INPUT (build): up to 8 characters, NUL padded, packed
                   * low byte first */
  uint64_t id8;   /* the packed value.  build() fills it from `id`; read()
                   * fills it from the page and leaves `id` NULL, so a caller
                   * comparing what is in memory to a name compares to
                   * nv_rpc_id8("RMARGS") and never to a local string. */
  uint64_t pa;    /* GPU-visible address of the region */
  uint64_t size;  /* bytes; need not be a multiple of the page */
  uint8_t kind;   /* NV_RPC_LIBOS_KIND_* */
  uint8_t loc;    /* NV_RPC_LIBOS_LOC_* */
};

/* "RMARGS" -> 0x0000534752414D52: first character in the least significant
 * byte, NUL padded to eight (_kgspGenerateInitArgId).  The reference names are
 * shorter than eight characters, so the zero bytes are part of the id.
 * Returns 0 for NULL, for an empty string, or for a name longer than eight --
 * a truncated name is a different region and must not be packed silently. */
uint64_t nv_rpc_id8(const char *s);

/* Zero the page, then write `n` entries at 32 bytes each in the given order.
 *   -EINVAL  NULL, n == 0, n > NV_RPC_LIBOS_MAX, a malformed id, a zero or
 *            unaligned pa, or a zero size.
 *   -EIO     an entry does not read back what was written. */
int nv_rpc_libos_build(uint8_t *page, const struct nv_rpc_libos_region *e,
                       uint32_t n);

/* Read entry `i` (0-based) back out of the page.  `out->id` is set to NULL:
 * the page holds the packed u64, and the caller that called build() already
 * knows the string.  Returns 0 or -EINVAL (NULL, or i >= NV_RPC_LIBOS_MAX). */
int nv_rpc_libos_read(const uint8_t *page, uint32_t i,
                      struct nv_rpc_libos_region *out);

/* ---- the pre-boot payloads (contract 4.1 rows 3-5, 5.1, 5.2) ----------
 *
 * Two messages exist before the firmware does: GSP_SET_SYSTEM_INFO (72) and
 * SET_REGISTRY (73).  They are staged into the command queue while the
 * processor is stopped and neither is waited on -- there is no processor to
 * answer yet, and any reply arrives later interleaved with events.  The
 * encoders here are PURE (bytes in, bytes out, no MMIO, no clock): the boot
 * path owns the queue and the one doorbell.
 *
 * Both return the element's elemCount (> 0), or a negative errno.  The seqNum
 * is a parameter and not something the encoder may invent: it is per queue and
 * per MESSAGE (contract 2.4), so the caller that knows how many messages are
 * already queued is the only one that can assign it.
 * ---------------------------------------------------------------------- */

#define NV_RPC_SYSTEM_INFO_SIZE 0x2C0u /* 704: the 535 shape of GspSystemInfo */

/* Payload offsets, straight from the contract's table (5.1).  A byte written
 * one field off is caught by nv_rpc_system_info_check(), which names the field
 * -- the offsets here are what it checks against. */
#define NV_RPC_SI_O_GPU_PHYS 0x000u      /* u64 gpuPhysAddr */
#define NV_RPC_SI_O_GPU_FB 0x008u        /* u64 gpuPhysFbAddr */
#define NV_RPC_SI_O_GPU_INST 0x010u      /* u64 gpuPhysInstAddr */
#define NV_RPC_SI_O_BUSDEVFUNC 0x018u    /* u64 nvDomainBusDeviceFunc */
#define NV_RPC_SI_O_SIM_ACCESS 0x020u    /* u64 simAccessBufPhysAddr */
#define NV_RPC_SI_O_ATOMICS 0x028u       /* u64 pcieAtomicsOpMask */
#define NV_RPC_SI_O_CONSOLE_SIZE 0x030u  /* u64 consoleMemSize */
#define NV_RPC_SI_O_MAX_USER_VA 0x038u   /* u64 maxUserVa */
#define NV_RPC_SI_O_PCI_MIRROR_BASE 0x040u /* u32 */
#define NV_RPC_SI_O_PCI_MIRROR_SIZE 0x044u /* u32 */
#define NV_RPC_SI_O_OOR_ARCH 0x048u      /* u8  oorArch */
#define NV_RPC_SI_O_CL_PDB 0x050u        /* u64 clPdbProperties */
#define NV_RPC_SI_O_CHIPSET 0x058u       /* u32 Chipset */
#define NV_RPC_SI_O_FLAGS 0x05Cu         /* 4 x u32 PCIe topology flags */
#define NV_RPC_SI_O_FLAGS2 0x06Cu        /* 2 x u32 */
#define NV_RPC_SI_O_UPSTREAM 0x074u      /* u8  upstreamAddressValid */
#define NV_RPC_SI_O_FHB_BUS_INFO 0x076u  /* 10 bytes */
#define NV_RPC_SI_O_CHIPSET_ID 0x080u    /* 10 bytes */
#define NV_RPC_SI_O_ACPI 0x08Cu          /* 488 bytes, all zero, bValid = 0 */
#define NV_RPC_SI_O_HYPERVISOR 0x274u    /* u32 */
#define NV_RPC_SI_O_VIRTUAL_CFG 0x278u   /* u16 virtualConfigBits */
#define NV_RPC_SI_O_IS_PASSTHRU 0x27Cu   /* u32 */
#define NV_RPC_SI_O_TIMER_OFFSET 0x280u  /* u64 sysTimerOffsetNs */
#define NV_RPC_SI_O_VF_INFO 0x288u       /* 48 bytes */
#define NV_RPC_SI_O_TDR 0x2B8u           /* u32 bTdrEventSupported */

/* The fields WE fill.  Everything else in the structure is zero by
 * construction: the contract's table says so for every remaining field
 * (simulation, atomics, console, topology flags, ACPI, VF info), and a zero
 * written by the builder is a decision while a zero left by luck is not. */
struct nv_rpc_system_info {
  uint64_t gpu_phys_addr;  /* +0x000 BAR0 base */
  uint64_t gpu_phys_fb_addr; /* +0x008 BAR1 aperture base */
  uint64_t gpu_phys_inst_addr; /* +0x010 NOT ESTABLISHED on this card (no BAR2) */
  uint64_t nv_domain_bus_device_func; /* +0x018 (bus << 8) | devfn */
  uint64_t max_user_va;    /* +0x038 NOT ESTABLISHED for a non-Linux host */
  uint64_t sys_timer_offset_ns; /* +0x280 0 is safe (only timestamped logs) */
  uint32_t pci_config_mirror_base; /* +0x040 0 = no mirror (not established) */
  uint32_t pci_config_mirror_size; /* +0x044 */
  uint32_t chipset;        /* +0x058 0: the vendor driver never assigns it */
};

/* Which field the checker disagreed with, by name, so a boot row says what is
 * wrong instead of "verify failed". */
enum nv_rpc_si_field {
  NV_RPC_SI_F_OK = 0,
  NV_RPC_SI_F_HEADER,
  NV_RPC_SI_F_GPU_PHYS,
  NV_RPC_SI_F_GPU_FB,
  NV_RPC_SI_F_GPU_INST,
  NV_RPC_SI_F_BUSDEVFUNC,
  NV_RPC_SI_F_MAX_USER_VA,
  NV_RPC_SI_F_PCI_MIRROR_BASE,
  NV_RPC_SI_F_PCI_MIRROR_SIZE,
  NV_RPC_SI_F_CHIPSET,
  NV_RPC_SI_F_TIMER_OFFSET,
  NV_RPC_SI_F_ZERO, /* a byte the contract says is zero is not */
  NV_RPC_SI_F_SIZE   /* total size is not 0x2C0 */
};

const char *nv_rpc_si_field_name(enum nv_rpc_si_field f);

/* Write one opcode-72 element: RPC header (version, signature, length
 * 0x20 + 0x2C0, function 72, both PENDING sentinels), the 0x2C0-byte payload
 * at every offset above, then the framing (elemCount, checksum over the whole
 * element).  `stage` must hold NV_RPC_SLOT * 16 bytes.
 *   -EINVAL  NULL stage/input.
 * Returns the elemCount (1 for this payload) or a negative errno from
 * nv_rpc_elem_build(). */
int nv_rpc_system_info_build(uint8_t *stage,
                             const struct nv_rpc_system_info *in, uint32_t seq);

/* Check a built opcode-72 element against the contract's offset table and the
 * values it was built from: header fields, every named field, every byte the
 * contract says is zero, and the total size.  Reads the BYTES, never the
 * input struct, so it catches a store that landed one field away.
 * Returns NV_RPC_SI_F_OK or the first disagreement. */
enum nv_rpc_si_field nv_rpc_system_info_check(
    const uint8_t *stage, const struct nv_rpc_system_info *in);

/* ---- SET_REGISTRY (contract 5.2): a packed table, not a list of structs --
 *
 *   +0x00 u32 size        total bytes of the table (header + entries + names
 *                         + string/binary values)
 *   +0x04 u32 numEntries
 *   +0x08 16 x N entries  nameOffset (u32, from the table start), type (u8),
 *                         data (u32: the dword value, or an offset for
 *                         string/binary), length (u32)
 *   then                  NUL-terminated key names, then values, packed
 *
 * The table starts immediately after the 32-byte RPC header, and both offsets
 * are relative to the table's start.
 *
 * `length` for a dword entry is 4 -- the size of the value it describes.
 * NEITHER source states it (contract 5.2 gives the field, not its value for
 * type 1); RM reads `data` according to `type`, so the number is bookkeeping.
 * It is pinned by T-28 and stated as INFERRED in the code, not presented as a
 * documented constant.
 *
 * WHICH KEYS ARE MANDATORY IS NOT ESTABLISHED (contract 5.2).  nv_rpc_registry_build
 * takes the list from the caller and accepts n == 0: an empty table is
 * well-formed, and a driver that needs keys the source does not name has to
 * find them from a firmware that answers, not from a guess.
 */
#define NV_RPC_REG_HDR 0x08u
#define NV_RPC_REG_ENTRY 0x10u
#define NV_RPC_REG_O_SIZE 0x00u
#define NV_RPC_REG_O_NUM 0x04u
#define NV_RPC_REG_E_NAMEOFF 0x00u
#define NV_RPC_REG_E_TYPE 0x04u
#define NV_RPC_REG_E_DATA 0x08u
#define NV_RPC_REG_E_LENGTH 0x0Cu
#define NV_RPC_REG_TYPE_DWORD 1u
#define NV_RPC_REG_TYPE_BINARY 2u
#define NV_RPC_REG_TYPE_STRING 3u

struct nv_rpc_registry_entry {
  const char *name; /* NUL-terminated key name; required */
  uint8_t type;     /* NV_RPC_REG_TYPE_* */
  uint32_t dword;   /* type 1: the value */
  const void *blob; /* type 2: `length` bytes */
  const char *str;  /* type 3: NUL-terminated, length derived (len + 1) */
  uint32_t length;  /* type 2 only */
};

/* Write one opcode-73 element: the RPC header (function 73), the packed table
 * with names after the entries and values after the names, then the framing.
 *   -EINVAL   NULL stage, a NULL/empty name, a type outside 1..3, a string
 *             entry without `str`, a binary entry with no blob or length 0.
 *   -EMSGSIZE the table does not fit one message (65456 payload bytes);
 *             splitting into CONTINUATION_RECORD (71) is the transport's, and
 *             the pre-boot path has exactly one slot for this opcode.
 * Returns the elemCount (> 0) or a negative errno. */
int nv_rpc_registry_build(uint8_t *stage,
                          const struct nv_rpc_registry_entry *e, uint32_t n,
                          uint32_t seq);

/* The table's total size in bytes for this entry list, or 0 when the list is
 * malformed (same rules as the builder).  Exposed because the row that reports
 * the staged message has to say what it expected before it reads it back. */
uint32_t nv_rpc_registry_table_size(const struct nv_rpc_registry_entry *e,
                                    uint32_t n);

/* ---- link (spec 4.2) -------------------------------------------------- */

struct nv_rpc_peer {
  uint32_t version, size, msg_size, msg_count, entry_off, rx_hdr_off, flags;
};

enum nv_rpc_link_field {
  NV_RPC_LINK_OK = 0,
  NV_RPC_LINK_VERSION,
  NV_RPC_LINK_SIZE,
  NV_RPC_LINK_MSG_SIZE,
  NV_RPC_LINK_RX_HDR_OFF,
  NV_RPC_LINK_ENTRY_OFF,
  NV_RPC_LINK_MSG_COUNT,
  NV_RPC_LINK_FLAGS,
};

const char *nv_rpc_link_field_name(enum nv_rpc_link_field f);

/* Validate the peer's producer header.  0 = usable, -EAGAIN = not written yet
 * (all 36 defined bytes zero), -EINVAL = written but unusable.  `bad` names
 * the field on -EINVAL; `peer` receives the fields, which are then USED rather
 * than hardcoded (spec 10). */
int nv_rpc_link_validate(const uint8_t *statq, uint32_t expect_size,
                         uint32_t expect_msg_size, struct nv_rpc_peer *peer,
                         enum nv_rpc_link_field *bad);

/* The SWAP_RX rule (spec 2.5) in one function, with the AND and not the
 * shortcut.  Returns the negotiated value and the two read-pointer offsets
 * (block-relative).  Either out pointer may be NULL. */
int nv_rpc_negotiate(uint32_t our_flags, uint32_t peer_flags,
                     uint32_t *own_readptr_off, uint32_t *peer_readptr_off);

/* ---- checksums (spec 0.1, 4.3 S6) ------------------------------------ */

/* Raw XOR accumulator over `bytes` (u64 words, little-endian).  `bytes`
 * must be a multiple of 8; every call site passes whole slots. */
uint64_t nv_rpc_cksum_acc(const uint8_t *buf, uint32_t bytes);

/* The fold: lo ^ hi of the accumulator.  Storing this value in the element's
 * checkSum word makes a re-fold of the whole element 0 AND leaves
 * acc_hi == acc_lo (spec 4.3 S6, spec 9 risk 3). */
uint32_t nv_rpc_cksum(const uint8_t *buf, uint32_t bytes);

/* ---- element framing (spec 4.3 S1/S5/S6, 4.4 R5..R7) ----------------- */

/* S1: elemCount = ceil((48 + rpc_len) / 4096), bounded to [1, 16]. */
int nv_rpc_elem_count(uint32_t rpc_len, uint32_t *ec_out);

/* Write a request's RPC header (version, signature, length, function, the two
 * PENDING sentinels, sequence 0, spare 0) at stage + 0x30. */
int nv_rpc_req_init(uint8_t *stage, uint32_t fn, uint32_t length);

/* Finish an element in a contiguous staging buffer: zero tag/aad/pad, zero the
 * slot tail, set seqNum/elemCount, then checksum the WHOLE element and store
 * it.  Returns elemCount (> 0) or -EINVAL/-EMSGSIZE/-EIO. */
int nv_rpc_elem_build(uint8_t *stage, uint32_t rpc_len, uint32_t seq);

/* Verify one element: whole-slot checksum, zero tag/aad, signature and length
 * bounds.  Returns 0 or -EIO with the reason. */
int nv_rpc_elem_check(const uint8_t *stage, uint32_t ec,
                      enum nv_rpc_reason *reason);

/* ---- split plan (spec 4.5, two levels) ------------------------------- */

/* chunk[0] carries the real opcode, chunk[k>=1] carries function 71; each is
 * its own message with its own header, seqNum and checksum.  Returns the
 * number of chunks (> 0) or -EMSGSIZE. */
int nv_rpc_split_plan(uint32_t payload, uint32_t *chunks, uint32_t max_chunks);

/* ---- the queue (spec 5.4) -------------------------------------------- */

struct nv_rpc_queue {
  uint8_t *block; /* kernel VA of the 0x81000 block */
  uint64_t bus;   /* GPU-visible base == PTE[0] */
  uint32_t pte_count, cmd_off, stat_off;
  uint32_t msg_count; /* 63, from our own header */

  uint32_t tx_seq, rx_seq;
  uint32_t write_ptr, read_ptr; /* MIRRORS.  Every assert re-reads memory. */
  int swapped;   /* negotiated AND of both sides' SWAP_RX */
  int linked;    /* the status queue's producer header validated */
  int tx_busy;   /* one outstanding request (spec 4.1 P4) */

  struct nv_rpc_peer peer;

  /* Resolved by nv_rpc_negotiate(); block-relative.  own_readptr_off ==
   * 0x1020 when swapped, which reads like a typo and is the whole point.
   *
   * NOTE on spec 2.5's accessor table: it prints `0x4120` for statq + 0x20 and
   * `0x4110` for statq + 0x10, but the status queue is published at 0x41000, so
   * the block-relative offsets are 0x41020 and 0x41010 -- the table dropped a
   * digit (the same class of slip as section 0.3's 8188).  The symbolic form
   * ("statq + 0x20") is the contract, and T-08 pins the arithmetic. */
  uint32_t own_readptr_off, peer_readptr_off;
  uint32_t own_writeptr_off, peer_writeptr_off;

  /* Caller-supplied scratch for reassembling a wrapped message: 16 slots. */
  uint8_t *rx_stage;

  uint32_t want_fn; /* the reply this queue is waiting for; NV_RPC_WANT_ANY */
  uint32_t tx_ec;   /* elemCount chosen by tx_prepare, consumed by tx_commit */
  uint32_t rx_ec;   /* elemCount returned by rx_take, consumed by rx_release */

  /* Observability.  The unit tier's only instrument, and the numbers the real
   * driver puts in its one [NV-RPC] row per stage. */
  uint32_t rx_cksum_fails;   /* elements that failed after all the retries */
  uint32_t rx_retries;       /* extra hashes performed by the last take */
  uint32_t rx_events, rx_unknown, rx_continuations, rx_stale;
  uint32_t dispatch_log[8];  /* last functions dispatched, oldest first */
  uint32_t dispatch_log_n;
};

/* Point a queue at a built block.  rx_stage may be NULL only if the caller
 * promises never to receive a wrapped message. */
int nv_rpc_queue_init(struct nv_rpc_queue *q, uint8_t *block, uint64_t block_bus,
                      uint8_t *rx_stage);

/* §4.2 step 2-3 without the poll: validate once, negotiate, publish readPtr 0,
 * set linked.  -EAGAIN when the peer's header is not written yet. */
int nv_rpc_link_apply(struct nv_rpc_queue *q, enum nv_rpc_link_field *bad);

/* The bounded poll (§4.2 step 1) using hw->now_us, so a test advances a fake
 * clock past the timeout instead of sleeping.  -ETIMEDOUT / NOT_LINKED. */
int nv_rpc_link(struct nv_rpc_queue *q, const struct nv_rpc_hw *hw,
                uint32_t timeout_us, enum nv_rpc_reason *reason);

/* ---- send (spec 4.3) ------------------------------------------------- */

/* Free slots for the producer: (pr + 63 - writePtr - 1) % 63, or 62 before the
 * link (msgqTxCreate's pre-boot allowance).  Negative errno on a peer pointer
 * outside the ring. */
int nv_rpc_free_slots(struct nv_rpc_queue *q, enum nv_rpc_reason *reason);

/* S1..S3: pick elemCount and prove it fits.  No stores to the ring. */
int nv_rpc_tx_prepare(struct nv_rpc_queue *q, uint32_t rpc_len,
                      uint32_t *ec_out, enum nv_rpc_reason *reason);

/* S4..S8: zero and copy each slot (with wrap), fence, advance writePtr.
 * Nobody is notified yet -- that is nv_rpc_kick(). */
int nv_rpc_tx_commit(struct nv_rpc_queue *q, const uint8_t *stage,
                     const struct nv_rpc_hw *hw);

/* S9/S10: fence, then exactly one doorbell write.  Returns -EINVAL when there
 * is no seam at all, because a silent no-op looks like success. */
int nv_rpc_kick(struct nv_rpc_queue *q, const struct nv_rpc_hw *hw);

/* ---- receive (spec 4.4) ---------------------------------------------- */

/* R1..R7 and R10's classification.  Returns the RPC length (>= 32) of the
 * message the caller asked for, copied to `out` as RPC header + payload, or
 * -EAGAIN/-EIO/-EMSGSIZE/-EINVAL with the reason.  Does NOT advance the read
 * pointer: nv_rpc_rx_release() does, after the copy. */
int nv_rpc_rx_take(struct nv_rpc_queue *q, const struct nv_rpc_hw *hw,
                   void *out, uint32_t cap, enum nv_rpc_reason *reason);

/* R8/R9 for the message rx_take() returned. */
int nv_rpc_rx_release(struct nv_rpc_queue *q);

/* ---- real seam + log row (x86_64 only, like the rest of the driver) --- */

#ifdef __x86_64__
struct nv_dev;
/* doorbell -> nv_wr32(d, NV_RPC_DOORBELL_OFF, 0); fence ->
 * arch_memory_barrier(); now_us -> timer_get_us(). */
void nv_rpc_hw_dev(struct nv_dev *d, struct nv_rpc_hw *hw);

/* The block is RAM, not BAR0, so NV_IOC_PEEK cannot reach it.  One row per
 * stage, naming the stage: "link", "send", "reply", "event" (spec 6.2 #6/#7). */
void nv_rpc_dump(const char *stage, const struct nv_rpc_queue *q);
#endif

/* Kernel unit tier (nv_rpc_test.c): the spec's T-01..T-24. */
void nv_rpc_test_suite(void);

#endif /* NV_RPC_H */
