# WP-RPCHARNESS — a round trip that cannot be fooled by our own write pointer

Card: **t_643f58a1**. Branch `nv/wp-rpcharness`, stacked on `nv/wp-rpcpub`
(32cf0ea1 — the sibling's 129-page publication), which is itself not yet folded
into `nv/integ-swarm`.

**One line:** `src/kernel/nv_rpc_harness.c` sends one message, waits for one
answer, and PASSES only when two reads of the **peer's own 32-byte producer
header** differ — with a write journal proving no store of ours overlaps the
bytes it read. Our write pointer is printed on its own row, labelled
`side=ours`, and is never one side of the comparison.

## 1. The discriminator

Two 32-bit "how far have we got" words live at the same offset of two queue
headers in the same block:

| word | block offset | whose |
|---|---|---|
| our writePtr (producer index, command ring) | `0x1010` | **ours** |
| the peer's writePtr (producer index, status ring) | `0x41010` | the peer's |
| our status readPtr under `SWAP_RX` | `0x1020` | **ours** |
| the peer's command readPtr under `SWAP_RX` | `0x41020` | the peer's |

Same shape of number. Reading one back and calling it progress is the false
positive this lane exists to kill. Three mechanisms:

1. **Peer-read vs peer-read.** `pre` is a 32-byte copy of `statq+0x00..0x1F`
   taken before the first message is sent; `post` is the same 32 bytes after
   the answer (or the reply timeout). The verdict compares those two and
   nothing else — no cached mirror, no `q->peer`, no struct.
2. **The window stops at +0x1F.** `+0x20` (the read pointer) is excluded
   because with `SWAP_RX` clear that word is *ours*; including it would let our
   own store manufacture the movement. Excluding it can only reduce false
   PASSes.
3. **A write journal.** Every store the transport makes into the block is
   recorded (`nv_rpc_journal()`: link's readPtr publication, each tx slot, the
   writePtr store, rx_advance's readPtr). The harness resets it at the start of
   the run and asserts `nv_rpc_journal_overlap(q, stat_off, 32) == 0` at both
   reads. Ring overflow is counted and is itself a FAIL — "cannot prove" is
   never reported as "found nothing".

## 2. The rows (the hardware card quotes these)

Format rules: one line per row, `[NV-RPC] stage=<name> key=value ...`;
addresses `0x` + 16 uppercase hex digits; 32-bit wire words `0x` + 8 uppercase
digits; counters decimal; `side=peer`/`side=ours` on every row that names a
word; the sink adds the newline. Full grammar and field table:
`docs/nv-rpc-roundtrip.md` §2.

MEASURED — these six lines are asserted **byte for byte** by T-27 and appear in
`evidence/nv/rpcharness-unit-green.log`:

```
[NV-RPC] stage=peer phase=pre side=peer addr=0x0000000020041000 off=0x0000000000041000 v=0x00000000 size=0x00000000 msgsize=0x00000000 count=0x00000000 wp=0x00000000 flags=0x00000000 rxhdroff=0x00000000 entryoff=0x00000000 link=0
[NV-RPC] stage=writer side=ours addr=0x0000000020001010 off=0x0000000000001010 wp=0x00000001 prev=0x00000000 ec=1
[NV-RPC] stage=send side=ours fn=0x00000041 len=0x00000020 ec=1 seq=0 doorbells=1
[NV-RPC] stage=reply side=peer fn=0x00000041 len=0x00000020 result=0x00000000 seq=1 ec=1
[NV-RPC] stage=peer phase=post side=peer addr=0x0000000020041000 off=0x0000000000041000 v=0x00000000 size=0x00040000 msgsize=0x00001000 count=0x0000003F wp=0x00000001 flags=0x00000001 rxhdroff=0x00000020 entryoff=0x00001000 link=1
[NV-RPC] stage=roundtrip verdict=pass reason=ok peer_moved=1 moved=7 first=0x00000006 pre_wp=0x00000000 post_wp=0x00000001 ours_wp=0x00000001 ours_wrote_peer=0 answers=1 waited_us=0
```

Read the last three lines then the fifth: the peer's `wp` goes `0x0 -> 0x1` at
`addr=…41000`, our `wp` goes `0x0 -> 0x1` at `addr=…01010`, and the verdict row
carries both (`pre_wp`/`post_wp` vs `ours_wp`) so no reader has to infer which
number moved. On failure the run also emits `stage=replay n=<k>
note=rows-follow-verbatim` followed by the last rows verbatim, and
`stage=words side=peer … w00..w08` — the whole 36-byte header as raw words.

## 3. The negative cases (all in the suite, none of them decoration)

MEASURED, from the same log:

| test | setup | verdict row |
|---|---|---|
| T-28 | peer never publishes its header | `verdict=FAIL reason=link_timeout … ours_wp=0x00000000 answers=0` (nothing sent, `doorbells=0`) |
| T-29 | header already up and static, we send, our wp 0→1, no answer | `verdict=FAIL reason=reply_timeout … pre_wp=0x00000000 post_wp=0x00000000 ours_wp=0x00000001 answers=0` |
| T-30 | bytes change **and** our own store touched the window | `verdict=FAIL reason=ours_overlap … peer_moved=1 … ours_wrote_peer=1 answers=1` |
| T-32 | an answer arrives but the watched word never moves | `verdict=FAIL reason=peer_static … pre_wp=0x00000000 post_wp=0x00000000 ours_wp=0x00000001 answers=1` |

T-29 and T-32 are the card's acceptance criterion in its two hardest forms:
T-29 is "our pointer advanced and we got nothing"; T-32 is "we got an answer, so
it must have worked" — both are FAILs, and both name the reason. T-30 is the
guard that a bytes-only harness would fail: the peer's header *did* move, and
the run is still refused because our journal says one of those bytes was ours.

A mutation run (vetoes disabled: `nv_rpc_journal_overlap` forced to 0 and the
`peer_moved` check dropped) turns exactly T-30 and T-32 red and leaves the other
four green — `evidence/nv/rpcharness-mut1-vetoes-off.log`.

## 4. Gates

| tier | result |
|---|---|
| intel unit (`SKIP_KVM=1`, TCG) | **616 run / 0 failed** — the 610 of `nv/wp-rpcpub` plus T-27..T-32 (`evidence/nv/rpcharness-unit-green.log`) |
| ARM unit (TCG) | **486 run / 0 failed**, reloc gate OK 440/440 (`evidence/nv/rpcharness-arm-green.log`) |
| ARM folded-pointer gate | **OK** after the baseline refresh — see §5 |

## 5. One finding that is not this lane's code: the publish half was ARM-red

`nv/wp-rpcpub` grew `arg_names` 10 -> 12 (`NV_RPC_ARG_MAGIC`/`NV_RPC_ARG_SIZE`
were added to the enum and the name table) and **never ran the ARM tier**, so
`tools/reloc_tables_baseline.json` no longer matched and every ARM link on that
branch failed the folded-table gate. This lane inherited that; it also added a
translation unit, which re-glued the audit's anonymous `.rodata` run
(18 -> 27 words — a contiguous-pointer run that now spans `fs.c`'s switch table
plus `cons_dnames` and 12 words of `cons_dtags`; every word still relocates,
440/440 applied, 0 left at the link address).

Resolution: `make reloc_baseline MODE=unit_tests|desktop|test`, and the two
changes are written up in `docs/nv-arm-reloc.md` §5.1/§5.2 rather than absorbed
silently. The baseline diff is exactly two keys per mode (`.rodata` 18 -> 27,
`arg_names` 10 -> 12). The one `switch` this lane would have added — the
verdict-name function — was rewritten as a compare chain precisely so it adds
no folded table.

## 6. What is NOT proven here

- **No GPU on this host.** The device entry `nv_gsp_rpc_roundtrip()` is
  compile-verified and its logic is the same pure code T-27 runs, but it has
  never been called with a real seam. The peer-visible half is `t_b80196f2`'s.
- The harness proves the bytes it read are not bytes it wrote; it cannot prove
  **who else** wrote them. `pre link=0 -> post link=1` is the evidence that the
  header appeared during the run; a header already valid at `pre` (RM started
  earlier) is judged by its writePtr alone.
- Nothing about multi-slot messages: the probe is one slot
  (`NV_RPC_RT_PAYLOAD_MAX` = 4016 bytes) and a larger payload is refused.
- `+0x20` being the right read-pointer word and the doorbell at `0x110C00`
  waking RM remain the link stage's and the card's questions.

## 7. Reproduce

```bash
git worktree add .worktrees/t_643f58a1 -b nv/wp-rpcharness origin/nv/wp-rpcpub
cd .worktrees/t_643f58a1
SKIP_KVM=1 TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1800 ./run_unit_tests_intel.sh   # 616 / 0
TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh                     # 486 / 0
```

Rows: `grep '^\[NV-RPC\]' qemu.log`. The five rows and every FAIL verdict in
this report are in that log unedited.
