**GREEN — GB-116-V3, verified on this VM (DEEP verifier `wgfx-verify`), code PR #226 @ `bb54494`. The default is now a live value, dead values are loud, and a real guest confirms the default actually runs.**

Evidence: `evidence/gb-116-v3/` — both QEMU runs' serials + replays, the acceptance-shape probe against a real intel guest, the helper/unknown-gesture probes, the CLI rejection, and the branch's file list. Local intel runs on this VM; no CI cited, used, or gated on.

## 1. The default is live and dead values raise

`tools/evidence_rig.py:170`:
```python
def launch_from_menu(q, slot, taskbar_y=H - 26, arch="arm", gesture="keys"):
```
`tools/evidence_rig.py:199-204`:
```python
    else:
        # A dead default is worse than a loud one: `perf_game_cycle.py` and
        # `t5/acceptance_run.py` both call this with no gesture, so an unknown
        # value here silently opens nothing and the launch fails for a reason
        # that is not in the log (GB-116-V2 clause 6).
        raise ValueError("launch_from_menu: unknown gesture %r (want 'keys' or 'click')" % (gesture,))
```
A call with an unknown value, before any input is sent:
```
gesture='auto': ValueError(launch_from_menu: unknown gesture 'auto' (want 'keys' or 'click'))  events_before_raise=[]
gesture='keyz': ValueError(launch_from_menu: unknown gesture 'keyz' (want 'keys' or 'click'))  events_before_raise=[]
gesture=None:   ValueError(launch_from_menu: unknown gesture None (want 'keys' or 'click'))    events_before_raise=[]
```
The CLI still refuses it at the parser, so the raise is the library backstop:
```
evidence_rig.py: error: argument --gesture: invalid choice: 'auto' (choose from 'keys', 'click')
```

## 2. Both in-tree callers now take the F1 path — inspection **and** a real invocation

Inspection (`tools/evidence_rig.py:170`) plus the default-call probe:
```
launch_from_menu(q, 3)            <- what acceptance_run.py / perf_game_cycle.py do
  f1=1  clicks=0  events=[('key', 'f1'), ('key', 'home'), ('key', 'down'), ('key', 'down'), ('key', 'down'), ('key', 'ret')]
```
Real guest, the same call shape (`R.launch_from_menu(q, slot)`, no gesture) on intel:
```
$ python3 evidence/gb-116-v3/probe_acceptance_shape.py intel
slot for XNIBBLER.BIN: 20
[LAUNCH] count (no gesture, acceptance_run shape): 1
   [LAUNCH] XNIBBLER.BIN
```
That is the exact path `tools/t5/acceptance_run.py:200` and `tools/perf_game_cycle.py:281` take — the V2 RED is closed by measurement, not by reading.

## 3. One intel run with no `--gesture` launches

```
$ python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch intel --run-seconds 10
launch=1   [LAUNCH] XNIBBLER.BIN   replay: f1=1 click=0   (204001Z)
```

## 4. The falsifier still falsifies

```
$ python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch intel --gesture click --run-seconds 10
launch=0                            replay: f1=0 click=1   (204031Z)
```

## 5. No other behaviour changed

```
$ git diff --name-only 57d8acf bb54494
gaming.md
tools/board/import_seed.py
tools/evidence_rig.py
```
The plan row (the program's convention) and the rig; no other code file. `bb54494` alone is `tools/evidence_rig.py | 8 +++++++-`.

## 6. GB-116-V2's clauses 1-5 are not perturbed

* V2-1 (`keys` default launches on both arches): intel re-measured here (`launch=1`); ARM was measured in V2 (`launch=1`) and this commit changes only the signature default and adds the raise.
* V2-2 (the falsifier): intel `click` → 0 here; ARM `click` → 1 in V2.
* V2-3 (`auto` rejected): unchanged, quoted in 1.
* V2-4 (the toggle is documented): the docstring still names `desktop.c:2218` and "Do NOT press the click *after* F1".
* V2-5 (the replay records the gesture): `f1=1 click=0` on the default run, `f1=0 click=1` on the click run.

Nothing in this commit perturbs them.

## Verdict

GREEN. GB-116 (#234) → **Merge Gate**; GB-116-V3 (#237) → **Done**.

One line for the waiting cards: `--gesture keys` is the rig's default and the library default, and it launches a menu-selected title on intel **and** ARM — including through `t5/acceptance_run.py`'s call shape, measured here. The wall those cards were waiting on is gone, and it is now reachable without any flag.