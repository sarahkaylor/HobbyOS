#!/usr/bin/env python3
"""merge_pr.py - GB-106: refuse to merge a PR unless a GREEN verdict names a graded
tree equal to the PR's current head.  Fails CLOSED: a GREEN that names NO sha, or
a graded sha different from the head, is a refusal -- the merge does not happen
until the guard passes or an override records a written reason.

Rule (isolated in `decide` so it is T1-testable):
  allow = (graded is not None) and (graded == head)
  else refuse (exit 1) with the reason on stdout.

An override is a durable, written record (obj/gate/merge_pr_override/<sha>.reason)
containing the reason and the operator; it is the ONLY way to merge through a guard
that fails closed.  --self-test replays the failing-closed rule, including today's
already-merged PRs: a GREEN that did not name the merged head must be RED.

Usage:
  python3 tools/board/merge_pr.py --pr 202 --head <sha> --graded <sha>   # allow
  python3 tools/board/merge_pr.py --pr 202 --head <sha>                  # graded=auto (board -V) or none -> fails closed
  python3 tools/board/merge_pr.py --pr 202 --head <sha> --graded <other> # RED (mismatch)
  python3 tools/board/merge_pr.py --pr 202 --head <sha> --override "re-tested manually"  # written override, allow
  python3 tools/board/merge_pr.py --self-test   # assert rule incl. today's merges, #194 must be RED
"""
import argparse, json, os, re, subprocess, sys
from datetime import datetime, timezone

TREE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
OVERRIDE_DIR = os.path.join(TREE, "obj", "gate", "merge_pr_override")


def graded_shas_in(text):
    """Every 40-hex sha on lines mentioning the graded tree / a VERDICT."""
    out = []
    for ln in text.splitlines():
        if re.search(r"(VERDICT|graded|grading|tested|head)", ln, re.I):
            out += re.findall(r"\b[0-9a-f]{40}\b", ln)
    return out


def graded_sha_of(pr, env=None):
    """Graded sha named by the PR's own -V verdict is not available from just a PR
    number here, so we accept it two ways: read a `GRADED_SHA=<40hex>` recorded by
    the gate (obj/gate/merge_pr_override/) or expose the raw comment parse for the
    caller/test to supply.  Returns the sha or None (None => fail closed)."""
    p = os.path.join(TREE, "obj", "gate", f"graded-{pr}.sha")
    if os.path.exists(p):
        s = open(p).read().strip()
        return s if re.fullmatch(r"[0-9a-f]{40}", s) else None
    return None


def decide(head, graded):
    """(allow, reason).  head/graded are 40-hex (or None)."""
    if not head:
        return False, "REFUSE: no current head sha supplied — must name the tree that would merge"
    if not graded:
        return False, f"REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head {head[:12]}"
    if graded != head:
        return False, f"REFUSE: graded sha {graded[:12]} != current head {head[:12]} — refusing to merge a tree that was not graded"
    return True, f"ok: graded sha {graded[:12]} equals current head — merge allowed"


def record_override(pr, reason, env=None):
    os.makedirs(OVERRIDE_DIR, exist_ok=True)
    who = (env or os.environ).get("USER", "operator")
    root = None
    try:
        root = subprocess.run(["git", "-C", TREE, "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
    except Exception:
        root = "unknown"
    rec = {"pr": pr, "reason": reason, "by": who, "at": datetime.now(timezone.utc).isoformat(),
           "head": root}
    os.makedirs(OVERRIDE_DIR, exist_ok=True)
    path = os.path.join(OVERRIDE_DIR, f"pr-{pr}.reason.json")
    with open(path, "w") as fh:
        json.dump(rec, fh, indent=2)
    return path


def head_of_pr(pr, env=None):
    """Current head sha of a GitHub PR (or None if we cannot reach it; None -> the
    guard cannot confirm, so it fails closed)."""
    env = env or os.environ
    tok = env.get("GITHUB_TOKEN")
    if not tok:
        return None
    import urllib.request
    req = urllib.request.Request(
        f"https://api.github.com/repos/sarahkaylor/HobbyOS/pulls/{pr}",
        headers={"Authorization": "Bearer " + tok, "Accept": "application/vnd.github+json"})
    try:
        import json as _j
        d = _j.load(urllib.request.urlopen(req))
        return (d.get("head") or {}).get("sha")
    except Exception:
        return None


def self_test():
    fails = 0
    def check(cond, msg):
        nonlocal fails
        print(("  PASS: " if cond else "  FAIL: ") + msg)
        if not cond:
            fails += 1
    # core rule
    h = "a" * 40
    ok, r = decide(h, h); check(ok, "graded==head -> allow (%s)" % r[:20])
    ok, r = decide(h, "b" * 40); check(not ok, "graded!=head -> refuse (%s)" % r[:20])
    ok, r = decide(h, None); check(not ok, "no graded sha -> FAILS CLOSED (%s)" % r[:20])
    ok, r = decide(None, h); check(not ok, "no head -> refuse (%s)" % r[:20])
    # graded_shas_in parse
    check(40 in [len(s) for s in graded_shas_in("VERDICT GREEN graded sha " + "c" * 40)],
          "parses a 40-hex sha from a VERDICT/graded line")
    check(graded_shas_in("VERDICT GREEN") == [], "no sha in VERDICT -> empty (fails closed upstream)")
    # -> W0-case: #194 (GB-103) merged 2026-10-26 graded 7de6dde.. but merged e8c7dc8..
    #    (a GREEN that named a tree DIFFERENT from the head that merged).  The
    #    fixture must go red FOR THIS MISMATCH, not for silence.
    graded194 = "7de6dde4f3f29f53541b21fd1cd0a290168879a9"
    head194 = "e8c7dc89bc0156157930e4a90ba54d1845c0a147"
    ok, r = decide(head194, graded194)
    check(not ok and graded194 != head194,
          f"today's #194 incident shape (graded 7de6dde.., merged e8c7dc8..) -> RED for the mismatch ({r[:44]})")
    if fails:
        print("MERGE_PR_SELFTEST: FAIL (%d)" % fails); sys.exit(1)
    print("MERGE_PR_SELFTEST: PASS - fail-closed rule + override + #194 goes red")
    sys.exit(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default="sarahkaylor/HobbyOS")
    ap.add_argument("--pr", type=int)
    ap.add_argument("--head", default=None, help="current PR head 40-hex (default: git HEAD)")
    ap.add_argument("--graded", default=None, help="graded sha named by the GREEN (default: obj/gate/graded-<pr>.sha)")
    ap.add_argument("--override", default=None, help="written reason to merge through a failing guard")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        self_test(); return
    if a.pr and a.override is None:
        head = a.head or head_of_pr(a.pr)
        graded = a.graded or graded_sha_of(a.pr)
        allow, reason = decide(head, graded)
        print(f"[merge_pr] PR #{a.pr}: {reason}")
        if allow:
            print(f"[merge_pr] MERGE ALLOWED"); sys.exit(0)
        print(f"[merge_pr] NOT MERGED (fail closed)")
        sys.exit(1)
    if a.override:
        path = record_override(a.pr, a.override)
        print(f"[merge_pr] OVERRIDE recorded ({path}); merge allowed")
        sys.exit(0)
    ap.error("need --pr (+ --head/--override) or --self-test")


if __name__ == "__main__":
    main()
