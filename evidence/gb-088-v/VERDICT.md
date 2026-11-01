# GB-088-V (#334) — VERDICT: GREEN

**Graded:** `76235701` (PR #286, base `06a33970`) — checked out detached in a fresh worktree `.worktrees/gb088v`; `git rev-parse --short HEAD` printed `76235701` and `git merge-base --is-ancestor 06a33970 HEAD` printed `base-ok`.
**Graded by:** verifier (wgames, deep-class), 2026-11-01, QEMU on this VM (KVM intel, TCG arm).

| # | Criterion | Result |
|---|---|---|
| 1 | Intel: TESTS all PASS, `diffs >= 5000` + elapsed | PASS — `diffs=163844 after 20.6s` |
| 2 | ARM: same line, same threshold | PASS — `diffs=163850 after 19.2s` |
| 3 | Full-res diff confined to the NES 1024×960 rect | PASS — 983,040 px, bbox `(448,60)-(1471,1019)`, 25 px on the taskbar clock |
| 4 | Editor control: `abc123`, changed px confined to the text line | PASS — 626 px on the text line, vision read confirms `abc123` |
| 5 | Falsifier A: pre-PR 4 s sample FAILs | PASS — `diffs=8 after 4.0s` → FAIL, rc=1 |
| 6 | Falsifier B: no Space press ⇒ no flip | PASS — `diffs=10 after 31.0s` → FAIL, rc=1 |
| 7 | No new host-test failures; ARM kernel builds | PASS with one disclosed flake — see below |

---

## 1–2. The card's own harness, both arches

```
$ make ARCH=intel disk.img && ARCH=intel python3 run_nes_test.py ; echo "nes_intel_rc=$?"
[TEST] PASS: NES.BIN present in launcher menu ()
[TEST] PASS: NES.BIN pinned adjacent after XCRYPT.BIN (XCRYPT=21 NES=22)
[TEST] PASS: NES frame within 30s (bright dominates + dark top band) (t=0.4s bright_frac=0.97 topband_dark_frac=1.00)
[TEST] PASS: bright field dominates the NES area (bright_frac=0.97)
[TEST] PASS: dark band present across the top row (topband_dark_frac=1.00)
[TEST] PASS: input reaches the emulator (frame visibly changes / game live) (diffs=163844 after 20.6s)
[TEST] RESULT: PASS (NES T5 playthrough harness green)
nes_intel_rc=0
```

```
$ make ARCH=arm disk.img && ARCH=arm python3 run_nes_test.py ; echo "nes_arm_rc=$?"
[TEST] PASS: input reaches the emulator (frame visibly changes / game live) (diffs=163850 after 19.2s)
[TEST] RESULT: PASS (NES T5 playthrough harness green)
nes_arm_rc=0
```

Six `[TEST] PASS` lines on intel (menu, adjacency, frame-within-30s, bright-field, dark-band, input), and the
input line on ARM. Elapsed time recorded rather than theorised: **20.6 s** (intel) and **19.2 s** (arm).

## 3. Full-resolution diff — `evidence/gb-088-v/diff_bbox.py`

The harness's own `diff()` samples every 3rd row and every 2nd pixel, so the count above is a sample. The
card asks for every pixel; `diff_bbox.py` compares all 1920×1080 pixels of the pair and splits at the
taskbar (`y = 1054`):

```
$ python3 evidence/gb-088-v/diff_bbox.py nes_before.ppm nes_after.ppm
size 1920x1080 taskbar_y=1054
above_taskbar 983040 bbox=(448,60)-(1471,1019)
below_taskbar 25
```

**983,040 px = exactly 1024 × 960**, the NES buffer at 4× — and the bbox is exactly
`(448,60)-(1471,1019)`. Outside the window only 25 px move, the taskbar clock. That is the card's criterion 3
met to the pixel, not approximately.

## 4. The editor control — a different app, the same measurement

```
$ ARCH=intel python3 probe_editor.py
[probe] NANO.BIN menu idx=15
[probe] before=…/ed_before.ppm
[probe] after=…/ed_after.ppm
[probe] changed_px=650 bbox=(10, 44, 1894, 1069)
[probe] serial_has_Modified=True
```

Row-banded breakdown of those 650 px (`diff_rows.py`):

```
rows with changes: 26 of 1080
  y 44-60: 594 px, x 11-1017      <- the text line "abc123"
  y 62-63: 32 px, x 10-65         <- cursor / status cell
  y 1063-1069: 24 px, x 1889-1894 <- taskbar clock
```

**626 px in the editor's text area, 24 px on the clock, nothing else.** Menu slot 15 is NANO, matching the card.
The after-frame was converted losslessly to PNG (`editor_after.png`) and read by vision:

> The screenshot shows a minimal GNU nano text editor window … the title/status bar displays `GNU nano 8.7`,
> `New Buffer`, `Modified` … the editor area contains one line of text: `abc123`, with the cursor at the end.

So the keystrokes reached a *second* application through the same F1 → Apps → launch path, and only the
buffer changed.

## 5. Falsifier A — the instrument change is what fixed the check

Copy of `run_nes_test.py` with ONLY the pre-PR sample window restored (press, fixed 4 s, one shot; F1 launch
kept, since the pointer click is GB-116's defect and reverting it only makes the harness un-runnable). Same
tree, same image:

```
[TEST] PASS: NES frame within 30s (bright dominates + dark top band) (t=0.4s bright_frac=0.97 topband_dark_frac=1.00)
[TEST] FAIL: input reaches the emulator (frame visibly changes / game live) (diffs=8 after 4.0s (pre-PR fixed window))
[TEST] RESULT: FAIL (1)
FALA_RC=1
```

Single digits, exactly the `diffs=4`-class the card predicted. The 30 s poll is what made the flip visible; the
4 s sample was reading the frame before it rendered.

## 6. Falsifier B — the flip is attributable to the KEY

Copy with ONLY the Space press deleted, 30 s poll and the same before-shot retained:

```
[TEST] FAIL: input reaches the emulator (frame visibly changes / game live) (diffs=10 after 31.0s)
[TEST] RESULT: FAIL (1)
FALB_RC=1
```

Thirty-one seconds of polling and nothing flips but the clock. **The static field renders identically without
an input edge**, so the green above measures `reacts to a keystroke`, not a render loop advancing.

## 7. Regressions

```
$ make ARCH=arm hobbyos.elf                        -> ARM_ELF_RC=0
$ python3 tools/t5/run_t5_tests.py                 -> [host-tests] done: 34/35 passed
    FAIL  tools/board/check_verdicts_test.py (ValueError: not enough values to unpack (expected 4, got 3))
```

34/35 with exactly the pre-existing `check_verdicts_test.py` unpack error the card names — not counted.

**Disclosed flake:** the card's literal `make -j16 host_tests` exited 2 on `t4_parity: FAIL` /
`FAIL: stdout differs (legA_musl vs legB_host)` before reaching the python suite. Run standalone immediately
afterwards, `make t4_compat_parity` → `t4_parity: PASS (host differential locked; device leg gated)` rc=0, and
the python suite then reported the normal 34/35. The leg-B rows in the failing run carried the harness's 999/100x
"synthetic" sentinels rather than measured values, i.e. the parallel run's scratch state collided. I did not
count that failure as evidence for or against this card, and I am not hiding it: a `-j16` host-tests run is not
currently reproducible green on this VM.

## Environment notes (not GB-088 defects, but they will bite the next verifier)

`disk.img` requires the vendored X11 stack, and this VM was missing every host tool it needs:
`pkg-config`, `gperf`, `bison`, `flex`, `meson`, `ninja` — installed via apt before the build could proceed
(`freetype` wanted libpng through `pkg-config`; `fontconfig` wanted `gperf`; `pixman` is a meson build).
Separately, `third_party/x11/build.sh --stage all` stopped after the `server` lane on **both** arches without
running the `icewm` lane, leaving `ICEWM.BIN` absent and `make disk.img` failing at `x11-staged`; running
`build.sh --arch <arch> --stage icewm` explicitly produced `ICEWM.BIN`/`ICESH.BIN`/… and the image then
assembled normally. Neither is touched by this PR (it changes only `run_nes_test.py`).

## Artefacts

`gb088_nes_intel.log` · `gb088_nes_arm.log` · `diff_bbox.py` · `nes_before.png` / `nes_after.png` (lossless PNG
of the compared pair) · `probe_editor.py` · `gb088_editor.log` · `editor_before.png` / `editor_after.png` ·
`gb088_falA.log` + `falA.py` · `gb088_falB.log` + `falB.py` · `serial_intel.log`