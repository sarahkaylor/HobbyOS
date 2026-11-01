# GB-144-V (#342) — VERDICT: GREEN

**Graded:** `6a1d5f18` (PR #292, base `14cf1e28`), detached in a fresh worktree `.worktrees/gb144v`; `git rev-parse --short HEAD` printed `6a1d5f18`.
**Diff:** `tools/gate/t4_w0.sh` +14 (the mirror branch + its self-test fixture) and `gaming.md` −3/+1 (documentation; see the last section).

| # | Criterion | Result |
|---|---|---|
| 1 | `--self-test` → `fails=0`, including the new mirror fixture | PASS |
| 2 | Induced log (marker line deleted) → rc=1, GB-144 FAIL line, others PASS | PASS |
| 3 | **Control:** pre-fix `t4_w0.sh` on the same induced log → rc=0 | PASS (`rc=0` vs `rc=1`) |
| 4 | Unmodified corpus → PASS, `0 FAIL lines`, `halted=1` | PASS |
| 5 | Suites rc=0 | PASS |

---

## 1. Self-test — six directions, `fails=0`

```
$ bash tools/gate/t4_w0.sh --self-test ; echo "selftest_rc=$?"
  OK — all W0 members PASS (matched Linux leg)
  OK — absent member FAILED as required
  OK — broken member FAILED as required
  OK — gate FAILED a member whose compat flag (abi=0x1) is absent
  OK — member emitting a marker with no .ref FAILED (absent ref is no longer a pass)
  OK — committed .ref + no marker FAILED (a silent member is no longer a pass)
self-test result: fails=0
selftest_rc=0
```

The new line is the mirror of the absent-ref case, and it is produced by running the real script as a
subprocess (`W0_OUT_DIR=$TMP/w0out`), not by a helper: only the new `elif` branch can fail that fixture.

## 2. Real induction on guest output

The serial is a **fresh capture at the graded head**, not a file from another day: `tools/gate/t4_w0_dispatch.sh`
staged the W0 manifest (`HELLO CAT SH LS W0PIE`) and booted the intel guest —
`T3_intel: 367 console lines, 0 FAIL lines, 0 user memory fault records across 0 processes, 5 programs exited, halted=1`.
One line deleted, exactly as the card specifies:

```
$ grep -v '^\[W0\] W0PIE linux=' w0_intel_6a1d5f18.log > w0_no_marker.log   # 2 markers left, W0PIE's gone
$ bash tools/gate/t4_w0.sh --serial w0_no_marker.log ; echo "rc=$?"
  PASS W0PIE (arrived in guest)
  FAIL W0PIE — has a committed .ref but emitted no '[W0] W0PIE linux=' marker: its output cannot be compared (GB-144)
  PASS HELLO (arrived in guest)
  PASS HELLO output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS SH (arrived in guest)
  PASS SH output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS CAT (arrived in guest)
  PASS CAT output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS LS (arrived in guest)
  PASS LS output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
t4_w0: FAIL — W0 corpus incomplete or output mismatch
rc=1
```

The silent member is named and the other four keep their output comparison — the failure is the missing marker,
not collateral damage.

## 3. The control — the pre-fix script passes the same log

```
$ git show 14cf1e28:tools/gate/t4_w0.sh > t4_w0_prefix_14cf1e28.sh      # 209 lines
$ T4_LEGA_OUT=$PWD/tests/fixtures/abi/w0_output bash t4_w0_prefix_14cf1e28.sh --serial w0_no_marker.log
  PASS W0PIE (arrived in guest)
  ...
t4_w0: PASS — every W0 corpus member arrived in the intel wave; members emitting a
               '[W0] <member> linux=' marker had their output verified against the
               Linux-leg .ref corpus (a mismatched marker FAILs).
prefix_rc=0
```

Same log, same `.ref` directory, same guest bytes: **head `rc=1`, prefix `rc=0`.** The pre-fix script's only
W0PIE line is `PASS W0PIE (arrived in guest)` — no output comparison at all, the silent skip in its own words.
This is the clause that makes the verdict attributable, and it is the one I would have refused to grade without.

*(Deviation stated: the card's literal command runs the prefix copy from `/tmp`, where its `W0_OUT_DIR` default
resolves to `/tests/fixtures/abi/w0_output` and every non-emitting ref check would fail for the wrong reason. I
passed `T4_LEGA_OUT` explicitly so both scripts read the same committed `.ref` corpus.)*

## 4. The unmodified corpus still passes

```
$ bash tools/gate/t4_w0.sh --serial w0_intel_6a1d5f18.log ; echo "rc=$?"
  PASS W0PIE (arrived in guest)
  PASS W0PIE output — guest 'joctl=25 fcntl=32770 poll=0 uname=Linux' == Linux-leg 'joctl=25 fcntl=32770 poll=0 uname=Linux'
  PASS HELLO … PASS HELLO output … PASS SH … PASS SH output … PASS CAT … PASS CAT output … PASS LS … PASS LS output
t4_w0: PASS — every W0 corpus member arrived in the intel wave; …
rc=0
```

Ten PASS lines (arrival + output for each of five members), with the tier line quoted above: `0 FAIL lines`,
`halted=1`.

## 5. Suites

```
bash tools/gate/merge_gate_test.sh         rc=0
python3 tools/board/groom_test.py          rc=0
python3 tools/board/check_stale_test.py    rc=0
python3 tools/board/drift_check_test.py    rc=0
```

## One observation, not a blocker

The same commit also edits `gaming.md`: it deletes the GB-144 **and GB-145** rows from M1's "carried forward"
list and leaves only GB-031's `fbcon` question. GB-144 is what this card closes, so removing its row is right;
**GB-145 is still open**, so the plan no longer names it — it survives only as board ticket #341. That is outside
this card's DoD (which is about the gate script) and does not affect the grade, but it is a documentation
regression a later reader of `gaming.md` would trip over.

## Artefacts

`selftest.log` · `w0_intel_6a1d5f18.log` (fresh 5-member W0 capture) · `w0_no_marker.log` (induced) ·
`w0_gate_unmodified.log` · `w0_gate_induced_head.log` · `w0_gate_induced_prefix.log` · `t4_w0_prefix_14cf1e28.sh` ·
`w0_dispatch_summary.txt` · `suite_*.log`