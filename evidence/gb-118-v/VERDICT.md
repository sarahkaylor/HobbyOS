**RED — GB-118-V, graded at `50e65fd72e0c5d91df04c4a0135df09e03356ae6` (PR #231, branch `ticket/GB-118-rig-staging`). DEEP verifier `wgfx-verify`; every run local, no CI cited, used, or gated on.**

The refusals, the fatality rule and the tests are all in place. **The happy path does not work: `--stage` never puts the file on the image**, because the Makefile's `RUN_STAGE` hook hands the command to make's expansion, which eats `$ROOT`. Clause 1's second half, and therefore clauses 2, 3 and 4, cannot be measured.

Evidence: `evidence/gb-118-v/` (off-default run, staged run + serial, clause-5 bad-source run, clause-6 refusals, mutation of the unit test, host_tests log/rc).

## The failing run, verbatim

```
$ python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch intel \
      --stage evidence/gb-118-v/GBFIX.BIN:GBFIX.BIN --run-seconds 6
[rig] stage (RUN_STAGE): mcopy -o -i "$ROOT/disk.img" "/home/…/wt-gb118v/evidence/gb-118-v/GBFIX.BIN" "::GBFIX.BIN"
[rig] booting intel QEMU, capturing to evidence/XNIBBLER/20261028T081044Z
… traceback …
rc=1
```
What the guest-side log (the Makefile's own shell) says:
```
=== run: stage ===
Can't open OOT/disk.img: No such file or directory
Cannot initialize '::'
::GBFIX.BIN: No such file or directory
run: STAGE FAILED - refusing to boot a stale image
make: *** [Makefile:2476: run] Error 2
```
`$ROOT` arrived as `$R` + `OOT`: the recipe is `$(SHELL) -c '$(RUN_STAGE)'`, so make expands the stage string — and the substituted text is expanded again in the same pass. The T3 tier does not have this problem because it runs `ROOT="$ROOT" sh -c "$T3_STAGE"` (`tools/t3_tier.sh:183`), i.e. a **shell** expands the variable, not make. So the rig and the tier are *not* one mechanism: the tier's stage runs, the rig's cannot.

That clause 5's `$(PWD)` form works through the same recipe proves the shell is fine and only `$ROOT` is corrupted:
```
$ RUN_STAGE='mcopy -o -i "$(PWD)/disk.img" /nonexistent_fixture ::BAD.BIN' make ARCH=intel run
=== run: stage ===
/nonexistent_fixture: No such file or directory
run: STAGE FAILED - refusing to boot a stale image
make: *** [Makefile:2476: run] Error 2      (rc=2)
```

## Clause by clause

**1. Off by default — half met, half FAILS.** No `--stage`: the guest's own listing proves the fixture absent —
```
$ python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch intel --run-seconds 6
grep -c GBFIX serial.log -> 0 ;  [MENU] count=128
```
With `--stage`: the run above dies before QEMU; there is no guest to list anything. A flag that can never stage fails this clause as surely as one that is always on.

**2. Stage after image, before QEMU, in one log — FAILS.** The stage line and its order are visible (`=== run: stage ===` follows the `disk.img` recipe lines and precedes the abort), but there is no *guest proof* line after it, because the boot never happens. Order in a single log cannot be shown for a stage that fails.

**3. Guest proves the fixture — FAILS.** Impossible: `grep -c GBFIX` in the staged run's serial is 0 and the guest never starts. Nothing to quote beside the host hash (`sha256 14a76fc1903791e498ac8c90d711924be41afbeb5a36fd3da10d2d5fda717cb4  evidence/gb-118-v/GBFIX.BIN`).

**4. Idempotent — FAILS.** Two runs cannot both stage; the first exits 2 at the stage step.

**5. A failed stage is fatal — MET.** Quoted above: `run: STAGE FAILED - refusing to boot a stale image`, `Error 2` (rc=2), and zero boot markers in the output (`Controller reset`/`[MENU]` count = 0). Note this property currently holds for *every* stage, because every stage using `$ROOT` fails — the assertion is satisfied, the feature is not.

**6. Both traversal refusals — MET**, all with rc=2:
```
--stage /tmp/outside_fixture.bin:X.BIN
  STAGE REFUSED - refusing to boot a stale image: stage: host source outside the repo root is refused: '/tmp/outside_fixture.bin' (root='/home/…/wt-gb118v')
--stage evidence/gb-118-v/GBFIX.BIN:../X.BIN
  STAGE REFUSED …: stage: guest path traversal refused: '../X.BIN'
--stage evidence/gb-118-v/GBFIX.BIN:/abs/X.BIN
  STAGE REFUSED …: stage: guest path must be a relative FAT name, not an absolute path that escapes the image root: '/abs/X.BIN'
```

**7. `make host_tests` green — MET.** `rc=0`, `Checks failed: 0`, `ALL APPS SUITE TESTS PASSED`, `TEST EXIT: 0`, with `tools/evidence_rig_test.py` executing in the run.

**8. A test proves rig↔tier agreement and can fail — MET on its letter, and it is the wrong test.** The test exists (`test_stage_cmd_matches_tier`), it runs inside `make host_tests`, and it can fail: perturbing one side (dropping `-o` from the rig's emitted command) turns it red —
```
FAIL: stage cmd is the tier's mcopy shape (rig & t3_tier agree) (mcopy -i "$ROOT/disk.img" …)
FAIL: stage shape carries the tier's $ROOT/disk.img + ::GUEST tokens ()
Tests run: 6, failed: 2      EVIDENCE RIG TEST FAILED   (rc=1)
```
But the string it compares contains the very token that breaks the run — `want = 'mcopy -o -i "$ROOT/disk.img" …'` — so the test certifies that the two commands are *textually* identical while both are unrunnable from make. That is the clause's own failure mode in a new place: an instrument that agrees with itself and cannot see the shell it will run in.

## What would make this GREEN

One fix, in the Makefile recipe: do not let make expand the stage string —
```make
run: $(TARGET) disk.img
	$(if $(RUN_STAGE),@echo "=== run: stage ==="; ROOT="$(CURDIR)" $(SHELL) -c "$$RUN_STAGE" || { echo "run: STAGE FAILED - refusing to boot a stale image"; exit 2; },)
```
(`$$RUN_STAGE` is passed to the shell and expanded there, with `ROOT` supplied — exactly the tier's `ROOT="$ROOT" sh -c "$T3_STAGE"` contract). I tried this and the stage lands on the image — but it is not sufficient on its own; see the next section.

## A second blocker, found by trying the fix

I patched the recipe locally (`ROOT="$(CURDIR)" $(SHELL) -c "$$RUN_STAGE"`, the tier's own contract) and re-ran the staged rig. **The stage then works** — the fixture is on the image — and QEMU boots:

```
$ mdir -i disk.img ::/ | grep GBFIX
GBFIX    BIN     55896 2026-10-28   8:21
serial: 314:=== run: stage ===    ...    560:[MENU] count=128
```
…but the guest's listing still does not show it, and the count does not move. The menu is a fixed array:

```c
src/user/desktop.c:31   #define MAX_MENU_ITEMS 128
src/user/desktop.c:32   char menu_items[MAX_MENU_ITEMS][16];
```
and this image already carries **149** BIN files (`mdir -i disk.img ::/ | grep -c 'BIN '` → 149), so the `[MENU]` dump is saturated at 128 entries and a newly staged BIN has no line to appear on. Clause 3's "the guest proves it with its own listing" therefore cannot be satisfied by staging a *program* on this image, even once the Makefile is fixed.

So the work needs two things, not one: the recipe fix above, and a proof instrument that does not depend on the capped menu (run the staged fixture and have it print its own size/hash, or have the rig stage a data file a probe program reads). The rest of the design — refusals, fatality, off-by-default, the shared string shape — is sound and does not need re-doing.

## Consequence

GB-118 (#245) → **In Progress** with clauses 1-4 named; GB-118-V (#246) → **Done**. Clauses 5, 6 and 7 pass and should not be re-run. Clause 8 should gain a case that asserts the *shell* contract (that `RUN_STAGE` survives to the shell), not only the string shape — otherwise the next round can pass on text again.