# GB-021-V (verifies GB-021) — gate run, revision 46f19bb

Worktree `hermes-9642df51`, branch `verify-gb019v` at **46f19bb** (= `origin/main`), swarm-farm-2.

## Step 1 — the gate can provably fail

```
$ bash tools/gate/t4_device.sh --self-test
[self-test] clause 2 (wrong shim must FAIL):
  OK — wrong shim FAILED as required
[self-test] clause 3 (absent probe must FAIL, not skip):
  OK — absent probe FAILED as required
self-test result: fail_count=0
exit=0
```

## Step 2 — probe and leg A

```
$ make ARCH=intel obj/intel/t4_probe.elf obj/intel/t4_legA.out
make: 'obj/intel/t4_legA.out' is up to date.
exit=0
```

(`obj/t4_probe.elf` in the ticket has no rule — `OBJ_DIR = obj/$(ARCH)` and `ARCH` defaults to `arm`;
the x86 objects are `obj/intel/…`.)

## Step 3 — the wave stages T4PROBE.BIN and never runs it

```
$ T3_STAGE='mcopy -o -i "$ROOT/disk.img" obj/intel/t4_probe.elf ::T4PROBE.BIN' bash tools/t3_tier.sh intel
- 61008 T4PROBE.BIN
T4PROBE.BIN
…
[System halt]                     # 1 occurrence — the wave completed
t4-probe-arrived occurrences: 0
```

## Step 4 — the gate verdict

```
$ T4_LEGA_OUT=obj/intel/t4_legA.out T4_SERIAL=obj/t3_intel.log bash tools/gate/t4_device.sh
=== t4_device: capture leg B from serial 'obj/t3_intel.log' ===
t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived'
  in obj/t3_intel.log after 120s).  This is a FAIL, not a SKIP (DoD clause 3).
gate_exit=1
```

## DoD clause 4 (repeat at least twice)

The two waves at this sha — this one and the one recorded for GB-019-V (#87,
`evidence/GB-019-V/03b-serial.log`) — both stage `T4PROBE.BIN` and both report **zero** arrivals.
Clause 4 exists to expose a racy increment total, which presupposes that rows execute; with no row
executing, a second wave can only re-confirm non-dispatch, and it does.

## Verdict

**RED.** No row in scope (thread-increment total, futex wait/wake handshake) can be compared, because
the staged program is never dispatched — the block this ticket names, **GB-072**. The instrument
failed rather than skipped, and its self-test proves both failure clauses fire, so this is a real
verdict and not a hollow one.

Incidental, not part of the verdict: this revision's wave also shows TSORTTEST and SEDTEST failures
(`[SEDTEST] FAIL amp (stdout)`), outside this row set.
