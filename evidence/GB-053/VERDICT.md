## S0/S1 Final Acceptance Run — ARM re-run with the GB-075 rig (2026-10-27)

Run with `python3 tools/t5/acceptance_run.py --arch arm` on the hermes-b53b5890 worktree at current `main`. disk.img built first (`make disk.img`, exit 0). This is the GB-075-hardened rig (whole-frame liveness + serial-load-line identity + whole-frame diff margin), which is the instrument GB-053-V's RED demanded. Verdict from OBSERVED evidence — the rig can now FAIL a title that never loads, so a real PASS requires the title to actually LOAD and render.

### Result: **FAIL** — 1 of 5 pinned native games loads via the launch path

Command (verbatim):

    make disk.img            # exit 0
    python3 tools/t5/acceptance_run.py --arch arm

Report emitted:

    | game      | slot | loaded | live frame | result |
    |-----------|------|--------|-----------|--------|
    | PONG.BIN   | 16   | yes    | True      | PASS   |
    | MILLIPED.BIN| 17  | yes    | True      | FAIL   |
    | XNIBBLER.BIN| 20  | no     | True      | FAIL   |
    | XCRYPT.BIN | 21  | no     | True      | FAIL   |
    | NES.BIN    | 22  | no     | True      | FAIL   |

Summary: **FAIL**

### Why — measured, not inferred

The serial shows only the AUTO-launched wave members reaching the process loader:

    [WAVE-TRY] DESKTOP.BIN   -> Loading program for scheduler (v2 AS): DESKTOP.BIN  -> [ARRIVED] rc>0
    [WAVE-TRY] PONG.BIN      -> Loading program ... PONG.BIN  -> [ARRIVED] rc>0
    [WAVE-TRY] MILLIPED.BIN  -> Loading program ... MILLIPED.BIN -> [ARRIVED] rc>0
    [WAVE-TRY] STARDUST.BIN  -> Loading program ... STARDUST.BIN -> [ARRIVED] rc>0

MILLIPED registers FAIL in the gate because its ["loaded=yes"] refers to the auto-launch, but the launch-path screendump for the launcher-driven XNIBBLER/XCRYPT/NES never produces a `Loading program` line for those BINs: they appear only in the [MENU] dump and the mcopy that stages them. The whole-frame diff margin then correctly declines to call a live-but-same frame a distinct title.

### Honest scope
- This leg is ARM (intel leg remains gated on GB-057, the intel spawn deadlock — still In Progress).
- "S0+S1 signed off" is the milestone judgment; this run says that judgment **cannot** yet be made green: three pinned native titles do not launch from the Apps menu into a running process on this tree.

### Files
- Evidence dir: evidence/acceptance/20261027T105835Z/ (serial.log, per-game .ppm)
- This run's artefact (this file) + manifest committed as evidence/gb-053/

The finding belongs to the launch-path class already owned by GB-088/GB-091 (launcher/desktop input forward); the acceptance can only sign S0+S1 once titles launch. This is recorded here as the honest current evidence, NOT a fabricated sign-off.
