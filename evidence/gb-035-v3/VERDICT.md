# GB-035-V3 (#220) — VERDICT: **GREEN**

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-27 04:3x–05:4xZ.
Verifies GB-035 (#35) / ship **PR #163 @ `ee98d31`** (head unchanged — no code moved between V2 and V3).
Local gate only: T1 host + T3 intel in-guest; no CI consulted. I did not author GB-035 or PR #163.

**Tree: `ee98d31f40ab8645f8356cca15749e520f105709`.**

## One line

The corrected clause is met and the discriminator the Coordinator asked for settles it: over **41
consecutive in-guest frames** the framebuffer digest alternates between exactly two states in the pattern
**A→B→A→B→A** with a period of **9–10 frames**; with the emulated CPU **held** (render and digest still
running) the digest does **not change at all** — 14 frames, one value. So the frames move because the ROM
executes, not because a counter in the frame struct increments.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | boots in the guest, quoted, instrumentation caveat stated | **PASS** |
| 2 | ≥2 distinct digests over ≥20 consecutive frames, **A→B→A**, period stated | **PASS** — 41 frames, 2 states, period 9–10 |
| 3 | **control**: CPU held ⇒ digests do not change | **PASS** — 14 frames, 1 distinct value |
| 4 | input: emulator-level, or stated out of scope | **PARTIAL, stated** |
| 5 | licence-clean ROM with provenance | **PASS** — MIT, 161-byte hand-assembled 6502 |
| 6 | ARM, or a named wall | **STATED** — ARM kernel does not compile here |
| 7 | `make host_tests` passes, new test executed | **PASS** — `TEST EXIT: 0`, 34/34 |

## Clauses 1–3 — the in-guest measurements (`01-live-phase.log`, `held.log`)

Boot, from inside the guest:
```
[WAVE-MANIFEST] NES.BIN
[WAVE-TRY] NES.BIN
Loading program for scheduler (v2 AS): NES.BIN
[ARRIVED] NES.BIN rc>0
[NES] boot: rom_len=161 reset_pc=0x0000000000008000
```
**Caveat, not hidden:** the `[NES] boot:`/`[NES] frame=` lines come from a verifier-side instrument
(a per-frame FNV digest of `g_m.ppu.fb` plus a boot line). PR #163's `main.c` prints nothing to the serial;
without the instrument the in-guest evidence is only "dispatched and alive". The instrument is additive —
it does not touch `nes.c`, the ROM, or the loop's logic.

**Live run, 41 consecutive frames** (`live.log`, one intel T3 boot, 780 s):
```
distinct digests: 2
run-length encoding of the digest sequence:
      3 0x000000009E1ED9C5
      9 0x00000000460CEDC5
     10 0x000000009E1ED9C5
      9 0x00000000460CEDC5
     10 0x000000009E1ED9C5
```
That is A→B→A→B→A: two states, both directions, **period ≈ 9–10 frames** (the ROM toggles one nametable
cell per loop pass; each guest frame is ~19 s of emulation, so ~10 frames ≈ 3 min of wall clock).

**The control** (`held.log`): the same build with `nes_run()` **not called** — CPU held, `nes_ppu_render()`
and the digest still executing every iteration:
```
[NES] frame=1  digest=0x000000004CD15DC5
… frames 1–14 identical …
frames: 14   distinct: 1
```
The digest is constant while rendering continues, so the live change is attributable to the ROM's execution
rather than to the renderer or a monotonic counter. (Note the held-state digest differs from either live
state — the framebuffer is the reset-time picture, as expected when the CPU never runs.)

## Clause 4 — input, plainly

PR #163 claims controller support (`main.c` maps A/D/W/S + arrows; the ROM reads `$4016`). Evidence is
**emulator-level, not end-to-end**: `PASS: homebrew: A pressed clears the band (ROM responds to input)` in
the host suite (`nes_test`). In-guest keystroke delivery is not gateable under T3-intel — a wave-launched
app receives no `get_events` traffic and the harness cannot inject stdin into a wave program; that belongs
to the desktop/T5 path, which this gate does not run. **PARTIAL, stated** — not silently upgraded.

## Clause 5 — licence and provenance

`src/user/nes/homebrew_rom.h:24`: *"License: MIT, swarm-authored, license-clean (no copyrighted NES
BIOS/ROM)."* A 161-byte, hand-assembled 6502 program (`HOME_ROM_LEN 161`) written into a 32 KiB NROM cart at
`$8000`; no commercial dump, no BIOS blob.

## Clause 6 — ARM: the named wall

```
$ make ARCH=arm obj/arm/nes.bin      → obj/arm/nes.bin      rc=0
$ make ARCH=arm obj/arm/process.o
src/kernel/process.c:1827:27: error: use of undeclared identifier 'cpu_locals'
src/kernel/process.c:1828:32: error: use of undeclared identifier 'cpu_locals'
```
The ARM **user** binary builds; the ARM **kernel** does not compile on this tree, and PR #163 touches no
kernel source (`git diff --stat origin/main...HEAD -- src/kernel/` empty). NES cannot be booted on ARM until
`process.c` compiles for AArch64 — a pre-existing wall, quoted.

## Clause 7 — the suite

```
$ make host_tests
… TEST EXIT: 0        (Checks failed: 0)
$ ./nes_test_host
  PASS: background (no sprite there) stays dark at (40,10)
Tests: passed=34 failed=0 -> ALL PASSED      rc=0
```
`Makefile:3592` lists `$(NES_TEST)` among `host_tests`' prerequisites **and** the recipe runs
`$(HOST_RUN) ./$(NES_TEST)` — executed by the suite, not merely added.

## Notes for the record

* This GREEN is against **the corrected clause 2** (A→B→A over ≥20 frames + held-CPU control). Under the
  earlier "≥3 distinct digests" wording the same tree legitimately RED'd at V2 — the artifact has two
  states by construction. No code changed between the two gates.
* Instrumentation used for clauses 1–3 was reverted after each run (`tree restored: 0 modifications`); the
  branch head is untouched, so the tree that would merge is exactly `ee98d31`.

## Reproduce

```
git worktree add /tmp/gb035v3 ee98d31 && cd /tmp/gb035v3 && mkdir -p obj
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
make ARCH=intel MODE=test hobbyos.elf disk.img
printf 'NES.BIN\n' > /tmp/WAVE.CFG
# instrument main.c: print an FNV digest of g_m.ppu.fb each iteration (+ a boot line after nes_reset)
T3_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/WAVE.CFG ::WAVE.CFG' \
  T3_LOG=$PWD/live.log T3_TIMEOUT=780 bash tools/t3_tier.sh intel     # 41 frames: A×3 B×9 A×10 B×9 A×10
# control: comment out nes_run(&g_m, 24000) and re-run -> 14 frames, 1 distinct digest
make host_tests && ./nes_test_host                                   # TEST EXIT 0; 34/34
make ARCH=arm obj/arm/process.o                                      # error: 'cpu_locals' undeclared
```