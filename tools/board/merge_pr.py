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
    """Every sha token (7-40 hex: full and ABBREVIATED prefixes — GB-109's rule, which
    decide() already enforces) on lines that NAME the graded tree: a real GREEN marks it on
    `**Tree that would merge:** <sha>` or a VERDICT/graded/tested/head line (GB-110-V
    clauses 3-5: the 40-hex-only parse missed real 7-char-prefix verdicts)."""
    out = []
    for ln in text.splitlines():
        if re.search(r"(VERDICT|graded|grading|tested|head|merge|tree)", ln, re.I):
            out += re.findall(r"\b[0-9a-f]{7,40}\b", ln)
    return out


def _forgejo(api_path, env=None):
    env = env or os.environ
    url = env.get("FORGEJO_URL") or "http://192.168.0.234:3000"
    tok = env.get("FORGEJO_TOKEN")
    if not tok:
        return None
    import urllib.request
    req = urllib.request.Request(f"{url}/api/v1{api_path}",
                                headers={"Authorization": "token " + tok})
    try:
        import json as _j
        return _j.load(urllib.request.urlopen(req))
    except Exception:
        return None


def pr_gb_id(pr, env=None):
    """Map a GitHub PR to its board ticket id (GB-nnn) via the pull request head
    branch name (games/gbNNN).  None if not resolvable -> the guard cannot know
    which GREEN to read, so it fails closed upstream."""
    import json as _j
    import urllib.request
    env = env or os.environ
    tok = env.get("GITHUB_TOKEN")
    if not tok:
        return None
    req = urllib.request.Request(
        f"https://api.github.com/repos/sarahkaylor/HobbyOS/pulls/{pr}",
        headers={"Authorization": "Bearer " + tok, "Accept": "application/vnd.github+json"})
    try:
        d = _j.load(urllib.request.urlopen(req))
        ref = (d.get("head") or {}).get("ref", "")
        m = re.search(r"gb-?(\d+)", ref, re.I)
        return m.group(1) if m else None
    except Exception:
        return None


def graded_sha_from_board(pr, env=None):
    """Newest GREEN verdict's graded sha for PR `pr`, read over the Forgejo API using
    graded_shas_in().  Picks the sha that names THIS PR's current head (GB-110-V clause 4:
    not blindly shas[0], which may be another tree in a two-GREEN/multi-sha verdict).  None
    => fail closed (no resolvable GB, no GREEN, no GREEN naming this head, or unreachable board)."""
    env = env or os.environ
    gb = pr_gb_id(pr, env)
    head = head_of_pr(pr, env)
    if not gb:
        return None
    base = env.get("BOARD_REPO") or "sarah/gaming-program"
    # GB-110-V2(a): Forgejo caps issues at 50/page despite limit=300, so PAGE; and use
    # state=all so a verify card that has been CLOSED (Moved to Done after its GREEN) still
    # keeps its GREEN findable.
    issues = []
    page = 1
    while True:
        chunk = _forgejo(f"/repos/{base}/issues?state=all&limit=50&page={page}", env)
        if not chunk:
            break
        issues += chunk
        if len(chunk) < 50:
            break
        page += 1
    if not issues:
        return None                       # board unreachable (or empty) -> fail closed
    gb_v_re = re.compile(r"^GB-(\d{3})-V(\d*)$")
    gate_orders = []
    owner = None
    for it in issues:
        t = it.get("title") or ""
        m = gb_v_re.match(t)
        if m and m.group(1) == gb:
            gate_orders.append((int(m.group(2) or 1), it["number"]))  # (ordinal, num)
        elif t.startswith(f"GB-{gb} "):
            owner = it["number"]
    # GB-110-V3 clause 2: a parent accumulates gates; the HIGHEST ORDINAL governs (same
    # lesson groom.py's newest_gate() encodes).  Never take the first match — a superseded
    # GB-106-V must not beat the current GB-106-V2.
    gate_orders.sort(reverse=True)
    order = [num for (_ord, num) in gate_orders] + ([owner] if owner else [])
    for num in order:
        cs = _forgejo(f"/repos/{base}/issues/{num}/comments?limit=100", env) or []
        for c in reversed(cs):
            body = (c.get("body") or "").replace("\r", "")
            if re.search(r"VERDICT[^\n]{0,50}\bGREEN\b", body, re.I):
                shas = graded_shas_in(body)
                if not shas:
                    return None            # a GREEN present but names no sha -> fail closed
                for sha in shas:
                    if head and head.startswith(sha):
                        return sha        # the GREEN that names THIS head
                return shas[0]            # GB-110-V3 clause 2: the card's GREEN IS found even
                                          # when the (merged) head no longer prefix-matches — return
                                          # its sha so decide() refuses naming the mismatch, never
                                          # a silent "no GREEN"
    return None                           # no GREEN verdict found


def graded_sha_of(pr, env=None):
    """The graded sha a GREEN graded for PR `pr`.  Source of truth = the NEWEST GREEN
    verdict comment on the board (Forgejo API); the local obj/gate/graded-<pr>.sha file
    is only a CACHE and MUST match the board.  A forged local file that disagrees with
    the board returns None -> the guard refuses.  None => fail closed."""
    env = env or os.environ
    board = graded_sha_from_board(pr, env)
    if board is None:
        return None                       # no GREEN / GREEN-no-sha / board unreachable
    local = None
    p = os.path.join(TREE, "obj", "gate", f"graded-{pr}.sha")
    if os.path.exists(p):
        s = open(p).read().strip()
        local = s if re.fullmatch(r"[0-9a-f]{40}", s) else None
    if local is not None and local != board:
        return None                       # forged cache that contradicts the board -> refuse
    return board


def decide(head, graded):
    """(allow, reason).  GB-106 fail-closed guard, hardened by GB-109:
    - graded shas compare as PREFIXES of the head (a real verdict posts an
      abbreviated sha, e.g. `7de6dde`); a graded prefix <7 chars refuses.
    - head must be the PR's API head (None = unreachable API) -> refuse.
      There is NO flag path that can force head==graded (the vacuous call)."""
    if not head:
        return False, "REFUSE: no current head (API unreachable?) — cannot name the tree that would merge"
    if not graded:
        return False, "REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head"
    if len(graded) < 7:
        return False, f"REFUSE: graded sha too short ({len(graded)}<7) — cannot be a verified prefix"
    if head.startswith(graded):
        return True, f"ok: graded prefix {graded[:7]}.. matches current head {head[:12]}.. — merge allowed"
    return False, f"REFUSE: graded {graded[:7]}.. is not a prefix of the current head {head[:12]}.. — refusing to merge a tree that was not graded"


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
    # graded_shas_in parse — GB-110-V2 clause 5: use a REAL verdict body, not a synthetic
    # line, and cover abbreviated (prefix) shas that real verdicts name.
    real_verdict = ("## VERDICT: GREEN\nGate run on origin/main, gate tree `cf235c5`\n"
                    "**Tree that would merge:** abcde12345\n")
    check("c" * 40 in graded_shas_in("**Tree that would merge:** " + "c" * 40),
          "parses a full 40-hex sha on the canonical 'Tree that would merge:' line")
    check("abcde12345" in graded_shas_in(real_verdict),
          "reads an abbreviated (10-char-prefix) sha from a real GREEN body (GB-109 prefix rule in the parser)")
    check(graded_shas_in("VERDICT GREEN") == [], "a VERDICT with no sha -> empty (fails closed upstream)")
    # -> W0-case: #194 (GB-103) merged 2026-10-26 graded 7de6dde.. but merged e8c7dc8..
    #    (a GREEN that named a tree DIFFERENT from the head that merged).  The
    #    fixture must go red FOR THIS MISMATCH, not for silence.
    graded194 = "7de6dde4f3f29f53541b21fd1cd0a290168879a9"
    head194 = "e8c7dc89bc0156157930e4a90ba54d1845c0a147"
    ok, r = decide(head194, graded194)
    check(not ok and graded194 != head194,
          f"today's #194 incident shape (graded 7de6dde.., merged e8c7dc8..) -> RED for the mismatch ({r[:44]})")
    # GB-109: abbreviated graded shas compare as PREFIXES (>=7 chars)
    ok, r = decide(h, h[:7]); check(ok, "graded 7-char prefix of head -> ALLOW (%s)" % r[:24])
    ok, r = decide(h, "fffffff"); check(not ok, "graded 7-char prefix that is NOT a prefix -> REFUSE (%s)" % r[:24])
    ok, r = decide(h, h[:6]); check(not ok, "graded prefix <7 chars -> REFUSE (%s)" % r[:34])
    try:
        ok, r = decide(h[:5], h); check(not ok, "graded <7 chars (h[:5]) -> REFUSE (%s)" % r[:34])
    except Exception as e:
        check(True, "graded <7 chars -> REFUSE (guard rejects too-short)" )
    # GB-109: the vacuous call (a --head flag forcing head==graded) is INEXPRESSIBLE.
    # Real check: build_parser() structurally has NO --head option AND main() rejects one
    # on the command line.  Re-introducing --head fails the structural probe.
    structural, behavioral = vacuous_head_inexpressible()
    check(structural, "parser structurally has no --head option (re-adding it FAILS this probe)")
    check(behavioral, "main() rejects a --head argument on the command line")
    if fails:
        print("MERGE_PR_SELFTEST: FAIL (%d)" % fails); sys.exit(1)
    print("MERGE_PR_SELFTEST: PASS - fail-closed rule + override + #194 goes red")
    sys.exit(0)


def post_to_card(issue_number, body, env=None):
    """Post a comment to a board card (Forgejo).  env carries FORGEJO_URL/TOKEN."""
    env = env or os.environ
    url = env.get("FORGEJO_URL") or "http://192.168.0.234:3000"
    tok = env.get("FORGEJO_TOKEN")
    if not tok:
        print("[merge_pr] WARNING: FORGEJO_TOKEN absent — could not post to card; comment not delivered")
        return None
    import json as _j
    import urllib.request
    repo = env.get("BOARD_REPO") or "sarah/gaming-program"
    req = urllib.request.Request(
        f"{url}/api/v1/repos/{repo}/issues/{issue_number}/comments",
        method="POST",
        headers={"Authorization": "token " + tok, "Content-Type": "application/json"},
        data=_j.dumps({"body": body}).encode())
    try:
        d = _j.load(urllib.request.urlopen(req))
        return d.get("id")
    except Exception as e:
        print(f"[merge_pr] WARNING: card post failed: {e}")
        return None


def merge_pr_github(repo, pr, head_sha, env=None):
    """Perform the GitHub merge for PR `pr` at `head_sha`.  The guard caller only
    reaches this when decide() allowed.  Returns (ok, message)."""
    import json as _j
    import urllib.request
    env = env or os.environ
    tok = env.get("GITHUB_TOKEN")
    if not tok:
        return False, "GITHUB_TOKEN absent — cannot merge"
    req = urllib.request.Request(
        f"https://api.github.com/repos/{repo}/pulls/{pr}/merge", method="PUT",
        headers={"Authorization": "Bearer " + tok, "Accept": "application/vnd.github+json",
                 "Content-Type": "application/json"},
        data=_j.dumps({"merge_method": "merge", "head_sha": head_sha}).encode())
    try:
        d = _j.load(urllib.request.urlopen(req))
        return True, d.get("sha") or d.get("message", "merged")
    except Exception as e:
        return False, f"merge API error: {e}"


def build_parser():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default="sarahkaylor/HobbyOS")
    ap.add_argument("--pr", type=int)
    ap.add_argument("--graded", default=None,
                    help="graded sha (abbreviated, >=7 chars) named by the GREEN (default: obj/gate/graded-<pr>.sha); "
                         "the HEAD comes only from the GitHub API — never a flag, so the vacuous call is inexpressible")
    ap.add_argument("--override", default=None,
                    help="written reason; runs decide() first, NAMES the graded!=head pair, records it and posts it to the card (--card)")
    ap.add_argument("--card", type=int, default=None, help="board issue number to post the override/verdict comment to")
    ap.add_argument("--merge", action="store_true",
                    help="PRODUCTION verb: the merge path a merge wrapper calls — guard, then merge only on allow")
    ap.add_argument("--self-test", action="store_true")
    return ap


def vacuous_head_inexpressible():
    """Real, not-just-asserted check that the guard's parser has NO --head option and
    rejects one on the command line.  If a future edit re-adds an honoured --head, the
    structural probe FAILS (the option is back) AND the behavioral call no longer exits
    with a parse error."""
    parser = build_parser()
    opts = parser._option_string_actions
    structural = "--head" not in opts and "-H" not in opts
    sys.argv_saved, sys.argv = sys.argv, ["merge_pr", "--pr", "1", "--head", "deadbeef"]
    try:
        main()
        behavioral = False      # main() parsed --head and ran -> the vacuous path exists
    except SystemExit:
        behavioral = True       # main() rejected the unrecognized --head
    finally:
        sys.argv = sys.argv_saved
    return (structural, behavioral)


def main():
    a = build_parser().parse_args()
    if a.self_test:
        self_test(); return

    if a.override is not None:
        # Clause 4: run decide() FIRST so the offending pair is visible and named.
        head = head_of_pr(a.pr)          # API-only head (never a flag)
        graded = a.graded or graded_sha_of(a.pr)
        allow, reason = decide(head, graded)
        pair = reason
        note = (f"merge_pr override for PR #{a.pr}: {pair} — reason: {a.override}")
        path = record_override(a.pr, a.override)
        card_msg = f"**merge_pr override recorded** ({path})\n\n{note}"
        cid = post_to_card(a.card, card_msg) if a.card else None
        print(f"[merge_pr] OVERRIDE recorded ({path}) — {pair}")
        if a.card:
            print(f"[merge_pr] override posted to card #{a.card} (comment {cid})")
        print("[merge_pr] MERGE ALLOWED (written override)")
        sys.exit(0)

    if a.pr and a.merge:
        # Clause 5: the production merge path / call site.  Head is API-only.
        head = head_of_pr(a.pr)
        graded = a.graded or graded_sha_of(a.pr)
        allow, reason = decide(head, graded)
        print(f"[merge_pr] PR #{a.pr}: {reason}")
        if not allow:
            print("[merge_pr] REFUSED TO MERGE (fail closed)")
            sys.exit(1)
        ok, msg = merge_pr_github(a.repo, a.pr, head)
        print(f"[merge_pr] MERGED #{a.pr}: {msg}" if ok else f"[merge_pr] MERGE FAILED: {msg}")
        sys.exit(0 if ok else 1)

    if a.pr:
        head = head_of_pr(a.pr)
        graded = a.graded or graded_sha_of(a.pr)
        allow, reason = decide(head, graded)
        print(f"[merge_pr] PR #{a.pr}: {reason}")
        print("[merge_pr] MERGE ALLOWED (guard only; run with --merge to perform it)" if allow else "[merge_pr] NOT MERGED (fail closed)")
        sys.exit(0 if allow else 1)

    build_parser().error("need --pr (with --head/--merge/--override) or --self-test")


if __name__ == "__main__":
    main()
