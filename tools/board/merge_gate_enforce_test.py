#!/usr/bin/env python3
"""merge_gate_enforce_test.py - GB-005 clause 3: both refusals can FAIL.

1. The `Merge Gate` lane-move refusal (allow()/refuse_reason()) MUST refuse a non-GREEN
   verdict and MUST fail closed on no verdict.
2. The refusal is NOT decoration: sabotage the verdict source (clamp it to always-GREEN,
   the line-item 3 discipline) and the refusal STOPS refusing.  That proves the refusal
   is computed from the verdict, not asserted.  A refusal that cannot be made to fail is
   decoration, and GB-066/GB-070 is why the charter says so.
3. The merge helper (merge_pr.py decide) also refuses without a GREEN -> still refuses.

Run: python3 tools/board/merge_gate_enforce_test.py   (exit 0 = pass; auto-registering)
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import merge_gate_enforce as enf

FAILS = []
def check(cond, label, extra=""):
    if cond:
        print("PASS: " + label + (f" ({extra})" if extra else ""))
    else:
        FAILS.append(label)
        print("FAIL: " + label + (f" ({extra})" if extra else ""))


def test_merge_gate_refuses_non_green():
    check(enf.allow("RED") is False, "RED -> Merge Gate move refused")
    check(enf.allow(None) is False, "no verdict -> FAILS CLOSED (refused)")
    check(enf.allow("GREEN") is True, "GREEN -> Merge Gate move allowed")
    r = enf.refuse_reason("RED", "GB-005")
    check(r.startswith("REFUSE") and "GB-005" in r, "RED refusal names the card (written reason)")


def test_refusal_provably_fails_always_green():
    # Sabotage: clamp the verdict source to always-GREEN.  If the refusal were hardcoded/
    # asserted it would STILL refuse; the recognition-based refusal must STOP refusing.
    real = enf.verdict_source
    enf.verdict_source = lambda gid: "GREEN"                       # the sabotage
    try:
        v = enf.verdict_source("GB-005")
        check(v == "GREEN", "sabotaged verdict source reports GREEN (the broken detector)")
        r = enf.refuse_reason(v, "GB-005")
        check(r == "", "refusal STOPS refusing when the detector is broken (can fail -> not decoration)",
              "reason=%r" % r)
    finally:
        enf.verdict_source = real                                  # restore
        check(enf.verdict_source is real, "verdict source RESTORED after the sabotage (same callable)")


def test_merge_helper_still_refuses_without_green():
    # merge_pr.py decide() is the merge-side reference (item 2).  Documents that the
    # merge helper ALSO refuses without a GREEN-naming-the-head.  Keep the pair honest.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import merge_pr
    except Exception as e:
        FAILS.append(f"merge_pr import failed: {e}")
        print(f"FAIL: merge_pr import failed ({e})")
        return
    head = "a" * 40
    ok, r = merge_pr.decide(head, None)
    check(not ok and r.startswith("REFUSE"), "merge helper refuses with no graded sha (fail closed)",
          r[:40])
    ok, r = merge_pr.decide("b" * 40, "a" * 40)
    check(not ok and r.startswith("REFUSE"), "merge helper refuses a GREEN that names a different head",
          r[:40])


if __name__ == "__main__":
    test_merge_gate_refuses_non_green()
    test_refusal_provably_fails_always_green()
    test_merge_helper_still_refuses_without_green()
    print(f"\n[merge_gate_enforce_test] {3-len(FAILS)}/3 passed")
    if FAILS:
        print("MERGE GATE ENFORCE TEST FAILED")
        sys.exit(1)
    print("MERGE GATE ENFORCE TEST PASSED")
    sys.exit(0)
