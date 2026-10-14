# HobbyOS "Chrome light" re-theme — acceptance recipe (Lane V)

Official re-run recipe for the controller. All commands run from the
**worktree root** (`/home/sarah/hobbyos-theme/wt-verify` unless stated).

## 0. Never broad-pkill QEMU on this host

The repository's python tiers (`run_desktop_test.py`, `run_desktop_apps_test.py`,
`run_console_test.py`, `run_*.py`, etc.) internally run `pkill -9 -f qemu-system*`
— **global**, killing every concurrent lane's VMs. On this shared host:

- Run any root `run_*.py` tier with a **cwd-scoped pkill shim** prepended to
  PATH in the SAME command so signals only reach processes whose
  `/proc/<pid>/cwd` is inside that tier's tree:

  ```bash
  # shim at ~/.hermes/cache/scratch/fs-shims/<lane>/pkill rewrites pkill to
  # owner-cwd-filtered signals (WT=<tree>); path-less fallback:
  #   for p in $(pgrep -f <pat>); do case "$(readlink /proc/$p/cwd)" in
  #     */<my-tree>*) kill -9 $p;; esac; done
  PATH="/home/sarah/.hermes/cache/scratch/fs-shims/verify:$PATH" \
    WT=/home/sarah/hobbyos-theme/wt-verify \
    python3 run_desktop_apps_test.py
  ```

- `tools/theme_shot.py` never broad-pkills: it records the QEMU PID(s) it
  started (cwd-scoped scan of `/proc/*/cwd` against the worktree) and kills
  only those.

## (a) Build: MODE=desktop_test

```bash
export PATH="$(dirname $(which clang)):$PATH"   # background shells lose ld.lld
make MODE=desktop_test hobbyos.elf disk.img
```

- Target names verified in the Makefile: `desktop_test_run` =
  `$(MAKE) MODE=desktop_test run`; `desktop_test` = `python3 run_desktop_test.py`.
- First full build in a fresh tree: ~10–20 min (ICU + 1 GiB disk). Run it in
  the background with a log and poll, e.g.
  `make MODE=desktop_test hobbyos.elf disk.img > /tmp/build.log 2>&1 &`
- Build artifacts: `hobbyos.elf`, `disk.img` (1 GiB). Do not run
  `make -n` on tier targets (recipes execute even under `-n`).

## (b) Screenshot

```bash
python3 tools/theme_shot.py /home/sarah/hobbyos-theme/evidence/baseline_dark.ppm
```

- Boots `MODE=desktop_test` headless with a QMP socket. In this tree
  (commit 722e3b4) desktop_test loads **EDITOR_T.BIN**: it auto-launches
  NETTEST.BIN then EDITOR.BIN, drives the UI, and prints
  `SCREENSHOT_READY` from the open File dialog. That is the readiness
  marker.
- The runner waits **up to 600 s** for the marker (stock script's 30 s is
  far too tight; even 240 s was exceeded on this host under TCG), then
  screendumps via QMP to the given path, and kills only its own QEMU.
- Serial output is teed to
  `/home/sarah/hobbyos-theme/evidence/theme_shot_serial.log`.
- Sanity: the runner rejects an all-black framebuffer.

## (c) Palette check

```bash
python3 tools/theme_check.py <file.ppm> --mode dark    # OLD dark theme expected
python3 tools/theme_check.py <file.ppm> --mode light   # NEW Chrome-light expected
python3 tools/theme_check.py <file.ppm> --mode dark --dump-samples   # eyeball RGB
python3 tools/theme_check.py <file.ppm> --mode dark --hist-top 12    # top colors
```

- Prints one `PASS`/`FAIL` line per check, then `VERDICT:`; exit 0 iff all
  pass. Checks: (a) sample points at fixed 1920x1080 coordinates; (b)
  whole-image exact-color presence thresholds: light —
  `#1A73E8` ≥ 300, `#FFFFFF` ≥ 200000, `#DADCE0` ≥ 2000 and **zero** pixels
  of the old dark distinctive palette; dark — the inverse.
- The dark palette list is empirical (derived from the captured baseline
  histogram). Calibration lives at the top of the file.
- Discrimination proof on the baseline: `--mode dark` PASS, `--mode light`
  FAIL (outputs saved in `evidence/theme_check_baseline.txt`).

## (d) QEMU acceptance tiers (run after the light merge; with the shim!)

```bash
PATH="/home/sarah/.hermes/cache/scratch/fs-shims/verify:$PATH" \
  WT=/home/sarah/hobbyos-theme/wt-verify python3 run_desktop_apps_test.py
#   Boots the real desktop, launches FILES/CALC/XCALC/XANTFARM/XEYES/CLOCK/
#   SYSMON/HEX/TASKS/FIND/DIFF/NOTES/UNIT via the start menu, screenshots to
#   /tmp/hobbyos_apps/, checks each app's serial marker. Exit 0 = pass.

PATH="/home/sarah/.hermes/cache/scratch/fs-shims/verify:$PATH" \
  WT=/home/sarah/hobbyos-theme/wt-verify python3 run_console_test.py
#   End-to-end console test: CONSOLE.BIN from Apps + right-click menus,
#   framebuffer-diff thresholds (BIG=150000/MED=3000/SMALL=8000, taskbar
#   strip excluded), screenshots to /tmp/hobbyos_console/. Exit 0 = pass.
#   Note: the script's default ROOT is ~/Documents/GitHub/HobbyOS — for a
#   worktree run, patch ROOT to the tree before invoking. Both scripts
#   broad-pkill internally; the shim above is mandatory on the shared host.
```

## (e) Browser receipt (Lane B) — x64-KVM path

- Lane B owns the browser chrome: `HobbyOS/docs/browser/ui-chrome.md`
  (WK5Chrome.h mirrors this palette), and its on-device receipt:
  - runner `wd_x64_runner.py` (boots KVM q35, F1-menu-launches the browser,
    waits for `[WIN] frame`):
    `python3 wd_x64_runner.py --disk ... --outdir /home/sarah/hobbyos-theme/evidence/browser-light --tag theme-light`
  - report: `HobbyOS/continuation/ui-theme/REPORT.md`
- The browser path is x64-KVM on this host; it is a **separate lane** — the
  light-mode receipt above only proves the ARM desktop/editor surface.

## Evidence & git policy

- PPMs and logs stay OUT of git under `/home/sarah/hobbyos-theme/evidence/`
  (a PPM is ~6 MB). `tools/theme_*.py`, `tools/theme_acceptance.md` and
  `THEME-VERIFY-REPORT.md` are committed on `theme/verify-tools`.
- No pushes, no merges to `main`.
