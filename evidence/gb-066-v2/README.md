# GB-066-V2 — gate evidence (VERDICT: RED, two named walls)

Ticket: GB-066-V2 (#195) — re-gate the T4 device leg now that GB-072's dispatch wall is gone
Verifies: GB-066 (#74), **re-gate, no implementation owed**. Graded on `origin/main` **`b6c34fd`**
(2026-10-26 15:09Z), the current tip — *newer* than the `6caf74a` the 14:57Z RED used.
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2). Local only — no CI cited.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | leg B runs inside HobbyOS-intel; guest `T4PROBE` rows quoted with a count | **FAIL** — dispatched, then refused by the loader: **0 rows** |
| 2 | diff reports a count; 0 rows must fail, not pass | **PASS** — `row count A=41 B=17` → FAIL; empty leg A → `FATAL … exit 2` |
| 3 | a deliberately-wrong probe makes the same harness FAIL | **PASS** — `--self-test` `fail_count=0`, injected row FAILs in both `--check` modes |
| 4 | absent probe reported absent, never silent | **PASS** — `FAIL — the probe never arrived`, exit 1 (not a skip) |
| 5 | ARM runs, or the verdict states plainly why not | **STATED** — not run; the refusal is in the shared loader path (arch-independent) |
| 6 | nothing else regresses | **PASS** — `make host_tests` exit 0 |

## Clause 1 FAIL — GB-072's wall is gone, and a second wall stands behind it

Staged `T4PROBE.BIN` + a one-line `WAVE.CFG` via `T3_STAGE`, booted intel on `b6c34fd`:

```
$ T3_STAGE='mcopy … ::T4PROBE.BIN && mcopy … ::WAVE.CFG' T3_LOG=obj/t4_legB3.log bash tools/t3_tier.sh intel
[WAVE-MANIFEST] T4PROBE.BIN
[WAVE-TRY] T4PROBE.BIN
Loading program for scheduler (v2 AS): T4PROBE.BIN
elf loader: ET_EXEC linked below the user window; a -static-pie build is required (docs/elf-loader.md §1)
[ARRIVED] T4PROBE.BIN rc<0
```
The manifest dispatch **works** (GB-072 is genuinely closed). The probe never executes: the loader refuses
the image class. `tools/gate/t4_device.sh` on that serial:
```
t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived')   [exit 1]
```

**New measurement this round — the build rule is the wall, and it is not the only one.** The probe is built
by `Makefile:3525` as `musl-gcc -static -O2` → `Type: EXEC`. Rebuilt with `-static-pie` → `Type: DYN`,
same source, same loader:

```
[WAVE-MANIFEST] T4PROBE.BIN
t4-probe
row=write3 rc=3 errno=0 out=68690a
row=uname_rc rc=-1 errno=1
…
probe-exit
[KERNEL] Process 2: T4PROBE.BIN exited unexpectedly or gracefully.
```
It arrives and runs — and stops after **17 of 41 rows**. The harness, honestly:
```
t4_device: probe ARRIVED in guest (t4-probe-arrived present).
t4_device: row count A=41 B=17
t4_device: FAIL — row count differs (A=41 B=17)
  FAIL row=write3 rc=3 errno=0 out=68690a  (not present in guest output; see B rows below)
```
So the RED names **two walls**, in order:
1. **`Makefile:3525` builds `ET_EXEC`; the loader requires static-PIE** (`docs/elf-loader.md §1`). One flag —
   but it is a real refusal, not a probe bug.
2. **Even as static-PIE the probe exits after 17/41 rows** (`probe-exit`, `rc>0`, `[EXITPC] pid=2
   last_user_pc=0x1000003F77`). Fixing (1) alone still leaves clause 1 red.

Neither wall is the old boot-wave wall, which is what the card asked for.

## Clause 2 PASS — counts, and zero rows refused
`row count A=41 B=17` then FAIL (not a bare "PASS"). A degenerate leg A is refused before comparing:
```
t4_device: FATAL — leg A reference '/tmp/fake_legA_0rows.out' absent/empty. Build leg A first:   [exit 2]
```

## Clause 3 PASS — the harness can fail
```
$ bash tools/gate/t4_device.sh --self-test
[self-test] clause 3 (absent probe must FAIL, not skip):  OK — absent probe FAILED as required
[self-test] GB-079 …  OK — healthy probe PASSES (leg A present) / OK — injected probe row FAILS (leg A present)
self-test result: fail_count=0
```

## Clause 4 PASS — absent is reported, never silent
```
t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived')   [exit 1]
```

## Clause 5 STATED — ARM not run
Not run. The refusal happens in the loader's common ELF path (`elf loader: ET_EXEC linked below the user
window`), which is shared by both arches, and the ARM leg would need the same `T3_STAGE` + `WAVE.CFG`
dance. Saying "intel only" would hide nothing here, but I am not claiming ARM coverage: it is untested.

## Clause 6 PASS
```
$ make host_tests
ALL APPS SUITE TESTS PASSED
HOST_TESTS_EXIT=0
```

## Verdict

**RED** on clause 1, with the walls named precisely and the first one proved by construction: the loader
refuses an `ET_EXEC` probe (`0 rows`), and a `-static-pie` rebuild gets in but exits after 17 of 41 rows.
Clauses 2, 3, 4 and 6 pass; 5 is stated as untested rather than passed.

The two walls are worth separating for whoever picks this up: (1) is a build-rule fix in `Makefile:3525`,
(2) is probe/dispatch work inside the guest and will need its own measurement.