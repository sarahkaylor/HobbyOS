**RED — GB-116-V, verified on this VM (DEEP verifier `wgfx-verify`), code PR #226 @ `792f9fb`. Clause 1 is GREEN and the diagnosis is right; clause 2 fails, and it fails on both arches: `--gesture auto` launches nothing, while `keys` and the pre-patch `click` both launch.**

Evidence: `evidence/gb-116-v/` — every run's `serial.log` + `replay.jsonl`, the probe copy of the rig used for the mechanism test, and the run stdout. Local intel and ARM QEMU runs on this VM; no CI is cited, used, or gated on.

## The measurements

XNIBBLER.BIN, slot resolved by name = 20, `--run-seconds 10`, `grep -c '[LAUNCH]'` on the captured serial:

| arch | `--gesture click` | `--gesture keys` | `--gesture auto` (the default) |
|---|---|---|---|
| **intel** | **0** | **1** — `[LAUNCH] XNIBBLER.BIN` | **0** (run twice: 192658Z, 192748Z) |
| **arm** | **1** — `[LAUNCH] XNIBBLER.BIN` | **1** — `[LAUNCH] XNIBBLER.BIN` | **0** (193347Z) |

Clause 1 is therefore met:
```
$ grep -c '\[LAUNCH\]' intel_click.serial.log   -> 0
$ grep -c '\[LAUNCH\]' intel_keys.serial.log    -> 1
[LAUNCH] XNIBBLER.BIN
```

## Clause 2 fails

> *"`auto` is not worse than `keys` on intel, and neither regresses ARM."*

On intel `auto` = 0 launches against `keys` = 1. On ARM `auto` = 0 against the pre-patch `click` = 1 — **the patch regresses ARM's previous default**, because `auto` is now the default and sends the click as well.

## Why — the mechanism, demonstrated

`auto` is `F1` (opens the menu) followed by the click on the taskbar Apps button, which **toggles the same menu shut**. `desktop.c:2218` gates F1 on `if (!start_menu_open && !app_menu_open)`, so F1 only opens; the click then closes what F1 just opened, and the `home`/`down` navigation that follows goes to a closed menu.

Discriminator, run with a probe copy of the rig (`evidence/gb-116-v/evidence_rig_probe.py`; **not** the submitted file) that sends **F1, click, F1** and is otherwise identical:
```
$ python3 evidence/gb-116-v/evidence_rig_probe.py --game XNIBBLER --bin XNIBBLER.BIN --arch intel --gesture reopen --run-seconds 10
launch=1        [LAUNCH] XNIBBLER.BIN
replay: {"t":"key","k":"f1"} {"t":"mouse",...} {"t":"click",...} {"t":"key","k":"f1"} {"t":"key","k":"home"} ...
```
The click did not wedge the input path — a second F1 re-opened the menu and the title launched. So `auto` is "open, then close", not "open".

**What would earn clause 2:** in `auto`, either click first and press F1 only if the menu is still shut, or on intel send F1 only (the click has no device there — clause 5's own evidence). Then re-run all four cells above.

## Clause 3 — met

```
intel_keys.replay.jsonl : {"t":"key","k":"f1"} ...                 f1=1, click=0
intel_auto.replay.jsonl : {"t":"key","k":"f1"} {"t":"mouse",...} {"t":"click",...}   f1=1, click=1
intel_click.replay.jsonl: {"t":"mouse",...} {"t":"click",...}      f1=0, click=1
```

## Clause 4 — met, with the default changed in the same edit

```
$ git diff --stat 57d8acf 792f9fb
 gaming.md                  |  1 +
 tools/board/import_seed.py |  1 +
 tools/evidence_rig.py      | 34 +++++++++++++++++++++++++++++-----
 3 files changed, 31 insertions(+), 5 deletions(-)
```
Inside `tools/evidence_rig.py` the change is `launch_from_menu()` (signature + gesture selection), the `--gesture` argparse option, and the call site — no edit to `find_menu_slot`, `boot()` or the replay writer. The card text accounts for `gaming.md`/`import_seed.py`. Worth noting for the record: the one thing that *is* global is the default — `auto` — which is what clause 2 finds broken.

## Clause 5 — met; the premise is correct

* `Makefile:257` (intel `QEMU_CMD`): no pointer device at all — `0` matches for `tablet|mouse` on that line.
* `Makefile:275` (ARM `QEMU_CMD`): `-device virtio-tablet-device`.
* `src/user/desktop.c:2212-2216`:
```c
            } else if (ev->code == 59) {
              /* F1: keyboard path to the Apps menu.  The desktop's menu is
               * normally opened by clicking the Apps button; on x64 the
               * kernel's PS/2 input path does not deliver mouse buttons
               * (browser.md, WF1B), so an F1 map keeps the menu launchable
               * on both arches.  ...
```
The intel click failure is real and reproduced (`click` → 0 launches). What the card did not measure is that adding the click *after* F1 undoes the F1.

## Consequence

Clause 1 says a `keys` gesture does launch a menu-selected title on intel — so the input pipe is **not** broken, and the five cards waiting on "a menu-selected title launches" are not blocked by it. But the submitted gesture plumbing does not deliver that: `auto` is 0 on both arches, so any caller that does not pass `--gesture keys` still sees no launch. GB-116 (#234) → **In Progress** with this failing check; GB-116-V (#235) → **Done**. One line echoed onto GB-088 (#141) as the card asked.