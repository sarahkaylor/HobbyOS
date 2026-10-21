# GB-035-V — gate run manifest

- Verifier: DEEP (DeepSeek v4.1 Flash, dgx1), worktree `hermes-bcb95915`, host **swarm-farm-2**
- Date: 2026-10-21 (UTC)
- Revision run: **b1e0827** — `HEAD` == `origin/main`; the ticket's `badc665` is an ancestor of it (`git merge-base --is-ancestor badc665 HEAD` → true), so all NES content under test is the shipped content, seven commits later.
- Env: dist-staging copied from this VM's main checkout; `make ARCH=arm disk.img` built clean (exit 0).

## DoD 1 — T1 core tests *inside* the suite: **FAIL**

`make host_tests` aborts before the NES test is ever executed. The recipe order in `Makefile` is
`python3 tools/t5/run_t5_tests.py` (line 3415) **then** `$(HOST_RUN) ./$(NES_TEST)` (3416), so the abort at
3415 means `nes_test_host` never runs in the suite:

```
  FAIL  tools/board/claim_race_test.py  (CLAIM_RACE_TEST: PASS — the read-then-move double claim is fixed)
  FAIL  tools/t5/acceptance_gate_test.py  (ACCEPTANCE_GATE_TEST: PASS — the acceptance rig no longer fabricates PASS)
  FAIL  tools/t5/acceptance_test.py  (Tests: 1 FAILED)
[host-tests] done: 11/14 passed
make: *** [Makefile:3415: host_tests] Error 3
```

Two of those three are a **grading defect**, not a test failure — run standalone each exits 0 and prints PASS:

```
$ python3 tools/board/claim_race_test.py        -> exit 0  "CLAIM_RACE_TEST: PASS — …"
$ python3 tools/t5/acceptance_gate_test.py      -> exit 0  "ACCEPTANCE_GATE_TEST: PASS — …"
```

`tools/t5/run_t5_tests.py:37` grades with `ok = "PASSED" in (r.stdout + r.stderr)` — a test that prints
`PASS` (no `PASSED`) is scored FAIL. The third is real:

```
$ python3 tools/t5/acceptance_test.py           -> exit 1
  PASS: a uniform black frame is NOT live
  FAIL: a frame with distinct pixels IS live
  Tests: 1 FAILED
```

The core test itself is green when run directly — `./nes_test_host` → `Tests: passed=19 failed=0 -> ALL
PASSED` — but DoD 1 asks for it **inside the suite**, and there it does not run.

## DoD 2 — ROM licence-clean, path recorded: **PASS**

`src/user/nes/homebrew_rom.h` — 43-byte hand-authored 6502 NROM, header states
`License: MIT, swarm-authored, license-clean (no copyrighted NES BIOS/ROM)`. No commercial dump anywhere in
the tree.

## DoD 3 — T5 playthrough: located by name, launched, frames **do not advance**: **FAIL**

Capture (`t5_capture.py`): boot arm, `find_menu_slot(serial, "NES.BIN")` → slot **21** (no hardcoded index),
launch, then 4 screendumps 2 s apart.

```
menu vs nes-0      differing= 506687   97.74%
nes-0 vs nes-1     differing=     13    0.00%
nes-0 vs nes-2     differing=     15    0.00%
nes-0 vs nes-3     differing=     14    0.00%
serial: [LAUNCH] NES.BIN present: True
```

Pixel bounding boxes of the sample-to-sample differences:

```
nes-0.ppm vs nes-3.ppm: bbox=(1881, 1063, 1895, 1070)      <- the desktop clock
nes-1.ppm vs nes-2.ppm: bbox=(1889, 1063, 1895, 1070)      <- the desktop clock
menu.ppm  vs nes-0.ppm: bbox=(0, 0, 1920, 1078)            <- the whole screen
differing px inside the game area (0..1820 x 0..1010) of nes-0 vs nes-3: 0
```

So the title renders (97.74% of the frame differs from the menu) and the launch is real, but **every game
pixel is identical across the samples** — the only change is the clock digits. Cause is in the design:
`homebrew_rom.h` writes the PPU once and parks in a `JMP` loop; `main.c` re-renders that unchanging PPU state
each iteration, and only a controller edge (`g_phase`) inverts the image. Frames cannot advance without input,
and a single invert is not "the emulator advances frames".

## DoD 4 — corrupt ROM fails rather than hangs: **NOT MEASURABLE**

There is no ROM input path to corrupt: `main.c:21-23` copies the compiled-in `home_rom[]` array into the cart
and `HOME_ROM_LEN` is fixed at 43. The emulator takes no ROM argument and reads no ROM file, so "a
deliberately-corrupt ROM" cannot be presented at all, and no test for it exists in the tree. Per §4.2 an
unmeasurable clause is RED, never SKIP.

## Verdict

**RED.** GB-035 (#35) → In Progress. Passing: licence-clean ROM (DoD 2); the core's own 19 host checks. Failing:
the suite never reaches them (DoD 1), the emulator cannot advance a frame (DoD 3), and the corrupt-ROM clause
has no mechanism to test (DoD 4).
