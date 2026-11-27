# The round-trip harness: proving the PEER moved its header

**One line:** `src/kernel/nv_rpc_harness.c` takes two reads of the peer's 32-byte
producer header — one before the first message is sent, one after the answer —
and PASSES only if those two reads differ **and** no store of ours in this run
overlaps the bytes it read. Our own write pointer is logged on its own row,
labelled `side=ours`, and is never one side of the comparison.

Tags as in `docs/nv-gsp-rpc-contract.md`: **MEASURED** (read out of this tree's
own run on this VM), **SOURCE** (the wire format the code implements, cited),
**INFERRED** (a reading of the code, with the test that pins it).

---

## 1. The false positive this exists to rule out

On the card there are two 32-bit "how far have we got" words in the same block,
at the same offset in two different queue headers:

| word | block offset | who writes it |
|---|---|---|
| our writePtr (producer index into the command ring) | `cmdq + 0x10` = `0x1010` | **us** |
| the peer's writePtr (producer index into the status ring) | `statq + 0x10` = `0x41010` | **the peer** |
| our status readPtr under `SWAP_RX` | `cmdq + 0x20` = `0x1020` | **us** |
| the peer's command readPtr under `SWAP_RX` | `statq + 0x20` = `0x41020` | **the peer** |

They are the same shape of word. A harness that reads back a location it just
wrote, sees the number go up, and reports progress has proved only that its own
store landed — the classic false positive for this bring-up. The failure is
silent: nothing errors, the counter looks healthy, and the firmware was never
involved.

Three mechanisms make the claim honest.

### 1.1 Two reads of the same peer-owned bytes

`pre` is a 32-byte copy of `statq + 0x00..0x1F` taken **before the first message
is sent**; `post` is the same 32 bytes taken **after the answer arrived** (or
after the reply wait expired). The verdict compares `pre` and `post` and nothing
else. There is no cached mirror, no struct, and no comparison against `q->peer`,
`q->write_ptr` or any other value the driver holds. Both reads are validated
with `nv_rpc_link_validate()` **on the copied bytes**, so the row and the verdict
describe the same read.

### 1.2 The window stops at +0x1F on purpose

The 36-byte defined producer header ends at `+0x24` (`usedBytes`). The harness
reads 32 bytes — `version, size, msgSize, msgCount, writePtr, flags, rxHdrOff,
entryOff` — and **excludes the read pointer at `+0x20`**, because with `SWAP_RX`
clear that word is *ours* (§1.4 of the contract: we publish our status read
index there). Including it would let our own store manufacture exactly the
movement the harness is supposed to detect.

Excluding it can only make a false PASS *less* likely: under both polarities the
eight words above are the peer's alone. **INFERRED**, pinned by T-29 and T-32:
a run in which our write pointer moves and the peer's header does not is
reported `reply_timeout` / `peer_static`, never `pass`.

RESIDUAL RISK, stated because it is real: if the peer's writePtr wrapped all the
way round to its pre-send value during the run, `pre == post` and a working round
trip would be reported as `peer_static`. That direction is a false FAIL, which is
the direction this harness is allowed to be wrong in. One message advances the
peer's producer index by 1, so it needs 62 messages between the two reads to
happen.

### 1.3 Every store of ours is journalled

`nv_rpc.c` records the range of every store it makes into the block
(`nv_rpc_journal()`), at the three sites that write:

| site | range recorded |
|---|---|
| `nv_rpc_link_apply()` — readPtr publication | `4` bytes at the negotiated read-pointer offset |
| `nv_rpc_tx_commit()` — each slot, then writePtr | `4096` bytes per slot; `4` bytes at `cmd_off + 0x10` |
| `rx_advance()` via `nv_rpc_rx_take`/`rx_release` | `4` bytes at the negotiated read-pointer offset |

The harness asserts `nv_rpc_journal_overlap(q, stat_off, 32) == 0` at the pre
read **and again** at the post read. `nv_rpc_journal_reset()` at the top of the
run is the run boundary: after it, the journal describes only this exchange.
Entries that fall out of the 32-entry ring are counted (`jrn_lost`), and a
non-zero count is a FAIL (`journal_overflow`) rather than a silent "nothing
found" — the two answers are different and the harness does not conflate them.

`nv_rpc_block_build()` is deliberately **not** journalled: it writes all 129
pages by definition (the status header is zeroed there on purpose) and it runs
before a queue exists. The run starts after the build.

### 1.4 Ours is printed as ours

The send path's own pointer is a row of its own:

```
[NV-RPC] stage=writer side=ours addr=… wp=… prev=… ec=…
```

`addr` is `block_bus + 0x1010` — a **different address** from the peer row's
`block_bus + 0x41000`, and the row says `side=ours` in the same position where
the peer rows say `side=peer`. A reader never has to infer which number is whose.

---

## 2. The row grammar (quote verbatim)

One row per line. The harness emits the text **without** a newline; the sink
terminates the line (the uart sink is `uart_puts(row); uart_puts("\n")`).

```
[NV-RPC] stage=<name> key=value key=value ...
```

Formatting rules, all pinned by the verbatim assertions in T-27:

| rule | why |
|---|---|
| every **address** is `0x` + **16 uppercase hex digits**, never fewer | it is a 64-bit bus address, the shape `uart_print_hex()` already prints |
| every **32-bit wire word** is `0x` + **8 uppercase hex digits** | the width itself says "header field", never "address" |
| every counter is **decimal**, no leading zeros | `wp` is a wire word and `ec` is a count; the eye should not have to guess |
| `side=peer` / `side=ours` appears on every row that names a pointer or a word | the whole point of the harness |
| keys are lowercase, values contain no spaces | a row splits into fields on whitespace with no quoting rules |
| `verdict=pass` or `verdict=FAIL` | `FAIL` in capitals, the same loud marker `nv_gsp.c` already uses |

### 2.1 The five rows of a round trip, in emission order

The rows below are **MEASURED**: they are the exact strings T-27 asserts,
byte for byte, against the synthetic queues (bus `0x20000000`, one 32-byte
GET_STATIC_INFO probe, no payload, a frozen clock so `waited_us=0`).

```
[NV-RPC] stage=peer phase=pre side=peer addr=0x0000000020041000 off=0x0000000000041000 v=0x00000000 size=0x00000000 msgsize=0x00000000 count=0x00000000 wp=0x00000000 flags=0x00000000 rxhdroff=0x00000000 entryoff=0x00000000 link=0
[NV-RPC] stage=writer side=ours addr=0x0000000020001010 off=0x0000000000001010 wp=0x00000001 prev=0x00000000 ec=1
[NV-RPC] stage=send side=ours fn=0x00000041 len=0x00000020 ec=1 seq=0 doorbells=1
[NV-RPC] stage=reply side=peer fn=0x00000041 len=0x00000020 result=0x00000000 seq=1 ec=1
[NV-RPC] stage=peer phase=post side=peer addr=0x0000000020041000 off=0x0000000000041000 v=0x00000000 size=0x00040000 msgsize=0x00001000 count=0x0000003F wp=0x00000001 flags=0x00000001 rxhdroff=0x00000020 entryoff=0x00001000 link=1
[NV-RPC] stage=roundtrip verdict=pass reason=ok peer_moved=1 moved=7 first=0x00000006 pre_wp=0x00000000 post_wp=0x00000001 ours_wp=0x00000001 ours_wrote_peer=0 answers=1 waited_us=0
```

Field meanings:

| row | field | meaning |
|---|---|---|
| `peer` | `phase` | `pre` = before the first message is sent; `post` = after the answer or the reply timeout |
| | `addr`, `off` | GPU-visible (`block_bus + stat_off`) and block-relative address of the 32 bytes read |
| | `v size msgsize count wp flags rxhdroff entryoff` | the eight producer-header words, decoded from those bytes |
| | `link` | `1` when those bytes validate as a producer header, `0` when all-zero or unusable |
| `writer` | `addr`, `off` | `block_bus + 0x1010`, `0x1010` — **ours**, not the peer's |
| | `wp`, `prev` | the word after and before `nv_rpc_tx_commit()`'s single store |
| | `ec` | elemCount the message occupies |
| `send` | `fn`, `len` | the request's RPC function and length (header + payload) |
| | `seq`, `doorbells` | the element's seqNum, and 1 (a kick that did not ring is `-EINVAL`, never 0) |
| `reply` | `fn`, `len`, `result` | read back **out of the answer bytes**, not from a copy of the request |
| | `seq`, `ec` | the queue's reply counter after release (exactly one), and the element's elemCount |
| `roundtrip` | `verdict`, `reason` | `pass`/`FAIL` and the reason name (§3) |
| | `peer_moved`, `moved`, `first` | 1/0, how many of the 32 bytes differ, the first differing offset |
| | `pre_wp`, `post_wp` | the peer's writePtr in each of the two reads — **this** is the number that must move |
| | `ours_wp` | our write pointer, repeated so the two live side by side in one row |
| | `ours_wrote_peer` | 1 when our journal intersects the window — a FAIL, always |
| | `answers`, `waited_us` | replies consumed (0/1), and the time both waits spent |

### 2.2 Failure rows

A failing run emits the rows it reached, then the verdict, then these two:

```
[NV-RPC] stage=replay n=3 note=rows-follow-verbatim
<N> rows, byte-identical to the ones already emitted, oldest first>
[NV-RPC] stage=words side=peer addr=0x… off=0x… n=9 w00=0x… w01=0x… … w08=0x…
```

`stage=words` is the **whole** 36-byte producer header (nine words, raw, read
live at the moment of failure), so a report can quote a header nobody has to
decode. `stage=replay` is a marker line; the rows that follow it are the last
six rows verbatim (a prefix on each would truncate a 230-byte peer row, and a
truncated copy is not the same row).

---

## 3. Verdicts

| verdict | rc | meaning |
|---|---|---|
| `ok` | 0 | answer received, the peer's header moved, no store of ours in the window |
| `no_seam` | -EINVAL | no doorbell or no `now_us`: a wait that cannot be bounded is refused at init |
| `journal_overflow` | -EIO | our journal lost entries; disjointness cannot be proved |
| `ours_overlap` | -EIO | our own store intersects the peer window — outranks every other reason |
| `link_timeout` | -ETIMEDOUT | the peer never published a usable header (all-zero or unusable); nothing was sent |
| `send_failed` | -EIO | prepare/commit/kick refused the message |
| `reply_timeout` | -ETIMEDOUT | the message went out and no verified answer arrived in the bound |
| `bad_answer` | -EIO / -EMSGSIZE | an element arrived but failed verification, was not the function asked for, still carried the PENDING result sentinel, or did not fit the caller's buffer |
| `peer_static` | -EIO | the two peer reads are identical: **our write pointer moving is not progress** |

The order of the decision is fixed and stated in the code: whose bytes were they
(`ours_overlap`), could we prove it (`journal_overflow`), is the header usable
(`link_timeout`), did it move (`peer_static`), did anything answer
(`reply_timeout`/`bad_answer`). A run cannot reach `pass` by any other path.

---

## 4. Running it

### Unit tier (no hardware — this is the proof that the instrumentation works)

```bash
SKIP_KVM=1 TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1800 ./run_unit_tests_intel.sh
```

The suite is `nv_rpc_harness_test_suite()`, registered after `nv_rpc_test_suite()`
in `src/kernel/unit_test.c`, and its banner is
`[NV-RPC] harness suite (round-trip discrimination: T-27..T-32)`.

| case | what it pins |
|---|---|
| T-27 | a peer that publishes its header and answers once → `pass`, all six rows asserted verbatim |
| T-28 | a peer that never publishes its header → `link_timeout`, nothing sent, nine words dumped |
| T-29 | header already up and static, we send, our writePtr 0→1, nobody answers → `reply_timeout` with `ours_wp=0x1`, `pre_wp=post_wp=0x0` |
| T-30 | the bytes move **and** our own store touched that window → `ours_overlap` (the journal veto) |
| T-31 | no clock / no doorbell / an oversized probe → refused at init (`no_seam`, `-EMSGSIZE`) |
| T-32 | an answer arrives but the watched header word never moves → `peer_static` |

The test sink echoes every row to the console, so the tier's log contains the
rows above byte for byte — the same text the card's uart sink prints.

### On the card

```c
/* after nv_gsp_rpc_stage8() has published the block */
int rc = nv_gsp_rpc_roundtrip(&gsp, d, NV_RPC_FN_GET_STATIC_INFO,
                              NULL, 0, 4000 /* us per wait */);
```

The adapter (`src/kernel/nv_gsp.c`, declared in `nv_gsp_glue.h`) supplies the
real seam (`nv_rpc_hw_dev()`), a one-slot staging element, a 4 KiB answer buffer
and the uart sink; the discrimination is all in `nv_rpc_harness.c`, which the
unit tier runs unchanged. The probe is one slot by construction
(`NV_RPC_RT_PAYLOAD_MAX` = 4016 bytes); a larger message is a different test and
is refused with `-EMSGSIZE` rather than truncated.

---

## 5. What this does not prove

* That the peer is the firmware. The harness proves the bytes it read are not
  bytes it wrote; it cannot prove who else wrote them. On a card with a staging
  area, an earlier stage could have written that header — which is why the
  verdict row prints `link` for both reads: `pre link=0` → `post link=1` is the
  header appearing *during* this run.
* That `+0x20` is the right read-pointer word on this chip, or that `SWAP_RX` is
  1 — those are T-08's and the link stage's job (`nv_rpc_link_apply()` reads the
  peer's `flags` and negotiates).
* That the doorbell at `0x110C00` wakes RM. A send that rings correctly into a
  firmware watching another address is `reply_timeout`, and nothing here can
  tell that apart from "RM is not running".
* Anything about a multi-slot message: the probe is one element, one slot.

---

## 6. Evidence for this lane

MEASURED on VM 201, `SKIP_KVM=1` (TCG), both unit tiers:

| file | what it is |
|---|---|
| `evidence/nv/rpcharness-unit-green.log` | intel tier, 616 run / 0 failed, and the six rows of §2.1 plus every FAIL verdict quoted above |
| `evidence/nv/rpcharness-arm-green.log` | ARM tier, 486 run / 0 failed, reloc gate OK 440/440 |
| `evidence/nv/rpcharness-mut1-vetoes-off.log` | mutation: `nv_rpc_journal_overlap()` forced to 0 and the `peer_moved` check removed → exactly T-30 and T-32 go red, the other four stay green |

REPRODUCE: the command in §4; every number in §2.1 comes from that log.
