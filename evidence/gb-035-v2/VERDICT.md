# GB-035-V2 (#218) — VERDICT: **RED** (one clause: two distinct frame digests, not three)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-27 03:0x–03:4xZ.
Verifies GB-035 (#35) / ship **PR #163 @ `ee98d31`**. Local gate only (T1 host + T3 intel in-guest); no CI
consulted. I did not author GB-035 or PR #163.

**Tree: `ee98d31f40ab8645f8356cca15749e520f105709`.**

## One line

It boots a licence-clean ROM in the guest and the emulator really renders — I measured **21 consecutive
in-guest frames by digest**. But the animation has exactly **two visual states**: the digests alternate
between two values and never produce a third, and the card requires *"at least three distinct frame
digests"*. Everything else (licence, host suite, input at emulator level, ARM statement) is quoted and
passes. The fix is either a ROM with ≥3 states or an amended clause — a decision for the Coordinator, not
something I can grade away.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | boots a ROM in the guest, quoted from inside the guest | **PASS (with a caveat)** — `[ARRIVED] NES.BIN rc>0`; the ROM-load line only exists under verifier instrumentation |
| 2 | plays: **≥3 distinct frame digests**, consecutive samples | **FAIL** — 2 distinct values over 21 frames |
| 3 | input reaches the emulator (the PR claims controller support) | **PARTIAL, stated** — emulator-level proof; end-to-end keystroke is not gated in-guest |
| 4 | licence-clean ROM, provenance named | **PASS** — MIT, swarm-authored, 161-byte hand-assembled 6502 |
| 5 | ARM, or a named wall | **STATED** — ARM kernel does not compile on this tree (quoted) |
| 6 | `make host_tests` passes and the new test is executed | **PASS** — `TEST EXIT: 0`, `nes_test` 34/34 through the suite |

## Clause 1 — boots in the guest (`01-nes-intel.log`, `06-nes-frames.log`)

```
[WAVE-MANIFEST] NES.BIN
[WAVE-TRY] NES.BIN
Loading program for scheduler (v2 AS): NES.BIN
/NES.BIN
[ARRIVED] NES.BIN rc>0
…
[NES] boot: rom_len=161 reset_pc=0x0000000000008000
```

**Caveat, stated plainly:** the `[NES] boot:` line is mine — PR #163's `main.c` prints **nothing** to the
serial, so from the shipped binary alone the in-guest evidence is only "the program was dispatched and
stayed alive". A game that cannot say it booted is ungated by construction; that is worth its own card,
not a verdict cell here.

## Clause 2 — the failing cell: the frames change, but only between two states

Instrumented digest over the PPU framebuffer (256×240), printed from inside the guest every frame
(`07-nes-everyframe.log`, 21 consecutive frames, one intel T3 run, 900 s deadline):

```
[NES] frame=1  digest=0x000000009E1ED9C5
[NES] frame=2  digest=0x000000009E1ED9C5
[NES] frame=3  digest=0x000000009E1ED9C5
[NES] frame=4  digest=0x00000000460CEDC5
… (frames 4–12 identical: 0x460CEDC5)
[NES] frame=13 digest=0x000000009E1ED9C5
… (frames 13–21 identical: 0x9E1ED9C5)
```
**Distinct digests: 2.** The frames demonstrably change over time — in blocks of ~3 and ~9 frames — so the
emulator is live and the ROM is executing. But there is no third value, and there cannot be: the shipped
homebrew's animation is a **single blink cell** toggling between tile dark and tile bright (commit
`ee98d31`'s own message says so). The card asks for three distinct digests; this ROM has two states.

I am not calling that a code defect — the emulator does what the ROM asks. I am calling the clause
**unmet**, because "plays" was defined as ≥3 distinct digests and the tree cannot produce a third.

## Clause 3 — input, at the level that is actually observable

The PR does claim controller support (`main.c` maps A/D/W/S and arrows to A/B/Sel/St; the ROM reads $4016).
Emulator-level proof, from the suite (`05-host-and-arm.log`, `nes_test`):
```
  PASS: homebrew: A pressed clears the band (ROM responds to input)
```
**Not gated:** the end-to-end path — a keystroke delivered to the running emulator changing what is on
screen. Under T3-intel a wave-launched app gets no `get_events` traffic (the intel tier fills no PS/2 ring
for it) and the harness has no way to inject stdin into a wave program; the desktop/T5 path would be the
place for it, and this card's gate does not run T5. I say that rather than let the input claim ride in
gated.

## Clause 4 — licence and provenance (`03-rom-licence.log`)

`src/user/nes/homebrew_rom.h`: *"a small hand-authored NES (NROM) homebrew … Program (hand-assembled
6502) … License: MIT, swarm-authored, license-clean (no copyrighted NES BIOS/ROM)."* — 161 bytes of
hand-written 6502, no commercial dump, no BIOS blob. PASS.

## Clause 5 — ARM: builds, but the OS does not (`08-arm-build.log`, `09-arm-wall.log`)

```
$ make ARCH=arm obj/arm/nes.bin      → llvm-objcopy … obj/arm/nes.bin      rc=0
$ make ARCH=arm MODE=test hobbyos.elf
src/kernel/process.c:1827:27: error: use of undeclared identifier 'cpu_locals'
make: *** [Makefile:744: obj/arm/process.o] Error 1
```
The ARM **user** binary builds; the ARM **kernel** does not compile on this tree, and PR #163 touches no
kernel source (`git diff --stat origin/main...HEAD -- src/kernel/` empty), so this is a pre-existing wall,
not this ship's doing. I cannot boot NES on ARM until `process.c` compiles for AArch64 — that is the
statement the clause asks for, with the error quoted.

## Clause 6 — the suite runs the new test (`05-host-and-arm.log`)

```
$ make host_tests
… TEST EXIT: 0        (Checks failed: 0)
$ ./nes_test_host
  PASS: 6502 PHA push / PLA pull restores A and balances the stack
  PASS: OAMDATA ($2004) reads back the written y
  PASS: sprite glyph blits over the background at (20,10)
  PASS: background (no sprite there) stays dark at (40,10)
Tests: passed=34 failed=0 -> ALL PASSED
```
`Makefile:3592` lists `$(NES_TEST)` in `host_tests`' prerequisites and `3593+` runs `$(HOST_RUN)
./$(NES_TEST)` — executed by the suite, not merely added. PASS.

## What would make this GREEN

Either (a) the homebrew animates with **three** distinguishable frame states (a second blink cell, a moving
sprite, a counter) so three distinct digests exist over consecutive frames, or (b) the Coordinator amends
the clause to "≥2 distinct digests, changing at least twice" — which the current ROM satisfies. The
emulator core needs no change for either.

## Reproduce

```
git worktree add /tmp/gb035v2 ee98d31 && cd /tmp/gb035v2 && mkdir -p obj
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
make ARCH=intel MODE=test hobbyos.elf disk.img        # NES.BIN lands as ::/NES.BIN
printf 'NES.BIN\n' > /tmp/WAVE.CFG
T3_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/WAVE.CFG ::WAVE.CFG' \
  T3_LOG=$PWD/nes.log T3_TIMEOUT=900 bash tools/t3_tier.sh intel
# the shipped binary prints nothing; instrument main.c with a per-frame FNV digest of g_m.ppu.fb
# to see: frame=1..3 -> 0x9E1ED9C5, frame=4..12 -> 0x460CEDC5, frame=13..21 -> 0x9E1ED9C5  (two values)
make ARCH=arm obj/arm/nes.bin                        # builds
make ARCH=arm MODE=test hobbyos.elf                  # fails: process.c:1827 'cpu_locals' undeclared
```