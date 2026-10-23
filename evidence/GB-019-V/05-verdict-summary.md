# GB-019-V (verifies GB-019) — gate run, revision 46f19bb

Worktree `hermes-9642df51`, branch `verify-gb019v` at **46f19bb** (= `origin/main`), swarm-farm-2.

## Step 1 — the gate can provably fail (self-test)

```
$ bash tools/gate/t4_device.sh --self-test
[self-test] clause 2 (wrong shim must FAIL):
  OK — wrong shim FAILED as required
[self-test] clause 3 (absent probe must FAIL, not skip):
  OK — absent probe FAILED as required
self-test result: fail_count=0
exit=0
```

## Step 2 — the probe builds, and leg A is captured on real Linux

```
$ make ARCH=intel obj/intel/t4_probe.elf        -> exit 0, obj/intel/t4_probe.elf (61008 bytes)
$ make ARCH=intel obj/intel/t4_legA.out         -> "[t4-device] leg A captured -> obj/intel/t4_legA.out"
```

(The ticket's `make obj/t4_probe.elf` has no rule: `OBJ_DIR = obj/$(ARCH)` and `ARCH` defaults to
`arm`, so the probe lives at `obj/intel/t4_probe.elf` for this x86 gate. Invoked with `ARCH=intel`,
which is the same object the Makefile's own T4 rules use.)

Leg A begins:

```
t4-probe
hi
row=write3 rc=3 errno=0 out=68690a
row=uname_rc rc=0 errno=0
...
```

## Step 3 — the wave stages the probe but never dispatches it

```
$ T3_STAGE='mcopy -o -i "$ROOT/disk.img" obj/intel/t4_probe.elf ::T4PROBE.BIN' \
  bash tools/t3_tier.sh intel
```

The staging itself worked — the probe is in the image:

```
- 61008 T4PROBE.BIN
```

…and then nothing. In 37,463 lines of serial there are **zero** `t4-probe` / `t4-probe-arrived`
occurrences; the wave ends on CMPTEST with `[HALT-CD] ... exited=12 ... System halt.`

## Step 4 — the gate verdict

```
$ T4_LEGA_OUT=obj/intel/t4_legA.out T4_SERIAL=obj/t3_intel.log bash tools/gate/t4_device.sh
=== t4_device: leg A reference: obj/intel/t4_legA.out ===
=== t4_device: capture leg B from serial 'obj/t3_intel.log' ===
t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived'
  in obj/t3_intel.log after 120s).  This is a FAIL, not a SKIP (DoD clause 3).
gate_exit=1
```

## Verdict

**RED.** DoD clause 2 ("every named row agrees with leg A") cannot be evaluated because the probe
never runs in the guest: the staged program is not dispatched, which is exactly the block this
ticket names (GB-072). The instrument behaved correctly — it FAILED instead of skipping, and its
own self-test proves both failure clauses work.

Note: the same wave also carries 5 CMPTEST failures and several TSORTTEST/WAITPID failures on this
revision. Those are outside this ticket's row set and are not the basis of this verdict; they are
mentioned only so nobody reads this serial as otherwise clean.

To re-open for GREEN: land GB-072 (dispatch of staged programs) so `T4PROBE.BIN` actually executes,
then re-run steps 3-4 unchanged.
