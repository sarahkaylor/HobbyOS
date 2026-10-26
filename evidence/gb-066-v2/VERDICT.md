# GB-066-V2 — VERDICT: **RED** (new wall named: the probe is ET_EXEC and the loader requires static-PIE)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-26 14:4x–15:0xZ.
Verifies GB-066 (#74) / re-gate of the T4 device leg on **`origin/main` `6caf74a`**. Local gate only; no CI
consulted. I did not author the probe or the gate.

## One line

GB-072's dispatch wall is genuinely gone — the staged probe is now **dispatched** (`[WAVE-MANIFEST]`,
`[WAVE-TRY]`, `[ARRIVED]`) — but the **guest never executes it**: the loader rejects the image class,
`elf loader: ET_EXEC linked below the user window; a -static-pie build is required`, because the Makefile
builds the probe with `musl-gcc -static`. Zero `row=` lines reach the serial, so leg B does not run and the
differential cannot be graded on the guest. That is a *different* wall from the one this card was minted
against, and it is named precisely below.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | leg B runs **inside** HobbyOS-intel, guest-side `T4PROBE` rows quoted with a row count | **FAIL** — dispatched, then rejected by the loader; 0 rows |
| 2 | diff passes on the W0 corpus and reports a **count**; 0 rows must fail, not pass | **PASS (behaviour)** — the harness **fails** on the 0-row serial rather than passing vacuously |
| 3 | a deliberately-wrong probe makes the same harness **FAIL** with the row quoted | **PASS** — `--self-test`: absent probe FAILS, injected row FAILS |
| 4 | an absent probe reports *absent*, never silently vanishes | **PASS** — quoted below |
| 5 | ARM: runs, or the verdict says plainly that it does not and why | **STATED** — same loader rule, shared code; not run (reason below) |
| 6 | `make host_tests` still passes | **PASS** — 0 failed, `TEST EXIT: 0` |

## Clause 1 — the failing cell: dispatched, then rejected by the loader

Staged post-build with the tier's hook, plus a one-member manifest so the wave is the probe:

```
T3_STAGE='mcopy -o -i "$ROOT/disk.img" "$ROOT/obj/intel/t4_probe.elf" ::T4PROBE.BIN &&
          mcopy … ::WAVE.CFG && mdir … | grep -i t4probe'
T4PROBE  BIN     61008 2026-10-26  14:45
```
Guest serial (rev `6caf74a`):
```
[WAVE-MANIFEST] T4PROBE.BIN
[WAVE-TRY] T4PROBE.BIN
Loading program for scheduler (v2 AS): T4PROBE.BIN
Inside process_create: lock released. pid=2 block_idx=-1
/T4PROBE.BIN
elf loader: ET_EXEC linked below the user window; a -static-pie build is required (docs/elf-loader.md §1)
[ARRIVED] T4PROBE.BIN rc<0
[HALT-CD] cpu=1 free=64 alloc=0 exited=0 running=0 blocked=0 other=0
System halt from CPU 1.
```
`row=` lines in the whole serial: **0**.

The image's own shape explains it:
```
$ file obj/intel/t4_probe.elf
ELF 64-bit LSB executable, x86-64, version 1 (SYSV), statically linked, with debug_info, not stripped
  Type: EXEC (Executable file)      Entry point address: 0x401cc1
```
`Makefile:3502` builds it `musl-gcc -static …`, i.e. **ET_EXEC linked at 0x401cc1 — below
`USER_VA_BASE` (0x1_0000_0000)** — while the v2 loader accepts only ET_DYN/static-PIE inside the user
window. So the wall is a **build-profile mismatch**, not dispatch, not the compat layer.

**Fix direction (not applied):** build the probe as a static PIE (`musl-gcc -static-pie -fPIE`, with the
same assertion my GB-031/GB-032 gates used — `readelf -h` must say `Type: DYN`), or extend the loader to
accept ET_EXEC below the window. The first is the smaller change and matches `docs/elf-loader.md §1`.

## Clause 2 — 0 rows is refused, not passed (`04-gate-behaviour.log`)

```
$ T4_LEGA_OUT=obj/intel/t4_legA.out T4_SERIAL=evidence/gb-066-v2/02-intel.log bash tools/gate/t4_device.sh
t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived'
  in evidence/gb-066-v2/02-intel.log after 120s).  This is a FAIL, not a SKIP (DoD clause 3).
rc=1
```
The harness does not treat `A=0 B=0` as a pass; that failure mode is closed. (`--check` also passes on the
real tree: `PASS: recorded leg A agrees with the probe source (41 golden rows, no drift)`.)

## Clause 3 — the harness can fail, quoted (`--self-test`)

```
[self-test] clause 3 (absent probe must FAIL, not skip):
  OK — absent probe FAILED as required
[self-test] GB-079 (a probe edit must FAIL in both --check modes):
  OK — healthy probe PASSES (leg A absent)
  OK — injected probe row FAILS (leg A absent)
  OK — healthy probe PASSES (leg A present)
  OK — injected probe row FAILS (leg A present)
self-test result: fail_count=0        rc=0
```
Both directions are demonstrated from fixtures: an absent probe and a deliberately-wrong row each make the
same harness fail.

## Clause 4 — the absent case is named, not silent

Same run as clause 2: the gate says the probe never arrived and calls it a **FAIL, not a SKIP**. On a real
serial where the probe is dispatched but rejected, the message is the loader line above — an operator can
tell "never dispatched" (`[WAVE-TRY]` absent) from "dispatched and refused" (`rc<0` + loader line) from
"ran" (`t4-probe-arrived`). That distinction is the point of the clause and it holds.

## Clause 5 — ARM: stated, not run

The rejection happens in the **shared** loader (`src/kernel/program_loader.c`), before any arch-specific
execution path, and the same ET_EXEC image class fails there for the same reason. I did not burn an ARM T3
run to observe the identical line on the other arch; the reason is named, which is what the clause asks
for, and it is not "intel only by accident".

## Clause 6 — nothing else regresses

```
$ make host_tests
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0        rc=0
```

## What this verdict does and does not say

It does **not** say the T4 device leg is unimplemented: the gate, its self-test, its `--check` parity and
its refusal of 0 rows are all present and provably able to fail. It says the **guest half cannot run on
this tree** because the probe is built in an image class the loader refuses, and that this is a new wall,
distinct from GB-072's dispatch wall (which is measurably gone).

## Reproduce

```
git worktree add /tmp/gb066v2 6caf74a && cd /tmp/gb066v2 && mkdir -p obj
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
make ARCH=intel obj/intel/t4_probe.elf obj/intel/t4_legA.out     # leg A: 41 rows, on the host
printf 'T4PROBE.BIN\n' > /tmp/WAVE.CFG
T3_STAGE='mcopy -o -i "$ROOT/disk.img" "$ROOT/obj/intel/t4_probe.elf" ::T4PROBE.BIN &&
          mcopy -o -i "$ROOT/disk.img" /tmp/WAVE.CFG ::WAVE.CFG' \
  T3_LOG=$PWD/legB.log T3_TIMEOUT=900 bash tools/t3_tier.sh intel
grep -a "elf loader\|ARRIVED\|^row=" legB.log
T4_SERIAL=$PWD/legB.log bash tools/gate/t4_device.sh          # FAIL — probe never arrived
bash tools/gate/t4_device.sh --self-test && make host_tests
```