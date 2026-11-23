# The GSP-RPC contract: talking to GSP-RM on AD102 once it boots

Target: **RTX 4090, chipset `0x192` (AD102), GSP firmware 535.113.01, 24 GiB**.
Scope: everything that happens **after** the RISC-V core is running — the
shared-memory transport, the element/RPC wire format, the init handshake, and
the failure modes that look like success. Booting the firmware itself (WPR2
layout, booter, FWSEC) is `docs/nv-gsp-firmware.md` and the staged machine in
`src/kernel/nv_gsp.c`; this document starts where that ends.

Method: the contract below was read out of three public sources and then
cross-checked against each other and against the measured BAR0 dumps in
`docs/nv-gsp-reference.md`. No code was copied; what follows is a description of
the wire format in table form, written from an understanding of those sources.

## Provenance tags

Every row carries one:

| tag | meaning |
|---|---|
| **MEASURED** | read off this exact card in `docs/nv-gsp-reference.md` (BAR0 only — no queue memory was ever mapped, so nothing about queue *contents* can be MEASURED here) |
| **SOURCE** | stated outright in a public source, cited by file and function. Where two independent sources agree, both are cited |
| **INFERRED** | my reading of the sources, consistent with more than one of them, but not stated as a rule anywhere. Treat as a hypothesis with a stated test |
| **NOT ESTABLISHED** | the sources do not settle it. No guess is offered; the impact is stated instead |

Sources, abbreviated throughout:

| short name | what it is |
|---|---|
| `NVIDIA/mq.h` | `open-gpu-kernel-modules`, branch **535**: `src/nvidia/inc/kernel/gpu/gsp/message_queue_priv.h` |
| `NVIDIA/mq.c` | same branch: `src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c` (`GspMsgQueuesInit`, `GspMsgQueueSendCommand`, `GspMsgQueueReceiveStatus`) |
| `NVIDIA/msgq.c` | same branch: `src/common/shared/msgq/msgq.c` (`msgqTxCreate`, `msgqRxLink`, `msgqTxSubmitBuffers`, `msgqRxMarkConsumed`) |
| `NVIDIA/initargs.h` | same branch: `src/nvidia/inc/kernel/gpu/gsp/gsp_init_args.h` |
| `NVIDIA/sysinfo.h` | same branch: `src/nvidia/inc/kernel/gpu/gsp/gsp_static_config.h` |
| `NVIDIA/kgsp.c` | same branch: `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c` |
| `NVIDIA/kgsp_tu102.c` | same branch: `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c` |
| `NVIDIA/rpc.c` | same branch: `src/nvidia/kernel/vgpu/nv/rpc.c` |
| `NVIDIA/rpc.h` | same branch: `src/nvidia/kernel/inc/vgpu/rpc_headers.h` |
| `NVIDIA/dev_gsp.h` | same branch: `src/common/inc/swref/published/ampere/ga102/dev_gsp.h` (the AD102 GSP register block) |
| `nvkm/rpc.c` | Linux `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/rpc.c` |
| `nvkm/gsp.c` | Linux `.../rm/r535/gsp.c` (`r535_gsp_shared_init`) |
| `nvkm/rpcfn.h` | Linux `.../r535/nvrm/rpcfn.h` (opcode numbering, "excerpt of RM headers from branch 535.113.01") |
| `nvkm/msgfn.h` | Linux `.../r535/nvrm/msgfn.h` (event numbering, same provenance) |

`NVIDIA/*` is the vendor's own host-side driver for the same firmware build;
`nvkm/*` is an independent reimplementation that has to interoperate with the
same firmware. Where they agree, the row is effectively settled. Where they
disagree, the disagreement is called out — that is usually where a naive
implementation hangs.

---

## 1. Shared-memory layout

### 1.1 Where the queues live

| property | value | tag |
|---|---|---|
| medium | **host system memory** (DRAM), not VRAM | SOURCE — `NVIDIA/mq.c` `GspMsgQueuesInit` creates one `MEMORY_DESCRIPTOR` with `ADDR_SYSMEM`, `NV_MEMORY_NONCONTIGUOUS`, `NV_MEMORY_CACHED`; `nvkm/gsp.c` `r535_gsp_shared_init` uses `dma_alloc_coherent` |
| contiguity | physically non-contiguous is allowed; the GPU sees it through a flat page table the host builds | SOURCE — same two functions |
| addressing | the GPU-side physical (bus/DMA) address. Not the CPU virtual address, and not the CPU physical address if an IOMMU is in front | SOURCE — `NVIDIA/mq.c` fills the PTE array with `memdescGetPhysAddrs(..., AT_GPU, ...)` |
| VRAM involvement | none for the transport. VRAM holds the firmware image, its heap and the WPR metadata | SOURCE — `nvkm/gsp.c`, `NVIDIA/kgsp_tu102.c` (`kgspCalculateFbLayout_TU102`) |

Consequence for `nv`: the queue block can be an ordinary kernel allocation. What
the firmware needs is the **list of GPU-visible page addresses**, not one
contiguous run. HobbyOS currently maps BAR1/BAR0 with no IOMMU in the path, so
"CPU physical = GPU physical" will hold — but that assumption must be written
down in the code, not assumed silently, because it is the difference between a
working transport and a firmware that reads zeros forever.

### 1.2 The block

One allocation holds a page table, then the command queue, then the status
queue, in that order. Every part is page aligned.

| region | offset in block | size | alignment | tag |
|---|---|---|---|---|
| page table (PTE array, 8 bytes per entry) | `0x0000` | 1 page for our sizes | 4 KiB | SOURCE — `NVIDIA/mq.h` "each of the following are page aligned"; `NVIDIA/mq.c` arithmetic |
| command queue | `0x1000` | `0x40000` (256 KiB) | 4 KiB | SOURCE — `NVIDIA/mq.c` `defaultCommandQueueSize`; `nvkm/gsp.c` `shm.cmdq.size` |
| status queue | `0x41000` | `0x40000` (256 KiB) | 4 KiB | SOURCE — same |
| total | `0x81000` | 129 pages | | INFERRED (arithmetic, see below) |

PTE count arithmetic, both sides doing it the same way:

| step | value |
|---|---|
| queue pages | `(0x40000 + 0x40000) >> 12` = 128 |
| plus pages needed to hold the PTEs themselves | `ceil(128 * 8 / 4096)` = 1 |
| **PTE count** | **129** |
| page-table size | `align_up(129 * 8, 4096)` = 4096 |
| block size | 4096 + 0x40000 + 0x40000 = `0x81000` = 129 * 4096 ✔ |

The self-covering detail matters: the count includes the pages that hold the
PTE array, and PTE[0] is the physical address of the block's first page (the
page-table page itself). `NVIDIA/mq.c` sets `sharedMemPA = pPageTbl[0]` — i.e.
the base address handed to firmware *is* the first PTE, and the firmware walks
the table from there. **INFERRED** as a rule, but the arithmetic only closes if
it is true, and both implementations behave as if it is.

### 1.3 Queue header

Each queue block begins with a 32-byte producer header, then a consumer read
pointer at `+0x20`, then nothing until `+0x1000`, where the slots start.

| offset | size | field | written by | value we must publish | tag |
|---|---|---|---|---|---|
| `+0x00` | u32 | `version` | producer | `0` | SOURCE — `MSGQ_VERSION` = 0, `NVIDIA/msgq.c` |
| `+0x04` | u32 | `size` | producer | block size in bytes, `0x40000` | SOURCE |
| `+0x08` | u32 | `msgSize` | producer | `0x1000` (slot size, power of two, ≥16) | SOURCE |
| `+0x0C` | u32 | `msgCount` | producer | `(size - entryOff) / msgSize` = **63** | SOURCE |
| `+0x10` | u32 | `writePtr` | producer | index of the next free slot, starts 0 | SOURCE |
| `+0x14` | u32 | `flags` | producer | bit 0 set (`SWAP_RX`), see §1.4 | SOURCE — `MSGQ_FLAGS_SWAP_RX` = 1 |
| `+0x18` | u32 | `rxHdrOff` | producer | `0x20` | SOURCE — `align_up(32, 16)` |
| `+0x1C` | u32 | `entryOff` | producer | `0x1000` | SOURCE |
| `+0x20` | u32 | `readPtr` | **consumer** | 0 at start | SOURCE |
| `+0x24`..`+0xFFF` | | unused, zero | | | INFERRED (header page is 4 KiB, only 36 bytes are defined) |

Capacity: `msgCount` slots, but only `msgCount - 1` are usable — a full ring is
indistinguishable from an empty one, so the producers leave one slot free
(`NVIDIA/msgq.c` `msgqTxCreate` sets `txFree = msgCount - 1`; `msgqTxGetFreeSpace`
subtracts the extra 1 again). **Effective capacity: 62 messages.**

### 1.4 Ownership, and the one thing that hangs cards

`SWAP_RX` (§1.3, bit 0) changes *where* a read pointer lives, and both sides
must agree. With it set — which is what the reference driver publishes — each
block carries the read pointer of the **other** queue:

| block | `+0x00..0x1F` | `+0x20` | `+0x1000..` |
|---|---|---|---|
| command queue (`cmdq`, block+`0x1000`) | host: producer header | **host** writes; **firmware** reads. It is the host's read index into the *status* queue | host writes RPCs, firmware reads them |
| status queue (`statq`, block+`0x41000`) | **firmware**: producer header | **firmware** writes; **host** reads. It is the firmware's read index into the *command* queue | firmware writes replies/events, host reads them |

Two independent sources agree on this placement: `nvkm/gsp.c`
`r535_gsp_shared_init` points its `msgq.rptr` at the cmdq header's read pointer
and its `cmdq.rptr` at the status header's read pointer; `NVIDIA/msgq.c`
`msgqTxCreate`/`msgqRxLink` implement exactly the `rxSwapped` branch those
pointers imply.

**Do not hardcode this.** `SWAP_RX` is negotiated: each side's header declares
it, and the layout is the AND of the two (`NVIDIA/msgq.c`: `rxSwapped =
(tx.flags & SWAP_RX) && (rx.flags & SWAP_RX)`). The implementation rule is:

1. publish `flags = 1` in your command-queue header before boot;
2. after the link, **read the firmware's `flags` word** out of the status-queue
   header and compute the AND;
3. only then decide which `+0x20` word to write your status-queue read pointer
   into.

If you skip step 2 and the firmware ever publishes `flags = 0`, both sides write
their read pointers where the other is not looking. Nothing errors. The command
queue fills after 62 messages and the status queue stops advancing — a
"succeeds but never completes" failure (§6, row 4).

Free space and available-count rules, both sides:

| quantity | rule |
|---|---|
| free slots in a queue | `(peerReadPtr + msgCount - writePtr - 1) mod msgCount` |
| messages available to read | `(peerWritePtr + msgCount - readPtr) mod msgCount` |
| wrap | slot index `i` lives at `block + entryOff + i * msgSize`; a message that spans the end of the ring continues at slot 0 |

### 1.5 How the firmware is told the addresses

The queue block address does **not** go into the command queue. It goes into a
one-page argument block, which the firmware reads at the start of its RM task.
Field order and offsets below are the 535 build's, computed from
`NVIDIA/initargs.h` under natural (8-byte) alignment; total **0xC8 (200)** bytes
inside a **4096**-byte allocation.

`GSP_ARGUMENTS_CACHED`:

| offset | size | field | we must set | tag |
|---|---|---|---|---|
| `+0x00` | u32 | `magic` | `'G' 'S' 'P' ' '` packed little-endian → bytes `47 53 50 20`, i.e. dword `0x20505347` | SOURCE — `GSP_ARGUMENTS_MAGIC_VALUE` |
| `+0x04` | u16 | `size` | size of this structure (`0xC8`) | SOURCE — field exists; **INFERRED** that firmware validates it, because it also validates the magic |
| `+0x06` | u8[2] | reserved | 0 | |
| `+0x08` | u64 | `flags` | 0 (bit 0 = "in PM transition", only for resume) | SOURCE |
| `+0x10` | u64 | `mq.flags` | 0 (bit 0 = queue encryption; we do not enable CC) | SOURCE — `MESSAGE_QUEUE_INIT_FLAG_ENCRYPTION` |
| `+0x18` | u64 | `mq.sharedMemPhysAddr` | **PTE[0]** = GPU address of the block base | SOURCE |
| `+0x20` | u64 | `mq.cmdQueueOffset` | `0x1000` (offset from block base) | SOURCE |
| `+0x28` | u64 | `mq.statQueueOffset` | `0x41000` | SOURCE |
| `+0x30` | u32 | `mq.queueElementHdrSize` | `0x30` | SOURCE — field present in 535 |
| `+0x34` | u32 | `mq.queueElementSizeMin` | `0x1000` | SOURCE |
| `+0x38` | u32 | `mq.queueElementSizeMax` | `0x10000` | SOURCE |
| `+0x3C` | u32 | `mq.queueHeaderAlign` | `4` (log2 bytes; 16-byte header alignment) | SOURCE |
| `+0x40` | u32 | `mq.queueElementAlign` | `12` (log2; 4096) | SOURCE |
| `+0x44` | u32 | `mq.pageTableEntryCount` | `129` | SOURCE |
| `+0x48` | u8[8] | reserved | 0 | |
| `+0x50` | u32 | `sr.oldLevel` | 0 (0 on a cold boot; 3 only for resume) | SOURCE — `nvkm/gsp.c` `r535_gsp_set_rmargs` |
| `+0x54` | u32 | `sr.flags` | 0 | SOURCE |
| `+0x58` | u8[8] | reserved | 0 | |
| `+0x60` | u8[4] | reserved | 0 | |
| `+0x64` | u32 | `gpuInstance` | 0 | SOURCE |
| `+0x68`..`+0xC7` | 6 × (u64, u64) | profiler / sysmem-heap / state-monitor / bindata / vbios / ctxsw-dump argument pairs | all zero when unused | SOURCE |

Note the newer branches carry more fields and a slightly different arrangement;
the 535 layout above is the one that matches firmware 535.113.01. `nvkm`
implements an older, shorter variant of the same structure and leaves the
geometry fields implicit — the firmware accepts that too (**INFERRED**: they
default to the same values), but since we are writing for one firmware build,
sending the explicit geometry is the safer choice: it fails loudly if we and
firmware disagree instead of silently disagreeing.

### 1.6 How the argument block itself is advertised

The firmware does not scan memory. Before the processor starts, the host hands
it a small table of named regions, and that table's address goes into the two
GSP falcon mailboxes.

| step | register / structure | value | tag |
|---|---|---|---|
| 1 | `NV_PGSP_FALCON_MAILBOX0` = BAR0 `0x110040` | low 32 bits of the libos-args page's GPU address | SOURCE — `NVIDIA/dev_gsp.h`; `NVIDIA/kgsp_tu102.c` `kgspProgramLibosBootArgsAddr_TU102` |
| 2 | `NV_PGSP_FALCON_MAILBOX1` = BAR0 `0x110044` | high 32 bits | SOURCE — same |
| 3 | libos-args page, entry *i*: 32 bytes each | `id8` (u64), `pa` (u64), `size` (u64), `kind` (u8), `loc` (u8) | SOURCE — `LibosMemoryRegionInitArgument`, `NVIDIA/kgsp.c` `kgspSetupLibosInitArgs` |

The four entries the reference driver publishes, in order:

| index | `id8` (chars → little-endian u64) | `pa` | `kind` | `loc` |
|---|---|---|---|---|
| 0 | `"LOGINIT"` | init-task log buffer | 1 (contiguous) | 1 (sysmem) |
| 1 | `"LOGINTR"` | interrupt-task log buffer | 1 | 1 |
| 2 | `"LOGRM"` | RM-task log buffer | 1 | 1 |
| 3 | `"RMARGS"` | **the argument block of §1.5** | 1 | 1 |

`id8` is just the eight characters packed with the first character in the least
significant byte (`NVIDIA/kgsp.c` `_kgspGenerateInitArgId`). So `"RMARGS"` is
`0x0000534752414D52` — note `R`=`0x52` in the low byte.

**MEASURED / important:** both mailbox words read `0x00000000` on this card in
*both* the pre-init and post-init reference dumps
(`docs/nv-gsp-reference.md` §3.4, offsets `0x110040`/`0x110044`). They are write
registers, not status. **They cannot be used as a "did the firmware take my
arguments" check.** `nv`'s current probe logs them, which is fine; nothing may
gate on them.

The `LOGINIT`/`LOGINTR`/`LOGRM` entries are logging plumbing, not transport.
For a first round trip they can be zero-sized; whether the firmware tolerates a
missing/zero-sized log region is **NOT ESTABLISHED** — it is cheap to provide
three valid 4 KiB regions and remove the question.

### 1.7 The doorbell

| property | value | tag |
|---|---|---|
| register | `NV_PGSP_QUEUE_HEAD(i)` = BAR0 `0x110C00 + 8*i`, 8 instances, 32-bit `ADDRESS` field | SOURCE — `NVIDIA/dev_gsp.h` |
| access | documented read-write, 32-bit; the value is written as a whole dword. Its readback is not a status: it reads 0 while GSP-RM runs | SOURCE — access column `RW-4A` in `NVIDIA/dev_gsp.h`; **MEASURED** `0x110C00 = 0` in both dumps |
| when | after the command-queue `writePtr` has been published | SOURCE — `NVIDIA/kgsp.c` `_kgspRpcSendMessage`, `nvkm/rpc.c` `r535_gsp_cmdq_push` |
| value | **0**, always, for queue index 0 | SOURCE — both call sites pass literal 0; the value is a queue selector, not a pointer |
| queue 0 | the single RM RPC queue | SOURCE — `RPC_TASK_RM_QUEUE_IDX` = 0 |

**MEASURED:** `0x110C00` reads `0x00000000` in the reference dumps in both
states, i.e. idle. A static dump can never show a doorbell being rung; that is
why this section is built from sources and not from the dump.

There is a second, separate mailbox block — `NV_PGSP_MAILBOX(i)` at `0x110804 +
4*i`, four instances — that is *not* part of the transport handshake. What it
carries, and whether a minimal driver must touch it, is **NOT ESTABLISHED**.
Nothing in either reference implementation writes it during normal RPC traffic.

---

## 2. Queue element format

### 2.1 Element header

A slot is 4096 bytes. The first 48 are the element header; the rest is the
RPC message (header + payload), zero padded.

| offset | size | field | meaning | tag |
|---|---|---|---|---|
| `+0x00` | 16 | `authTagBuffer` | AEAD authentication tag. **All zero when confidential compute is off** | SOURCE — `NVIDIA/mq.h`; CC-only path in `NVIDIA/mq.c` |
| `+0x10` | 16 | `aadBuffer` | AEAD additional data. Zero without CC; with CC it holds the element sequence number | SOURCE |
| `+0x20` | u32 | `checkSum` | chosen so that a checksum over the element, *including this field*, is 0 (§2.2) | SOURCE — field comment "value needed to make checksum always zero" |
| `+0x24` | u32 | `seqNum` | per-queue message sequence number, one per message (not per slot) | SOURCE |
| `+0x28` | u32 | `elemCount` | number of 4096-byte slots this message occupies, `ceil((48 + rpc.length)/4096)` | SOURCE — `GSP_MSG_QUEUE_BYTES_TO_ELEMENTS` |
| `+0x2C` | u32 | padding to the 8-byte aligned RPC start | zero; **included in the checksum** | INFERRED — `nvkm/rpc.c` explicitly zeroes it before hashing, and the struct's RPC member is 8-aligned at 0x30 |
| `+0x30` | ... | RPC message | §3 | SOURCE — `NV_OFFSETOF(GSP_MSG_QUEUE_ELEMENT, rpc)` = `GSP_MSG_QUEUE_ELEMENT_HDR_SIZE` |

Without confidential compute the 32 bytes of tag+AAD are dead weight: always
zero, never interpreted. There is no other authentication on the transport.

**Version note:** the 535 element header is the 48-byte shape above. Newer
branches (Blackwell-era) replace it with an "MCTP" framing (`"MCTP"` magic,
16-byte header, different payload layout). That format is **not** what
535.113.01 speaks, and mixing them produces a firmware that reads a magic number
where a length should be. Do not backport it.

### 2.2 The checksum

Stated as a rule, not as code:

1. Zero `checkSum` and the `+0x2C` padding word first.
2. Zero the bytes between the end of the RPC message and the next 8-byte
   boundary.
3. Read from the element start through the 8-byte boundary at or after
   `48 + rpc.length` — the last partial word is included whole — as a sequence
   of 64-bit words, and XOR them all into one 64-bit accumulator.
4. The stored `checkSum` is the high 32 bits of that accumulator XOR the low 32
   bits.

The result is self-checking in both directions: after the field is filled in,
re-running step 3 over the same region yields exactly 0. Both sides verify that
way (`NVIDIA/mq.c` `GspMsgQueueReceiveStatus` treats `checkSum != 0` as
`NV_ERR_INVALID_DATA`; `nvkm/rpc.c` writes the same fold). Note what the range
excludes: the padding *inside* the slot after the message is not hashed, so a
partially written slot can still checksum correctly — the checksum protects
framing, not staleness.

### 2.3 Sizing, spanning, wrapping

| quantity | value | tag |
|---|---|---|
| slot size | 4096 | SOURCE |
| element header | 48 | SOURCE |
| minimum message | a header-only RPC: 48 + 32 = 80 bytes, still one full slot | INFERRED |
| maximum single message | 16 slots = 65536 bytes total, i.e. `0x10000 - 0x30` = **65488** bytes of RPC | SOURCE — `GSP_MSG_QUEUE_ELEMENT_SIZE_MAX` = 16 × page |
| maximum payload | 65488 − 32 = **65456** | INFERRED (arithmetic) |
| larger messages | split: first message carries the real opcode, the rest are `CONTINUATION_RECORD` (opcode **71**) | SOURCE — `NVIDIA/mq.h` doc comment; `nvkm/rpc.c` `r535_gsp_rpc_push` |
| element 2..N contents | raw byte-continuation of the message; **no element header, no RPC header** | SOURCE — `NVIDIA/mq.c` `GspMsgQueueSendCommand` advances the source by one slot per element |
| `seqNum` across a split | assigned once for the whole message, identical in every element's header | INFERRED — the header is only physically present in element 1, so only its copy can matter |
| wrap | a multi-slot message may straddle the end of the ring; each slot is fetched by its own index | SOURCE — `nvkm/rpc.c` handles both halves |

### 2.4 Two different sequence numbers, do not confuse them

| number | where | scope | rule |
|---|---|---|---|
| element `seqNum` (`+0x24`) | every element header | per queue, per **message** | starts at 0, increments by 1 per message pushed, wraps naturally as a u32. The receiver keeps its own expected counter and rejects a mismatch (`NVIDIA/mq.c`: "Bad sequence number. Expected %u got %u") |
| RPC `sequence` (§3) | RPC header, byte 24 | per host, per **RPC** | host-assigned; `0` means "no sequence wanted" (used for fire-and-forget). Replies are matched by opcode, not by this field (§3.5) |

The element sequence is transport hygiene — it detects a slot that was written
twice, read out of order, or never written. The RPC sequence is bookkeeping for
the host's own outstanding-request table. Setting both from one counter is a
tempting shortcut and wrong: the RPC layer's `NOSEQ` path deliberately publishes
`sequence = 0` while still consuming an element sequence number.

### 2.5 How completion is signalled

There is no per-element acknowledgement, no completion bit, and no interrupt
required for correctness. Completion is three separate observable events:

| event | how it is observed | what it proves |
|---|---|---|
| the firmware **took** the message | the `readPtr` at status-block `+0x20` advances past the slots | the doorbell rang, the header was well-formed enough to parse, the checksum passed |
| the firmware **answered** | the status queue's `writePtr` (`statq+0x10`) advances | there is a message to read |
| the answer **belongs to our request** | the reply's RPC opcode equals the request's | §3.5 |

A host that only waits for "something appeared in the status queue" will
eventually match an event notification (opcode ≥ `0x1000`) to a request and
report success on the wrong message. All three checks are required.

After reading a message the host advances its own read pointer (cmdq block
`+0x20`, per §1.4) by `elemCount` slots. Not advancing it is not an error the
firmware reports — it just stops being able to write more replies once 62
messages are outstanding, which looks like a hang on the *next* call.

---

## 3. The RPC message format

### 3.1 Header

32 bytes at element offset `+0x30`; payload follows immediately, 8-byte aligned.

| offset (in RPC) | size | field | request value | reply value | tag |
|---|---|---|---|---|---|
| `+0x00` | u32 | `header_version` | `0x03000000` | `0x03000000` | SOURCE — major 3 in bits 31:24, minor 0 in bits 23:16 (`NVIDIA/rpc.h`) |
| `+0x04` | u32 | `signature` | `0x43505256` = bytes `V R P C` | same | SOURCE — `NV_VGPU_MSG_SIGNATURE_VALID` |
| `+0x08` | u32 | `length` | **total** bytes of RPC header + payload (not just payload) | same | SOURCE — `nvkm/rpc.c` sets `sizeof(hdr) + payload`; `NVIDIA/rpc.c` `headerLength` check |
| `+0x0C` | u32 | `function` | opcode, §4 | opcode echoed, or an event id | SOURCE |
| `+0x10` | u32 | `rpc_result` | `0xFFFFFFFF` (pending sentinel) | `0` = success, otherwise an `NV_STATUS` | SOURCE — `NV_VGPU_MSG_RESULT_RPC_PENDING` = `0xFFFFFFFF`; `NVIDIA/rpc.c` `_issueRpcAndWait` compares against 0 |
| `+0x14` | u32 | `rpc_result_private` | `0xFFFFFFFF` | secondary status, `0` on success | SOURCE — `NVIDIA/rpc.c` also checks this field for some RPCs |
| `+0x18` | u32 | `sequence` | host counter, or 0 for no-sequence sends | echo, or 0 | SOURCE for the field; echo behaviour **NOT ESTABLISHED** |
| `+0x1C` | u32 | `spare` / `cpuRmGfid` | 0 | 0 on a bare-metal host | SOURCE — union field |
| `+0x20` | ... | payload | | | |

Length rules that fall out of the header:

| rule | consequence | tag |
|---|---|---|
| `length` includes the 32-byte header | a header-only message has `length = 32` | SOURCE |
| `length` must be ≤ 65488 | larger must be split with continuation records (§2.3) | SOURCE |
| a request declares the **reply** size it can accept | `GET_GSP_STATIC_INFO` sends `length = 32 + 0xCF0` with a zeroed payload of that size, purely to tell firmware how much to send back | SOURCE — `NVIDIA/rpc.c` `rpcGetGspStaticInfo_v14_00` builds the header with `sizeof(GspStaticConfigInfo)` |
| payload alignment | 8 bytes; NVIDIA's structs use 8- or 16-byte-aligned members and rely on natural packing | SOURCE / INFERRED for the padding bytes themselves being zero |
| size classes | none. There is no small/medium frame; the only quantum is the 4096-byte slot and the 16-slot ceiling | INFERRED from the two constants |

That third rule is the one that bites: a naive `GET_GSP_STATIC_INFO` with
`length = 32` asks the firmware for zero bytes of reply. Whether firmware
truncates, refuses, or writes past the element is **NOT ESTABLISHED**; size the
request to the reply.

### 3.2 Version and signature handling

`header_version` is a bitfield, not an opaque magic: major version 3, minor 0.
The check that matters is the major version. What firmware does with a request
whose major is not 3 is **NOT ESTABLISHED** — the header comment in
`NVIDIA/rpc.h` describes the *invalid message* path (below) but not a
version-mismatch path, and no other source mentions one.

The signature **is** specified: if a message arrives with a bad length or a bad
signature, firmware rewrites the signature and length to valid values and sets
`rpc_result` to `NV_VGPU_MSG_RESULT_RPC_INVALID_MESSAGE_FORMAT` (`0xFF100002`).
That is a gift for bring-up: a malformed RPC produces a *reply that names
itself*, rather than silence. It only applies once the element framing itself
was good enough to deliver the message.

### 3.3 Result codes worth having on the wall

| value | name | when it appears |
|---|---|---|
| `0x00000000` | success | reply is valid |
| `0x00000051` | `NV_ERR_NO_MEMORY` | allocation RPCs (out of firmware heap) |
| `0x00000055` | `NV_ERR_NOT_READY` | called too early / subsystem still coming up |
| `0x00000066` | `NV_ERR_TIMEOUT_RETRY` | firmware asks the host to retry |
| `0xFFFFFFFF` | `RPC_PENDING` | not a status: the field was never filled in, or the message is a request, not a reply |
| `0xFF100001` | `RPC_UNKNOWN_FUNCTION` | opcode not implemented by this firmware |
| `0xFF100002` | `RPC_INVALID_MESSAGE_FORMAT` | bad signature or bad `length` |
| `0xFF100003` | `RPC_HANDLE_NOT_FOUND` | stale client/device/subdevice handle |

The first five are `NV_STATUS` values and are the ones a bring-up log will show;
the `0xFF10xxxx` block is RPC-layer-only. Treat any non-zero `rpc_result` as
failure and log it verbatim — the distinction between `0x51` and `0xFF100002`
tells you whether you are out of heap or sending garbage.

### 3.4 Opcodes used before anything else works

Numbers are from `nvkm/rpcfn.h`, whose header says it is an excerpt of the RM
headers at branch **535.113.01** — i.e. the same build as our firmware.

| opcode | name | direction | reply expected |
|---|---|---|---|
| 0 | `NOP` | host → FW | unknown; not a reliable liveness probe |
| 1 | `SET_GUEST_SYSTEM_INFO` | either | the vendor driver sends it after init; the independent implementation never does |
| 65 (`0x41`) | `GET_GSP_STATIC_INFO` | host → FW | yes, ~`0xCF0` bytes |
| 71 (`0x47`) | `CONTINUATION_RECORD` | both | part of a split message, never a request on its own |
| 72 (`0x48`) | `GSP_SET_SYSTEM_INFO` | host → FW, **pre-boot** | asynchronous, none awaited |
| 73 (`0x49`) | `SET_REGISTRY` | host → FW, **pre-boot** | asynchronous, none awaited |
| 76 (`0x4C`) | `GSP_RM_CONTROL` | host → FW | yes — the workhorse for everything after init |
| 103 (`0x67`) | `GSP_RM_ALLOC` | host → FW | yes — object allocation |
| `0x1001` | `GSP_INIT_DONE` | FW → host | unsolicited event; the init signal |

### 3.5 Matching a reply to a request

| mechanism | used? | note |
|---|---|---|
| opcode equality | **yes, primary** | both implementations match on the function number (`nvkm/rpc.c` `r535_gsp_msg_recv`; `NVIDIA/kgsp.c` `_kgspRpcRecvPoll` takes `expectedFunc`) |
| RPC `sequence` | present, **not used for matching** | no source shows firmware echoing it; assume it may be 0 |
| strict FIFO | true in practice, but do not rely on it for matching | events interleave with replies on the same queue |
| one outstanding request | **recommended** | both implementations serialise sends — a per-queue mutex in one, the GPU lock in the other. With opcode matching alone, two requests for the same opcode in flight are indistinguishable |

So: one request at a time, match on opcode, and **drain and log anything that
does not match** — events (`≥ 0x1000`) and stale replies both arrive on the same
queue, and a host that treats "next message" as "my reply" will happily parse an
event as a control reply.

---

## 4. The init handshake, in the order the firmware expects it

Everything below happens on queue 0. "Pre-boot" means the message is written
into the command queue **while the processor is not running yet**; firmware
consumes it as its first work item.

### 4.0 The states we are trying to reach

| state | observable | tag |
|---|---|---|
| GSP started | RISC-V core reports active; falcon front end can still read `UC_CTRL.STOPPED` | MEASURED — `0x110100 = 0x10` with GSP-RM fully up (the front end stays "stopped"; it is not a failure indicator) |
| transport alive | status queue header is valid and stable (§4.1, row 7) | SOURCE |
| RM initialised | `GSP_INIT_DONE` event with `rpc_result = 0` | SOURCE |
| RM answering | `GET_GSP_STATIC_INFO` returns sane handles | SOURCE |

### 4.1 Step table

| # | step | opcode / register | payload required | proof of success | bounded wait |
|---|---|---|---|---|---|
| 1 | build and zero the shared block; publish both producer headers; build the PTE array | — | §1.2–1.4 | host-side re-read of the headers | n/a |
| 2 | publish the argument page (§1.5) and the libos table (§1.6) | `0x110040/0x110044` | `sharedMemPhysAddr`, offsets, PTE count, geometry | none observable (mailboxes read 0 by design) | n/a |
| 3 | **pre-queue** system info | **72** `GSP_SET_SYSTEM_INFO` | `GspSystemInfo`, §5.1 | message sits in slot 0 with a correct checksum; `writePtr` = `elemCount` | none — do not wait |
| 4 | **pre-queue** registry table | **73** `SET_REGISTRY` | `PACKED_REGISTRY_TABLE`, §5.2 | as above, subsequent slots | none — do not wait |
| 5 | ring the doorbell | `0x110C00` ← 0 | — | nothing (write-only) | n/a |
| 6 | start the processor (booter, then RM) | — | — | other doc's scope | — |
| 7 | **link the status queue**: read firmware's producer header, validate it | block + `0x41000` + `0x00` | — | `version=0`, `size=0x40000`, `msgSize=0x1000`, `entryOff=0x1000`, `msgCount=63` | **4 s**, retrying (`NVIDIA/mq.c` `GspStatusQueueInit` uses a 4 000 000 µs timeout and re-links in a loop) |
| 8 | write our status-queue read pointer to the place §1.4 dictates | `cmdq+0x20` or `statq+0x20` | — | firmware's cmdq readPtr starts advancing | — |
| 9 | wait for the init event | **`0x1001`** `GSP_INIT_DONE` | none | message on the status queue with `function = 0x1001` **and `rpc_result = 0`** | 4 s suggested; the vendor driver resets its watchdog first to give RM a chance to report a failure rather than be killed |
| 10 | read static config | **65** `GET_GSP_STATIC_INFO`, request `length = 0x20 + 0xCF0` | zeroed 0xCF0-byte payload | reply `length ≥ 0xCF0`, `rpc_result = 0`, `hInternalClient/Device/Subdevice` non-zero | 4 s |
| 11 | (vendor driver only) guest system info | **1** `SET_GUEST_SYSTEM_INFO` | vGPU-shaped `rpc_guest_system_info` | `rpc_result = 0` | 4 s |

Notes on the rows that are surprising:

- **Row 3 and 4 are the only messages that exist before firmware does.** The
  vendor comment on `rpcSetRegistry_v17_00` is explicit — "this is an
  asynchronous RPC. It is stuffed into the message queue before the GSP is
  booted" — and the call site in `kgspBootstrapRiscvOSEarly_TU102` sits between
  the falcon reset and the booter launch. `nvkm` does the same thing in a
  different order (`r535_gsp_oneinit` → `set_system_info`, `set_registry`,
  then `r535_gsp_init` waits for `GSP_INIT_DONE`).
- **Neither is waited on.** There is no processor to answer yet. Any reply they
  might produce would arrive later, interleaved with `GSP_INIT_DONE`; the host
  must tolerate that (§3.5).
- **Row 7 is the first real proof of life.** Before it, "GSP started" only means
  a core is running. A valid status-queue header means the RM task reached the
  point where it created its own producer header — i.e. it parsed our argument
  page, found the block, and agreed with our geometry. It is worth logging the
  five fields verbatim; a `size` or `msgSize` that differs from ours means row 2
  was wrong, and that is a completely different bug from a firmware crash.
- **Row 9 carries its own verdict.** The event's `rpc_result` is the RM's init
  status. `0` means initialised; anything else is the reason it did not. Do not
  treat "an event with opcode 0x1001 arrived" as success without reading it.
- **Row 10 exists because row 9 does not hand you any handles.** Without
  `hInternalClient/Device/Subdevice` from static config there is no way to issue
  a control RPC, so this is the natural *first round trip*: a request whose reply
  is large, self-describing, and harmless.

### 4.2 What "initialised" finally looks like

Three things true at once, and nothing less:

1. `GSP_INIT_DONE` was received, with `rpc_result = 0`.
2. `GET_GSP_STATIC_INFO` round-tripped with `rpc_result = 0` and three non-zero
   handles.
3. The command queue's firmware-owned read pointer is advancing on each send —
   the observable that separates "firmware is answering" from "firmware answered
   once and died".

---

## 5. The payloads that have to be right

### 5.1 `GspSystemInfo` (opcode 72)

Offsets computed from `NVIDIA/sysinfo.h` under natural alignment; total size
**0x2C0 (704)**. This is the 535 shape — it has two fields
(`virtualConfigBits`, `bTdrEventSupported`) that older copies of the same struct
lack, and dropping them shifts nothing before `+0x278` but everything after it.

| offset | size | field | what the vendor driver puts there | our value for this card | tag |
|---|---|---|---|---|---|
| `+0x000` | u64 | `gpuPhysAddr` | BAR0 base | `0x80000000` (MEASURED from our own log) | SOURCE for the field, MEASURED for the value |
| `+0x008` | u64 | `gpuPhysFbAddr` | BAR1 base | 32 GiB aperture base, above 4 GiB (MEASURED) | SOURCE / MEASURED |
| `+0x010` | u64 | `gpuPhysInstAddr` | BAR2 base ("instance" window) | **NOT ESTABLISHED** — this card has no BAR2; it has a 32 MiB BAR3. The vendor code reads "BAR2_INST" which is 0 when absent | SOURCE for field; value open |
| `+0x018` | u64 | `nvDomainBusDeviceFunc` | PCI requester id: `(bus << 8) \| devfn` | e.g. `0x0100` for `01:00.0` | SOURCE — `pci_dev_id()` |
| `+0x020` | u64 | `simAccessBufPhysAddr` | simulation only | 0 | SOURCE |
| `+0x028` | u64 | `pcieAtomicsOpMask` | never assigned on this path | 0 | SOURCE (no assignment in `rpcGspSetSystemInfo_v17_00`) |
| `+0x030` | u64 | `consoleMemSize` | reserved console memory | 0 (no display console) | SOURCE for fill |
| `+0x038` | u64 | `maxUserVa` | OS userspace VA limit | **NOT ESTABLISHED** for a non-Linux host | SOURCE for fill |
| `+0x040` | u32 | `pciConfigMirrorBase` | physical address of a memory-mapped copy of this function's config space | ECAM address of `01:00.0` if we can express it | SOURCE for fill |
| `+0x044` | u32 | `pciConfigMirrorSize` | size of that window | 4096 | SOURCE |
| `+0x048` | u8 | `oorArch` | out-of-range architecture hint | 0 | SOURCE |
| `+0x050` | u64 | `clPdbProperties` | never assigned on this path | 0 | SOURCE |
| `+0x058` | u32 | `Chipset` | **never assigned on this path** | 0 (see §5.3) | SOURCE |
| `+0x05C` | 4×u32 | `bGpuBehindBridge`, `bMnocAvailable`, `bUpstreamL0sUnsupported`, `bUpstreamL1Unsupported` | PCIe topology flags | 0 unless known | SOURCE |
| `+0x06C` | 2×u32 | `bUpstreamL1PorSupported`, `bUpstreamL1PorMobileOnly` | as above | 0 | SOURCE |
| `+0x074` | u8 | `upstreamAddressValid` | 0/1 | 0 | SOURCE |
| `+0x076` | 10 | `FHBBusInfo` | never assigned on this path | 0 | SOURCE |
| `+0x080` | 10 | `chipsetIDInfo` | never assigned on this path | 0 | SOURCE |
| `+0x08C` | 488 | `acpiMethodData` | ACPI `_DOD`/`_MUX`/`_JT`/`_DSM` results; `bValid = 0` when absent | all zero, `bValid = 0` | SOURCE |
| `+0x274` | u32 | `hypervisorType` | 0 on bare metal | 0 | SOURCE |
| `+0x278` | u16 | `virtualConfigBits` | VF config bits | 0 | SOURCE |
| `+0x27C` | u32 | `bIsPassthru` | 0 on bare metal | 0 | SOURCE |
| `+0x280` | u64 | `sysTimerOffsetNs` | GPU-timer ↔ wall-clock offset | 0 is safe; a real offset only matters for timestamped logs | SOURCE |
| `+0x288` | 48 | `gspVFInfo` | SR-IOV VF info | all zero (not virtualised) | SOURCE |
| `+0x2B8` | u32 | `bTdrEventSupported` | 0/1 | 0 | SOURCE |

### 5.2 `SET_REGISTRY` (opcode 73)

A packed table, not a list of structs:

| offset | size | field |
|---|---|---|
| `+0x00` | u32 | `size` — total bytes of the table, including names and string/binary values |
| `+0x04` | u32 | `numEntries` |
| `+0x08` | 16 × N | entries: `nameOffset` (u32, from the table start), `type` (u8), `data` (u32; the dword value, or an offset for string/binary), `length` (u32) |
| after entries | | NUL-terminated key names, then string/binary values, packed contiguously |

Types: `1` = dword, `2` = binary blob, `3` = NUL-terminated string. The
`nameOffset`/`data` offsets are relative to the start of the *table*, and the
table starts immediately after the 32-byte RPC header.

Two honest gaps:

- **Which keys are mandatory is NOT ESTABLISHED.** The vendor driver ships a
  long static list plus command-line overrides; the independent implementation
  ships its own different list. Neither documents a minimum. An empty table
  (`numEntries = 0`) is well-formed and might be accepted; it might also leave
  RM with defaults it cannot boot from. Impact: an init failure whose only
  symptom is a non-zero `GSP_INIT_DONE` result.
- **Whether the table is size-limited per message is NOT ESTABLISHED.** It is
  split into continuation records when it exceeds one message (the vendor code
  has an explicit large-table path), so the answer is "handle splitting".

### 5.3 What happens when a field is wrong or zero

| field | zero / wrong | evidence | impact |
|---|---|---|---|
| `sharedMemPhysAddr` | firmware walks a page table that is not there | not observable in any source | hang at row 7 or row 3. **The highest-cost mistake in this document** — it is why the doc insists on a written-down address-translation assumption |
| `pageTableEntryCount` too small | firmware maps fewer pages than the queues need; the tail of the status queue is not mapped | — | replies land in memory firmware cannot write → looks like "consumed, never produced" |
| `cmdQueueOffset` / `statQueueOffset` wrong | firmware finds a header where there is none | — | row 7 fails validation (good) or firmware reads garbage geometry (bad) |
| `queueElementHdrSize` wrong | firmware's idea of where the RPC starts differs from ours | — | every message rejected as malformed, or silently misparsed |
| `magic` wrong | the whole argument page is ignored | — | row 7 never happens; no error anywhere |
| `Chipset` = 0 | **tolerated.** The vendor driver never assigns it | `NVIDIA/rpc.c` `rpcGspSetSystemInfo_v17_00` has no `Chipset` write | none for init on this chip |
| `gpuPhysFbAddr` = 0 | firmware cannot build its BAR1 mappings | — | post-init failures, not init failures |
| `maxUserVa` = 0 | unknown | — | unknown; the vendor driver always sets it |
| `GSP_INIT_DONE` result non-zero | that *is* the failure report | `NVIDIA/kgsp.c` asserts on it | read it, don't discard it |

The asymmetry is worth stating plainly: **the fields that matter for init are
the four addresses/geometry fields in the argument page; the fields in
`GspSystemInfo` that a naive reading suggests are critical (chipset id, BAR
sizes, firmware version) are either not sent at all or are ignored by firmware.**
There is no VRAM-size field and no firmware-version-string field anywhere in
either payload. Firmware learns its own VRAM from PFB and its own identity from
PMC.

### 5.4 Where the firmware version actually matters

Three places, none of them a string in a payload:

| mechanism | what it compares | tag |
|---|---|---|
| `header_version` major = 3 | RPC wire format, not firmware build | SOURCE |
| `GSP_ARGUMENTS_CACHED.size` / queue geometry | the host's expectation of the transport vs firmware's | INFERRED |
| `NV_VGPU_MSG_RESULT_GUEST_HOST_DRIVER_MISMATCH` (`NV_ERR_LIB_RM_VERSION_MISMATCH`) | host driver build vs RM build, reported by firmware | SOURCE — the result code exists in `NVIDIA/rpc.h` |

That last one is the honest answer to "what if the version string is wrong": you
do not get a version negotiation, you get a status code on some RPC after init.
The `GSP Firmware Version` string that `nvidia-smi` prints is read by the host
driver from its own bookkeeping of which blob it loaded, not from an RPC reply —
which is why a card whose GSP never came up prints `N/A` there.

One more version-ish field, for completeness: before waiting for
`GSP_INIT_DONE`, the vendor driver writes the boot image's `appVersion` into
`0x110080` (falcon "scratch 2", not a mailbox). **MEASURED:** that word reads 0
in both reference dumps, so it leaves no trace to compare against, and nothing
in the transport depends on it.

---

## 6. Failure modes that look like success

The theme: the transport has no negative acknowledgements. Most mistakes
produce *silence*, and silence is indistinguishable from "the firmware is
thinking". Every row below needs a bounded wait and a named diagnostic, not a
retry loop.

| # | symptom | most likely cause | bounded wait | diagnostic that distinguishes | safe next action |
|---|---|---|---|---|---|
| 1 | message written, doorbell rung, firmware's `readPtr` never advances | wrong `sharedMemPhysAddr` (CPU phys vs GPU bus); PTEs wrong; geometry fields wrong | 100 ms of polling `statq+0x20` | compare `readPtr` before/after; if it *ever* moves at all the address is right | stop. Do not re-ring indefinitely; a second doorbell does not help and obscures the first |
| 2 | `readPtr` advances, `writePtr` (status) never does | firmware took the message and died (assert, heap exhaustion, unsupported opcode) | 4 s | `readPtr` moved but no reply ⇒ message-level success, firmware-level failure. Check the RM log region (`LOGRM`) and `PMC` interrupt state | classify as firmware fault; do not resend |
| 3 | nothing at all, not even row 7 | firmware never reached its RM task; argument page ignored (`magic`, or mailbox address) | 4 s on the link retry | `size`/`msgSize`/`entryOff` mismatch vs. never-valid-at-all: the former means wrong offsets, the latter means firmware did not find the page | re-check §1.5/§1.6, not the queue code |
| 4 | both pointers frozen after the first few messages | **`SWAP_RX` disagreement** — read pointers written where the peer does not look | 62 messages (the ring fills) | `flags` word in the firmware's status header: 1 = agrees with us, 0 = do not place pointers as if swapped | re-place the read pointer per the negotiated value; §1.4 |
| 5 | message accepted, reply never arrives, but events do | reply was consumed and discarded as "unknown opcode", or opcode matched an event | 4 s | log every received opcode; if `0x1001`/`0x1006` show up, matching is broken | fix matching; re-send only after the queue is drained |
| 6 | reply arrives with `rpc_result = 0xFFFFFFFF` | the field was never filled — you are looking at a copy of your own request buffer, or at a pending/aborted message | immediate | `length` and `sequence` also equal the request's | treat as "no reply", not success. This is why requests carry the `0xFFFFFFFF` sentinel |
| 7 | reply with `0xFF100002` | bad `signature` or bad `length` (including `length` smaller than the header) | immediate | the code names it | fix the request; the element framing was fine |
| 8 | reply with `0xFF100001` | opcode unknown to *this* firmware build | immediate | — | wrong opcode for 535; check §3.4 |
| 9 | no reply, and the ring never drains past one slot | element `checkSum` wrong — firmware rejected the element before parsing it | 100 ms | recompute the checksum over `[start, start+48+length)` and compare with the stored value; both must be 0 after the field is filled | never resend a bad element; it will be rejected identically |
| 10 | every message "succeeds" but nothing ever works | `elemCount` understated (firmware reads one slot, message truncated) or overstated (firmware consumes slots belonging to the next message) | immediate, from `length` vs `elemCount` | `elemCount == ceil((48 + length)/4096)` is a one-line assertion | assert it in the send path |
| 11 | status queue stops after ~62 replies | host is not advancing its read pointer, or advancing it by 1 per *message* instead of `elemCount` | 62 messages | read pointer value vs. `elemCount` sum | fix accounting; firmware will not warn |
| 12 | element `seqNum` mismatch on receive | slot read twice (read pointer not advanced) or firmware restarted and reset its counter | immediate | expected vs actual, and whether the last read pointer advance landed | skip the stale element rather than aborting; log both numbers |
| 13 | `GSP_INIT_DONE` arrives with a non-zero result | RM init failed for its own reasons (heap, registry, unsupported config) | n/a | the value is an `NV_STATUS`; map it against §3.3 | report and stop; retrying the boot with the same inputs reproduces it |
| 14 | init succeeds, first control RPC returns `INVALID_STATE`/`NOT_READY` | called before the RM task is fully up, or with a stale handle | 4 s | the specific status distinguishes "too early" (`0x55`/`0x66`) from "wrong handle" (`0xFF100003`) | retry only on `0x55`/`0x66`, and only a bounded number of times |
| 15 | works on bare metal, hangs under any IOMMU/VFIO | `sharedMemPhysAddr` is the CPU physical address, not the GPU-visible one | row 1 | works/does-not-work split by environment | fix the address source, not the queue |

Two habits that prevent half of these: assert `elemCount` and `length`
arithmetic in the send path, and log every received opcode with its result —
including the ones you did not ask for. A GSP bring-up log with no `0x1001` line
is a different problem from one with `0x1001` and `result=0x51`, and only the
log can tell you which.

---

## 7. Implementation order: smallest independently verifiable steps

Each step is verifiable **without** the next one, and each failure names itself.
Steps 1–3 need no GPU at all.

| # | step | how it is verified | if it fails |
|---|---|---|---|
| 0 | **precondition** — firmware reaches "RISC-V running" (existing staged machine, stage 7) | the other lane's start signal | nothing below applies |
| 1 | build the block in host memory: zero 129 pages, fill 129 PTEs, publish both 32-byte producer headers | host-side re-read: header words equal the table in §1.3, each PTE equals `base + i*4096` | arithmetic bug; caught before any device write |
| 2 | **checksum self-test, offline**: build a synthetic 80-byte message, fill `checkSum`, re-hash, assert result is 0; then corrupt one byte and assert it is not 0 | pure unit test | the checksum rule is wrong; no point proceeding |
| 3 | **element packing self-test, offline**: a 9000-byte message must produce `elemCount = 3`, `seqNum` identical in all three headers, slot 2 starting at payload offset 8188 | unit test | sizing/wrap bug |
| 4 | publish §1.5 + §1.6, **start firmware with zero messages queued** | row 7 of §4.1: the status-queue header appears with `version 0 / size 0x40000 / msgSize 0x1000 / entryOff 0x1000 / msgCount 63` and is stable across two reads | if never valid: argument page wrong. If valid but `size`/`msgSize` differ: offsets wrong. This single step separates "firmware cannot see us" from "firmware disagrees with us" |
| 5 | now pre-queue **72** and **73** and repeat step 4 | same proof, plus: firmware's cmdq read pointer advances past those slots | if the pointers move but no event: `GSP_INIT_DONE` path broken; if nothing moves: doorbell or element framing |
| 6 | receive `GSP_INIT_DONE`, check `rpc_result` | one log line, `fn=0x1001 result=0` | a non-zero result is the firmware's own diagnosis — stop and read it |
| 7 | **first real round trip:** `GET_GSP_STATIC_INFO` (65) with `length = 0x20 + 0xCF0` | reply `length ≥ 0xCF0`, `rpc_result = 0`, `hInternalClient/Device/Subdevice` non-zero, `fb_length` ≈ 24 GiB less the WPR carve-out | if the reply is truncated, the request length was too small; if `rpc_result` is non-zero, §3.3 names it |
| 8 | parse the reply's FB region table and sanity-check it against 24 GiB | sum of usable regions < 24 GiB, top of the last region ≈ `0x5FF...` | a wrong `Chipset`/`gpuPhysFbAddr` would show here, not earlier |
| 9 | re-run step 7 in a loop, 100 times | read pointer advances every time; no leaked slots; `GSP_INIT_DONE` does not repeat | queue accounting bug (§6 row 11/12) |
| 10 | only now: `GSP_RM_CONTROL` (76) with the simplest read-only control | `rpc_result = 0` and a payload that matches the control's shape | — |

Step 4 is the load-bearing one. It requires no correct RPC at all, no message
format, no checksum — just a well-formed argument page and two queue headers. If
it passes, every later failure is a message-format problem. If it fails, no
amount of RPC debugging will help, and the diagnostic is unambiguous: either no
valid status-queue header (firmware never found the block) or one with different
geometry (firmware found it and disagreed).

---

## 8. Explicit unknowns, and what each one costs

| # | unknown | impact if wrong | how to settle it cheaply |
|---|---|---|---|
| 1 | firmware's reaction to a bad element `checkSum` (drop silently / assert / reply) | a checksum bug looks exactly like a dead queue (§6 row 9) | step 2's self-test plus step 5's pointer watch: a bad checksum that still advances `readPtr` means "dropped after parse" |
| 2 | whether firmware validates the command queue's element `seqNum` | a sequence bug may pass for a while and then fail mysteriously | start at 0 and increment monotonically; if step 9's 100-message loop is clean, this is settled empirically |
| 3 | which registry keys are mandatory | init fails with only a status code to show for it | step 6's result; then bisect the reference list |
| 4 | `gpuPhysInstAddr` for a card with no BAR2 (this one has BAR3 = 32 MiB) | some RM features, possibly init | step 6/8: if init fails, try BAR3; log whichever was sent |
| 5 | `maxUserVa` semantics on a non-Linux host | unknown; the vendor driver always sets it | set a plausible non-zero limit, log it, see whether init changes |
| 6 | whether a `pciConfigMirror` window is required | firmware config-space reads (power, link training) | provide it if cheap; otherwise watch for failures that mention config space |
| 7 | whether zero-sized `LOGINIT`/`LOGINTR`/`LOGRM` regions are tolerated | init failure with no log to explain it | provide three real 4 KiB regions; removes the question |
| 8 | what `NV_PGSP_MAILBOX(0..3)` at `0x110804` is for | none for the first round trip; may matter for interrupts later | not touched by either reference during RPC; leave it |
| 9 | whether the RPC `sequence` field is echoed | only affects bookkeeping, since matching is by opcode | log it on every reply for the first 100 messages |
| 10 | interrupt-driven status delivery (vector, mask, ack) | none — polling is sufficient, and is what the independent implementation does | defer until RPC works; then read the interrupt table from static config |
| 11 | whether `SET_GUEST_SYSTEM_INFO` (1) is required before control RPCs | if required, every control RPC returns `INVALID_STATE` | step 10 will show it; adding the call is cheap |
| 12 | exact `GSP_INIT_DONE` payload (if any) | we only need the header's `rpc_result` | log `length`; if non-zero, dump it |

None of these unknowns can wedge the card on their own. The two that can are
already pinned down: the queue block's **physical address** (§1.5, GPU-visible,
must be right) and the **argument page's magic/geometry** (§1.5, wrong values
mean firmware reads nothing). Everything else fails into a status code or a
stalled queue, i.e. into a diagnostic.
