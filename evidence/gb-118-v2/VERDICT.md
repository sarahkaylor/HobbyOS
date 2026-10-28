# GB-118-V2 — verdict: GREEN (all eight clauses measured at `35f1fb41`; the `--stage` fix reaches the image)

Subject: PR **#231**, graded head **`35f1fb41`** (`35f1fb4` — "fix make-eat `$ROOT` in RUN_STAGE (use
`$$RUN_STAGE`)"), in a fresh detached worktree. Host: farm-3 VM, QEMU ARM guest (the rig's default and
the arch on which its launch gesture works). **No CI consulted.**

## Clause matrix

| # | Clause | Result |
|---|---|---|
| 1 | Staging off by default AND on when asked | **PASS** — no flag: 0 menu entries for the fixture; with flag: the guest loads the staged bytes |
| 2 | After the image, before QEMU, from ONE run | **PASS** — `=== run: stage ===` then boot then the guest's load line, one log |
| 3 | The guest proves the fixture with its own output | **PASS** — `v2 loader: image mapped (lazy), bytes=38120`, matching the host artefact's size (sha printed) |
| 4 | Idempotent | **PASS** — two staged runs, both `bytes=38120` |
| 5 | A failed stage is fatal | **PASS** — `run: STAGE FAILED - refusing to boot a stale image`, exit 2, **0** QEMU lines |
| 6 | Both traversal refusals | **PASS** — both refusals, exit 2, no boot |
| 7 | `make host_tests` green | **PASS** — `Checks run: 482 / Checks failed: 0`, `TEST EXIT: 0` |
| 8 | A test proves rig↔tier agreement, and can fail | **PASS with a limitation named** — the test executes and is falsifiable, but both "runners" are the same shell call |

## 1, 2, 3 — the stage reaches the image, proved by the guest

Fixture: `obj/arm/hello.bin`, **38120 bytes**, sha256 `cd32e67…4608d4fa`.

**(a) off by default** — `python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch arm`
(the only difference from the run below being the absent flag):
```
[rig] booting arm QEMU, capturing to evidence/XNIBBLER/20261028T092903Z
[rig] XNIBBLER.BIN slot: found by name = 20 (launching 20, by name)
$ grep -c RIGFIX .../serial.log
0
```

**(b) on when asked** — `--stage obj/arm/hello.bin:XNIBBLER.BIN`, one log, stage line before the boot:
```
303:=== run: stage ===
...
499:[LAUNCH] XNIBBLER.BIN
v2 loader: image mapped (lazy), bytes=38120 pages=10 first=in elab=0ms
[ARRIVED] XNIBBLER.BIN rc>0
```
`bytes=38120` is the staged file's size, not `XNIBBLER.BIN`'s — the guest loaded *my* host bytes at the
named guest path. That is the proof clause 3 asks for, in the only form this rig can produce (see
residual 1).

**(c) idempotent** — the same command twice, one minute apart:
```
evidence/XNIBBLER/20261028T093301Z: [LAUNCH] XNIBBLER.BIN … bytes=38120 pages=10
evidence/XNIBBLER/20261028T093326Z: [LAUNCH] XNIBBLER.BIN … bytes=38120 pages=10
```

## 5, 6 — fatality and both refusals

```
$ RUN_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/no-such-fixture ::BAD.BIN' make run ARCH=arm
=== run: stage ===
/tmp/no-such-fixture: No such file or directory
run: STAGE FAILED - refusing to boot a stale image
make: *** [Makefile:2478: run] Error 2      RC=2      (qemu lines after the stage: 0)
```
```
[rig] STAGE REFUSED - refusing to boot a stale image: stage: host source outside the repo root is refused: '/etc/hostname' (root='…/verify-gb118v2')     RC=2
[rig] STAGE REFUSED - refusing to boot a stale image: stage: guest path traversal refused: '../EVIL.BIN'                                              RC=2
```

## 7, 8 — the in-gate run and the agreement test

```
  PASS  tools/evidence_rig_test.py
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
```
Standalone, for the clause's own wording:
```
PASS: both runners execute the stage (rig RUN_STAGE == tier T3_STAGE)
PASS: guest-visible fixture lands via the stage (content round-trips)
PASS: agreement test is falsifiable (a different fixture differs)
PASS: rig and tier agree: the SAME fixture is guest-visible after either stage
Tests run: 7, failed: 0
```

## Three residuals, disclosed rather than tidied away

1. **The rig's only guest-side instrument is a capped listing.** `[MENU]` is `MAX_MENU_ITEMS = 128`
   (`src/user/desktop.c:31`) and the arm image carries **176** root entries, so a *newly staged root
   name* falls outside the dump. Measured: staging `RIGFIX.BIN` (a name that is not already in the menu)
   puts the file on the image — `mdir -i disk.img ::/RIGFIX.BIN` → `1 file 38120 bytes` — but the rig
   then reports `[rig] WARNING: RIGFIX.BIN not in start-menu dump` / `FATAL` and exits 1. So the documented
   flag works, yet "stage a *new* fixture and prove it" is unmeasurable through this instrument on a
   realistic image; the runs above therefore stage over an existing menu name. This is not the `$ROOT`
   bug the card was minted for — it is the proof instrument's next limit, and it should be named before
   a rig-driven test is built on staging a new file.
2. **Clause 8's "agreement" is between two identical shell invocations.** In
   `test_stage_executes_both_paths_one_fixture()` both `ra` and `rb` are
   `_run_shell(cmd, root, root, mtools_path)` with the same `cmd` — the test proves the stage *command
   shape* survives `ROOT=<dir> sh -c` and that content round-trips and perturbation is caught. It never
   invokes `make run` or `t3_tier.sh`. The two real consumers are covered by my runs instead: the make
   recipe at clause 5 (`RUN_STAGE` via `$$RUN_STAGE`, fatality observed) and the tier's own
   `ROOT="$ROOT" sh -c "$T3_STAGE"` (`tools/t3_tier.sh:183`) which this card quotes as the reference.
   The test is useful; it is not the evidence its name suggests.
3. **The staged program's own stdout did not reach the serial** — the guest loaded the staged bytes
   (`bytes=38120`), arrived, and exited (`[EXITPC] pid=3 last_user_pc=0x0`) without printing `[HELLO]`
   lines. The content proof rests on the loader's byte count, not on program output; I did not chase why
   hello's console writes are absent because that is not this card's clause.

## What this verdict claims and does not

- Claims: the `$ROOT`-eaten-by-make defect is fixed and the rig stages on the tier's terms — off by
  default, after the image, before QEMU, fatal on failure, both traversal refusals, idempotent,
  in-gate green.
- Does not claim: that a *new* guest name can be proven through the rig on a full image (residual 1), or
  that the agreement test exercises the two real consumers (residual 2).

## Reproduce

```
git worktree add /tmp/w118 --detach 35f1fb41 && cd /tmp/w118   # + third_party symlinks, mkdir obj
export PATH=/usr/sbin:$PATH && make disk.img ARCH=arm -j32
python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch arm                    # absent
python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch arm \
        --stage obj/arm/hello.bin:XNIBBLER.BIN                                               # present, bytes=38120
RUN_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/nope ::BAD.BIN' make run ARCH=arm                # fatal, rc=2
make host_tests                                                                              # 482/0
```