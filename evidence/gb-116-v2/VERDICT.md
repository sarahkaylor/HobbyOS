**RED — GB-116-V2, verified on this VM (DEEP verifier `wgfx-verify`), code PR #226 @ `a12ef79`. Clauses 1-5 are GREEN and the wall has moved; clause 6 fails, because the shared helper's own default is now a no-op and the acceptance instrument that imports it no longer opens the menu.**

Evidence: `evidence/gb-116-v2/` — four QEMU runs' serials + replays, the helper and acceptance-shape probes, the argparse rejection and diff stat. Local intel and ARM runs, this VM; no CI cited, used, or gated on.

## 1. `keys` is the default and it launches on both arches — no flag passed

```
intel  --run-seconds 10 (no --gesture)  -> launch=1   [LAUNCH] XNIBBLER.BIN   replay: f1=1 click=0   (201336Z)
arm    --run-seconds 10 (no --gesture)  -> launch=1   [LAUNCH] XNIBBLER.BIN   replay: f1=1 click=0   (201854Z)
```
Slot resolved by name = 20 in both runs.

## 2. The falsifier still falsifies

```
intel  --gesture click -> launch=0                                        (201405Z)  replay: f1=0 click=1
arm    --gesture click -> launch=1   [LAUNCH] XNIBBLER.BIN                (201926Z)  replay: f1=0 click=1
```

## 3. `auto` is gone

```
$ python3 tools/evidence_rig.py --gesture auto --game X --bin X
evidence_rig.py: error: argument --gesture: invalid choice: 'auto' (choose from 'keys', 'click')
```
Help lists `[--gesture {keys,click}]`.

## 4. The toggle is documented

`tools/evidence_rig.py:184-191`:
```
    Do NOT press the click *after* F1: the Apps button toggles the same menu shut
    (`desktop.c:2218` gates F1 on `!start_menu_open && !app_menu_open`), so an
    "F1 then click" gesture measures 0 on intel *and* 0 on ARM -- GB-116-V’s RED,
    which is why the first cut of this card (`--gesture auto`) was withdrawn.
```

## 5. The replay records the gesture

`f1=1 click=0` in both default runs; `f1=0 click=1` in both click runs (quoted in 1 and 2).

## 6. FAILS — the helper's default is a no-op, and two in-tree callers use it

`tools/evidence_rig.py:170` still declares:
```python
def launch_from_menu(q, slot, taskbar_y=H - 26, arch="arm", gesture="auto"):
```
and the body after `a12ef79` handles only `"keys"` and `"click"` — `"auto"` matches neither branch, so **no F1 and no click are sent**. Two callers in this tree pass no gesture:

* `tools/t5/acceptance_run.py:200` — `launch_from_menu(q, slot)` (the S4 acceptance instrument)
* `tools/perf_game_cycle.py:281` — `launch_from_menu(q, slot)`

Measured, not inferred:

```
$ python3 evidence/gb-116-v2/probe_helper_default.py
signature: (q, slot, taskbar_y=1054, arch='arm', gesture='auto')

launch_from_menu(q, 3)            <- what acceptance_run.py / perf_game_cycle.py do
  f1=0  clicks=0  events=[('key', 'home'), ('key', 'down'), ... ('key', 'ret')]
launch_from_menu(q, 3, gesture='keys')
  f1=1  clicks=0  events=[('key', 'f1'), ('key', 'home'), ...]
launch_from_menu(q, 3, gesture='click')
  f1=0  clicks=1  events=[('click', 38, 1067), ('key', 'home'), ...]
```

And against a real ARM guest — the arch where the pre-GB-116 click worked — the acceptance-run shape launches nothing while both explicit gestures do:

```
$ python3 evidence/gb-116-v2/probe_acceptance_shape.py arm
slot for XNIBBLER.BIN: 20
[LAUNCH] count (no gesture, acceptance_run shape): 0
```
(default run: 1, click run: 1 — clauses 1 and 2 above.)

This is the outcome clause 6 exists to rule out: the diff is confined to `evidence_rig.py` plus the card text, but the *behaviour of other tools* changed. An acceptance run that opens no menu reports `[LAUNCH] = 0`, which is indistinguishable from the wall those five cards are waiting on — the same false-signal class this program keeps paying for.

**Fix (one line):** make the signature default `gesture="keys"` (and treat an unknown gesture as `keys`), then re-run the acceptance-shape probe above and require 1.

## What the board should hear

`--gesture keys` (now the CLI default) **does** launch a menu-selected title on intel **and** ARM, measured four ways: the wall `GB-039`, `GB-039-V`, `GB-041`, `GB-053`, `GB-059-V` and `GB-088` are waiting on is gone **for the rig CLI**. It is not yet gone for `tools/t5/acceptance_run.py`, which imports the helper and passes no gesture — that is this RED.

## Consequence

GB-116 (#234) → **In Progress** with clause 6 named; GB-116-V2 (#236) → **Done**. Clauses 1-5 should not be re-run: they are quoted above and they pass.