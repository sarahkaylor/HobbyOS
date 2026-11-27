# The GSP-RPC transport: an implementation spec you can code and test offline

Target: **RTX 4090, chipset `0x192` (AD102), GSP firmware 535.113.01**.
Scope: the shared-memory transport between the host and GSP-RM. It starts where
`docs/nv-gsp-rpc-contract.md` stops being a contract and becomes bytes: the exact
field layout, the exact order of stores, the exact asserts, and the split between
what can be tested in the kernel unit tier with synthetic queues and what needs
the card.

This document does **not** describe how GSP gets started. FWSEC, the SEC2 booter
and the WPR are `docs/nv-gsp-wpr-contract.md`; this lane assumes a running
RISC-V core and stops at "first round trip".

Method: every wire fact below was re-derived from the public sources named in
§12 and cross-checked between them. Where the two reference implementations
disagree, the disagreement is called out. **No code was copied**: what follows is
a table-and-prose description written from an understanding of the format, and
the checksum vectors in §7 were computed from the rule, not lifted from a
driver.

---

## 0. Provenance tags, and three corrections to the contract doc

| tag | meaning |
|---|---|
| **SOURCE** | stated outright in a public source; cited by file + function |
| **INFERRED** | my reading, consistent with ≥2 sources, not stated as a rule |
| **DECISION** | a choice this spec makes. Not forced by any source; both reference implementations are compatible with it. Must be implemented as written so the tests mean something |
| **NOT ESTABLISHED** | the sources do not settle it. No guess; the impact is stated |

Three things in `docs/nv-gsp-rpc-contract.md` are wrong or ambiguous in a way
that would produce a passing unit test and a dead queue. Read these first.

### 0.1 The checksum covers **whole slots**, not `48 + rpc.length`

The contract (§2.2 step 3) says the hash runs "through the 8-byte boundary at or
after `48 + rpc.length`". Both implementations hash the **whole element**:

- `NVIDIA/mqueue_cpu.c` `GspMsgQueueSendCommand` computes it over
  `elemCount * 4096` bytes of its staging copy; `GspMsgQueueReceiveStatus`
  verifies over `nElements * 4096`.
- `nvkm/rpc.c` `r535_gsp_cmdq_push` sets its hash length to
  `ALIGN(48 + rpc_len, 4096)`.

These coincide *numerically* only because the tail of every slot is zero (XOR
with zero is the identity). They do **not** coincide operationally: a driver
that hashes the short range will accept its own element while the firmware's
whole-slot check fails, and the element is dropped before it is parsed
(contract §6 row 9). The rule that must be implemented is §4.3 steps S4 and S6:

> zero the whole element region first, then hash `elemCount * 4096` bytes.

This also retires the contract's "the padding inside the slot after the message
is not hashed, so a partially written slot can still checksum correctly". Under
the real rule a stale byte anywhere in the last slot changes the checksum. That
is a feature: it catches a slot that was reused without being cleared.

### 0.2 Two different splits, not one

The contract's §2.3 puts "element 2..N is a raw byte continuation with no header"
and "messages larger than 16 slots are split with `CONTINUATION_RECORD` (71)" in
the same table, which reads as one mechanism. They are two:

| level | when | what is in slot 0 | what is in slots 1..N−1 |
|---|---|---|---|
| **multi-slot message** | one RPC needs 2..16 slots (`48+length > 4096`) | element header + RPC header + first 4048 RPC bytes | **raw continuation bytes of the same RPC**: no element header, no RPC header, `seqNum` present only in slot 0 |
| **continuation messages** | one logical operation needs more than 16 slots (RPC `length > 65488`) | element header + RPC header (real opcode) + chunk | **a separate message**: its own element header, its own `seqNum`, its own RPC header with `function = 71`, its own checksum, its own `elemCount` |

So `seqNum` increments **per message**, which is per RPC-framed unit — one for a
6-slot message, and one *per continuation message* for a big operation.
SOURCE: `nvkm/rpc.c` `r535_gsp_rpc_push` (it calls the same `rpc_get()` for the
continuation, and `rpc_get()` fills in a full RPC header).

### 0.3 The contract's §7 step-3 figure is off by 44

"slot 2 starting at payload offset 8188" — slot 2 begins at stream offset 8192,
which is RPC offset **8144** and payload offset **8112**. Test case T-11 in §7
uses the arithmetic, not the prose.

---

## 1. The block

One allocation, 129 pages, 4 KiB aligned, physically contiguous (DECISION: we do
not need the non-contiguous case; see §6.3).

```
block + 0x00000   page table      129 × u64 PTEs      (1 page)
block + 0x01000   command queue   0x40000             (63 slots + 1 header page)
block + 0x41000   status queue    0x40000             (63 slots + 1 header page)
                  total           0x81000 = 129 × 0x1000
```

| field | value | tag |
|---|---|---|
| PTE width | 8 bytes, little-endian, GPU-visible page address | SOURCE — `NVIDIA/mqueue_priv.h` `RmPhysAddr *pPageTbl`; `nvkm/gsp.c` `gsp->shm.ptes.ptr[i] = mem.addr + (i << 12)` |
| PTE[i] | `block_bus + i*0x1000`, for i = 0..128 | INFERRED from the self-covering arithmetic; both implementations behave this way |
| `block_bus` handed to firmware | **PTE[0]** (the address of the page that holds the PTEs) | SOURCE — `NVIDIA/mqueue_cpu.c` `GspMsgQueuesInit`: `sharedMemPA = pPageTbl[0]` |
| queue offsets published | cmdq `0x1000`, statq `0x41000` | SOURCE — `nvkm/gsp.c` `r535_gsp_set_rmargs` computes them as pointer differences |

**Consequence that must be written into the code, not assumed:** the firmware
walks `PTE[]`, so `PTE[i]` must be the address the **GPU** uses, which is not
necessarily the CPU physical address. Today HobbyOS has no IOMMU *translation*
in the GPU's DMA path (§6.1: VT-d is initialised, its translation bit is never
set), so they are equal — but that equality is a property of the current state,
not of the format, and it must live behind one function
(`nv_dma.bus`), never be spelled `phys` at a call site.

---

## 2. The four tables

All multi-byte fields are **little-endian** (`SOURCE`: x86-64 / RISC-V LE on both
sides; neither implementation byte-swaps). No struct in this format has a
bitfield except `header_version`, and none is packed beyond what its own
alignment requires — but **DECISION: do not overlay a C struct on these bytes.**
Read and write through explicit offset accessors. The reasons: the element
header's RPC member is 8-aligned by declaration (`NV_DECLARE_ALIGNED(..., 8)`)
and a compiler may pad differently than the next compiler; the block is built
inside a DMA buffer whose layout must be a property of this document, not of the
toolchain; and offset accessors are what let the pure half run against a
synthetic buffer in the unit tier.

### 2.1 Queue header page (per queue, 4096 bytes; 36 used)

Base = `block + 0x1000` for the command queue, `block + 0x41000` for the status
queue. Offsets below are **relative to the queue base**.

| off | size | type | field | written by | value we publish | tag |
|---|---|---|---|---|---|---|
| `0x00` | 4 | u32 | `version` | producer | `0` | SOURCE — `MSGQ_VERSION` = 0 |
| `0x04` | 4 | u32 | `size` | producer | `0x40000` (queue region bytes) | SOURCE — `msgqTxCreate`: `tx.size = size` |
| `0x08` | 4 | u32 | `msgSize` | producer | `0x1000` (slot size; power of two; ≥ 16) | SOURCE |
| `0x0C` | 4 | u32 | `msgCount` | producer | `(size - entryOff) / msgSize` = **63** | SOURCE |
| `0x10` | 4 | u32 | `writePtr` | producer | slot index of the next free slot; starts 0; stored **mod 63** | SOURCE |
| `0x14` | 4 | u32 | `flags` | producer | `1` (bit 0 = `SWAP_RX`) | SOURCE — `MSGQ_FLAGS_SWAP_RX` |
| `0x18` | 4 | u32 | `rxHdrOff` | producer | `0x20` — offset of this queue's read-pointer word, i.e. where **this queue's consumer** writes it (§2.5) | SOURCE — `msgqTxCreate`: `align_up(sizeof(tx hdr)=32, 16)` |
| `0x1C` | 4 | u32 | `entryOff` | producer | `0x1000` | SOURCE — `align_up(rxHdrOff + 4, 4096)` |
| `0x20` | 4 | u32 | `readPtr` | **consumer** | 0 at start; see §2.5 for *which* block it lives in | SOURCE — `msgqRxHeader { u32 readPtr; }` |
| `0x24` | 0xFDC | — | unused | — | zero | INFERRED — the header page is 4 KiB, 36 bytes are defined |

`flags` bit definitions:

| bit | name | meaning | tag |
|---|---|---|---|
| 0 | `SWAP_RX` | "I want the read pointers swapped" — see §2.5 | SOURCE |
| 1..31 | — | reserved. **DECISION: publish 0, and reject a peer whose reserved bits are nonzero** (an unknown flag is an unknown protocol) | INFERRED |

`version` is what `msgqRxLink` checks first and refuses on (`-9` in
`n_msgq.c`), so a nonzero `version` in the peer's header is a hard link failure.

### 2.2 Slot i

`slot(i) = queue_base + entryOff + i*msgSize = queue_base + 0x1000 + i*0x1000`,
`i ∈ [0, 63)`. Capacity is **63 slots, 62 usable** — a full ring is
indistinguishable from an empty one, so one slot is always left empty
(`SOURCE`: `msgqTxCreate` sets `txFree = msgCount - 1`).

### 2.3 Element header (48 bytes at slot offset 0)

| off | size | type | field | value | tag |
|---|---|---|---|---|---|
| `0x00` | 16 | u8[16] | `authTagBuffer` | all zero without confidential compute | SOURCE — `GSP_MSG_QUEUE_ELEMENT` |
| `0x10` | 16 | u8[16] | `aadBuffer` | all zero without CC; with CC it carries `seqNum` | SOURCE |
| `0x20` | 4 | u32 | `checkSum` | chosen so a whole-slot hash over the element, *including this word*, folds to 0 | SOURCE |
| `0x24` | 4 | u32 | `seqNum` | per-queue, per-**message**, starts 0, +1 per message, natural u32 wrap | SOURCE |
| `0x28` | 4 | u32 | `elemCount` | number of 4096-byte slots this message occupies | SOURCE |
| `0x2C` | 4 | u32 | `pad` (declaration padding) | `0`; **inside the hashed range** | SOURCE — `nvkm/rpc.c` sets `msg->pad = 0` before hashing |
| `0x30` | — | — | RPC message (§2.4) | — | SOURCE — `GSP_MSG_QUEUE_ELEMENT_HDR_SIZE == offsetof(..., rpc) == 48` |

Without CC the 32 bytes of tag+AAD are dead weight and are never interpreted —
there is no other authentication on the transport. **DECISION:** zero them on
every element and assert they are zero before the checksum, so that a later CC
path cannot be silently half-implemented.

### 2.4 RPC header (32 bytes at element offset `0x30`)

Offsets are **relative to the start of the RPC**, i.e. `slot+0x30`.

| off | size | type | field | request value | reply value | tag |
|---|---|---|---|---|---|---|
| `0x00` | 4 | u32 | `header_version` | `0x03000000` | `0x03000000` | SOURCE — `NV_VGPU_MSG_HEADER_VERSION_MAJOR 31:24`, `MINOR 23:16`, majors 3/0 |
| `0x04` | 4 | u32 | `signature` | `0x43505256` (`"VRPC"` in memory order) | same | SOURCE — `NV_VGPU_MSG_SIGNATURE_VALID` |
| `0x08` | 4 | u32 | `length` | **total** bytes of RPC header + payload | same rule | SOURCE — `NV_VGPU_MSG_HEADER` doc + `nvkm/rpc.c` |
| `0x0C` | 4 | u32 | `function` | opcode (§2.6) | echoed opcode, or an event id `≥ 0x1000` | SOURCE |
| `0x10` | 4 | u32 | `rpc_result` | `0xFFFFFFFF` (pending sentinel) | `0` = success, else an `NV_STATUS` | SOURCE — `NV_VGPU_MSG_RESULT_RPC_PENDING` |
| `0x14` | 4 | u32 | `rpc_result_private` | `0xFFFFFFFF` | secondary status | SOURCE |
| `0x18` | 4 | u32 | `sequence` | host counter, or 0 for "no sequence" | echo — **NOT ESTABLISHED** | SOURCE for the field; echo unproven |
| `0x1C` | 4 | u32 | `spare` / `cpuRmGfid` (union) | `0` | `0` on bare metal | SOURCE — `rpc_message_rpc_union_field_v` |
| `0x20` | — | — | payload | 8-byte aligned; size classes: none | — | SOURCE / INFERRED |

`header_version` bit layout, exactly:

| bits | field | value |
|---|---|---|
| 31:24 | major | `3` |
| 23:16 | minor | `0` |
| 15:0 | unused | `0` |

`rpc_result` is a **32-bit NV_STATUS**, not a signed value. Compare against 0 and
against `0xFFFFFFFF` as unsigned. Do not "improve" it into an `int`.

### 2.5 Who writes which read pointer (the `SWAP_RX` rule)

Both sides publish `flags = 1`, and the layout is the **AND** of the two:
`rxSwapped = (our.flags & SWAP_RX) && (peer.flags & SWAP_RX)`
(`SOURCE`: `msgqRxLink`, `msgqTxCreate`). Since our flags are 1, the negotiated
value is exactly the peer's bit 0 — but implement the AND, not the shortcut,
because the shortcut is what breaks if we ever publish 0.

The read pointer at `queue+0x20` belongs to the **consumer** of that queue, and
`SWAP_RX` decides *which block it is stored in*:

| negotiated | word we **store** (our status read index) | word we **load** (firmware's cmdq read index) | tag |
|---|---|---|---|
| **swapped (1)** — expected | `cmdq + 0x20` | `statq + 0x20` | SOURCE — `nvkm/gsp.c` `r535_gsp_shared_init`: `cmdq.rptr = &msgq->rx.readPtr`, `msgq.rptr = &cmdq->rx.readPtr` |
| **not swapped (0)** | `statq + 0x20` (the block the queue belongs to) | `cmdq + 0x20` | SOURCE — `msgqTxCreate`/`msgqRxLink` non-swapped branch |

Read that table twice. With `SWAP_RX` set — the case on this firmware — the words
**cross**: we publish our status read index into the *command* block, and the
firmware publishes its command read index into the *status* block. That is why
the status queue can be mapped read-only to the host, and it is why "poll
`statq+0x20` for progress" is correct only in the swapped case.

The four offsets the send/receive paths need, as one table (offsets relative to
the block base):

| accessor | swapped | not swapped |
|---|---|---|
| `own_readptr_off` (we store) | `0x1020` (cmdq + 0x20) | `0x4120` (statq + 0x20) |
| `peer_readptr_off` (we load) | `0x4120` (statq + 0x20) | `0x1020` (cmdq + 0x20) |
| `peer_writeptr_off` (we load) | `0x4110` (statq + 0x10) | `0x4110` |
| `own_writeptr_off` (we store) | `0x1010` (cmdq + 0x10) | `0x1010` |

`own_readptr_off == 0x1020` when swapped is the single most common transcription
error in this protocol: it reads like a typo and is the whole point. Test T-08 in
§7 pins it in both polarities.

### 2.6 Constants

| name | value | tag |
|---|---|---|
| `NV_RPC_SLOT` | `0x1000` | SOURCE |
| `NV_RPC_ELEM_HDR` | `0x30` (48) | SOURCE |
| `NV_RPC_RPC_HDR` | `0x20` (32) | SOURCE |
| `NV_RPC_MSG_COUNT` | `63` | SOURCE |
| `NV_RPC_USABLE` | `62` | SOURCE |
| `NV_RPC_ELEM_MAX` | `16` slots | SOURCE — `GSP_MSG_QUEUE_ELEMENT_SIZE_MAX` |
| `NV_RPC_LEN_MAX` | `65488` = `0x10000 − 0x30` | SOURCE — `GSP_MSG_QUEUE_RPC_SIZE_MAX` |
| `NV_RPC_PAYLOAD_MAX` | `65456` | INFERRED (subtract 32) |
| `NV_RPC_SIG` | `0x43505256` | SOURCE |
| `NV_RPC_VERSION` | `0x03000000` | SOURCE |
| `NV_RPC_PENDING` | `0xFFFFFFFF` | SOURCE |
| `NV_RPC_FN_CONTINUATION` | `71` (`0x47`) | SOURCE — `nvkm/rpcfn.h` |
| doorbell | BAR0 `0x110C00`, write `0`, queue 0 | SOURCE — `NV_PGSP_QUEUE_HEAD(0)`; `NV_GSP_QUEUE_HEAD0` in `nv_gsp.c` |

Opcodes this lane needs: `65` `GET_GSP_STATIC_INFO` (first round trip),
`72` `GSP_SET_SYSTEM_INFO` and `73` `SET_REGISTRY` (pre-boot, fire-and-forget),
`76` `GSP_RM_CONTROL` later, `0x1001` `GSP_INIT_DONE` (event).

---

## 3. Building the block (pure; no hardware)

Ordered, because two of the steps are load-bearing:

1. **Zero all 129 pages.** `frame_alloc_contig()` does not zero, and
   `frame_alloc_zeroed()` is single-frame. Zeroing is not tidiness: the checksum
   covers whole slots, and a zeroed status header is what makes "not linked yet"
   distinguishable from "linked" (§4.2).
2. **Fill 129 PTEs**: `pte[i] = bus + i*0x1000`. Assert `pte[0] == bus` and
   `pte[128] == bus + 0x80000`.
3. **Publish the command queue's producer header** per §2.1: version 0, size
   `0x40000`, msgSize `0x1000`, msgCount 63, writePtr 0, flags 1, rxHdrOff
   `0x20`, entryOff `0x1000`. `readPtr` at `+0x20` stays 0.
4. **Do not write the status queue's producer header.** The firmware writes it
   when its RM task starts; that write is the first proof of life on the whole
   boot path (contract §4.1 row 7). Pre-filling it would make our own bytes look
   like the firmware's and destroy that evidence. Assert instead that
   validation of the *zeroed* header fails.
5. **Cross-check the two descriptions.** The RMARGS page
   (`docs/nv-gsp-rpc-contract.md` §1.5) and this block describe the same
   geometry twice: `sharedMemPhysAddr == pte[0]`, `pageTableEntryCount == 129`,
   `cmdQueueOffset == 0x1000`, `statQueueOffset == 0x41000`,
   `queueElementHdrSize == 48`, `queueElementSizeMin == 0x1000`,
   `queueElementSizeMax == 0x10000`, `queueHeaderAlign == 4`,
   `queueElementAlign == 12`. Make that a function that takes both structs and
   returns the first field that disagrees by name. It is the cheapest test in
   this document and it catches the class of bug that costs a boot.

Invariants, asserted here and re-asserted in the send path:

```
pte_count        == 129
pt_size          == 0x1000
cmd_off + 0x40000 == stat_off
stat_off + 0x40000 == 0x81000
msgCount         == (0x40000 - 0x1000) / 0x1000 == 63
```

---

## 4. One message, end to end

### 4.1 Preconditions

| # | assert | failure it catches |
|---|---|---|
| P1 | `q->base != NULL`, `q->bus & 0xFFF == 0`, `pte[0] == q->bus` | a block the GPU cannot walk; the hang with no error (contract §6 row 1) |
| P2 | `rpc_len >= 32 && rpc_len <= 65488` | a message that cannot be framed at all |
| P3 | `rpc_len == 32 + declared_payload(rpc_len, opcode)` | a request whose `length` does not describe its payload, or a reply-size declaration that is too small (contract §3.1 third rule) |
| P4 | `q->tx_locked` (one outstanding request), or `q->preboot` | two in-flight requests with the same opcode, which opcode-matching cannot distinguish (contract §3.5) |

### 4.2 Link (the only step that needs firmware)

Not part of the pure half; listed here so the state machine is complete.

1. Poll `statq+0x00..0x1F` for up to 4 s (`timer_get_us`). Validate:
   `version == 0`, `size == 0x40000`, `msgSize == 0x1000`,
   `rxHdrOff >= 0x20`, `entryOff >= rxHdrOff + 4`, `entryOff % 0x1000 == 0`,
   `msgCount == (size - entryOff)/msgSize`. These are `msgqRxLink`'s own
   checks (`-6`..`-10` returns, including its recomputation of `msgCount` from
   `size` and `entryOff`). Log **all five fields verbatim** —
   a wrong `size` means the RMARGS offsets were wrong, a never-valid header
   means the firmware never found the block, and those are different bugs.
2. `swapped = (our_flags & 1) && (peer_flags & 1)`, then store `readPtr = 0` at
   the offset §2.5 dictates.
3. Only now set `q->linked`. Before this, free space is `msgCount - 1` (the
   pre-boot case: `msgqTxCreate` allows filling before the link).

### 4.3 Send algorithm

Numbered; each step's assert is the thing that makes a later failure name
itself. `q` is the queue object; `mb` is `arch_memory_barrier()`.

| # | step | assert | failure it catches |
|---|---|---|---|
| S1 | `ec = ceil((48 + rpc_len) / 4096)` | `1 <= ec <= 16` and `(48+rpc_len) > (ec-1)*4096` | `elemCount` understated (firmware reads one slot, message truncated) or overstated (firmware consumes the next message's slots). Contract §6 row 10. One-line assert, two catastrophic failures |
| S2 | read peer read pointer `pr`; if `pr >= 63` treat as 0 free and fail | `pr < 63` | firmware wrote a pointer outside the ring: the queue is already in an unknown state; do not compound it |
| S3 | `free = (pr + 63 - writePtr - 1) % 63` — the `- 1` is the slot the producer must always leave empty | `ec <= free` | writing into the reserved slot; a *full* ring then looks *empty* and the two sides desynchronise permanently (row 11) |
| S4 | for k in `[0, ec)`: `idx = (writePtr + k) % 63`; `dst = base + entryOff + idx*4096`; **zero 4096 bytes**; copy `min(4096, 48+rpc_len − k*4096)` bytes of the staging stream | `k*4096 < 48+rpc_len` | stale bytes in the tail of a reused slot → checksum fail on the receiver, message dropped without a diagnostic (row 9) |
| S5 | in slot 0: `authTag`/`aad` zero, `checkSum = 0`, `seqNum = q->tx_seq`, `elemCount = ec`, `pad = 0` | tag/aad/pad all zero | a nonzero `pad` word is inside the hash and guarantees rejection |
| S6 | `csum = fold64(XOR of u64 over the ec*4096-byte stream)`; store at slot0 `+0x20` | recompute over the same bytes == 0 **and** `acc_hi == acc_lo` | the checksum rule itself is wrong. This is the last point where the failure is cheap; after the doorbell it is indistinguishable from a dead queue |
| S7 | `mb()` | — | compiler (x86: only the compiler) moving the header store after the `writePtr` store, so the firmware sees an advanced pointer over an unfinished slot |
| S8 | `writePtr = (writePtr + ec) % 63`; **volatile** store u32 at `cmdq+0x10` | `new_writePtr < 63` | a pointer of 63 makes every consumer computation wrap wrong |
| S9 | `mb()` | — | store-store reordering against the MMIO doorbell below |
| S10 | doorbell: `nv_wr32(d, 0x110C00, 0)` | **none** | nothing. The word reads back 0 in every state (MEASURED), so a readback check is worse than no check — it looks like a status and is not |
| S11 | `q->tx_seq++` | only reached if S1..S10 all passed | a sequence number consumed by a message that was never sent: every later element rejected (row 12) |

Notes that are easy to get wrong:

- **XOR is order-independent**, so hashing the ring slots in ring order (across
  the wrap) gives the same value as hashing the reassembled stream. That is what
  makes step S6 implementable in place, and it is worth an assert in the test
  tier rather than a comment.
- The whole-slot copy in S4 means the last slot's tail is zero; the checksum from
  S6 therefore covers exactly what the receiver will hash.
- One store advances `writePtr` by `ec`. There is no per-slot publication, so a
  consumer can never observe a half-advanced message — `avail < elemCount` at
  the receiver is a producer bug, not a race (see T-20).

### 4.4 Receive algorithm

| # | step | assert | failure it catches |
|---|---|---|---|
| R1 | `pw = volatile load statq+0x10` | `pw < 63` else "producer corrupt" | a firmware that wrote a bad pointer; refuse rather than compute nonsense |
| R2 | `avail = (pw + 63 - readPtr) % 63`; if 0 → return "nothing" (not an error) | — | treating an empty queue as an error turns a quiet boot into a spurious failure |
| R3 | `ec = load slot(readPtr)+0x28` | `1 <= ec <= 16` and `avail >= ec` | `avail < ec` means the producer advanced `writePtr` in more than one step, or wrapped inconsistently |
| R4 | copy `ec` slots into a contiguous staging buffer, reassembling the wrap | — | a message that straddles the end of the ring read as if it were contiguous |
| R5 | hash `ec*4096` bytes of the staging buffer | `== 0` | a torn or stale element. **Do not advance the read pointer on failure**; retry up to 3 times with a fence (the vendor does exactly this: the first read can beat the data), then fail `-EIO`/`CHECKSUM` |
| R6 | compare `seqNum` with `q->rx_seq` | equal; if less → stale, consume and log (`SEQ_STALE`); if greater → gap, fail `-EIO`/`SEQ_GAP` | a slot read twice (read pointer not advanced) or a firmware restart that reset its counter (row 12) |
| R7 | `signature == 0x43505256`, `32 <= length <= ec*4096 - 48` | — | a reply that is a copy of our own request, or a length that would run past the slots we own |
| R8 | `readPtr = (readPtr + ec) % 63`; store at the §2.5 offset | `< 63` | not advancing at all: the queue stops accepting replies after 62 and looks like a hang on the *next* call (row 11) |
| R9 | `q->rx_seq++` | — | — |
| R10 | dispatch on `function`: `== wanted` → reply; `>= 0x1000` → event, log and continue; `== 71` → continuation (append to the in-progress assembly); else log and drop | — | a host that treats "next message" as "my reply" parses an event as a control reply and reports success on the wrong message (row 5) |

R8 must come **after** R4/R5: publishing the read pointer before the copy lets
the producer overwrite the slots while they are being read. (`SOURCE`:
`msgqRxMarkConsumed` is called after the copy in `GspMsgQueueReceiveStatus`.)

### 4.5 Continuations

Planner for a logical operation whose RPC would exceed 65488 bytes:

```
chunk[0] = min(payload, 65456)          function = real opcode
chunk[k] = min(remaining, 65456)        function = 71, for k >= 1
each chunk is its own message: own elem header, own seqNum, own checksum,
own length = 32 + chunk_bytes
```

Asserts: `sum(chunks) == payload`; each `32 + chunk <= 65488`; `count >= 1`. The
assembler cannot learn the total from the wire — each continuation's `length`
covers only its own chunk (`SOURCE`: `nvkm/rpc.c` builds each continuation with
`rpc_get(fn = CONTINUATION, size)`), so **the reply size must be known per
opcode** before the first read. That is the same requirement that makes a
request declare the reply size it can accept.

For this lane's first round trip (`GET_GSP_STATIC_INFO`, reply `0xCF0` payload,
`length 0xD10`, one slot) continuation is not exercised — which is exactly why
it needs unit tests now rather than on the card.

---

## 5. The pure/hardware split

### 5.1 The rule

> The pure half may read and write **the block**, and nothing else. It may not
> touch MMIO, may not read the clock, and may not sleep.

Everything in §3 and §4.3/§4.4 except `mb()` and the doorbell satisfies this.
That is a smaller hardware surface than it sounds, because the peer's header
words are ordinary RAM: the test tier mutates them directly to simulate the
firmware, and real hardware does the same thing by DMA.

### 5.2 The seam (three function pointers, not an abstraction layer)

```c
struct nv_rpc_hw {
  void     (*doorbell)(void *ctx, uint32_t queue_idx); /* MMIO, value 0 */
  void     (*fence)(void *ctx);                        /* arch_memory_barrier */
  uint64_t (*now_us)(void *ctx);                       /* timer_get_us, or a fake clock */
  void     *ctx;
};
```

- Real: `doorbell` → `nv_wr32(d, NV_GSP_QUEUE_HEAD0, 0)`; `fence` →
  `arch_memory_barrier()`; `now_us` → `timer_get_us()`.
- Unit tier: `doorbell` increments a counter and records the last argument;
  `fence` increments a counter (so ordering can be asserted, not assumed);
  `now_us` returns a value the test controls.

`now_us` being in the seam is what makes "bounded wait" testable: a test can
advance the fake clock past the timeout and assert the exact return code without
sleeping, and a test for "the doorbell was never rung" is simply
`hw.doorbell_calls == 0`.

### 5.3 Pure functions (exact signatures)

| function | in | out | notes |
|---|---|---|---|
| `nv_rpc_queue_geom` | — | pte_count, cmd_off, stat_off, block_size | the §3 invariants, in one place |
| `nv_rpc_block_build` | block, bus | 0/-EINVAL | zero, PTEs, cmdq header, **not** statq header |
| `nv_rpc_args_crosscheck` | block geometry, RMARGS view | 0, or an `enum nv_rpc_arg_field` naming the first field that disagrees | §3 step 5 |
| `nv_rpc_link_validate` | statq bytes, expected size/msgSize | 0 / `-EAGAIN` (not written yet) / `-EINVAL` (written, unusable) | distinguishes the two link failures, which are different bugs |
| `nv_rpc_negotiate` | our_flags, peer_flags | `own_readptr_off`, `peer_readptr_off` | §2.5, in one function so it is tested once |
| `nv_rpc_cksum` | `buf`, `bytes` | `u32` fold | `bytes % 8 == 0` asserted; also expose the raw `u64` accumulator for the `acc_hi == acc_lo` assert |
| `nv_rpc_elem_build` | staging, rpc_len, seq | elem_count | §4.3 S1/S5/S6 on a contiguous staging buffer |
| `nv_rpc_elem_check` | staging, elem_count | 0 / `-EIO` | R5..R7 |
| `nv_rpc_tx_prepare` | q, rpc_len | elem_count or `-ENOSPC`/`-EMSGSIZE` | S1..S3, no stores to the ring |
| `nv_rpc_tx_commit` | q, staging | 0 | S4..S8: the in-place copy into the ring (with wrap) + `writePtr` |
| `nv_rpc_kick` | q, hw | 0 | S9/S10: fence + doorbell, in that order |
| `nv_rpc_rx_take` | q, out, cap, reason | len or `-EAGAIN`/`-EIO`/`-EMSGSIZE`/`-EINVAL` | R1..R7 and R10's classification |
| `nv_rpc_rx_release` | q | 0 | R8/R9, split out so "took but did not release" is testable |
| `nv_rpc_split_plan` | payload | array of chunk sizes | §4.5 |
| `nv_rpc_peer_snapshot` | q | the eight statq words + a nonzero count, **read from `block+stat_off` at call time** | the row must observe the peer's word, not echo a cached `struct nv_rpc_peer` |
| `nv_rpc_static_info_request` | staging, seq | elem_count | `GET_GSP_STATIC_INFO` (65), `length = 0x20 + 0xCF0`, zeroed payload, result PENDING (contract §4.1 row 10) |
| `nv_rpc_rx_wait` | q, hw, fn, timeout | len, or `-ETIMEDOUT`/`NO_REPLY` | bounded wait for **one** function code; reads `rpc_result` from the RPC stream and releases the element |
| `nv_rpc_roundtrip` | q, hw, staging, fn, timeout | len, or the transport's error | link gate **before any store**, then prepare/commit/**one** kick/wait. An unlinked queue is `-EAGAIN` with zero doorbells |

`tx_prepare` / `tx_commit` / `kick` being three calls is what makes the
"doorbell not written" failure a *test* rather than an incident: after
`tx_commit` the queue is byte-correct and nobody has been notified.

**Result codes.** `src/include/errno.h` defines `EAGAIN`, `ENOSPC`, `EMSGSIZE`,
`ETIMEDOUT`, `EINVAL`, `EIO` — and **not** `EPROTO` or `EBADMSG`. Rather than
extend a shared header on a branch that is going to be merged, the coarse errno
comes from what exists and the fine reason is an explicit enum carried out of
every call. The log row prints the enum, so a serial log names the failure
without a symbol table.

| return | `enum nv_rpc_reason` | when |
|---|---|---|
| `-EAGAIN` | `NOTHING` / `NOT_LINKED` / `SHORT_PUBLISH` | nothing to read yet; peer header not written yet; `avail < elemCount` |
| `-ENOSPC` | `NO_SPACE` | `ec > free` |
| `-EMSGSIZE` | `TOO_BIG` | `rpc_len > 65488` |
| `-EINVAL` | `PEER_GEOM` / `BIG_ELEM` / `BAD_EC` | peer header written but unusable; `elemCount > 16` or `0` |
| `-EIO` | `CHECKSUM` / `SEQ_GAP` / `BAD_SIG` / `BAD_LEN` | element integrity failures — the message was consumed by the peer's accounting but cannot be trusted |
| 0 | `EVENT` / `UNKNOWN_FN` | dispatch outcomes that are not errors |

`SEQ_STALE` is not an error at all: consume, log, return `-EAGAIN`/`NOTHING`.

### 5.4 State that lives in the queue object

```c
struct nv_rpc_queue {
  uint8_t *block;      /* kernel VA of the 0x81000 block */
  uint64_t bus;        /* GPU-visible base == pte[0] */
  uint32_t pte_count, cmd_off, stat_off;
  uint32_t msg_count;  /* 63, from our own header */
  uint32_t tx_seq, rx_seq;
  uint32_t write_ptr, read_ptr;  /* mirrors; memory is authoritative */
  int swapped;         /* negotiated AND */
  int linked;          /* statq header validated */
  int tx_busy;         /* one outstanding request */
  struct { uint32_t version, size, msg_size, msg_count, entry_off, rx_hdr_off, flags; } peer;
};
```

The `write_ptr`/`read_ptr` mirrors are caches; every assert re-reads memory.
A cached pointer that drifts from memory is a bug the tests must be able to
catch, so keep the mirror but never let it be the source of truth in an assert.

### 5.5 Synthetic queues in the kernel unit tier

Two ways to build one, both used by §7:

1. **In-pool**: `frame_alloc_contig(129)` in the test, `phys_to_virt`, then
   `nv_rpc_block_build`. Real addresses, real contiguity. This is the version
   that validates §6's facility.
2. **Placement-new over a static array**: `static uint8_t blk[0x81000]
   __attribute__((aligned(4096)))`, `bus = virt_to_phys(blk)`. Faster, and the
   only option inside a test that wants deterministic addresses.

Either way the "firmware side" is a helper that writes a producer header into
`statq` and appends a reply element, exactly as §2 describes. If that helper and
`nv_rpc_elem_build` share code, the test is circular for the checksum — so the
test must build at least one fixture by hand from §2.3 / §7's golden vectors.

---

## 6. What HobbyOS has today, and what it does not

### 6.1 Exists (verified in this tree)

| facility | where | note |
|---|---|---|
| physically contiguous allocation | `src/include/frame.h:95` `frame_alloc_contig(int n)`; impl in `src/kernel/frame.c` | 4 KiB-granular, returns phys, first-fit under one lock hold. 129 pages is trivial next to the 9288-page contiguous run the firmware loader already takes (`nv_fw_load_blob`, `nv_fw.c:775`, `frame_alloc_contig(pages)` at `:795`) |
| release | `frame.h:71` `frame_free(phys)` | page-at-a-time; a 129-page block is 129 calls |
| phys → kernel VA | `src/include/kmem.h:60` `phys_to_virt()` | DMAP, write-back, cached. This is the pointer the transport writes through |
| kernel VA → phys | `kmem.h:64` `virt_to_phys()` | tripwires on a non-DMAP pointer (`kmem_bad_va`), which is exactly the guard we want in `nv_dma_alloc` |
| full memory barrier | `src/include/arch/cpu.h:75` `arch_memory_barrier()`; x64 impl `src/kernel/arch/x64/cpu.c:200` = `mfence`, arm `dmb sy` | the `fence` the transport needs; stronger than x86 requires, correct |
| MMIO through a fake-able op table | `src/include/nv.h:134-135` `nv_rd32`/`nv_wr32`; `src/kernel/nv.c:90-98` | the doorbell goes through this, so `nv_test.c`'s fake register file can assert the write happened |
| doorbell offset | `src/kernel/nv_gsp.c:105` `NV_GSP_QUEUE_HEAD0 = NV_FALCON_GSP_BASE + 0xC00` | `0x110C00` |
| monotonic time | `src/include/timer.h:20,26` `timer_get_us()` / `timer_get_ms()` | bounded waits; `nv_falcon_wait_idle` already uses the PTIMER for the same purpose |
| spinlock + irqsave variant | `src/include/lock.h` | for `tx_busy` / the one-outstanding rule |
| unit tier | `src/kernel/unit_test.h` (`ASSERT`, `EXPECT_EQ`, `TEST`), suite list `src/kernel/unit_test.c:57`, `KERNEL_MODE_UNIT_TEST` guard | `nv_test.c` is the model: fake hardware, real driver code |
| new objects compile automatically | `Makefile:752` (`$(wildcard $(SRC_DIR)/*.c)`) | a new `src/kernel/nv_rpc.c` needs **no** Makefile edit; it needs a prototype in `unit_test.h` and one call in `unit_test.c` |
| VT-d driver, initialised, **translation off** | `src/kernel/iommu_vtd.c`, `src/include/iommu_vtd.h`; called from `src/kernel/net_rdma.c:1243` (`iommu_vtd_init`) and `:1255` (`iommu_vtd_set_passthrough` for this GPU) | `iommu_vtd_enable_translation()` (`iommu_vtd.c:584`) has **no caller** and `net_rdma.c:1245-1249` says why (it would break NVMe/virtio DMA). So the GPU's DMA is untranslated and its DMA address *is* the CPU physical address — today. `iommu_vtd_is_active()` (`iommu_vtd.c:873`) reports only "initialised", not "translating" |

### 6.2 Must be added

| # | what | file(s) | why it cannot be skipped |
|---|---|---|---|
| 1 | **DMA buffer type carrying a bus address** | new `src/include/nv_dma.h`, `src/kernel/nv_dma.c` | `frame.h` hands out a *physical* address; the format needs a *GPU-visible* one. This is the difference between a working transport and "firmware reads zeros forever" (contract §5.3, §6 row 15). `struct nv_dma { uint8_t *va; uint64_t phys; uint64_t bus; uint32_t size; uint32_t pages; }`, `nv_dma_alloc(size, align, &dma)`, `nv_dma_free(&dma)` |
| 2 | the identity statement, in one place | `nv_dma.c` | `bus = phys` **today**, with (a) a comment naming the reason (VT-d is initialised but `iommu_vtd_enable_translation()` has no caller), and (b) an assert `phys == virt_to_phys(va)`. Plus a hard guard: if translation ever *is* enabled, refuse — add `int iommu_vtd_translation_enabled(void)` to `iommu_vtd.c` (read `GSTS.TES`, the bit `iommu_vtd_enable_translation` waits on) and return `-ENOTSUP` from `nv_dma_bus()` while it is 1. Until an IOVA allocator exists, `phys` is not a bus address |
| 3 | zeroing + alignment + pool guards | `nv_dma_alloc` | assert `size % 0x1000 == 0`, `align % 0x1000 == 0`, `frame_phys_in_pool(phys)` (so a BAR address can never be handed to the GPU as RAM — `frame.h:120`), and zero the range (`frame_alloc_contig` does not) |
| 4 | bounded poll with a named timeout | `nv_rpc.c` (`nv_rpc_wait_le32(q, off, timeout_us, hw)`) | both link and reply waits need it; it must take `now_us` from the seam so it is testable |
| 5 | the queue object in the driver | `src/include/nv.h` `struct nv_dev`: add `struct nv_rpc_queue rpc; uint64_t rpc_block_phys;` alongside `gsp_meta_phys` | the block must outlive the call, like `gsp_meta_phys` does |
| 6 | a block dump | `nv_rpc_dump(q)` | the block is **RAM**, not BAR0, so `NV_IOC_PEEK` (which is BAR0-only, whitelisted in `nv.c:386`) cannot reach it. Without a driver-side dump a stalled transport is unobservable from userland |
| 7 | failure rows | `nv_gsp.c` log style (`[NV-RPC] ...`) | one row per stage, naming the stage: `link`, `send`, `reply`, `event` |

**Not needed on x86-64, and must not be invented:** a cache flush. Sysmem DMA is
snooped and coherent here, there is no `clflush`/`wbinvd` helper anywhere in the
tree today, and a hand-rolled one would be wrong (cache-line granularity against
a device that writes whole headers). `nv_dma.h` must say so in one sentence plus
the existing `#ifdef __x86_64__` guard, so the assumption is visible rather than
implicit.

### 6.3 Allocation ordering (a real risk, not a formality)

`frame_alloc_contig` is first-fit from a rotating hint. A 129-page run can fail
after long uptime with fragmentation even though free memory is plentiful. The
transport has no degraded mode: no block, no GSP. So:

- allocate the block during `nv_init`/`nv_gsp_boot`, **before** the desktop and
  the block cache have taken their share;
- on `-ENOMEM`, log the largest contiguous run available (a diagnostic that
  tells the operator whether this is fragmentation or exhaustion), and stop with
  `-ENOMEM` rather than retrying in a loop.

### 6.4 ABI

`struct nv_info` is userland-visible and `NV_ABI_VERSION` is 2. **DECISION: this
lane does not add fields to it.** Transport state is log-only until a tool needs
it. Adding fields would force an ABI bump on a lane that has no consumer for
them, and a bump on an unmerged branch is a merge hazard.

---

## 7. Test plan: the pure half

Twenty-two cases in the table, plus two link-validation cases (T-23/T-24). Each
is runnable in the kernel unit tier with a synthetic queue, no card. "Expected" is the exact result, not "passes".

| # | case | setup | expected result | catches |
|---|---|---|---|---|
| T-01 | **block geometry** | `nv_rpc_block_build` into a 0x81000 array | `pte_count 129`, `cmd 0x1000`, `stat 0x41000`, `pte[0] == bus`, `pte[i] == bus+i*0x1000`, `pte[128] == bus+0x80000` | self-covering arithmetic (§1) |
| T-02 | **cmdq header published** | same | `+0x00 0`, `+0x04 0x40000`, `+0x08 0x1000`, `+0x0C 63`, `+0x10 0`, `+0x14 1`, `+0x18 0x20`, `+0x1C 0x1000`, `+0x20 0` | field order / `rxHdrOff` |
| T-03 | **statq header not pre-written** | same | bytes `0x00..0x23` of `statq` all zero; `nv_rpc_link_validate` returns `-EAGAIN` | destroying the boot's only proof of life (§3 step 4) |
| T-04 | **RMARGS cross-check** | geometry + a correct args view; then `statQueueOffset = 0x40000` | `0`; then nonzero, naming `statQueueOffset` | the two descriptions drifting apart (§3 step 5) |
| T-05 | **checksum golden vectors** | three fixtures built byte-by-byte from §2 (all bytes zero except the fields named): (a) NOP, `length 32`, `seq 0`, 1 slot; (b) `function 65`, `length 0xD10`, `seq 7`, 1 slot; (c) `function 76`, `length 9000`, `seq 3`, 3 slots, payload = 9000 × `0x11` | `40505277` / `40505f01` / `40507132` | the rule itself, against numbers not derived from the implementation under test |
| T-06 | **checksum self-zero** | fill, then re-hash | fold `== 0` **and** `acc_hi == acc_lo` | a fold that is self-consistent but backwards |
| T-07 | **stale tail rejected** | take the T-05(a) fixture, write `0xAA` at offset `48+32` (inside the slot, past the message) | `nv_rpc_elem_check` → `-EIO`, reason `CHECKSUM` | the contract's short-range hash (correction §0.1). Under the old rule this fixture would pass and the card would hang |
| T-08 | **SWAP_RX both ways** | `nv_rpc_negotiate(1,1)` then `(1,0)` | swapped: own `cmdq+0x20`, peer `statq+0x20`. unswapped: own `statq+0x20`, peer `cmdq+0x20` | the crossing in §2.5, incl. the "poll the wrong word" variant |
| T-09 | **doorbell not written** | `tx_prepare` + `tx_commit`, no `kick` | `writePtr` advanced by `ec`, element byte-correct, `hw.doorbell_calls == 0`; then `kick` → `1`, last arg `0` | conflating commit with notify |
| T-10 | **doorbell value/offset** | `kick` with the real `nv_mmio_ops` fake | exactly one write, offset `0x110C00`, value `0` | a queue-index value mistaken for a pointer |
| T-11 | **wrap-around** | `writePtr = 61`, send a 3-slot message | slot 61, 62, 0 written; `writePtr == 1`; the third slot's bytes equal stream offset 8192 (RPC offset **8144**) | wrap arithmetic and the §0.3 figure |
| T-12 | **full queue** | advance peer `readPtr` so `free == 2`, send a 3-slot message | `-ENOSPC`, `writePtr` unchanged, **no** slot written | writing into the reserved slot (row 11) |
| T-13 | **empty queue** | fresh queue, `writePtr == readPtr == 0` | `free == 62` (not 63); `rx_take` → `-EAGAIN` | the one-empty-slot rule |
| T-14 | **oversize message** | `rpc_len = 65488` then `65489` | `ec == 16`, accepted; `65489` → `-EMSGSIZE` | the 16-slot ceiling |
| T-15 | **elemCount arithmetic** | `rpc_len = 4048, 4049, 8144, 8145` | `ec = 1, 2, 2, 3` | off-by-one at the slot boundary (the boundary is `48+len > k*4096`) |
| T-16 | **bad checksum on receive** | append a valid reply, flip one byte at `+0x2C` (the pad word), then call `rx_take` | `-EIO`/`CHECKSUM` after 3 retries, **read pointer unchanged**, second `rx_take` sees the same element | the "consume a message you failed to verify" bug |
| T-17 | **mismatched sequence** | `rx_seq = 5`, element `seqNum = 7`; then `seqNum = 3` | `7` → `-EIO`/`SEQ_GAP`, not consumed; `3` → consumed, reason `SEQ_STALE`, returns "nothing" | both halves of R6 |
| T-18 | **sequence wrap** | `tx_seq = 0xFFFFFFFF`, one send | element `seqNum == 0xFFFFFFFF`, `tx_seq == 0` | a u32 wrap banished with a 64-bit counter |
| T-19 | **continuation split** | `nv_rpc_split_plan(70000)` | `[65456, 4544]`, count 2, each `32 + chunk <= 65488`; message 0 `function == 76`, message 1 `function == 71`, distinct `seqNum`s | the two-level split of §0.2 |
| T-20 | **short publish** | `peer.writePtr = readPtr + 2` for a 3-slot element | `-EAGAIN`, read pointer unchanged | `avail < elemCount` misread as "read anyway" |
| T-21 | **read pointer not advanced** | `rx_take` twice without `release` | second call returns the *same* element; `avail` unchanged; a third after `release` returns the next | row 11 modelled as a test |
| T-22 | **reply answered by an event** | queue holds `fn 0x1001 result 0` then `fn 65 result 0`; wait for 65 | the event is classified as an event and skipped; the 65 reply is returned; both opcodes logged | row 5: success reported on the wrong message |

Two more that are cheap and worth having: **T-23** `nv_rpc_link_validate` on a
header with `size = 0x20000` returns `-EINVAL` and names `size`; **T-24** a
link header with `msgCount` inconsistent with `(size-entryOff)/msgSize` returns
`-EINVAL` (the "header arrived intact" recomputation).

Five more, added with the publish side and the first round trip and
implemented in `src/kernel/nv_rpc_test.c`:

- **T-25** the `GSP_ARGUMENTS_CACHED` encoder: every field at the contract §1.5
  offset, and the cross-check names the first field that disagrees.
- **T-26** the libos-args table: `id8` NUL padding (`"RMARGS"` is six
  characters and its id contains two zero bytes), entry stride 0x20, refusals
  for a truncating name, an unaligned `pa` and a zero-sized region.
- **T-27** the status header is **read, not remembered**: after the peer's words
  are zeroed, `q->peer.size` still holds `0x40000` (the cache) while
  `nv_rpc_peer_snapshot()` returns `nonzero == 0` and `link_validate` agrees
  with `-EAGAIN`. A row that printed the cache would be an inference.
- **T-28** `GET_GSP_STATIC_INFO` framing: `length 0xD10`, one element,
  `result 0xFFFFFFFF`, element seq set, whole-element fold 0, and one flipped
  payload byte breaks the fold.
- **T-29** the round trip: (a) unlinked → `-EAGAIN`/`NOT_LINKED`, **zero**
  doorbells, no slot written; (b) linked, `INIT_DONE` ahead of the reply → the
  event is consumed and logged, the `fn 65` reply is returned, exactly one
  doorbell, read pointer advanced past both; (c) linked and silent →
  `-ETIMEDOUT`/`NO_REPLY`, one doorbell, no retry.

Test-tier mechanics: one `TEST(fn)` per row, `tests_run++`, `EXPECT_EQ` on
return codes, `ASSERT` on preconditions, registered in `nv_test_suite()`
(`src/kernel/nv_test.c:639`) or a new `nv_rpc_test_suite()` called from
`unit_test.c:57`. Each prints one `Running test_...` line so a failure names
itself in the serial log.

---

## 8. What still cannot be tested offline

| behaviour | why not | where it will fail first |
|---|---|---|
| firmware accepts our PTE table | needs the GPU to walk it | link never validates (row 1 vs row 3 distinguishes address from geometry) |
| `SWAP_RX` is really 1 on this firmware | needs the firmware's header | both pointers frozen after ≤62 messages (row 4) |
| checksum domain matches the receiver exactly | needs the receiver | element consumed but never parsed, i.e. "pointer moved, no reply" (row 9) |
| doorbell is the right register on AD102 | MEASURED only as "reads 0" | nothing moves at all |
| firmware tolerates an empty registry / zero log regions | needs init | `GSP_INIT_DONE` with a nonzero result |
| reply `sequence` echo | needs a reply | bookkeeping only |

These are the things the doc deliberately does not guess at. Everything else in
§2/§4 is a byte-level fact that can be got right before the card is available.

---

## 9. The three highest risks

**1. GPU-visible address vs CPU physical address.** If `bus` is not what the GPU
uses, the firmware walks a page table that is not there and nothing reports
anything — the queue simply never advances. Every other bug in this document
produces a diagnostic; this one produces silence (contract §6 row 15). It is
also the assumption with the shortest shelf life in the tree: `net_rdma.c:1243`
already initialises VT-d and installs a pass-through entry for *this* GPU, and
the same file has a shadow-map path that would install translated entries. The
current equality `bus == phys` is a consequence of translation being off, not of
the platform having no IOMMU. Mitigation: `nv_dma.bus` exists as a type from the
first line of code, is asserted equal to `phys` in exactly one function with the
reason written beside it, refuses to run when `GSTS.TES` says translation is on,
and `PTE[0] == bus` is asserted at build time *and* re-asserted before every
send.

**2. `SWAP_RX` and read-pointer ownership.** The read pointer of each queue is
written by its consumer, and with `SWAP_RX` set the words cross between the two
blocks. Get this wrong and both sides write where the other is not looking: no
error, the command queue fills at 62 messages, and the symptom appears one call
later than the cause. The negotiation must be an explicit AND computed after the
peer's header is read, routed through one accessor function, and pinned by T-08
in both polarities.

**3. The checksum domain.** The contract says one range, both implementations
use another, and the difference is invisible until a slot is reused without
being cleared — at which point the element is dropped before parsing and looks
exactly like a dead queue. Mitigation: zero whole slots, hash whole slots, assert
the self-zero property *and* `acc_hi == acc_lo`, and keep T-07 (a stale byte in
the padding must fail) as the regression test for the correction in §0.1.

Runners-up, in order: the doorbell as a false status (it reads 0 always —
never gate on it); sequence-number ownership split across two counters; and
treating "something arrived" as "my reply arrived".

---

## 10. Fields that are still unknown

Nothing in §2 blocks coding. These are the rows where the table is a fact but
the *semantics* are not, and each is handled by not depending on it.

| field | unknown | handling |
|---|---|---|
| `flags` bits 1..31 | meaning | publish 0; reject a peer with any set (DECISION) |
| peer `rxHdrOff` | whether the firmware publishes exactly `0x20` | accept `>= 0x20` and *use* the value rather than hardcoding the offset (`msgqRxLink` does the same) |
| peer `entryOff` | same, `0x1000` | accept `>= rxHdrOff+4`, page-aligned, and use it |
| `authTagBuffer` / `aadBuffer` | whether the firmware validates them with CC off | zero them and assert; no behaviour depends on the answer |
| `pad` at `+0x2C` | whether a nonzero value is rejected or merely breaks the checksum | always 0 |
| `rpc_result_private` | per-opcode meaning | log verbatim, never gate on it except "nonzero ⇒ failure" for the RPCs where the vendor does |
| `sequence` echo | whether the firmware echoes it | never match on it; log it for the first 100 messages |
| `header_version` minor | whether a nonzero minor is rejected | send 0, do not validate the reply's minor |
| continuation total | not on the wire | reply size known per opcode (§4.5) |
| firmware's `elemCount` policy on replies | whether it can exceed 16 | treat `> 16` as `-EINVAL`/`BIG_ELEM` |
| `NV_PGSP_MAILBOX(0..3)` at `0x110804` | purpose | untouched |

---

## 11. Implementation order for this lane

Each step is independently verifiable, and steps 1–4 need no card.

| # | step | verified by |
|---|---|---|
| 1 | `nv_dma.h`/`nv_dma.c`: alloc/free/bus, guards | T-01 with a real `frame_alloc_contig(129)` |
| 2 | `nv_rpc.c` §2 accessors + `nv_rpc_cksum` + `nv_rpc_elem_build/check` | T-05, T-06, T-07, T-15 |
| 3 | `nv_rpc_block_build`, geometry, `args_crosscheck`, `link_validate` | T-01..T-04, T-23, T-24 |
| 4 | `tx_prepare/commit/kick`, `rx_take/release`, `negotiate`, `split_plan` | T-08..T-22 |
| 5 | glue `nv_gsp.c` stage 8: allocate, build, publish RMARGS, pre-queue 72/73, doorbell | the card; link row + "did 72/73 move the peer's read pointer" |
| 6 | link wait + `GET_GSP_STATIC_INFO` round trip | reply `result 0`, three non-zero handles |

Step 4 is the one that pays: it is where every failure that would otherwise
present as "the GSP is thinking" becomes a named return code.

---

## 12. Sources

Vendor driver, branch `535.113.01` (the same build as the firmware on this card):

| short name | file | functions read |
|---|---|---|
| `NVIDIA/msgq_priv.h` | `src/common/shared/msgq/inc/msgq/msgq_priv.h` | `msgqTxHeader`, `msgqRxHeader`, `MSGQ_VERSION` |
| `NVIDIA/msgq.h` | `src/common/shared/msgq/inc/msgq/msgq.h` | `MSGQ_FLAGS_SWAP_RX`, size/alignment minima |
| `NVIDIA/msgq.c` | `src/common/shared/msgq/msgq.c` | `msgqTxCreate`, `msgqRxLink`, `msgqTxGetFreeSpace`, `msgqTxGetWriteBuffer`, `msgqTxSubmitBuffers`, `msgqRxGetReadAvailable`, `msgqRxGetReadBuffer`, `msgqRxMarkConsumed` |
| `NVIDIA/mqueue_priv.h` | `src/nvidia/inc/kernel/gpu/gsp/message_queue_priv.h` | `GSP_MSG_QUEUE_ELEMENT`, the size/how-many macros |
| `NVIDIA/mqueue_cpu.c` | `src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c` | `_checkSum32`, `GspMsgQueueSendCommand`, `GspMsgQueueReceiveStatus`, `GspMsgQueuesInit`, `_getMsgQueueParams`, `GspStatusQueueInit` |
| `NVIDIA/rpc_headers.h` | `src/nvidia/kernel/inc/vgpu/rpc_headers.h` | header-version bits, signature, result codes |
| `NVIDIA/msg_hdr.h` | `src/nvidia/generated/g_rpc-message-header.h` | `rpc_message_header_v03_00` field order |
| `NVIDIA/kgsp.c` | `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c` | `_kgspRpcSendMessage` (send then `kgspSetCmdQueueHead_HAL`) |

Independent reimplementation (Linux, `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/`):

| short name | file | functions read |
|---|---|---|
| `nvkm/rpc.c` | `rpc.c` | `r535_gsp_cmdq_push`, `r535_gsp_cmdq_get`, `r535_gsp_msgq_recv_one_elem`, `r535_gsp_rpc_get`, `r535_gsp_rpc_push` |
| `nvkm/gsp.c` | `gsp.c` | `r535_gsp_shared_init`, `r535_gsp_set_rmargs` |
| `nvkm/rpcfn.h` | `nvrm/rpcfn.h` | opcode numbering (0/1/65/71/72/73/76/103) |
| `nvkm/msgfn.h` | `nvrm/msgfn.h` | `GSP_INIT_DONE` = `0x1001` |

HobbyOS tree, cited in §6: `src/include/frame.h`, `src/kernel/frame.c`,
`src/include/kmem.h`, `src/include/arch/cpu.h`, `src/kernel/arch/x64/cpu.c`,
`src/include/nv.h`, `src/kernel/nv.c`, `src/kernel/nv_gsp.c`,
`src/kernel/nv_fw.c`, `src/include/nv_fw.h`, `src/include/timer.h`,
`src/include/lock.h`, `src/kernel/unit_test.h`, `src/kernel/unit_test.c`,
`src/kernel/nv_test.c`, `src/include/iommu_vtd.h`, `src/kernel/iommu_vtd.c`,
`Makefile`.

On the no-copying rule: this document contains no code from any of the above.
The field tables were written from the format, cross-checked between the two
implementations, and the checksum vectors in §7 were computed from the rule in
§4.3 (S4/S6) with an independent tool — they are fixtures, not extracts.
