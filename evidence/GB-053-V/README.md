# GB-053-V — verification evidence (DEEP Verifier, 2026-10-20)

Subject: GB-053 (#53) "S0/S1 final acceptance run" — verdict **RED**.
Revision: `0d381008f3c3626a5cd2d0a335a129ab3b13f78f` = `refs/heads/main`, fresh worktree.

| # | file | what it is |
|---|---|---|
| 01 | `01-arm-run.log` | `python3 tools/t5/acceptance_run.py --arch arm` → harness says PASS 5/5 (ARM_EXIT=0) |
| 02 | `02-arm-build.log` | clean `make ARCH=arm disk.img` (the first attempt's QMP timeout was a cold-build race, see finding 4) |
| 03 | `03-intel-run.log` | `--arch intel` → FAIL 5/5, every slot `None` |
| 04 | `04-arm-pixel-stats.txt` | per-image unique colours, dominant-colour share, pairwise mean-abs-diff |
| 05 | `05-arm-nonblack.txt` | non-black pixel counts + bounding boxes + top colours |
| 06 | `06-intel-build.log` | `make ARCH=intel disk.img` → exit 0 |
| 07 | `07-manifest-provenance.txt` | sha16 of each PPM vs the manifest → 5/5 OK |
| 08 | `08-launch-evidence.txt` | every `Loading program for scheduler` line in the ARM wave; intel desktop death |
| 09 | `09-liveness-criterion.txt` | measured behaviour of the harness's own `ppm_live` |
| 10/11 | `10-…-after-intel-run.md`, `11-manifest-after-intel-run.json` | the report/manifest the gate rewrote (intel/FAIL); reverted in-tree to keep the branch clean |

## The findings

1. **ARM "PASS 5/5" is not five games.** The ARM wave loads exactly three game programs —
   `PONG.BIN` (line 470), `MILLIPED.BIN` (483), `XNIBBLER.BIN` (488) — and all three load **before**
   the menu interaction. `XCRYPT` and `NES` appear in the serial log **twice each**: once when
   `mcopy` stages the binary, once in the `[MENU]` dump. They are never loaded, never run, never
   mentioned as processes. The harness still reported `XCRYPT.BIN slot 20 live=True PASS` and
   `NES.BIN slot 21 live=True PASS`.

2. **"Live frame" means "the top ~104 rows are not uniform" — the title bar.** `ppm_live` samples raw
   bytes `[0 : 200000*3]` with a stride of 9, i.e. only the first 600 000 bytes of pixel data
   (≈104 rows of 1920×1080). Measured:
   ```
   all black                     -> False
   50x50 white block at row 5    -> True
   50x50 white block at row 540  -> False
   50x50 white block at row 1000 -> False
   ```
   Every captured frame is 96.9–99.8 % black (dominant colour `(0,0,0)`), and the five frames differ
   from each other by a mean absolute difference of 0.15–0.97 out of 255 — that is one window's title
   bar plus a small animated element, not five distinct games.

3. **Intel leg: the desktop dies before the menu.** `[MENU]` count 0, `Desktop starting` count 0:
   ```
   [KERNEL] pid=1 (DESKTOP.BIN) memory fault VA=0x00000000755BDBC8 PC=0x0000001000008610 rw=r in=PROT region=NONE cr3frame=0x0000000020C03000 asroot=0x0000000020C03000 -> killed
   [KERNEL] Process 1: DESKTOP.BIN exited unexpectedly or gracefully.
   System halt from CPU 4.
   ```
   All five games come back `slot None`, `live False`, FAIL. This is the GB-068/GB-061 family
   (user-space address in a PROT region with no mapping), not a game defect.

4. **Harness defects that make the gate unreliable, not just red:**
   - The QMP client waits 100 s from process start, while `make run` may still be building a cold
     tree (intel build ≈ 4 min here). Two runs died with `RuntimeError: QMP socket never appeared`,
     indistinguishable in the log from "the guest never booted".
   - The report hardcodes `- intel leg gated on GB-057 (intel spawn deadlock); this run is ARM.` —
     it appends that line to an **intel** report too (see `10-…`).
   - `boot()` runs `pkill -9 -f qemu-system[-]` on start and `main()` does it again at the end: on a
     shared farm VM this kills every other agent's guest.
   - `S0-S1-ACCEPTANCE.md` / `-MANIFEST.json` are tracked files the gate rewrites in place, so two
     consecutive runs destroy the first arch's evidence base unless it is copied out first.

## What is good

The manifest does regenerate with sha16 provenance and the hashes resolve against the PPMs it names
(5/5 OK), so DoD 2 holds for the ARM run. And PONG's frame really is a Pong court (verified by eye
on the converted PNG: centre line, two paddles, ball, score 0–1), so at least one pinned game is
genuinely live on ARM.
