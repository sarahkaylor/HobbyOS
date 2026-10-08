# BRIEF rp-h final — fill the baselines, run the missing scenario, close the report (OS lane)

You are the **rp-h finisher** lane for the HobbyOS render-pipeline workstream (verification
harness: perf receipts, tile-bleed probe, responsive-close probe). Background subagent;
the controller reviews and merges. You CANNOT ask questions — run, commit, report honestly.

## Ownership / boundaries

- You own: **`/home/sarah/hobbyos-lanes/rp-h`** (OS worktree), branch `browser/rp-h`.
  Only writer in this tree. Do NOT touch the main tree or other lanes.
- Host is shared (other lanes run QEMU + builds). NEVER bare `pkill -f qemu-system*`;
  probes kill only their own saved PIDs (they already do). Use `--instance rp-hf` and
  `--port 8830` for any new runs to stay socket-scoped.

## THE RACE — first action (do this before anything else)

The "frozen" WebProcess ELF at `~/webkit-hobbyos/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess`
(same path your doc calls the frozen binary) **already changed once**
(`b583486e…` → current `0a815545…`, the smoke-stage build) and the rp-f lane will rebuild
it again within the hour. The controller has pinned a copy for you:

- **Pinned baseline ELF**: `/home/sarah/hobbyos-scratch/rp-baseline-WP-0a8155451f2d.bin`
  — verify `sha256sum` == `0a8155451f2d73e6a43b057f6da21003d9af6b90908431eb847b09967acf512d`.
- From now on, ALL remaining baseline runs use `--wp-elf <pinned copy>`.
- Update the baselines doc to say this explicitly: the frozen-binary thread is
  `816805f9…` (original canonical, perf/tile-bleed receipts) → `b583486e…` (same build
  re-copied, run-111 + perf receipts) → `0a815545…` (current, smoke-path-inert — behavior
  identical for markerless runs; pinned copy = the comparison base for rp-f's after-runs).
  Note: the pinned copy is a raw copy of the ELF (not objcopy-flat) — sha differs from the
  flat; use whichever hash your probe reports, consistently.

## Deliverables

1. **Pin + doc note** (above), committed.
2. **Fill the perf table** in `docs/browser/render-pipeline-baselines.md` from the three
   existing evidence reports `tools/render_pipeline/evidence/perf-rp-perf{1,2,3}-*/report.json`
   (HOME + TALL rows: load-ok ms, render ms, blit ms, total ms, viewport apply end ms).
   Read numbers FROM the reports (no hand-typing from memory; spot-check one against its
   serial).
3. **Fill the tile-bleed table** from the existing evidence dirs
   (`tile-bleed-rp-bleed-{load,openclose,settle}-*/report.json`). The `gentle` scenario's
   report is missing — RE-RUN it against the pinned ELF:
   `tools/render_pipeline/run_rp.sh tile-bleed --wp-elf /home/sarah/hobbyos-scratch/rp-baseline-WP-0a8155451f2d.bin --scenario gentle --instance rp-hf --port 8830`.
   Record verdict + raw counters (pixel census) per scenario. If any other scenario's
   report is incomplete, decide: re-run bounded (≤2 extra runs total for the whole brief)
   or mark the cell `not-run (reason)` — honesty over completeness.
4. **Fill the responsive-close table** from `resp-close-rp-rclose-{midload,midsettle,starvation}-*/report.json`;
   run the MISSING `titlebar` scenario against the pinned ELF (same run discipline).
5. **Reconcile uncommitted work**: `tools/render_pipeline/run_rp.sh` and
   `tools/render_pipeline/tile_bleed_probe.py` have uncommitted modifications — inspect
   them (git diff), they are legitimate fixes from the original lane; commit with a clear
   message. Also commit `.gitignore` if it's a sensible evidence ignore, plus
   `BRIEF-rp-h-final.md`, `docs/browser/render-pipeline-baselines.md`, and the updated
   `rp-h-REPORT.json`.
6. **Close the report**: verdict `pass` if the doc is complete with real numbers and the
   remaining gates pass (`run_rp.sh` runs completed; report JSON valid); otherwise
   `partial` with the exact residual. Include in the report the pinned-ELF sha, the
   completed table values (or file references), and any run-to-run variance observed
   (TCG single-machine numbers — always labeled).

## Hard constraints

- One QEMU at a time from your side; respect the lock discipline the tools implement.
- Timebox ≤ ~1.5 h. Deadlock rule: a run stuck >30 s with no serial growth = investigate,
  don't just re-run (but a legit TALL load under TCG is minutes — judge by log growth).
- No invented numbers. "Not-run" is acceptable when labeled.
- Final message: compact summary + updated report JSON (lane=`rp-h`).
