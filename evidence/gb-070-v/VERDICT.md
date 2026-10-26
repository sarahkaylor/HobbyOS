# GB-070-V — VERDICT: **GREEN**

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-25 ~19:2xZ.
Verifies GB-070 (#79) / ship **PR #183 @ `309e049`** ("GB-070: emit CAT's W0 marker to stderr, not
stdout") on top of the wiring commit `4eb225b`. Intel T3 wave + W0 differential. I did not author
GB-070 or PR #183 — independent gate.

## One line

All four W0 members **arrive and byte-match their Linux-leg `.ref` on a real intel wave**, and the gate
provably fails three ways (self-test absent/broken, plus a real-wave absent member), so the corpus the M1
gate names is now the corpus the harness actually checks.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | every W0 member's output matches the Linux leg byte-for-byte, **live** (HELLO/SH/CAT/LS on a real wave) | **PASS** — 4/4 |
| 2 | the gate can genuinely fail: self-test absent member + broken member; a real-wave absent member FAILs | **PASS** — 3/3 self-test clauses + a real-wave control |
| 3 | no silent widening: the `t4_w0.sh` predicate is unchanged vs its base | **PASS** — empty diff; only markers + a 7-line tier wire |

## Clause 1 — the real wave (quoted)

```
$ T3_LOG=... bash tools/t3_tier.sh intel        # rev 309e049, intel, KVM
T3_intel: 11611 console lines, 94 FAIL lines, 6 user memory fault records across 6 processes, 81 programs exited, halted=0
```

```
$ bash tools/gate/t4_w0.sh --serial evidence/gb-070-v/02-t3-intel.log --members 'HELLO SH CAT LS'
=== t4_w0: W0 corpus gate over evidence/gb-070-v/02-t3-intel.log ===
  PASS HELLO (arrived in guest)
  PASS HELLO output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS SH (arrived in guest)
  PASS SH output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS CAT (arrived in guest)
  PASS CAT output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS LS (arrived in guest)
  PASS LS output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
t4_w0: PASS — every W0 corpus member arrived in the intel wave; …
rc=0
```
(`03-w0-gate-real-wave.log`) — the wave logged 10 `[W0]` marker lines; CAT's marker reaches the gate
**via fd 2**, so it is no longer swallowed into the harness pipe (that was the round-2 false positive at
`4eb225b`).

## Clause 2 — the gate can fail, three ways (quoted)

```
$ bash tools/gate/t4_w0.sh --self-test
[self-test] good wave (all members arrived):   OK — all W0 members PASS (matched Linux leg)
[self-test] missing CAT (must FAIL):           OK — absent member FAILED as required
[self-test] broken HELLO output (must FAIL):   OK — broken member FAILED as required
self-test result: fails=0
```
and on the **real** wave, with one member that never ran:
```
$ bash tools/gate/t4_w0.sh --serial <log> --members 'HELLO SH CAT LS PIE'
  … four PASS lines …
  FAIL PIE — not in the intel wave's exited-programs/arrival set (W0 corpus member never ran)
t4_w0: FAIL — W0 corpus incomplete or output mismatch
rc=1
```
So an absent member is reported as a failure by the same code path that just PASSed four real members —
not skipped, and not vacuous.

## Clause 3 — no silent widening

```
$ git diff origin/main...309e049 -- tools/gate/t4_w0.sh
(empty)
$ git diff --stat origin/main...309e049
 src/user/cat.c (15+), src/user/ls.c (6+), src/user/sh.c (6+),
 tests/fixtures/abi/w0_output/{CAT,LS,SH}.ref (1+ each), tools/t3_tier.sh (7+)
```
The gate's predicate is byte-identical to its base; the change is marker emission in three user programs,
three `.ref` fixtures, and a 7-line wire in `t3_tier.sh` that invokes the gate and sets `rc=1` only on
gate failure. Non-W0 verdict criteria are untouched.

## State, not counted (per the card's own instruction)

1. **The wave did not reach a clean halt** — `halted=0`, 94 FAIL lines, 81 programs exited, the tier hit
   its 900 s deadline. That is the GB-057 IDLESTUCK/stall family, which this card explicitly says is a
   separate card and must not be the ground for a RED here. The W0 markers all fired and matched inside
   that same wave, which is what this card measures.
2. **The tier wire is inert on a red tier** — `t3_tier.sh` runs the W0 gate only `if [ "$rc" -eq 0 ]`.
   Since today's tier is red on FAIL lines / no-halt, the live invocation never executes on this tree; the
   gate still works when invoked directly, which is how clauses 1–2 were measured. Not a defect against
   this card's DoD (which asks that the gate be wired and not silently widened), but it means the wire
   adds no information while the tier is red.
3. **Marker repeats are tolerated by design** — the wave emitted 10 `[W0]` lines across repeated member
   runs and one was visibly interleaved (`[W0] SH linux[=KEReNntEeLr]e …`). The gate PASSes on any clean
   occurrence, so console gluing cannot manufacture a spurious FAIL; the trade-off is that a member whose
   output matched on *one* of several runs passes. That is the intended semantics for "the member ran and
   its output matched", and I state it rather than leave it implicit.

## Reproduce

```
git worktree add /tmp/gb070v 309e049 && cd /tmp/gb070v && mkdir -p obj
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
bash tools/gate/t4_w0.sh --self-test
T3_LOG=$PWD/evidence/gb-070-v/02-t3-intel.log T3_TIMEOUT=900 W0_GATE=0 bash tools/t3_tier.sh intel
bash tools/gate/t4_w0.sh --serial evidence/gb-070-v/02-t3-intel.log --members 'HELLO SH CAT LS'
bash tools/gate/t4_w0.sh --serial evidence/gb-070-v/02-t3-intel.log --members 'HELLO SH CAT LS PIE'   # must FAIL
```