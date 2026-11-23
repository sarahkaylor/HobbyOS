# SWARM-REPORT — WP-C: the GSP-RPC transport, offline

Branch `nv/wp-c-rpc`, cut from integration tip `ce4c44af`. One code commit
(`feat(nv-rpc): GSP-RPC transport, spec T-01..T-24 green in the unit tier`) plus
this report.

The whole of `docs/nv-rpc-implementation-spec.md` that can run without a card is
implemented and green in the kernel unit tier: the 129-page block and PTE map,
both queue headers, 48-byte element framing, the whole-slot checksum (spec §0.1,
**not** the contract's short range), SWAP_RX negotiation, wrap-around, the
one-empty-slot rule, the 16-slot ceiling, continuation splitting, doorbell
commit-vs-notify, and receive-side verification (checksum, sequence gap/stale,
event-vs-reply classification).

## Files

| file | what |
|---|---|
| `src/include/nv_rpc.h` | new. Wire offsets (slot-relative and RPC-relative), the reason enums, `struct nv_rpc_hw` (the 3-pointer seam), `struct nv_rpc_queue`, every prototype. |
| `src/kernel/nv_rpc.c` | new. The pure half plus, under `#ifdef __x86_64__`, the real seam (`nv_rpc_hw_dev`: doorbell → `nv_wr32(d, 0x110C00, 0)`, fence → `arch_memory_barrier`, clock → `timer_get_us`) and `nv_rpc_dump` for the `[NV-RPC]` row. |
| `src/kernel/nv_rpc_test.c` | new. T-01..T-24, one `TEST` per row, on synthetic queues. |
| `src/kernel/unit_test.c` | one registration line inside the existing `#ifdef __x86_64__` block. |
| `src/kernel/unit_test.h` | one prototype line, next to `nv_test_suite()`/`nv_fw_test_suite()` (that is where those two are declared). |

No Makefile entry: kernel TUs are `$(wildcard $(SRC_DIR)/*.c)` (`Makefile:752`).

**Dependency decision honoured.** `nv_rpc_block_build(block, block_bus)` builds
every PTE from `block_bus`; nothing here calls `nv_dma`, and no bus address is
spelled `phys` anywhere in the interface. This branch links standalone. The
doorbell's register number is `NV_RPC_DOORBELL_OFF` (0x110C00 = `NV_FALCON_GSP_BASE`
+ 0xC00, SOURCE), not a copy of `nv_gsp.c`'s static macro.

## Gate: exact lines

Unit tier (`nvswarm sh wp-c 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh'`):

```
Tests run:    537
Tests failed: 0
UNIT TESTS PASSED
```

Baseline was **513 tests, 0 failed**; this suite adds **24** cases → **537**.
Zero `FAILED` lines anywhere in that log, and all 24 `Running test_tNN_…` lines
appear (no early return).

ARM: **the full ARM link is a known pre-existing failure at `ce4c44af`**
(`ld.lld: error: kernel .bss exceeds the limine placement window`, reproduces on
an untouched tree on both hosts), so the gate is a per-TU compile of everything
this work package adds:

```
nvswarm sh wp-c 'TOOLS_SKIP=1 BROWSER_SKIP=1 make ARCH=arm MODE=unit_tests obj/arm/nv_rpc.o obj/arm/nv_rpc_test.o'
clang -O2 -Wall -Wextra … --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only … -c src/kernel/nv_rpc.c -o obj/arm/nv_rpc.o
clang -O2 -Wall -Wextra … --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only … -c src/kernel/nv_rpc_test.c -o obj/arm/nv_rpc_test.o
```

Both TUs compile with no warnings. `llvm-objdump -d obj/arm/nv_rpc.o` → **0**
`q`-register references and 0 wide vector loads/stores, i.e. `-mgeneral-regs-only`
held and nothing x86-only leaked outside its guard. `nv_rpc.c` compiles on ARM as
the real transport (not an empty TU); only the seam/dump block is x86-only, which
is where the ARM build would have caught an unguarded `nv.h`/`nv_dev` reference.

## Case table (all MEASURED: green in the run above)

| # | case | what the test asserts | result |
|---|---|---|---|
| T-01 | block geometry | `pte_count 129`, `cmd 0x1000`, `stat 0x41000`, block `0x81000`, msgCount 63; `pte[0]==bus`, `pte[i]==bus+i*0x1000` for all 129, `pte[128]==bus+0x80000`, the table stops at 129; misaligned and zero bus refused; **then the same build over a real `frame_alloc_contig(129)`** | PASS |
| T-02 | cmdq header published | `+00 0`, `+04 0x40000`, `+08 0x1000`, `+0C 63`, `+10 0`, `+14 1`, `+18 0x20`, `+1C 0x1000`, `+20 0`; `rxHdrOff+4 <= entryOff`; nothing dirty past the 36 defined bytes | PASS |
| T-03 | statq header not written | bytes `0x00..0x23` of statq all zero; `link_validate` → `-EAGAIN`; `link_apply` → `-EAGAIN`, `linked == 0` | PASS |
| T-04 | RMARGS cross-check | correct view → `0`; `statQueueOffset = 0x40000` → `NV_RPC_ARG_STAT_OFF` and the name is `"statQueueOffset"`; four more single-field drifts each named correctly | PASS |
| T-05 | checksum golden vectors | three fixtures built byte-by-byte from §2.3/§2.4: (a) NOP len 32 → `40505277`, (b) fn 65 len 0xD10 seq 7 → `40505f01`, (c) fn 76 len 9000 seq 3, 3 slots, 8968 payload bytes → `40507132`. Each checksum is BOTH accepted by `elem_check` AND produced by `elem_build` from a cleared word | PASS |
| T-06 | checksum self-zero | re-fold `== 0` and `acc_lo == acc_hi`; fold is order-independent (two halves XOR to the whole); checksum word is nonzero; the slot tail `[48+len, 4096)` is zero even though the buffer arrived full of noise | PASS |
| T-07 | stale tail rejected | `0xAA` at offset `48+32` → `elem_check` `-EIO`/`CHECKSUM`; **and the same fixture still folds to 0 over `48+32` bytes**, i.e. the old short-range rule would have accepted it; clearing the byte restores it | PASS |
| T-08 | SWAP_RX both ways | `(1,1)` → own `0x1020`, peer `0x41020`; `(1,0)` → own `0x41020`, peer `0x1020`; `(0,1)` → not swapped (the AND, not the shortcut). In memory: swapped, one consume moves `cmdq+0x20` to 1 and leaves `statq+0x20` at 0; unswapped, the mirror image | PASS |
| T-09 | doorbell not written | after `tx_prepare`+`tx_commit`: writePtr advanced by `ec`, the ring element passes `elem_check`, `doorbell_calls == 0`, `tx_seq == 0`; then `kick` → 1 call, last arg 0, `tx_seq == 1`; `kick(q, NULL)` → `-EINVAL` and no extra call | PASS |
| T-10 | doorbell value/offset | through the real `nv_rpc_hw_dev` → `nv_wr32` → fake `struct nv_mmio_ops`: **exactly one** write, offset `0x110C00`, value `0`; a queue index of 1 writes nothing | PASS |
| T-11 | wrap-around | `writePtr 61`, `free == 3`, 3-slot message: slots 61/62/0 written, `writePtr == 1`, slot 0 == stream offset 8192; third slot is RPC offset **8144** = payload offset **8112** → byte `0xB0`, and it is *not* `8188 & 0xFF` (§0.3's off-by-44). The reader reassembles the same bytes across the wrap and reads the same `0xB0` | PASS |
| T-12 | full queue | `free == 2`, 3-slot message → `-ENOSPC`/`NO_SPACE`, writePtr unchanged, all three candidate slots still `0x5A`; `tx_busy` → `-EBUSY`/`BUSY` | PASS |
| T-13 | empty queue | fresh: writePtr == readPtr == 0 → `free == 62` (not 63), `rx_take` → `-EAGAIN`/`NOTHING`, read ptr unchanged; linked: `(0,0)` → 62, `(read 0, write 62)` → **0** although one slot is physically free, `(read 2, write 0)` → 1 | PASS |
| T-14 | oversize | `len 65488` → `ec 16` and `tx_prepare` accepted (48+65488 == 16·4096); `65489` → `-EMSGSIZE`/`TOO_BIG`; `31` → `-EINVAL` | PASS |
| T-15 | elemCount arithmetic | `32,4047,4048 → 1`; `4049 → 2`; `8144 → 2`; `8145 → 3`; `12240 → 3`; the boundary `48+4048 == 4096` computed in the test | PASS |
| T-16 | bad checksum on receive | flip `+0x2C` → `-EIO`/`CHECKSUM` after **3** retries, `read pointer unchanged`, second call sees the same element and still refuses; repairing the byte makes the same call succeed, and only `release` moves the pointer | PASS |
| T-17 | mismatched sequence | `rx_seq 5` then `seq 7` → `-EIO`/`SEQ_GAP`, not consumed; `seq 3` → consumed, `-EAGAIN`/`SEQ_STALE`, read ptr +1, `rx_stale == 1`, `rx_seq` still 5 | PASS |
| T-18 | sequence wrap | `tx_seq 0xFFFFFFFF` → element `seqNum == 0xFFFFFFFF`; after `kick`, `tx_seq == 0` (a u32 wrap, not a 64-bit counter) | PASS |
| T-19 | continuation split | `split_plan(70000)` → `[65456, 4544]`, count 2, each `32+chunk <= 65488`; msg 0 is function **76** with seq 10 and `elemCount 16`, msg 1 is function **71** with seq 11 and `elemCount 2`; `65456 → 1`, `0 → 1`, `max_chunks 1 → -EMSGSIZE` | PASS |
| T-20 | short publish | 3-slot element published as 2 → `-EAGAIN`/`SHORT_PUBLISH`, read ptr unchanged; `elemCount 17` → `BIG_ELEM`, `0` → `BAD_EC` (both `-EINVAL`, neither consumed) | PASS |
| T-21 | read pointer not advanced | take twice without release → same element, same 64 bytes, read ptr still 0, avail still 2; `release` → pointer 1 and `rx_seq 1`; third take is the *next* message; a second `release` with nothing taken → `-EINVAL` and no movement | PASS |
| T-22 | reply answered by an event | queue holds `fn 0x1001 result 0` then `fn 65 result 0`, waiting for 65: returns length 32 with `function == 65`, `rx_events == 1`, read ptr 1 (event consumed, reply not), `dispatch_log == [0x1001, 65]` | PASS |
| T-23 | link size | `size 0x20000` → `-EINVAL`, field `size`, name `"size"`; `nv_rpc_link` returns at once with `PEER_GEOM` (a written-but-wrong header never improves); reserved flag bit 1 → `-EINVAL`/`flags`; an all-zero header times out at the fake clock (`-ETIMEDOUT`/`NOT_LINKED`, `fh.now >= 400000`, no sleeping); then the firmware appears and the same call links with all five fields | PASS |
| T-24 | msgCount recomputed | `msgCount 62` → `-EINVAL`/`msgCount`, `(0x40000-0x1000)/0x1000 == 63` computed in the test; correct → 0; `entryOff 0x1100` → `entryOff`; `msgSize 0x800` → `msgSize` | PASS |

## The suite can fail: two mutation runs, both reverted

Reverted with `git checkout -- src/kernel/nv_rpc.c` each time; the run after the
revert is the green one quoted above.

**Run 0 — first ever run, no mutation, my own implementation.** `537 run / 11
failed`. Every one traced to a real defect, and five were fixed:

- `elem_build` compared **both** accumulator halves against 0 instead of against
  **each other**. After storing the fold, `acc_lo == A.lo ^ csum == A.hi`, which
  is generally nonzero. T-06 caught exactly this (it is what the `acc_hi ==
  acc_lo` assert is for). → implementation fixed.
- Tests read the received function code at a slot-relative offset from an
  RPC-relative buffer (`rx_out + 0x3C` instead of `+0x0C`) — and read 0xB2B2B2B2
  left over from the previous test, which is how it was caught. → RPC-relative
  `NV_RPC_H_*` offsets added and used.
- `T-13` exercised the ring arithmetic on an **unlinked** queue (which returns
  the pre-boot 62 regardless), so the assertion was vacuous. → links first.
- `T-09`/`T-18` committed a staging buffer without ever calling `elem_build`
  (`tx_commit` expects a finished element). → helper finishes the element.
- `T-01`'s "the table stops at 129" assertion was inverted (it demanded the
  130th PTE be **nonzero**).

**Run 1 — mutation A: hash the short range instead of the whole slot.**
`elem_build`/`elem_check` hash `ALIGN(48 + length, 8)`.

```
Tests run:    537
Tests failed: 1
UNIT TESTS FAILED
EXPECT_EQ FAILED: nv_rpc_elem_check(tx_stage, 1, &why) == -EIO at src/kernel/nv_rpc_test.c:504
```

Red: **T-07** only. That is the correct and expected result, and it is worth
writing down why the others stay green: with a zeroed tail, XOR with zero is the
identity, so T-05's three golden vectors numerically coincide under both rules
(spec §0.1 says so in as many words) and T-16's flipped byte is at `+0x2C`,
inside the 48-byte element header, which both rules cover. **T-07 is the only
test that can distinguish the two domains, and it does.** A suite whose only
checksum test was T-05 would have shipped the contract's wrong rule.

**Run 2 — mutation B: drop the one-empty-slot rule** (`free = (pr + 63 - wp) % 63`
and the pre-link allowance becomes `msg_count`).

```
Tests run:    537
Tests failed: 5
UNIT TESTS FAILED
ASSERTION FAILED: nv_rpc_tx_prepare(...) == 0               (T-09, nv_rpc_test.c:589)
EXPECT_EQ FAILED: nv_rpc_free_slots(...) == 3               (T-11, :689)
EXPECT_EQ FAILED: free_slots == 2                            (T-12, :765)
EXPECT_EQ FAILED: nv_rpc_free_slots(...) == 62               (T-13, :806)
ASSERTION FAILED: nv_rpc_tx_prepare(...) == 0                (T-18, :979)
```

Red: **T-09, T-11, T-12, T-13, T-18**. The direction it shows is the one that
matters: with `read == write` the free count becomes **0**, so an empty ring
looks *full* (T-09/T-18 cannot send at all) while a ring with one physical slot
left looks like it has room (T-12). That is the "both sides desynchronise
permanently, with no diagnostic" failure the rule exists to prevent.

## Two defects found in the spec by writing its arithmetic as tests

1. **§2.5's accessor table drops a digit.** It prints `0x4120` for `statq + 0x20`
   and `0x4110` for `statq + 0x10`. The status queue is published at `0x41000`,
   so the block-relative offsets are **`0x41020`** and **`0x41010`**
   (`0x41000 + 0x20`, `0x41000 + 0x10`). The symbolic form in the same table
   ("statq + 0x20") is the contract; the hex is wrong. T-08 computes both sums in
   the test. The implementation uses `stat_off + offset`, so it was right and the
   test expectation (copied from the table) was wrong — found on the first run.
2. **§4.3 S6's `acc_hi == acc_lo` means "equal", not "both zero."** Storing the
   fold makes the halves equal *to each other*; they are zero only in the
   degenerate case `csum == 0`. My first implementation read the spec's phrase as
   "both zero" and rejected every valid element; T-06 pinned it.

## What is NOT verified

Tagged honestly; none of these can be settled offline (spec §8).

- **That the firmware walks our PTE table.** Needs the card. If `bus` is not the
  GPU-visible address the link never validates; nothing else here can tell.
- **That `SWAP_RX` is really 1 on this firmware.** Both polarities are implemented
  and tested; which one the firmware picks is unknown until its header appears.
- **That our checksum domain matches the receiver exactly.** The rule is
  implemented as §0.1 states and anchored to the spec's own vectors, but only the
  receiver can confirm it. Symptom if wrong: element consumed, never parsed.
- **That `0x110C00` is the right doorbell register on AD102.** MEASURED only as
  "reads 0"; the transport is correct, the register choice is not proven.
- **Continuation *assembly* on the receive side.** Splitting is complete
  (`split_plan` + per-chunk framing, T-19). `rx_take` classifies function 71,
  consumes it and counts it (`rx_continuations`), but does not reassemble: the
  total is not on the wire, so assembly needs a per-opcode reply-size table that
  this lane does not have.
- **P3's per-opcode payload-size check.** `elem_build` verifies that the RPC's
  `length` field equals the length being framed and that the signature is ours;
  it cannot verify "`length` == 32 + the payload this opcode declares" without
  that table. No claim is made that it does.
- **The real `nv_dma` bus value.** By decision this branch takes `block_bus` as
  an argument; the identity `bus == phys` and the VT-d refusal live in WP-D.
- **Allocation ordering** (§6.3: allocate before the desktop). T-01 allocates and
  frees 129 contiguous frames inside the tier to prove the pool can serve the
  size, which is not the same as the boot path doing it early enough.
- **Thread safety.** `tx_busy` is a caller-owned field; no lock is taken in the
  pure half (the unit tier is single-threaded). The glue layer owns the lock.
- **`nv_rpc_dump` output.** Compiled and reachable but not exercised by a test
  (it is the on-card diagnostic, and there is no card).

## Next step

1. WP-D lands `nv_dma_alloc`/`nv_dma_bus`; the glue in `nv_gsp.c` stage 8 then
   calls `nv_rpc_block_build(va, nv_dma_bus(&map))` and publishes the RMARGS page
   from the same geometry struct (T-04's cross-check is already the guard).
2. Add `struct nv_rpc_queue rpc; uint64_t rpc_block_phys;` to `struct nv_dev` and
   wire `nv_rpc_hw_dev(&nv, &hw)` — the seam exists for exactly that call site.
3. Then the first on-card observation is §4.2's link: one `[NV-RPC] stage=link`
   row with the peer's five fields verbatim. "never valid" means the firmware
   never found the block; "valid but different" means the RMARGS offsets were
   wrong — T-23 already distinguishes those paths.
