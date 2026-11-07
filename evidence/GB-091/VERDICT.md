# GB-091 — DOOM input-response, intel T5 re-measured with the GB-116 F1-menu launch (2026-10-28)

Intended input signal test, done the way the DoD requires: launch DOOM from the **Apps
menu** (F1 keys gesture, not a manifest; `[LAUNCH] DOOM.BIN`, slot 18, `[ARRIVED] DOOM.BIN
rc>0`), send QMP keys, and diff whole frames.

## Measurement

Two rig runs on current main (branch ticket/91-doom-input-t5), `tools/evidence_rig.py --game
doom --arch intel --gesture keys`, whole-frame mean-abs-diff (acceptance_run.ppm_whole_mad):

```
WITH input    (--input right,up,right,up, run 09:21:36):  launch->gameplay mad = 0.20577
NO-input      (control,                run 09:24:05):  launch->gameplay mad = 0.20692
```

Both legs land in evidence/doom/<ts>/. DOOM reached the process loader on both
(`[LAUNCH] DOOM.BIN` -> `[ARRIVED] DOOM.BIN rc>0`, via Apps-menu F1 keys gesture).

## Result: input clause RED (intent signal does not move the frame)

**with-input delta (0.20577) == no-input delta (0.20692)** — identical within noise. The QMP
`right,up,right,up` keys changed nothing beyond DOOM's own title auto-demo. The title renders
gameplay (live frame, large boot->gameplay delta) but **does not respond to the sent keys**:
the desktop->window keystroke forward is still dead on intel. This is the same wall the
earlier GB-091 evidence flagged, now re-measured with the corrected (GB-116) Apps-menu launch.

## DoD mapping
- "title launched through the Apps menu, not a manifest" — PASS (F1 keys gesture, slot 18).
- "with input: whole-frame delta exceeds margin **and** a key changes position/HUD" — **FAIL**:
  with-input == no-input (0.20577 vs 0.20692), no key-driven change observed.
- "without input: two frames identical" — the auto-demo makes this clause unanswerable as
  written; the meaningful control is that input ADDS nothing over the no-input baseline.

## Verdict
GB-091 stays **In Progress**. The observed wall is GB-088 (desktop->window keystroke forward
on intel). This is a fresh, correctly-instrumented measurement; not a fabricated pass.
