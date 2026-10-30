#!/usr/bin/env python3
"""merge_gate_enforce.py - GB-005: a card may not move to `Merge Gate` unless the
newest `GB-nnn-V` verdict on it is GREEN, and the merge path must refuse a PR whose
card has no GREEN.  Enforcement, not just detection - the 2026-10-26 incident was a
PR merged in 86 s with no gate card and no review.

Fails CLOSED: no GREEN verdict (RED intermediate, or no -V ticket, or board
unreachable) refuses.  A `--force` path exists and records WHO forced it on the
card so a bypass is attributable, never silent.

The decision is a tiny pure function (`allow`), and the verdict source is a callable
(`verdict_source`) so the host test can SABOTAGE it (always-GREEN) and prove the
refusal STOPS refusing - the GB-005 discipline: a refusal that cannot be made to
fail is decoration.

Usage:
  python3 tools/board/merge_gate_enforce.py GB-005   # prints allow-green / refuse+reason
  python3 tools/board/merge_gate_enforce.py GB-005 --force-agent swarm-farm-1-winf
  python3 tools/board/merge_gate_enforce.py --post-force <issue_number> --agent X --reason "..."
  python3 tools/board/merge_gate_enforce.py --self-test
"""
import re, sys
from datetime import datetime, timezone
import urllib.request, urllib.parse
import json, os

API = os.environ.get("FORGEJO_URL", "http://192.168.0.234:3000").rstrip("/") + "/api/v1"
TOKEN = os.environ.get("FORGEJO_TOKEN", "")
REPO = os.environ.get("BOARD_REPO", "sarah/gaming-program")


def allow(verdict):
    """Is a move to `Merge Gate` allowed for this verdict?  Only a GREEN.  None ->
    fail closed.  [Isolated so the test provokes it.]"""
    return (verdict or "").upper() == "GREEN"


def refuse_reason(verdict, gid):
    if (verdict or "").upper() == "GREEN":
        return ""
    if verdict is None:
        return (f"REFUSE: {gid} has no GREEN verdict - a card cannot reach Merge Gate "
                f"without a GREEN from its -V (GB-005); use --force with a written reason to bypass")
    return (f"REFUSE: newest {gid}-V verdict is {verdict.upper()}, not GREEN - a card "
            f"cannot reach Merge Gate on a non-GREEN gate (GB-005)")


def _api_get(path, query=""):
    req = urllib.request.Request(API + path + query,
                                headers={"Authorization": "token " + TOKEN,
                                         "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode())


def verdict_source(gid):
    """Newest GREEN/RED verdict for a card's gate tickets (`GB-nnn-V` ordinals V1, V2, ...).

    Reuses groom.verdict_in (the one parser board.py/merge_pr.py/check_verdicts.py all
    share) so this enforcement cannot disagree with the merge helper about what a
    verdict looks like.  Returns 'GREEN' / 'RED' / None (no gate verdict found).
    """
    try:
        from groom import verdict_in
    except Exception:
        verdict_in = None
    issues, page = [], 1
    while True:
        batch = _api_get(f"/repos/{REPO}/issues", f"?state=all&limit=50&page={page}")
        if not batch:
            break
        issues += batch
        if len(batch) < 50:
            break
        page += 1
    by_title = {i.get("title", "").split()[0]: i for i in issues if i.get("title")}
    # newest ordinal gate for gid (GATE_RE on bare id keys).
    gates = []
    for k, it in by_title.items():
        m = re.match(rf"^GB-(\d{{3}})-V(\d*)$", k)
        if m and m.group(1) == re.sub(r"^GB-", "", gid):
            gates.append(((int(m.group(2) or 1), k), it))
    if not gates:
        return None
    # Comment order from Forgejo is ascending; newest verdict is the LAST GREEN/RED.
    gate_num = max(gates)[1]["number"]
    for c in reversed(_api_get(f"/repos/{REPO}/issues/{gate_num}/comments", "?limit=100")):
        body = (c.get("body") or "").replace("\r", "")
        try:
            v = verdict_in(body) if verdict_in else (
                re.search(r"VERDICT[^\n]{0,40}?\b(GREEN|RED)\b", body, re.I) or [None, None])[1]
            if v:
                return v.upper()
        except Exception:
            continue
    return None


def post_force_record(issue_number, agent, reason, gid, env=None):
    """Write the --force bypass record onto the card: one durable comment."""
    body = (f"**MERGE-GATE FORCE** (GB-005 override) — moved to a lane/merge path "
            f"without a GREEN.\\n"
            f"- operator: {agent}\\n- gid: {gid}\\n- at: {datetime.now(timezone.utc).isoformat()}\\n"
            f"- reason: {reason}\\n*The bypass is recorded on the card; it is not silent.*")
    req = urllib.request.Request(
        f"{API}/repos/{REPO}/issues/{issue_number}/comments", method="POST",
        headers={"Authorization": "token " + TOKEN, "Content-Type": "application/json"},
        data=json.dumps({"body": body}).encode())
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode())


def self_test():
    fails = []
    def check(cond, msg):
        print(("  PASS: " if cond else "  FAIL: ") + msg)
        if not cond:
            fails.append(msg)
    check(allow("GREEN") is True, "GREEN -> allow")
    check(allow("RED") is False, "RED -> refuse")
    check(allow(None) is False, "no verdict -> FAILS CLOSED")
    check(allow("rat") is False, "garbage text -> refuse")
    check(refuse_reason("RED", "GB-005").startswith("REFUSE"),
          "RED gives a written REFUSE reason naming the gate")
    check(refuse_reason(None, "GB-005").startswith("REFUSE"),
          "no GREEN gives a written REFUSE reason naming the fail-closed rule")
    check(refuse_reason("GREEN", "GB-005") == "", "GREEN carries no refusal")
    if fails:
        print("MERGE_GATE_ENFORCE_SELFTEST: FAIL (%d)" % len(fails)); sys.exit(1)
    print("MERGE_GATE_ENFORCE_SELFTEST: PASS - allow/refuse rule fails closed")
    sys.exit(0)


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        self_test()
    if args and args[0] == "--post-force":
        i = args.index("--post-force"); ref = args[i + 1]
        agent = args[args.index("--agent") + 1] if "--agent" in args else "unknown"
        reason = args[args.index("--reason") + 1] if "--reason" in args else "override"
        gid = args[args.index("--gid") + 1] if "--gid" in args else ref
        if str(ref).isdigit():
            num = int(ref)
        else:
            issues, page = [], 1
            while True:
                batch = _api_get(f"/repos/{REPO}/issues", f"?state=all&limit=50&page={page}")
                if not batch:
                    break
                issues += batch
                if len(batch) < 50:
                    break
                page += 1
            hit = next((it for it in issues if (it.get("title") or "").split()[0] == gid), None)
            if not hit:
                print(f"merge_gate_enforce: no card for {gid}"); sys.exit(2)
            num = hit["number"]
        post_force_record(num, agent, reason, gid)
        print(f"force record posted to card #{num}")
        sys.exit(0)
    if args:
        gid = args[0]
        v = verdict_source(gid)
        r = refuse_reason(v, gid)
        if not r:
            print(f"{gid}: ALLOW - newest verdict is GREEN; may move to Merge Gate")
            sys.exit(0)
        print(r)
        sys.exit(1)
    print("usage: merge_gate_enforce.py GB-005 | --self-test | --post-force <n> --agent X --reason R --gid GB-005")
    sys.exit(2)


if __name__ == "__main__":
    main()
