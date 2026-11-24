/* ============================================================================
 *  nv_rpc: the GSP-RPC shared-memory transport (docs/nv-rpc-implementation-spec.md).
 *
 *  The pure half.  Everything here reads and writes THE BLOCK and nothing
 *  else: no MMIO, no clock, no sleep (spec 5.1).  The two things it cannot do
 *  itself -- `mb()` and the doorbell -- arrive through struct nv_rpc_hw, so the
 *  unit tier counts them instead of assuming them, and the "written but not
 *  notified" failure is a test rather than an incident.
 *
 *  Four rules from the spec are load-bearing and each is implemented once:
 *
 *  1. The checksum covers WHOLE SLOTS, not 48 + rpc.length (spec 0.1).  The
 *     element region is zeroed, then fold64(XOR of u64 over ec*4096 bytes) is
 *     stored at slot+0x20.  The short range would accept our own element while
 *     the firmware's whole-slot check fails, and the element is dropped before
 *     it is parsed -- indistinguishable from a dead queue.
 *  2. There are TWO splits (spec 0.2): one RPC spanning 2..16 slots (slots 1..N
 *     are raw continuation bytes, one seqNum) and continuation MESSAGES with
 *     function 71 beyond 16 slots (each with its own header, seqNum, checksum).
 *  3. SWAP_RX makes the read-pointer words CROSS (spec 2.5).  With it set, we
 *     publish our status read index into the command block (cmdq+0x20) and
 *     load the firmware's command read index from the status block
 *     (statq+0x20).  own_readptr_off == 0x1020 reads like a typo; it is the
 *     whole point, and it is negotiated in exactly one function.
 *  4. One slot is always left empty (spec 2.2), so a full ring cannot look
 *     empty.  free = (peer_read + 63 - write - 1) % 63.
 *
 *  NO STRUCT OVERLAY, and no wide loads at an odd offset: every wire field is
 *  assembled or stored byte by byte through volatile uint8_t.  That is what
 *  AArch64 EL1 requires of Device-typed memory (skills/hobbyos-kernel-
 *  constraints), it keeps the byte layout a property of the document rather
 *  than of the compiler, and it makes the "the tail of the slot is part of the
 *  hash" rule expressible.
 *
 *  DEPENDENCY DECISION: nv_rpc_block_build() takes block_bus.  Nothing here
 *  calls nv_dma; this branch links standalone.  No bus address is ever spelled
 *  `phys`.
 * ========================================================================== */

#include "nv_rpc.h"
#include "errno.h"

#include <stddef.h>

extern void *memset(void *dst, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

/* ---- little-endian, byte-wise, volatile --------------------------------
 *
 * volatile on both sides on purpose: the peer's header words are ordinary RAM
 * that a device writes by DMA, so a load must not be hoisted out of a poll and
 * a store must not be elided.  Byte stores are also individually ordered by
 * the compiler, which is the ordering S7/S8 need. */

static uint32_t ld32(const uint8_t *p) {
  const volatile uint8_t *b = p;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

static uint64_t ld64(const uint8_t *p) {
  const volatile uint8_t *b = p;
  uint64_t lo = (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) |
                ((uint64_t)b[3] << 24);
  uint64_t hi = (uint64_t)b[4] | ((uint64_t)b[5] << 8) | ((uint64_t)b[6] << 16) |
                ((uint64_t)b[7] << 24);
  return lo | (hi << 32);
}

static void st32(uint8_t *p, uint32_t v) {
  volatile uint8_t *b = p;
  b[0] = (uint8_t)v;
  b[1] = (uint8_t)(v >> 8);
  b[2] = (uint8_t)(v >> 16);
  b[3] = (uint8_t)(v >> 24);
}

static void st64(uint8_t *p, uint64_t v) {
  volatile uint8_t *b = p;
  b[0] = (uint8_t)v;
  b[1] = (uint8_t)(v >> 8);
  b[2] = (uint8_t)(v >> 16);
  b[3] = (uint8_t)(v >> 24);
  b[4] = (uint8_t)(v >> 32);
  b[5] = (uint8_t)(v >> 40);
  b[6] = (uint8_t)(v >> 48);
  b[7] = (uint8_t)(v >> 56);
}

static void fence(const struct nv_rpc_hw *hw) {
  if (hw && hw->fence) hw->fence(hw->ctx);
}

static const char *const reason_names[] = {
    "ok",           "nothing",        "not_linked", "short_publish",
    "no_space",     "too_big",        "peer_geom",  "big_elem",
    "bad_ec",       "checksum",       "seq_gap",    "seq_stale",
    "bad_sig",      "bad_len",        "event",      "unknown_fn",
    "continuation", "busy",           "no_reply",
};

const char *nv_rpc_reason_name(enum nv_rpc_reason r) {
  if ((int)r < 0 || (unsigned)r >= sizeof(reason_names) / sizeof(reason_names[0]))
    return "?";
  return reason_names[r];
}

static const char *const arg_names[] = {
    "ok",                 "sharedMemPhysAddr",   "pageTableEntryCount",
    "cmdQueueOffset",     "statQueueOffset",     "queueElementHdrSize",
    "queueElementSizeMin", "queueElementSizeMax", "queueHeaderAlign",
    "queueElementAlign",  "magic",               "size",
};

const char *nv_rpc_arg_field_name(enum nv_rpc_arg_field f) {
  if ((int)f < 0 || (unsigned)f >= sizeof(arg_names) / sizeof(arg_names[0]))
    return "?";
  return arg_names[f];
}

static const char *const link_names[] = {
    "ok",      "version", "size",     "msgSize",
    "rxHdrOff", "entryOff", "msgCount", "flags",
};

const char *nv_rpc_link_field_name(enum nv_rpc_link_field f) {
  if ((int)f < 0 || (unsigned)f >= sizeof(link_names) / sizeof(link_names[0]))
    return "?";
  return link_names[f];
}

/* ======================================================================== */
/* geometry and the block (spec 1, 3)                                        */
/* ======================================================================== */

int nv_rpc_queue_geom(struct nv_rpc_geom *g) {
  if (!g) return -EINVAL;
  g->pte_count = NV_RPC_PTE_COUNT;
  g->cmd_off = NV_RPC_CMD_OFF;
  g->stat_off = NV_RPC_STAT_OFF;
  g->block_size = NV_RPC_BLOCK_SIZE;
  g->msg_size = NV_RPC_SLOT;
  g->msg_count = NV_RPC_MSG_COUNT;

  /* The invariants of spec 3, checked here instead of asserted in prose.  A
   * drifted constant is a block the firmware walks into the wrong place. */
  if ((uint32_t)g->pte_count * 8u > NV_RPC_PTE_BYTES) return -EINVAL;
  if ((g->cmd_off & 0xFFFu) || (g->stat_off & 0xFFFu)) return -EINVAL;
  if (g->cmd_off + NV_RPC_QUEUE_BYTES != g->stat_off) return -EINVAL;
  if (g->stat_off + NV_RPC_QUEUE_BYTES != g->block_size) return -EINVAL;
  if (g->msg_size < 16u || (g->msg_size & (g->msg_size - 1u))) return -EINVAL;
  if ((NV_RPC_QUEUE_BYTES - NV_RPC_SLOT) / NV_RPC_SLOT != g->msg_count)
    return -EINVAL;
  if (g->msg_count < 2u) return -EINVAL; /* else the ring has no empty slot */
  return 0;
}

int nv_rpc_block_build(uint8_t *block, uint64_t block_bus) {
  struct nv_rpc_geom g;

  if (!block) return -EINVAL;
  if (nv_rpc_queue_geom(&g) != 0) return -EINVAL;
  /* The GPU walks PTE[] and each PTE is a page address: 4 KiB alignment is the
   * format, not a preference.  A zero bus is "no address" and must not be
   * mistaken for one (prefill rule: zero must never mean success). */
  if (block_bus == 0 || (block_bus & 0xFFFu)) return -EINVAL;

  /* Spec 3 step 1: zero ALL 129 pages.  Not tidiness -- the checksum covers
   * whole slots, and a zeroed status header is what makes "the firmware has
   * not written its header yet" distinguishable from "it wrote a bad one". */
  memset(block, 0, NV_RPC_BLOCK_SIZE);

  /* Step 2: the self-covering page table.  PTE[i] is the GPU-visible address
   * of page i, which is what the firmware will use; it is not necessarily the
   * CPU physical address (spec 9 risk 1) and this file never assumes it is. */
  for (uint32_t i = 0; i < g.pte_count; i++)
    st64(block + (uint64_t)i * 8u, block_bus + (uint64_t)i * NV_RPC_SLOT);

  if (ld64(block) != block_bus) return -EINVAL;
  if (ld64(block + (uint64_t)(g.pte_count - 1u) * 8u) !=
      block_bus + (uint64_t)(g.pte_count - 1u) * NV_RPC_SLOT)
    return -EINVAL;

  /* Step 3: the COMMAND queue's producer header (spec 2.1).  readPtr at +0x20
   * stays 0 -- it is the consumer's word. */
  uint8_t *q = block + g.cmd_off;
  st32(q + NV_RPC_Q_VERSION, 0);
  st32(q + NV_RPC_Q_SIZE, NV_RPC_QUEUE_BYTES);
  st32(q + NV_RPC_Q_MSG_SIZE, NV_RPC_SLOT);
  st32(q + NV_RPC_Q_MSG_COUNT, g.msg_count);
  st32(q + NV_RPC_Q_WRITE_PTR, 0);
  st32(q + NV_RPC_Q_FLAGS, NV_RPC_FLAGS_SWAP_RX);
  st32(q + NV_RPC_Q_RX_HDR_OFF, NV_RPC_Q_READ_PTR);  /* 0x20 */
  st32(q + NV_RPC_Q_ENTRY_OFF, NV_RPC_SLOT);         /* 0x1000 */

  /* Step 4: the STATUS queue's header is deliberately NOT written.  The
   * firmware writes it when its RM task starts, and that write is the first
   * proof of life on the whole boot path.  Pre-filling it would make our own
   * bytes look like the firmware's and destroy the evidence.  (T-03 asserts
   * the bytes are still zero and that validation fails with -EAGAIN.) */
  return 0;
}

int nv_rpc_args_crosscheck(const struct nv_rpc_geom *g, uint64_t block_bus,
                           const struct nv_rpc_rmargs *a) {
  if (!g || !a) return NV_RPC_ARG_SHARED_MEM;

  /* The RMARGS page and this block describe the same geometry twice (spec 3
   * step 5).  One function, one field at a time, named on failure: this is the
   * cheapest test in the document and it catches the class of bug that costs a
   * boot.
   *
   * magic and size come first: without them a page of correct-looking numbers
   * is just a page of numbers, and "the firmware did not find the block" and
   * "the firmware found something that is not our block" would print the same
   * row. */
  if (a->magic != NV_RPC_GSP_ARGS_MAGIC) return NV_RPC_ARG_MAGIC;
  if (a->size != NV_RPC_GSP_ARGS_SIZE) return NV_RPC_ARG_SIZE;
  if (a->shared_mem_phys != block_bus) return NV_RPC_ARG_SHARED_MEM;
  if (a->pte_count != g->pte_count) return NV_RPC_ARG_PTE_COUNT;
  if (a->cmd_queue_off != g->cmd_off) return NV_RPC_ARG_CMD_OFF;
  if (a->stat_queue_off != g->stat_off) return NV_RPC_ARG_STAT_OFF;
  if (a->elem_hdr_size != NV_RPC_ELEM_HDR) return NV_RPC_ARG_HDR_SIZE;
  if (a->elem_size_min != g->msg_size) return NV_RPC_ARG_ELEM_MIN;
  if (a->elem_size_max != NV_RPC_ARG_ELEM_MAX) return NV_RPC_ARG_ELEM_MAX;
  if (a->header_align != NV_RPC_ARG_HDR_ALIGN) return NV_RPC_ARG_HDR_ALIGN;
  if (a->elem_align != NV_RPC_ARG_ELEM_ALIGN) return NV_RPC_ARG_ELEM_ALIGN;
  return NV_RPC_ARG_OK;
}

/* ======================================================================== */
/* publishing: GSP_ARGUMENTS_CACHED + the libos-args table (contract 1.5/6)  */
/* ======================================================================== */

int nv_rpc_args_build(uint8_t *page, const struct nv_rpc_geom *g,
                      uint64_t block_bus) {
  if (!page || !g) return -EINVAL;
  /* The argument block describes THIS transport.  A geometry that is not the
   * one nv_rpc_queue_geom() fixes would publish a page describing queues the
   * firmware cannot find, which reads as "RM never started". */
  struct nv_rpc_geom chk;
  if (nv_rpc_queue_geom(&chk) != 0) return -EINVAL;
  if (g->pte_count != chk.pte_count || g->cmd_off != chk.cmd_off ||
      g->stat_off != chk.stat_off || g->msg_size != chk.msg_size ||
      g->block_size != chk.block_size || g->msg_count != chk.msg_count)
    return -EINVAL;
  /* Same rule the PTE table is built under: a zero or unaligned base is not
   * an address, and this one is handed to firmware as PTE[0]. */
  if (block_bus == 0 || (block_bus & 0xFFFu)) return -EINVAL;

  /* The reserved tail (0x68..0xC7 and everything to the page end) is zero by
   * construction rather than by remembering to clear it. */
  memset(page, 0, NV_RPC_GSP_ARGS_PAGE);

  st32(page + NV_RPC_ARG_O_MAGIC, NV_RPC_GSP_ARGS_MAGIC);
  /* size is u16 at +0x04 followed by two reserved bytes: one 32-bit store of
   * 0x000000C8 writes exactly the three bytes the contract asks for. */
  st32(page + NV_RPC_ARG_O_SIZE, NV_RPC_GSP_ARGS_SIZE);
  st64(page + NV_RPC_ARG_O_FLAGS, 0);    /* not a resume */
  st64(page + NV_RPC_ARG_O_MQ_FLAGS, 0); /* no queue encryption (no CC) */
  st64(page + NV_RPC_ARG_O_MQ_SHARED, block_bus);
  st64(page + NV_RPC_ARG_O_MQ_CMD, (uint64_t)g->cmd_off);
  st64(page + NV_RPC_ARG_O_MQ_STAT, (uint64_t)g->stat_off);
  st32(page + NV_RPC_ARG_O_MQ_HDR_SIZE, NV_RPC_ELEM_HDR);
  st32(page + NV_RPC_ARG_O_MQ_ELEM_MIN, g->msg_size);
  st32(page + NV_RPC_ARG_O_MQ_ELEM_MAX, NV_RPC_ARG_ELEM_MAX);
  st32(page + NV_RPC_ARG_O_MQ_HDR_ALIGN, NV_RPC_ARG_HDR_ALIGN);
  st32(page + NV_RPC_ARG_O_MQ_ELEM_ALIGN, NV_RPC_ARG_ELEM_ALIGN);
  st32(page + NV_RPC_ARG_O_MQ_PTE_COUNT, g->pte_count);
  st32(page + NV_RPC_ARG_O_SR_OLD_LEVEL, 0); /* cold boot, not a resume */
  st32(page + NV_RPC_ARG_O_SR_FLAGS, 0);
  st32(page + NV_RPC_ARG_O_GPU_INSTANCE, 0);

  /* Read back the two fields a writer could get wrong without noticing (a
   * store that landed somewhere else still returns from this function). */
  if (ld32(page + NV_RPC_ARG_O_MAGIC) != NV_RPC_GSP_ARGS_MAGIC) return -EIO;
  if ((uint16_t)(ld32(page + NV_RPC_ARG_O_SIZE) & 0xFFFFu) !=
      NV_RPC_GSP_ARGS_SIZE)
    return -EIO;
  if (ld64(page + NV_RPC_ARG_O_MQ_SHARED) != block_bus) return -EIO;
  return 0;
}

int nv_rpc_args_read(const uint8_t *page, struct nv_rpc_rmargs *a) {
  if (!page || !a) return -EINVAL;
  /* Every field the cross-check names, from the bytes and from nowhere else:
   * a caller that built the page from a struct could otherwise "verify" its
   * own variables. */
  a->magic = ld32(page + NV_RPC_ARG_O_MAGIC);
  a->size = (uint16_t)(ld32(page + NV_RPC_ARG_O_SIZE) & 0xFFFFu);
  a->shared_mem_phys = ld64(page + NV_RPC_ARG_O_MQ_SHARED);
  a->cmd_queue_off = ld64(page + NV_RPC_ARG_O_MQ_CMD);
  a->stat_queue_off = ld64(page + NV_RPC_ARG_O_MQ_STAT);
  a->elem_hdr_size = ld32(page + NV_RPC_ARG_O_MQ_HDR_SIZE);
  a->elem_size_min = ld32(page + NV_RPC_ARG_O_MQ_ELEM_MIN);
  a->elem_size_max = ld32(page + NV_RPC_ARG_O_MQ_ELEM_MAX);
  a->header_align = ld32(page + NV_RPC_ARG_O_MQ_HDR_ALIGN);
  a->elem_align = ld32(page + NV_RPC_ARG_O_MQ_ELEM_ALIGN);
  a->pte_count = ld32(page + NV_RPC_ARG_O_MQ_PTE_COUNT);
  return 0;
}

uint64_t nv_rpc_id8(const char *s) {
  if (!s) return 0;
  /* Up to eight characters, NUL padded to eight: "RMARGS" is six characters
   * and its id is 0x0000534752414D52 -- the two zero bytes are part of the
   * name, not an accident.
   *
   * Length first, and only within the string: reading s[8] to decide whether
   * the name was too long reads past a shorter literal, and the corruption
   * that follows is a NUL-padded name silently turning into "no id". */
  uint32_t n = 0;
  while (n < NV_RPC_LIBOS_ID_BYTES && s[n] != 0) n++;
  if (n == 0) return 0;
  if (s[n] != 0) return 0; /* a ninth character: the name would be truncated */

  uint64_t v = 0;
  for (uint32_t i = 0; i < n; i++)
    v |= (uint64_t)(uint8_t)s[i] << (8u * i);
  return v;
}

int nv_rpc_libos_build(uint8_t *page, const struct nv_rpc_libos_region *e,
                       uint32_t n) {
  if (!page || !e || n == 0 || n > NV_RPC_LIBOS_MAX) return -EINVAL;

  memset(page, 0, NV_RPC_GSP_ARGS_PAGE);

  for (uint32_t i = 0; i < n; i++) {
    uint64_t id = e[i].id ? nv_rpc_id8(e[i].id) : e[i].id8;
    if (id == 0) return -EINVAL;
    if (e[i].pa == 0 || (e[i].pa & 0xFFFu)) return -EINVAL;
    if (e[i].size == 0) return -EINVAL;

    uint8_t *p = page + (uint64_t)i * NV_RPC_LIBOS_ENTRY;
    st64(p + NV_RPC_LIBOS_O_ID8, id);
    st64(p + NV_RPC_LIBOS_O_PA, e[i].pa);
    st64(p + NV_RPC_LIBOS_O_SIZE, e[i].size);
    p[NV_RPC_LIBOS_O_KIND] = e[i].kind;
    p[NV_RPC_LIBOS_O_LOC] = e[i].loc;

    if (ld64(p + NV_RPC_LIBOS_O_ID8) != id ||
        ld64(p + NV_RPC_LIBOS_O_PA) != e[i].pa)
      return -EIO;
  }
  return 0;
}

int nv_rpc_libos_read(const uint8_t *page, uint32_t i,
                      struct nv_rpc_libos_region *out) {
  if (!page || !out || i >= NV_RPC_LIBOS_MAX) return -EINVAL;
  const uint8_t *p = page + (uint64_t)i * NV_RPC_LIBOS_ENTRY;
  /* id8 is the packed u64 from the page, not a string: the page has no NUL,
   * so a caller comparing it to a name compares it to nv_rpc_id8("RMARGS"). */
  out->id = 0;
  out->id8 = ld64(p + NV_RPC_LIBOS_O_ID8);
  out->pa = ld64(p + NV_RPC_LIBOS_O_PA);
  out->size = ld64(p + NV_RPC_LIBOS_O_SIZE);
  out->kind = p[NV_RPC_LIBOS_O_KIND];
  out->loc = p[NV_RPC_LIBOS_O_LOC];
  return 0;
}

/* ======================================================================== */
/* link (spec 4.2)                                                           */
/* ======================================================================== */

int nv_rpc_link_validate(const uint8_t *statq, uint32_t expect_size,
                         uint32_t expect_msg_size, struct nv_rpc_peer *peer,
                         enum nv_rpc_link_field *bad) {
  if (bad) *bad = NV_RPC_LINK_OK;
  if (!statq) {
    if (bad) *bad = NV_RPC_LINK_VERSION;
    return -EINVAL;
  }

  uint32_t version = ld32(statq + NV_RPC_Q_VERSION);
  uint32_t size = ld32(statq + NV_RPC_Q_SIZE);
  uint32_t msg_size = ld32(statq + NV_RPC_Q_MSG_SIZE);
  uint32_t msg_count = ld32(statq + NV_RPC_Q_MSG_COUNT);
  uint32_t write_ptr = ld32(statq + NV_RPC_Q_WRITE_PTR);
  uint32_t flags = ld32(statq + NV_RPC_Q_FLAGS);
  uint32_t rx_hdr_off = ld32(statq + NV_RPC_Q_RX_HDR_OFF);
  uint32_t entry_off = ld32(statq + NV_RPC_Q_ENTRY_OFF);

  /* "Not written yet" and "written but unusable" are different bugs: the first
   * means the firmware never found the block, the second means the RMARGS
   * offsets were wrong.  An entirely zero header is the first case; every
   * field we publish is nonzero except `version`, so one nonzero byte in the
   * 32-byte producer header means somebody wrote it. */
  if (version == 0 && size == 0 && msg_size == 0 && msg_count == 0 &&
      write_ptr == 0 && flags == 0 && rx_hdr_off == 0 && entry_off == 0)
    return -EAGAIN;

  /* msgqRxLink's own order (spec 4.2 step 1, -6..-10). */
  if (version != 0) {
    if (bad) *bad = NV_RPC_LINK_VERSION;
    return -EINVAL;
  }
  if (size != expect_size) {
    if (bad) *bad = NV_RPC_LINK_SIZE;
    return -EINVAL;
  }
  if (msg_size != expect_msg_size) {
    if (bad) *bad = NV_RPC_LINK_MSG_SIZE;
    return -EINVAL;
  }
  if (rx_hdr_off < NV_RPC_Q_READ_PTR) {
    if (bad) *bad = NV_RPC_LINK_RX_HDR_OFF;
    return -EINVAL;
  }
  if (entry_off < rx_hdr_off + 4u || (entry_off & 0xFFFu)) {
    if (bad) *bad = NV_RPC_LINK_ENTRY_OFF;
    return -EINVAL;
  }
  /* The "header arrived intact" recomputation: msgCount must follow from size
   * and entryOff, or the two sides index different slots. */
  if (msg_size == 0 || (size - entry_off) / msg_size != msg_count) {
    if (bad) *bad = NV_RPC_LINK_MSG_COUNT;
    return -EINVAL;
  }
  /* An unknown flag is an unknown protocol (spec 2.1 DECISION). */
  if (flags & NV_RPC_FLAGS_RESERVED) {
    if (bad) *bad = NV_RPC_LINK_FLAGS;
    return -EINVAL;
  }

  if (peer) {
    peer->version = version;
    peer->size = size;
    peer->msg_size = msg_size;
    peer->msg_count = msg_count;
    peer->entry_off = entry_off;
    peer->rx_hdr_off = rx_hdr_off;
    peer->flags = flags;
  }
  return 0;
}

int nv_rpc_negotiate(uint32_t our_flags, uint32_t peer_flags,
                     uint32_t *own_readptr_off, uint32_t *peer_readptr_off) {
  /* The layout is the AND of the two sides (spec 2.5).  Implement the AND, not
   * the shortcut "our flags are 1 so it is the peer's bit 0" -- the shortcut is
   * exactly what breaks if we ever publish 0. */
  int swapped = ((our_flags & NV_RPC_FLAGS_SWAP_RX) &&
                 (peer_flags & NV_RPC_FLAGS_SWAP_RX))
                    ? 1
                    : 0;
  if (swapped) {
    /* The words cross: we store our status read index in the COMMAND block,
     * and we load the firmware's command read index from the STATUS block. */
    if (own_readptr_off) *own_readptr_off = NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR;
    if (peer_readptr_off) *peer_readptr_off = NV_RPC_STAT_OFF + NV_RPC_Q_READ_PTR;
  } else {
    if (own_readptr_off) *own_readptr_off = NV_RPC_STAT_OFF + NV_RPC_Q_READ_PTR;
    if (peer_readptr_off) *peer_readptr_off = NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR;
  }
  return swapped;
}

int nv_rpc_queue_init(struct nv_rpc_queue *q, uint8_t *block, uint64_t block_bus,
                      uint8_t *rx_stage) {
  struct nv_rpc_geom g;
  if (!q) return -EINVAL;
  if (nv_rpc_queue_geom(&g) != 0) return -EINVAL;
  memset(q, 0, sizeof(*q));
  q->block = block;
  q->bus = block_bus;
  q->pte_count = g.pte_count;
  q->cmd_off = g.cmd_off;
  q->stat_off = g.stat_off;
  q->msg_count = g.msg_count;
  q->rx_stage = rx_stage;
  q->want_fn = NV_RPC_WANT_ANY;
  q->own_writeptr_off = g.cmd_off + NV_RPC_Q_WRITE_PTR;
  q->peer_writeptr_off = g.stat_off + NV_RPC_Q_WRITE_PTR;
  /* Before the peer's flags are known, assume the expected case so the offsets
   * are always defined; nv_rpc_link_apply() renegotiates them for real. */
  q->swapped = nv_rpc_negotiate(NV_RPC_FLAGS_SWAP_RX, NV_RPC_FLAGS_SWAP_RX,
                                &q->own_readptr_off, &q->peer_readptr_off);
  return 0;
}

int nv_rpc_link_apply(struct nv_rpc_queue *q, enum nv_rpc_link_field *bad) {
  if (bad) *bad = NV_RPC_LINK_OK;
  if (!q || !q->block) return -EINVAL;

  struct nv_rpc_peer peer;
  enum nv_rpc_link_field b = NV_RPC_LINK_OK;
  int rc = nv_rpc_link_validate(q->block + q->stat_off, NV_RPC_QUEUE_BYTES,
                                NV_RPC_SLOT, &peer, &b);
  if (bad) *bad = b;
  if (rc == -EINVAL) return -EINVAL;
  if (rc == -EAGAIN) return -EAGAIN;

  q->peer = peer;
  q->swapped = nv_rpc_negotiate(NV_RPC_FLAGS_SWAP_RX, peer.flags,
                                &q->own_readptr_off, &q->peer_readptr_off);
  q->own_writeptr_off = q->cmd_off + NV_RPC_Q_WRITE_PTR;
  q->peer_writeptr_off = q->stat_off + NV_RPC_Q_WRITE_PTR;

  /* Spec 4.2 step 2: now that the polarity is known, publish readPtr = 0 at
   * the offset SWAP_RX dictates.  This is the one store that must not be at the
   * obvious place. */
  st32(q->block + q->own_readptr_off, 0);
  q->read_ptr = 0;
  q->write_ptr = ld32(q->block + q->cmd_off + NV_RPC_Q_WRITE_PTR);
  q->linked = 1;
  return 0;
}

int nv_rpc_link(struct nv_rpc_queue *q, const struct nv_rpc_hw *hw,
                uint32_t timeout_us, enum nv_rpc_reason *reason) {
  if (reason) *reason = NV_RPC_OK;
  if (!q || !q->block) return -EINVAL;

  uint64_t start = 0;
  int have_clock = (hw && hw->now_us);
  if (have_clock) start = hw->now_us(hw->ctx);

  for (;;) {
    enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
    int rc = nv_rpc_link_apply(q, &bad);
    if (rc == 0) return 0;
    if (rc == -EINVAL) {
      if (reason) *reason = NV_RPC_PEER_GEOM;
      return -EINVAL; /* written but unusable: waiting will not fix it */
    }
    if (!have_clock) {
      if (reason) *reason = NV_RPC_NOT_LINKED;
      return -EAGAIN;
    }
    if (hw->now_us(hw->ctx) - start >= timeout_us) {
      if (reason) *reason = NV_RPC_NOT_LINKED;
      return -ETIMEDOUT;
    }
    fence(hw);
  }
}

/* ======================================================================== */
/* checksums (spec 0.1, 4.3 S6)                                              */
/* ======================================================================== */

uint64_t nv_rpc_cksum_acc(const uint8_t *buf, uint32_t bytes) {
  uint64_t acc = 0;
  if (!buf) return 0;
  /* bytes % 8 == 0 is a precondition, not a suggestion: every call site passes
   * whole slots.  A short tail is folded here as zero-padding would be, which
   * is why no call site relies on it. */
  uint32_t full = bytes & ~7u;
  for (uint32_t i = 0; i < full; i += 8) acc ^= ld64(buf + i);
  if (bytes != full) {
    uint8_t tail[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (uint32_t i = full; i < bytes; i++) tail[i - full] = buf[i];
    acc ^= ld64(tail);
  }
  return acc;
}

uint32_t nv_rpc_cksum(const uint8_t *buf, uint32_t bytes) {
  uint64_t a = nv_rpc_cksum_acc(buf, bytes);
  return (uint32_t)a ^ (uint32_t)(a >> 32);
}

/* ======================================================================== */
/* element framing (spec 4.3 S1/S5/S6, 4.4 R5..R7)                           */
/* ======================================================================== */

int nv_rpc_elem_count(uint32_t rpc_len, uint32_t *ec_out) {
  /* P2: a message shorter than its own RPC header cannot be framed at all. */
  if (rpc_len < NV_RPC_RPC_HDR) return -EINVAL;
  if (rpc_len > NV_RPC_LEN_MAX) return -EMSGSIZE;

  /* S1: ceil((48 + rpc_len) / 4096), and the boundary is STRICT: a message
   * that ends exactly on a slot boundary occupies that slot and no more. */
  uint64_t total = (uint64_t)NV_RPC_ELEM_HDR + (uint64_t)rpc_len;
  uint32_t ec = (uint32_t)((total + (NV_RPC_SLOT - 1u)) / NV_RPC_SLOT);
  if (ec < 1u || ec > NV_RPC_ELEM_MAX) return -EMSGSIZE;
  /* The assert that makes an understated or overstated elemCount impossible in
   * one line (contract row 10): both failure directions are catastrophic and
   * silent. */
  if (total <= (uint64_t)(ec - 1u) * NV_RPC_SLOT) return -EINVAL;
  if (total > (uint64_t)ec * NV_RPC_SLOT) return -EINVAL;
  if (ec_out) *ec_out = ec;
  return 0;
}

int nv_rpc_req_init(uint8_t *stage, uint32_t fn, uint32_t length) {
  if (!stage || length < NV_RPC_RPC_HDR) return -EINVAL;
  st32(stage + NV_RPC_R_HDRVER, NV_RPC_VERSION);
  st32(stage + NV_RPC_R_SIG, NV_RPC_SIG);
  st32(stage + NV_RPC_R_LENGTH, length);
  st32(stage + NV_RPC_R_FUNCTION, fn);
  st32(stage + NV_RPC_R_RESULT, NV_RPC_PENDING);
  st32(stage + NV_RPC_R_RESULT_PRIV, NV_RPC_PENDING);
  st32(stage + NV_RPC_R_SEQUENCE, 0);
  st32(stage + NV_RPC_R_SPARE, 0);
  return 0;
}

int nv_rpc_elem_build(uint8_t *stage, uint32_t rpc_len, uint32_t seq) {
  uint32_t ec;
  if (!stage) return -EINVAL;
  int rc = nv_rpc_elem_count(rpc_len, &ec);
  if (rc != 0) return rc;

  /* P3's checkable half: the RPC's own length field must describe the message
   * we are framing, and it must be one of ours.  (The per-opcode payload-size
   * table lives with the opcode, not here.) */
  if (ld32(stage + NV_RPC_R_SIG) != NV_RPC_SIG) return -EINVAL;
  if (ld32(stage + NV_RPC_R_LENGTH) != rpc_len) return -EINVAL;

  /* S5: without confidential compute the 32 bytes of authTag+AAD are dead
   * weight and never interpreted.  Zero them on every element and assert, so a
   * later CC path cannot be silently half-implemented. */
  memset(stage + NV_RPC_E_AUTHTAG, 0, 16);
  memset(stage + NV_RPC_E_AAD, 0, 16);
  st32(stage + NV_RPC_E_CKSUM, 0); /* computed below, inside the hashed range */
  st32(stage + NV_RPC_E_SEQ, seq);
  st32(stage + NV_RPC_E_ELEMCOUNT, ec);
  st32(stage + NV_RPC_E_PAD, 0); /* nonzero pad is inside the hash: guaranteed
                                  * rejection */

  /* S4's tail: zero the rest of the last slot.  The checksum then covers
   * exactly what the receiver will hash, and a slot reused without being
   * cleared cannot pass. */
  uint32_t used = NV_RPC_ELEM_HDR + rpc_len;
  if (used < ec * NV_RPC_SLOT)
    memset(stage + used, 0, (size_t)(ec * NV_RPC_SLOT - used));

  /* S6: hash the WHOLE element (spec 0.1), store the fold at +0x20, then prove
   * the property the receiver relies on: a re-fold is 0 and the accumulator's
   * halves are equal.  This is the last point where a wrong checksum rule is
   * cheap -- after the doorbell it is indistinguishable from a dead queue. */
  uint32_t csum = nv_rpc_cksum(stage, ec * NV_RPC_SLOT);
  st32(stage + NV_RPC_E_CKSUM, csum);

  uint64_t a = nv_rpc_cksum_acc(stage, ec * NV_RPC_SLOT);
  /* After storing the fold, the two halves of the accumulator are EQUAL (not
   * zero): acc_lo = A.lo ^ csum = A.hi.  Comparing them, rather than comparing
   * both against 0, is the whole content of the acc_hi == acc_lo assert. */
  if ((uint32_t)a != (uint32_t)(a >> 32)) return -EIO;
  return (int)ec;
}

int nv_rpc_elem_check(const uint8_t *stage, uint32_t ec,
                      enum nv_rpc_reason *reason) {
  if (reason) *reason = NV_RPC_OK;
  if (!stage) return -EINVAL;
  if (ec < 1u) {
    if (reason) *reason = NV_RPC_BAD_EC;
    return -EINVAL;
  }
  if (ec > NV_RPC_ELEM_MAX) {
    if (reason) *reason = NV_RPC_BIG_ELEM;
    return -EINVAL;
  }

  /* R5: the whole-slot fold must be 0.  Never "consume a message you failed to
   * verify" -- the caller does not advance the read pointer on this path. */
  if ((ec * NV_RPC_SLOT) & 7u) { /* unreachable: slot size is a multiple of 8 */
    if (reason) *reason = NV_RPC_CHECKSUM;
    return -EIO;
  }
  if (nv_rpc_cksum(stage, ec * NV_RPC_SLOT) != 0) {
    if (reason) *reason = NV_RPC_CHECKSUM;
    return -EIO;
  }

  /* With a valid fold, a nonzero tag or AAD means the peer is running a
   * confidentiality path we cannot verify. */
  for (uint32_t i = 0; i < 32; i++) {
    if (stage[i] != 0) {
      if (reason) *reason = NV_RPC_CHECKSUM;
      return -EIO;
    }
  }

  /* R7: a signature check, so a reply that is a copy of our own request cannot
   * be mistaken for one, and a length that would run past the slots we own. */
  if (ld32(stage + NV_RPC_R_SIG) != NV_RPC_SIG) {
    if (reason) *reason = NV_RPC_BAD_SIG;
    return -EIO;
  }
  uint32_t length = ld32(stage + NV_RPC_R_LENGTH);
  if (length < NV_RPC_RPC_HDR || length > ec * NV_RPC_SLOT - NV_RPC_ELEM_HDR) {
    if (reason) *reason = NV_RPC_BAD_LEN;
    return -EIO;
  }
  return 0;
}

/* ======================================================================== */
/* split plan (spec 4.5)                                                     */
/* ======================================================================== */

int nv_rpc_split_plan(uint32_t payload, uint32_t *chunks, uint32_t max_chunks) {
  if (!chunks || max_chunks == 0) return -EINVAL;

  uint32_t n = 0, rem = payload;
  do {
    if (n >= max_chunks) return -EMSGSIZE;
    uint32_t c = rem > NV_RPC_SPLIT_CHUNK ? NV_RPC_SPLIT_CHUNK : rem;
    chunks[n++] = c;
    rem -= c;
    /* Each chunk must frame: 32 + chunk <= 65488, i.e. one message, 16 slots. */
    if (NV_RPC_RPC_HDR + c > NV_RPC_LEN_MAX) return -EMSGSIZE;
  } while (rem && n < max_chunks); /* the empty payload is one message */

  if (rem != 0) return -EMSGSIZE;

  uint64_t sum = 0;
  for (uint32_t i = 0; i < n; i++) sum += chunks[i];
  if (sum != payload) return -EINVAL; /* sum(chunks) == payload */
  return (int)n;
}

/* ======================================================================== */
/* send (spec 4.3)                                                           */
/* ======================================================================== */

int nv_rpc_free_slots(struct nv_rpc_queue *q, enum nv_rpc_reason *reason) {
  if (reason) *reason = NV_RPC_OK;
  if (!q || !q->block) return -EINVAL;

  /* Before the link, msgqTxCreate's pre-boot allowance: the whole ring minus
   * the slot the producer must leave empty. */
  if (!q->linked) return (int)(q->msg_count - 1u);

  /* S2: the firmware's read index into OUR command queue.  With SWAP_RX that
   * word lives in the STATUS block (0x4120), not next to our write pointer. */
  uint32_t pr = ld32(q->block + q->peer_readptr_off);
  uint32_t wp = ld32(q->block + q->own_writeptr_off);
  if (pr >= NV_RPC_MSG_COUNT || wp >= NV_RPC_MSG_COUNT) {
    /* A pointer outside the ring means the queue is already in an unknown
     * state; do not compound it by computing free from nonsense. */
    if (reason) *reason = NV_RPC_PEER_GEOM;
    return -EINVAL;
  }
  q->write_ptr = wp; /* mirror only; memory above is authoritative */
  q->read_ptr = pr;

  /* S3: the -1 is the slot the producer must always leave empty.  Without it a
   * full ring looks empty and the two sides desynchronise permanently. */
  return (int)((pr + NV_RPC_MSG_COUNT - wp - 1u) % NV_RPC_MSG_COUNT);
}

int nv_rpc_tx_prepare(struct nv_rpc_queue *q, uint32_t rpc_len, uint32_t *ec_out,
                      enum nv_rpc_reason *reason) {
  if (reason) *reason = NV_RPC_OK;
  if (!q || !q->block) return -EINVAL;

  /* P1, re-asserted before every send: a block the GPU cannot walk is the hang
   * with no error. */
  if (q->bus & 0xFFFu) return -EINVAL;
  if (ld64(q->block) != q->bus) {
    if (reason) *reason = NV_RPC_PEER_GEOM;
    return -EINVAL;
  }

  /* P4: one outstanding request, because opcode matching cannot tell two
   * in-flight requests with the same opcode apart. */
  if (q->tx_busy) {
    if (reason) *reason = NV_RPC_BUSY;
    return -EBUSY;
  }

  uint32_t ec;
  int rc = nv_rpc_elem_count(rpc_len, &ec);
  if (rc == -EMSGSIZE) {
    if (reason) *reason = NV_RPC_TOO_BIG;
    return rc;
  }
  if (rc != 0) {
    if (reason) *reason = NV_RPC_BAD_LEN;
    return rc;
  }

  int free_slots = nv_rpc_free_slots(q, reason);
  if (free_slots < 0) return free_slots;
  if ((int)ec > free_slots) {
    if (reason) *reason = NV_RPC_NO_SPACE;
    return -ENOSPC;
  }

  q->tx_ec = ec;
  if (ec_out) *ec_out = ec;
  return 0;
}

int nv_rpc_tx_commit(struct nv_rpc_queue *q, const uint8_t *stage,
                     const struct nv_rpc_hw *hw) {
  if (!q || !q->block || !stage) return -EINVAL;
  if (q->tx_ec < 1u || q->tx_ec > NV_RPC_ELEM_MAX) return -EINVAL;

  uint32_t rpc_len = ld32(stage + NV_RPC_R_LENGTH);
  uint32_t ec = 0;
  if (nv_rpc_elem_count(rpc_len, &ec) != 0 || ec != q->tx_ec) return -EINVAL;

  uint8_t *cmdq = q->block + q->cmd_off;
  uint32_t entry_off = ld32(cmdq + NV_RPC_Q_ENTRY_OFF);
  if (entry_off < NV_RPC_SLOT || (entry_off & 0xFFFu)) return -EINVAL;

  uint32_t wp = ld32(cmdq + NV_RPC_Q_WRITE_PTR);
  if (wp >= NV_RPC_MSG_COUNT) return -EINVAL;

  uint32_t total = NV_RPC_ELEM_HDR + rpc_len;
  for (uint32_t k = 0; k < ec; k++) {
    uint32_t idx = (wp + k) % NV_RPC_MSG_COUNT; /* wrap inside the ring */
    uint8_t *dst = cmdq + entry_off + (uint64_t)idx * NV_RPC_SLOT;
    /* S4: zero the WHOLE slot, then copy what the message needs.  Stale bytes
     * in the tail of a reused slot are a checksum failure at the receiver, and
     * the message is dropped without a diagnostic. */
    memset(dst, 0, NV_RPC_SLOT);
    uint32_t off = k * NV_RPC_SLOT;
    if (off >= total) return -EINVAL;
    uint32_t n = total - off;
    if (n > NV_RPC_SLOT) n = NV_RPC_SLOT;
    memcpy(dst, stage + off, n);
  }

  /* S7: the header store must not be reordered after the writePtr store, or
   * the firmware sees an advanced pointer over an unfinished slot. */
  fence(hw);

  /* S8: one store advances by elemCount.  There is no per-slot publication, so
   * a consumer can never observe a half-advanced message. */
  uint32_t new_wp = (wp + ec) % NV_RPC_MSG_COUNT;
  if (new_wp >= NV_RPC_MSG_COUNT) return -EINVAL;
  st32(cmdq + NV_RPC_Q_WRITE_PTR, new_wp);
  q->write_ptr = new_wp;
  return 0;
}

int nv_rpc_kick(struct nv_rpc_queue *q, const struct nv_rpc_hw *hw) {
  /* A kick with no seam is not "nothing to do": it is a send that will never be
   * seen.  Fail loudly rather than return success (prefill rule). */
  if (!hw || !hw->doorbell) return -EINVAL;
  /* S9: store-store ordering against the MMIO doorbell.  S10: one write, value
   * 0.  The word reads back 0 in every state, so a readback is worse than no
   * check -- it looks like a status and is not. */
  fence(hw);
  hw->doorbell(hw->ctx, NV_RPC_DOORBELL_QUEUE);
  /* S11: the sequence number is consumed only by a message that was actually
   * notified.  A number burned on a send that never reached the ring makes
   * every later element rejected (contract row 12). */
  if (q) q->tx_seq++;
  return 0;
}

/* ======================================================================== */
/* receive (spec 4.4)                                                        */
/* ======================================================================== */

static void dispatch_note(struct nv_rpc_queue *q, uint32_t fn) {
  /* Keep the last eight (op, seq-independent) functions dispatched.  This is
   * the transport's only observable in the unit tier; on the card the same fact
   * goes into the one [NV-RPC] row per stage. */
  if (q->dispatch_log_n >= 8) {
    for (uint32_t i = 1; i < 8; i++) q->dispatch_log[i - 1] = q->dispatch_log[i];
    q->dispatch_log[7] = fn;
  } else {
    q->dispatch_log[q->dispatch_log_n++] = fn;
  }
}

static int rx_advance(struct nv_rpc_queue *q, uint32_t rp, uint32_t ec,
                      int bump_seq) {
  uint32_t nrp = (rp + ec) % NV_RPC_MSG_COUNT;
  if (nrp >= NV_RPC_MSG_COUNT) return -EINVAL; /* < 63, always */
  /* R8: published only AFTER the copy and the verification.  Publishing it
   * first would let the producer overwrite the slots while they are read. */
  st32(q->block + q->own_readptr_off, nrp);
  q->read_ptr = nrp;
  if (bump_seq) q->rx_seq++;
  return 0;
}

int nv_rpc_rx_take(struct nv_rpc_queue *q, const struct nv_rpc_hw *hw, void *out,
                   uint32_t cap, enum nv_rpc_reason *reason) {
  enum nv_rpc_reason last = NV_RPC_NOTHING;
  if (reason) *reason = NV_RPC_OK;
  if (!q || !q->block || !q->rx_stage) return -EINVAL;

  q->rx_retries = 0;
  q->rx_ec = 0;

  /* Bounded by the ring: a message is at most 16 slots, so at most 63
   * dispatches can be consumed in one call before the queue is empty. */
  for (uint32_t guard = 0; guard < NV_RPC_MSG_COUNT; guard++) {
    uint8_t *statq = q->block + q->stat_off;

    /* R1: the firmware's write index into the status queue. */
    uint32_t pw = ld32(statq + NV_RPC_Q_WRITE_PTR);
    if (pw >= NV_RPC_MSG_COUNT) {
      if (reason) *reason = NV_RPC_PEER_GEOM;
      return -EINVAL;
    }
    uint32_t rp = ld32(q->block + q->own_readptr_off);
    if (rp >= NV_RPC_MSG_COUNT) {
      if (reason) *reason = NV_RPC_PEER_GEOM;
      return -EINVAL;
    }
    q->read_ptr = rp;

    /* R2: an empty queue is not an error; a quiet boot must stay quiet. */
    uint32_t avail = (pw + NV_RPC_MSG_COUNT - rp) % NV_RPC_MSG_COUNT;
    if (avail == 0) {
      if (reason) *reason = last;
      return -EAGAIN;
    }

    uint8_t *slot = 0;
    uint32_t entry_off = ld32(statq + NV_RPC_Q_ENTRY_OFF);
    if (entry_off < NV_RPC_SLOT || (entry_off & 0xFFFu)) {
      /* Our own header, written by us; if it is wrong the queue is unusable. */
      if (reason) *reason = NV_RPC_PEER_GEOM;
      return -EINVAL;
    }
    slot = statq + entry_off + (uint64_t)rp * NV_RPC_SLOT;

    /* R3: elemCount, and the producer must have published the whole message.
     * avail < elemCount means writePtr advanced in more than one step: a
     * producer bug, not a race. */
    uint32_t ec = ld32(slot + NV_RPC_E_ELEMCOUNT);
    if (ec == 0) {
      if (reason) *reason = NV_RPC_BAD_EC;
      return -EINVAL;
    }
    if (ec > NV_RPC_ELEM_MAX) {
      if (reason) *reason = NV_RPC_BIG_ELEM;
      return -EINVAL;
    }
    if (avail < ec) {
      if (reason) *reason = NV_RPC_SHORT_PUBLISH;
      return -EAGAIN;
    }

    /* R4: reassemble the wrap into contiguous scratch.  A message that
     * straddles the end of the ring read as if it were contiguous is the bug
     * this copy exists to prevent. */
    for (uint32_t k = 0; k < ec; k++) {
      uint32_t idx = (rp + k) % NV_RPC_MSG_COUNT;
      const uint8_t *src = statq + entry_off + (uint64_t)idx * NV_RPC_SLOT;
      memcpy(q->rx_stage + (uint64_t)k * NV_RPC_SLOT, src, NV_RPC_SLOT);
    }

    /* R5: whole-slot fold.  The first read can beat the data, so retry up to
     * three times with a fence -- and never advance the read pointer on
     * failure. */
    uint32_t bytes = ec * NV_RPC_SLOT;
    int ok = (nv_rpc_cksum(q->rx_stage, bytes) == 0);
    if (!ok) {
      for (uint32_t attempt = 0; attempt < 3 && !ok; attempt++) {
        fence(hw);
        q->rx_retries++;
        for (uint32_t k = 0; k < ec; k++) {
          uint32_t idx = (rp + k) % NV_RPC_MSG_COUNT;
          const uint8_t *src = statq + entry_off + (uint64_t)idx * NV_RPC_SLOT;
          memcpy(q->rx_stage + (uint64_t)k * NV_RPC_SLOT, src, NV_RPC_SLOT);
        }
        ok = (nv_rpc_cksum(q->rx_stage, bytes) == 0);
      }
    }
    if (!ok) {
      q->rx_cksum_fails++;
      if (reason) *reason = NV_RPC_CHECKSUM;
      return -EIO;
    }

    enum nv_rpc_reason why = NV_RPC_OK;
    if (nv_rpc_elem_check(q->rx_stage, ec, &why) != 0) {
      if (reason) *reason = why;
      return -EIO;
    }

    /* R6: the sequence half.  A stale element is consumed and dropped (not an
     * error); a gap is refused, because the queue has lost a message we can
     * never reconstruct. */
    uint32_t seq = ld32(q->rx_stage + NV_RPC_E_SEQ);
    if (seq > q->rx_seq) {
      if (reason) *reason = NV_RPC_SEQ_GAP;
      return -EIO;
    }
    if (seq < q->rx_seq) {
      q->rx_stale++;
      last = NV_RPC_SEQ_STALE;
      if (rx_advance(q, rp, ec, 0) != 0) return -EINVAL;
      continue;
    }

    /* R10: dispatch on the function code.  A host that treats "the next
     * message" as "my reply" parses an event as a control reply and reports
     * success on the wrong message (contract row 5). */
    uint32_t fn = ld32(q->rx_stage + NV_RPC_R_FUNCTION);
    dispatch_note(q, fn);

    if (fn == q->want_fn) {
      uint32_t length = ld32(q->rx_stage + NV_RPC_R_LENGTH);
      if (!out || cap < length) {
        if (reason) *reason = NV_RPC_TOO_BIG;
        return -EMSGSIZE;
      }
      memcpy(out, q->rx_stage + NV_RPC_ELEM_HDR, length);
      q->rx_ec = ec; /* the caller must release; R8 comes after the copy */
      if (reason) *reason = NV_RPC_OK;
      return (int)length;
    }
    if (fn >= NV_RPC_EVENT_BASE) {
      q->rx_events++;
      last = NV_RPC_EVENT;
      if (rx_advance(q, rp, ec, 1) != 0) return -EINVAL;
      continue;
    }
    if (fn == NV_RPC_FN_CONTINUATION) {
      q->rx_continuations++;
      last = NV_RPC_CONTINUATION;
      if (rx_advance(q, rp, ec, 1) != 0) return -EINVAL;
      continue;
    }
    q->rx_unknown++;
    last = NV_RPC_UNKNOWN_FN;
    if (rx_advance(q, rp, ec, 1) != 0) return -EINVAL;
  }

  if (reason) *reason = last;
  return -EAGAIN;
}

int nv_rpc_rx_release(struct nv_rpc_queue *q) {
  if (!q || !q->block) return -EINVAL;
  if (q->rx_ec == 0) return -EINVAL; /* nothing was taken */

  uint32_t rp = ld32(q->block + q->own_readptr_off);
  if (rp >= NV_RPC_MSG_COUNT) return -EINVAL;
  uint32_t ec = q->rx_ec;
  q->rx_ec = 0;
  /* R9: the sequence advances only once the element was verified. */
  return rx_advance(q, rp, ec, 1);
}

/* ======================================================================== */
/* the peer's header, read at call time (spec 4.2, 6.2 #2)                   */
/* ======================================================================== */

void nv_rpc_peer_snapshot(const struct nv_rpc_queue *q,
                          struct nv_rpc_peer_statq *s) {
  if (!s) return;
  for (uint32_t i = 0; i < 8; i++) s->w[i] = 0;
  s->nonzero = 0;
  if (!q || !q->block) return;
  /* The eight words are exactly what nv_rpc_link_validate() reads, in the
   * order it reads them, so a row built from this snapshot explains that
   * function's -EAGAIN without a second interpretation of the block. */
  for (uint32_t i = 0; i < 8; i++) {
    s->w[i] = ld32(q->block + q->stat_off + i * 4u);
    if (s->w[i] != 0) s->nonzero++;
  }
}

/* ======================================================================== */
/* the first round trip (contract 4.1 steps 9-10)                            */
/* ======================================================================== */

int nv_rpc_static_info_request(uint8_t *stage, uint32_t seq) {
  if (!stage) return -EINVAL;
  const uint32_t len = NV_RPC_STATIC_INFO_LEN;
  /* The payload is zeroed because the request is a size declaration, not data:
   * the contract's rpcGetGspStaticInfo_v14_00 sends sizeof(GspStaticConfigInfo)
   * zero bytes to tell firmware how much it may send back. */
  memset(stage, 0, NV_RPC_ELEM_HDR + len);
  int rc = nv_rpc_req_init(stage, NV_RPC_FN_GET_STATIC_INFO, len);
  if (rc != 0) return rc;
  /* The RPC's own sequence stays 0 (no source shows the firmware echoing it);
   * the ELEMENT sequence is what the receiver orders messages by. */
  return nv_rpc_elem_build(stage, len, seq);
}

int nv_rpc_rx_wait(struct nv_rpc_queue *q, const struct nv_rpc_hw *hw,
                   uint32_t fn, uint32_t timeout_us, void *out, uint32_t cap,
                   uint32_t *result, enum nv_rpc_reason *reason) {
  if (reason) *reason = NV_RPC_OK;
  if (!q || !q->block) return -EINVAL;

  int have_clock = (hw && hw->now_us);
  uint64_t start = have_clock ? hw->now_us(hw->ctx) : 0;
  /* rx_take() matches on want_fn, so the wait is that function and nothing
   * else.  Saved and restored: a caller's outstanding request is its own
   * business, and this helper must not leave the queue pointed at an event. */
  uint32_t saved = q->want_fn;
  q->want_fn = fn;

  for (;;) {
    enum nv_rpc_reason why = NV_RPC_OK;
    int n = nv_rpc_rx_take(q, hw, out, cap, &why);
    if (n > 0) {
      /* Read the verdict from the RPC stream (out + 0x10), not from a cached
       * field: a reply that is a copy of our own PENDING request would report
       * 0xFFFFFFFF, which is the "no reply" sentinel and not success. */
      if (result) *result = ld32((const uint8_t *)out + NV_RPC_H_RESULT);
      int rrc = nv_rpc_rx_release(q);
      q->want_fn = saved;
      if (rrc != 0) {
        if (reason) *reason = NV_RPC_PEER_GEOM;
        return rrc;
      }
      if (reason) *reason = NV_RPC_OK;
      return n;
    }
    if (n != -EAGAIN) {
      q->want_fn = saved;
      if (reason) *reason = why;
      return n;
    }
    if (!have_clock) {
      q->want_fn = saved;
      if (reason) *reason = why ? why : NV_RPC_NOTHING;
      return -EAGAIN;
    }
    if (hw->now_us(hw->ctx) - start >= timeout_us) {
      q->want_fn = saved;
      if (reason) *reason = NV_RPC_NO_REPLY;
      return -ETIMEDOUT;
    }
    fence(hw);
  }
}

int nv_rpc_roundtrip(struct nv_rpc_queue *q, const struct nv_rpc_hw *hw,
                     uint8_t *stage, uint32_t reply_fn, uint32_t timeout_us,
                     void *out, uint32_t cap, uint32_t *result,
                     enum nv_rpc_reason *reason) {
  if (reason) *reason = NV_RPC_OK;
  if (!q || !q->block || !hw || !stage) return -EINVAL;

  /* The link gate comes before every store.  This is the difference between a
   * monitored send and a send nobody can see: with no peer header there is no
   * consumer, and a doorbell written into that window is not a delivery. */
  if (!q->linked) {
    if (reason) *reason = NV_RPC_NOT_LINKED;
    return -EAGAIN;
  }

  uint32_t rpc_len = ld32(stage + NV_RPC_R_LENGTH);
  uint32_t ec = 0;
  int rc = nv_rpc_tx_prepare(q, rpc_len, &ec, reason);
  if (rc != 0) return rc;

  q->tx_busy = 1;
  rc = nv_rpc_tx_commit(q, stage, hw);
  if (rc != 0) {
    q->tx_busy = 0;
    return rc;
  }
  rc = nv_rpc_kick(q, hw);
  if (rc != 0) {
    q->tx_busy = 0;
    return rc;
  }

  rc = nv_rpc_rx_wait(q, hw, reply_fn, timeout_us, out, cap, result, reason);
  /* The one outstanding request ends here, answered or timed out.  A timeout is
   * NOT a licence to resend: contract 6 row 2 says the message may have been
   * consumed by a firmware that then died, and a resend turns one failure into
   * two. */
  q->tx_busy = 0;
  return rc;
}

/* ======================================================================== */
/* real seam + the log row (x86_64 only, like the rest of the driver)         */
/* ======================================================================== */

#ifdef __x86_64__

#include "nv.h"
#include "arch/cpu.h"
#include "timer.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

static void rpc_doorbell_dev(void *ctx, uint32_t queue_idx) {
  struct nv_dev *d = ctx;
  /* One queue pair on this card; the offset for queue n > 0 is not
   * established (spec 10), so it is refused rather than guessed. */
  if (queue_idx != NV_RPC_DOORBELL_QUEUE) return;
  nv_wr32(d, NV_RPC_DOORBELL_OFF, 0);
}

static void rpc_fence_dev(void *ctx) {
  (void)ctx;
  arch_memory_barrier();
}

static uint64_t rpc_now_dev(void *ctx) {
  (void)ctx;
  return timer_get_us();
}

void nv_rpc_hw_dev(struct nv_dev *d, struct nv_rpc_hw *hw) {
  if (!hw) return;
  hw->doorbell = rpc_doorbell_dev;
  hw->fence = rpc_fence_dev;
  hw->now_us = rpc_now_dev;
  hw->ctx = d;
}

/* The block is RAM, not BAR0, so NV_IOC_PEEK (which is BAR0-only and
 * whitelisted) cannot reach it.  Without this a stalled transport is
 * unobservable from userland.  One row, naming the stage. */
void nv_rpc_dump(const char *stage, const struct nv_rpc_queue *q) {
  if (!q) return;
  uart_puts("[NV-RPC] stage=");
  uart_puts(stage ? stage : "?");
  uart_puts(" bus=");
  uart_print_hex(q->bus);
  uart_puts(" linked=");
  print_int(q->linked);
  uart_puts(" swapped=");
  print_int(q->swapped);
  uart_puts(" wr=");
  print_int((int)(q->block ? ld32(q->block + q->cmd_off + NV_RPC_Q_WRITE_PTR)
                           : 0));
  uart_puts(" rd=");
  print_int((int)(q->block ? ld32(q->block + q->own_readptr_off) : 0));
  uart_puts(" prd=");
  print_int((int)(q->block ? ld32(q->block + q->peer_readptr_off) : 0));
  uart_puts(" pw=");
  print_int((int)(q->block ? ld32(q->block + q->peer_writeptr_off) : 0));
  /* The two words the acceptance row turns on, named by ADDRESS: `wr` is our
   * producer pointer at block+cmd_off+0x10, `pw` is the peer's at
   * block+stat_off+0x10.  They are different words 0x40000 apart, and a row
   * that printed only their values could not show that. */
  uart_puts(" wr_off=");
  uart_print_hex(q->cmd_off + NV_RPC_Q_WRITE_PTR);
  uart_puts(" pw_off=");
  uart_print_hex(q->stat_off + NV_RPC_Q_WRITE_PTR);
  uart_puts(" txseq=");
  print_int((int)q->tx_seq);
  uart_puts(" rxseq=");
  print_int((int)q->rx_seq);
  uart_puts("\n");

  /* The peer's own words, read NOW (not the cached struct nv_rpc_peer): a row
   * that says "no peer" from our bookkeeping is an inference, the same row
   * from block+stat_off is an observation.  This is the eight-word set
   * nv_rpc_link_validate() starts from, so a nonzero count of 0 is exactly the
   * -EAGAIN case. */
  struct nv_rpc_peer_statq st;
  nv_rpc_peer_snapshot(q, &st);
  uart_puts("[NV-RPC] statq off=");
  uart_print_hex(q->stat_off);
  uart_puts(" nonzero=");
  print_int((int)st.nonzero);
  uart_puts("/8");
  for (uint32_t i = 0; i < 8; i++) {
    uart_puts(" w");
    print_int((int)i);
    uart_puts("=");
    uart_print_hex(st.w[i]);
  }
  uart_puts(st.nonzero == 0 ? " (nothing has written the peer header)\n"
                            : " (the peer header is written)\n");

  if (st.nonzero == 0) {
    uart_puts("[NV-RPC] peer=none (read from block+");
    uart_print_hex(q->stat_off);
    uart_puts(", not from the cached peer struct)\n");
    return;
  }
  /* All five fields verbatim: a wrong `size` means the RMARGS offsets were
   * wrong, a never-valid header means the firmware never found the block, and
   * those are different bugs. */
  uart_puts("[NV-RPC] peer version=");
  print_int((int)st.w[0]);
  uart_puts(" size=");
  uart_print_hex(st.w[1]);
  uart_puts(" msgSize=");
  uart_print_hex(st.w[2]);
  uart_puts(" msgCount=");
  print_int((int)st.w[3]);
  uart_puts(" entryOff=");
  uart_print_hex(st.w[7]);
  uart_puts(" rxHdrOff=");
  uart_print_hex(st.w[6]);
  uart_puts(" flags=");
  print_int((int)st.w[5]);
  uart_puts(" (the peer's words; linked=");
  print_int(q->linked);
  uart_puts(" is our verdict on them)\n");
}

#else /* !__x86_64__ */

/* The transport itself is arch-independent, but there is no nv driver on
 * aarch64 (no PCIe, no BAR window), so the seam and the log row do not exist
 * there.  Keep nv_rpc.c free of any x86-only reference: this branch is what the
 * ARM compile check compiles. */

#endif /* __x86_64__ */
