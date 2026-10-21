# GB-070-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-21.
Tree: `origin/main` @ `79d14e7`, clean worktree; `disk.img` built here, intel wave run here.
Gate: both DoD commands, the second over a **real** intel wave log produced by `tools/t3_tier.sh intel`.

## 1. `--self-test` — PASS, all three clauses proven

```
$ bash tools/gate/t4_w0.sh --self-test                  # 01_selftest.log
[self-test] good wave (all members arrived):
  OK — all W0 members PASS (matched Linux leg)
[self-test] missing CAT (must FAIL):
  OK — absent member FAILED as required
[self-test] broken HELLO output (must FAIL):
  OK — broken member FAILED as required
self-test result: fails=0
EXIT=0
```

## 2. The real intel wave — the corpus now runs, and the gate says PASS

```
$ bash tools/t3_tier.sh intel                            # 02_t3_intel_run.log
T3_intel: 1129 console lines, 4 FAIL lines, 59 user memory fault records across 51 processes, 62 programs exited, halted=1
T3_intel: FAIL — 4 FAIL line(s) …                        # graphics rows = GB-065, not this ticket
```

```
$ bash tools/gate/t4_w0.sh --serial obj/t3_intel.log --members 'HELLO SH CAT LS'    # 03
=== t4_w0: W0 corpus gate over obj/t3_intel.log ===
  PASS HELLO (arrived in guest)
  PASS SH (arrived in guest)
  PASS CAT (arrived in guest)
  PASS LS (arrived in guest)
t4_w0: PASS — every W0 corpus member arrived in the intel wave (arrival only: the
               output half needs a '[W0] <member> linux=' marker, which no program emits yet)
EXIT=0
```

Confirmed independently in the same log — the DoD's "exited-programs set" is real now:

```
$ grep -o "\[ARRIVED\] [A-Z0-9]*\.BIN" obj/t3_intel.log | sort | uniq -c
  1 [ARRIVED] CAT.BIN      1 [ARRIVED] HELLO.BIN    1 [ARRIVED] LS.BIN    1 [ARRIVED] SH.BIN
[KERNEL] Process 29: CAT.BIN exited unexpectedly or gracefully.
[KERNEL] Process 30: LS.BIN exited unexpectedly or gracefully.
```

So the expected state in the card ("only HELLO is a W0 member … expected honest verdict is RED naming
CAT/SH/LS") is **stale**: the arrival instrument (`2bab246`) landed and all four members run.

## 3. Why it is still RED: the gate cannot fail on the thing its DoD grades

GB-070's DoD is *"every W0 corpus member appears in an intel wave's exited-programs set **and its output
matches the Linux leg byte-for-byte**; a deliberately-broken member fails the run."* The live path
(`t4_w0.sh:112-139`) checks **arrival only**, and says so in its own PASS text — there is no `[W0] <member>
linux=` marker emitted by any program, so the comparison branch exists only inside the self-test's
synthetic logs. In the live path a member whose output is completely wrong would still PASS.

That is a gate that cannot fail on the DoD it certifies: exactly what this card's rules forbid. The
self-test proves the *comparison logic* works; nothing proves the *wave* passes it.

## 4. Wiring (DoD item 3, reported as asked)

```
$ grep -n "t4_w0" Makefile
3357: T4_W0_GATE = t4_w0_gate
3359: $(T4_W0_GATE): tools/gate/t4_w0.sh
3360:	@bash tools/gate/t4_w0.sh --serial $(T3_LOG)
3361: $(T4_W0_SELFTEST): t4_w0_selftest
```

Both are standalone targets: `t4_w0_selftest` is not a prerequisite of `host_tests`, and `t4_w0_gate`
is not run by `t3_tier.sh`, `parity_ci`, or any workflow (there is no `.github/workflows` on `main`).
So today only the **self-test** can run automatically — the real-wave half is manual, and the one time
it was run (here) it produced an arrival-only PASS.

## DoD table

| # | criterion | result |
|---|---|---|
| 1 | `--self-test` exits 0 with three proven clauses | PASS |
| 2 | names exactly which W0 members are absent on a real wave | **FAIL** — none absent, and the PASS is arrival-only; the output half the DoD requires is not implemented |
| 3 | report wiring (self-test only vs real wave) | REPORTED — self-test: suite-able; real wave: manual target, not wired |

## What closes it

1. Have the corpus programs (or the loader on their behalf) emit `[W0] <member> linux=<output-digest>`,
   so the live path compares output instead of arrival.
2. Make a deliberately-broken member fail the **run**, not just the self-test.
3. Wire `t4_w0_gate` into `t3_tier.sh intel` (or the merge gate) so it runs on every wave.
4. Fix the card's stale expectation: the corpus does run now; the missing half is the comparison.
