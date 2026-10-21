# GB-059-V — no verdict issued: half the DoD depends on an artifact that does not exist (GB-041)

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-21.
Tree: `origin/main` @ `79d14e7`, clean worktree, `make disk.img` built here (EXIT=0).
Claimed at 03:3xZ because the card was `Ready`; measured; returned to `Backlog`+`blocked`.

**This is not a RED on GB-059.** Three and a half of the four criteria pass as measured; criterion 1's
second half ("**both** titles", i.e. a compat title) cannot be run because there is no compat title in
the tree — GB-041 (`Doom as static-musl ELF under compat`, lane `Ready`) has not landed.

## What passes, measured

```
$ python3 tools/evidence_rig.py --game XNIBBLER --arch arm        # 01_rig_xnibbler_arm.log
[rig] booting arm QEMU, capturing to evidence/XNIBBLER/20261021T034428Z
[rig] XNIBBLER.BIN slot: found by name = 19 (launching 19, by name)
[rig] evidence bundle: … report: XNIBBLER-EVIDENCE-REPORT.md
EXIT=0
```

Launched **by name** out of the parsed `[MENU]` dump — no slot index was supplied. The frame is a real
game frame, not a desktop fill (centre region, x 25–75 % × y 20–80 %, 1920×1080):

```
boot      70 distinct colours, 100.0% nonzero px
launch     5 distinct colours, 100.0% nonzero px
gameplay   5 distinct colours, 100.0% nonzero px
boot -> launch: 2026751/2073600 px changed (97.74%)
launch -> gameplay: 48/2073600 px changed (0.00%)      # 02_frame_stats.log
```

and with scripted input the same comparison moves:

```
$ python3 tools/evidence_rig.py --game XNIBBLER --arch arm --input down,down,left --run-seconds 10
[rig] XNIBBLER.BIN slot: found by name = 19 (launching 19, by name)   EXIT=0
launch -> gameplay: 9748/2073600 px changed (0.47%)                    # 04_frame_stats_input.log
```

`vision_analyze` on `gameplay.png` reads: window titled **xnibbler**, taskbar tab `xnibbler`, `SCORE 0`
top-left, a three-block green snake and one orange food block — i.e. the game drew and the board is live,
and the 48 px vs 9748 px contrast shows the frame responds to input rather than sitting on a static splash.

```
$ git ls-files | grep -E 'run_(doom|stardust|xnibbler|xcrypt)_test\.py'
(none tracked)                                        # criterion 2: the four hand-rolled runners are gone
$ grep -nE 'slot\s*=\s*[0-9]+|index\s*=\s*[0-9]+' tools/evidence_rig.py
(no literal assignments)                              # criterion 3: no magic slot index
$ grep -nE 'add_argument' tools/evidence_rig.py
192: --game (required)   193: --bin (default None)   195: --slot (default None) …
```

Criterion 4 (run from a worktree) is satisfied by construction: this run happened in
`.worktrees/hermes-fd0edabc`, with `disk.img` built in that worktree.

## What cannot be measured: criterion 1's compat half

```
$ grep -niE "compat" tools/evidence_rig.py            -> no compat path in the rig
$ mdir -i disk.img ::/ | grep -iE '\.elf|doom'        -> nothing
GB-041 (#41): lane Ready — "Doom as static-musl ELF under compat (W4)"
```

There is no compat title on this tree to hand `--game <compat title>`. A GREEN here would certify
"both titles" having run one, which is exactly the shape of gate the honesty law forbids; a RED would
blame GB-059's rig for a missing dependency.

## Unblock

1. GB-041 lands and a compat title is launchable through the rig (same by-name path).
2. Re-mint (or re-scope) the DoD: if the intent is "the rig works by name for whatever titles exist",
   the wording should say so, and the compat half becomes its own check once GB-041 is Done.

Everything measured above is worth keeping: the rig does what GB-059 asked of it for the native track.
