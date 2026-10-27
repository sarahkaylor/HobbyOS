# GB-047-V (#223, re-verify) — gate evidence (VERDICT: RED, narrow and reproducible)

Ticket: GB-047-V (#223). Verifies GB-047, ship = PR **#215** head **`ed53eca`**
(`evidence_pack.py fails (exit 1) over an empty tree instead of silently passing criterion 2`).
Host-side, T1, no QEMU, no CI. Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2).

## Clause table

| # | clause | result |
|---|---|---|
| 1 | empty tree FAILS CLOSED (exit 1) | **FAIL on re-invocation** — exit 1 only the *first* time; the second run over the same empty tree exits 0 |
| 2 | a real artifact tree still exits 0 | **PASS** — 42 artifacts, exit 0 |
| 3 | the landed scanner/tests still pass | **PASS** — 30/30 suite incl. `evidence_pack_test.py`; `evidence_rig_test.py` 5/5 |

## 1 FAIL — the fix is one-shot, because the pack writes its manifest into its own scan root

The change is exactly what it says:

```python
+    if not manifest["artifacts"]:
+        print("[pack] FAIL: zero artifacts found under --root %s — an empty tree is not an "
+              "acceptance evidence pack (GB-047-V criterion 2: this must not exit 0)" % root)
+        return 1
+    return 0
 if __name__ == "__main__":
-    main()
+    sys.exit(main())
```
First run on a fresh empty dir — correct:
```
$ python3 tools/t5/evidence_pack.py --root /tmp/gb047v2_fresh_348503 --out /tmp/gb047v2_p1.md
[pack] wrote /tmp/gb047v2_p1.md + S0-S1-EVIDENCE-MANIFEST.json (0 artifacts)
[pack] FAIL: zero artifacts found under --root … (GB-047-V criterion 2: this must not exit 0)
run1 exit=1
```
And what that run left behind:
```
    -rw-r--r-- 1 sarah sarah 60 … S0-S1-EVIDENCE-MANIFEST.json
```
`evidence_pack.py:98` writes `S0-S1-EVIDENCE-MANIFEST.json` **into `--root`**, and the artifact scan globs
`--root`. So the second invocation finds the first run's manifest and counts it as evidence:

```
$ python3 tools/t5/evidence_pack.py --root /tmp/gb047v2_fresh_348503 --out /tmp/gb047v2_p2.md
[pack] wrote /tmp/gb047v2_p2.md + S0-S1-EVIDENCE-MANIFEST.json (1 artifacts)
run2 exit=0            <-- an empty tree, exit 0, criterion 2 violated
run3 exit=0
```
The same command, the same empty tree, one run apart: `1` then `0`. **Criterion 2 is "must not exit 0 over an
empty tree"** — and after any previous run it does. That is not a hypothetical: any pipeline that re-packs a
tree, or packs a workspace that has been packed before, gets a green over nothing.

## 2 PASS — a real tree is still packed
```
$ python3 tools/t5/evidence_pack.py --root "$PWD" --out /tmp/PACK-REAL.md
[pack] wrote /tmp/PACK-REAL.md + S0-S1-EVIDENCE-MANIFEST.json (42 artifacts)
exit=0
```
(42 artifacts, e.g. `xnibbler-EVIDENCE-REPORT.md (sha16 f36463a426a1e5bc)`.) The fix did not make the pack
refuse real trees.

## 3 PASS — the landed tests, with one observation
```
$ python3 tools/t5/run_t5_tests.py
  PASS  tools/t5/evidence_pack_test.py
[host-tests] done: 30/30 passed

$ python3 tools/evidence_rig_test.py
Tests run: 5, failed: 0      EVIDENCE RIG TEST PASSED
```
(The card says `evidence_rig_test`; the file is `tools/evidence_rig_test.py`, not under `tools/t5/`.)

Observation worth carrying into the follow-up: `evidence_pack_test.py` contains **no** reference to
`artifacts` — the empty-tree behaviour this PR introduces is not covered by the suite at all. That is why a
regression (or this re-invocation hole) would ship green.

## Verdict

**RED** on clause 1, narrowly and by measurement. The first-run behaviour is correct and the fix does what the
commit says; but the pack injects its own manifest into the tree it scans, so the fail-closed verdict does not
survive a second run — the exact state criterion 2 exists to forbid.

Two ways out, both small: exclude the pack's own outputs (`S0-S1-EVIDENCE-MANIFEST.json` and the `--out` file)
from the artifact scan, or write the manifest beside `--out` instead of into `--root`. Either way, add a test
that runs the pack **twice** over the same empty dir and requires exit 1 both times — otherwise this hole
returns the first time somebody re-runs the pack in a script.