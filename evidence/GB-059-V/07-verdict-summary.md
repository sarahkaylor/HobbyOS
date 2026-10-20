# GB-059-V — gate run manifest

- Verifier: DEEP (DeepSeek v4.1 Flash, dgx1), worktree `hermes-37d612f8` (**ran from the worktree, DoD 4**), host **swarm-farm-2**
- Date: 2026-10-20 (UTC)
- Revision run: **98247dd9eccbf9ec434a8841abb1b3b267dea5f5** — `HEAD` == `origin/main` (fetched), tree clean
- Env: `dist-staging` copied from this VM's main checkout before `make ARCH=arm disk.img` (no dist-staging in a fresh worktree)

## Gate, literal

```
$ python3 tools/evidence_rig.py --game XNIBBLER --arch arm
usage: evidence_rig.py [-h] --game GAME --bin BIN --slot SLOT …
evidence_rig.py: error: the following arguments are required: --bin, --slot
exit=2
```

The rig cannot be driven by name alone — `--bin` and `--slot` are **required**:

```
171:    ap.add_argument("--slot", type=int, required=True, help="launcher slot index")
196:    slot = args.slot
197:    found = find_menu_slot(serial_log, args.bin)
198:    print(f"[rig] {args.bin} slot: expected={slot} found={found}")   # found is printed, never used
```

## The run, with a magic index supplied

```
$ python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --slot 0 --arch arm --root $PWD --run-seconds 6
[rig] XNIBBLER.BIN slot: expected=0 found=19
```

Guest's own dump — the title is at 19, slot 0 is CONSOLE.BIN:

```
[MENU] 0=CONSOLE.BIN
[MENU] 19=XNIBBLER.BIN
Loading program for scheduler (v2 AS): CONSOLE.BIN      <-- what actually launched
```

The bundle's report then claims `Game: XNIBBLER.BIN (launcher slot 0, arm)` — it stamps the CLI index,
not the measured one, so the evidence file asserts a launch that did not happen.

Control run with the correct index — proving the rig *works* when hand-fed the slot, which is exactly
the anti-pattern DoD 3 forbids:

```
$ python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --slot 19 --arch arm …
[rig] XNIBBLER.BIN slot: expected=19 found=19
Loading program for scheduler (v2 AS): XNIBBLER.BIN      <-- correct app this time
```

## DoD scorecard

| clause | result |
|---|---|
| 1 both titles launch **by name** from the parsed dumper | **FAIL** — by name it launches the wrong app (CONSOLE.BIN); the compat leg has no title at all: `mdir` shows no `.ELF` on `disk.img` |
| 2 four hand-rolled runners deleted | **PASS** — `git ls-files` grep returns nothing |
| 3 no magic launcher slot index | **FAIL** — `--slot` is a required CLI argument and is the value used to navigate; `find_menu_slot`'s answer is printed only |
| 4 run from a worktree | **PASS** — `cwd=/…/.worktrees/hermes-37d612f8`, `--root` = that path |

## Incidental

First attempt died `RuntimeError: QMP socket never appeared` because `boot()` allows ~100 s for the QMP
socket while a cold `make run` is still configuring LLVM. Pre-building `make ARCH=arm disk.img` made the
rig work. Worth knowing for the next operator; not this ticket's failure.

## Verdict

**RED** on DoD 1 and DoD 3. GB-059 (#61) → In Progress.
