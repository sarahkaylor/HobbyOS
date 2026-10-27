# GB-035-V2 (#218) — gate evidence (VERDICT: RED on clause 2)

Ticket: GB-035-V2 (#218). Verifies GB-035 (#35), PR **#163** head **`ee98d31`**
(`games(nes): homebrew self-animates (blink cell toggles each frame, no input)`).
Head checked first: `refs/pull/163/head` = `ee98d31` — **the same tree the 04:05Z RED graded**, so this is not a
re-run of a stale gate; I built that tree and measured it myself.
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2). Local gate only (T1 host + T3 intel
in-guest). No CI.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | boots a ROM in the guest, quoted from inside | **PASS** — `[LAUNCH] NES.BIN`, `[ARRIVED] NES.BIN rc>0`, and the screendump shows the ROM's rendered frame |
| 2 | plays: **≥3 distinct frame digests** | **FAIL** — 12 frames, **1** distinct digest (mine); 2 over 21 (the earlier RED); the ROM is designed for **two** states |
| 3 | input reaches the emulator | **STATED** — emulator-level only, not exercised in this run |
| 4 | licence-clean ROM, provenance named | **PASS** — MIT, hand-assembled 6502, in-tree |
| 5 | ARM, or a named wall | **STATED** — not run (see below) |
| 6 | `make host_tests` passes, new test executed | **PASS** — exit 0, and `nes_test_host` runs in the suite |

## How this was measured (so the numbers can be checked)

1. `git worktree add /tmp/gb035v2tree pr/163` — a clean checkout of the PR head, built from scratch
   (`make disk.img ARCH=intel`). The first build failed on the unstaged toolchain
   (`No rule to make target 'obj/arm/libc.a'` / `did not produce third_party/llvm-21.1.8/dist-staging`), fixed by
   rsyncing `dist-staging` from this VM's main tree (§11.5 wall, met locally).
2. Booted that image under QEMU with a QMP socket, drove the **virtio-input** menu (F1 → Home → Down×22 → Enter),
   then took 12 screendumps ~0.95 s apart and hashed the playfield crop `0,0,1820,1010` — clock and taskbar
   excluded, so a ticking clock cannot fake animation.
3. `evidence/gb-035-v2/frames.py` is the harness; `03_frames_pr163.log` is the raw output.

## 1 PASS — it boots the ROM and renders it

```
[LAUNCH] NES.BIN
[WAVE-TRY] NES.BIN
Loading program for scheduler (v2 AS): NES.BIN
[ARRIVED] NES.BIN rc>0
```
and the screendump of frame 5 is not a desktop: the NES window shows a light field with a black top band and a
small sprite in the top-left of the playfield — the three things `homebrew_rom.h` says the ROM draws.

## 2 FAIL — one distinct digest in twelve frames, and the design caps it at two

```
[harness] frame 00 digest 61e9348b8d1f
[harness] frame 01 digest 61e9348b8d1f
…
[harness] frame 11 digest 61e9348b8d1f

[harness] frames=12 distinct=1 -> ['61e9348b8d1f']
```
The earlier RED measured **2 distinct over 21 frames**. Mine measured **1 over 12**. Both are below the card's
three, and the difference between them is itself a finding: whether the blink is visible at all depends on when
you sample.

The ROM cannot do better than two. From `src/user/nes/homebrew_rom.h` on this head:

> 6. Every loop iteration also toggles a single nametable cell ($2000+0x34, row 1, col 20 …) between tile dark /
> bright. This makes the ROM ANIMATE ON ITS OWN …

One cell, two tiles. The PR's own host test asserts exactly that and nothing more:

```
  PASS: homebrew self-advances: blink cell toggles across frames with no input
```
So clause 2 — three *distinct* digests — is unreachable by construction, not by measurement luck. A vertical
slice of 2 states is not "plays" under this card.

## 3 STATED — input

The PR claims A-button input (band drawn when released, cleared when pressed). That is visible in the ROM source
and asserted at emulator level by `nes_test_host`, but **I did not drive a controller event into the guest**, so
I am not claiming it. Card clause 3 allows this to be stated rather than gated when the change does not claim
input; this one does claim it, so the honest position is: claimed, not verified here.

## 4 PASS — licence and provenance

`homebrew_rom.h` header: *"License: MIT, swarm-authored, license-clean (no copyrighted NES BIOS/ROM)"*, a
hand-assembled 6502 program carried as raw PRG bytes in the tree. No commercial dump, no BIOS blob.

## 5 STATED — ARM

Not run. The PR tree needed the staged toolchain before it would build at all on this host, and I did not spend
a second build on the ARM kernel for a card already failing on clause 2. Named, not hidden.

## 6 PASS — host tests, and the new test is executed

```
$ make host_tests          (in the PR worktree)
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
HOST_TESTS=0
  PASS: homebrew self-advances: blink cell toggles across frames with no input
```
`nes_test_host` is built and run by the suite, so the new test is executed rather than merely added.

## Verdict

**RED** on clause 2. It boots a licence-clean ROM in the guest and genuinely renders it — clauses 1, 4 and 6
pass with quoted output — but the animation is a two-state blink and the card asks for three distinct frame
digests, which this ROM cannot produce by design. Clause 2 needs either a ROM that renders three or more states
(a moving sprite, a counter, a scrolling field) or a card whose clause says what "plays" means for a two-state
demo. The evidence for that is in the ROM's own comment block and its own test, not only in my sampling.