# G3 evidence bundle (launcher graphics acceleration)

Raw evidence for lane G3 of the GX program (docs/graphics-accel.md §3-D5).
All commands were run in the lane worktree (`~/hobbyos-lanes/gx-launch`) on
2026-10-04; QEMU 10.2.1, X11 `:20`, host GL = llvmpipe.

- `matrix.txt` — `make -n` selection matrix (GX-BRIEF.md §5 items 1–6)
- `gpu-check.txt` / `gpu-check-nodisp.txt` — `make gpu-check` with and
  without `DISPLAY`
- `boot-arm-soft.txt` — item 7: ARM soft/headless boot + QMP screendump (PASS)
- `boot-arm-gl.txt` — item 8: ARM gl boot + X11 capture (PASS; the
  `[IDLESTUCK]` lines are the §1-documented GL-device anomaly, not a gate)
- `e2e-desktop-test.txt` — item 9: `python3 run_desktop_test.py` green

Note (item 9): `run_desktop_test.py` internally runs `pkill -9 -f
qemu-system-*` (global). To honor the lane rule "never signal other lanes'
QEMUs", it was run with a PATH shim (a `pkill` wrapper that only signals
processes whose cwd is inside this worktree). The shim was unit-proven: a
worktree-cwd dummy was killed, a `/tmp`-cwd dummy survived; during the
actual E2E a sibling lane's qemu (cwd `~/hobbyos-lanes/gx-arm`) was alive
and survived. The test result is unaffected (`[TEST] OK`, exit 0).
