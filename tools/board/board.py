#!/usr/bin/env python3
"""tools/board/board.py — HobbyOS Gaming Program board client (Forgejo).

The board is Forgejo `sarah/gaming-program` (§4.1).  Forgejo 17.0.0+gitea-1.22.0
(the version on swarm-ticketing .234) does NOT expose the Projects REST
endpoint (`/api/v1/repos/{owner}/{repo}/projects` -> 404), so the kanban lanes
are emulated with **labels** (one label per lane) plus **milestones** (M0..M4).
See tools/board/README.md — GB-002 notes the limitation.

Auth:  FORGEJO_URL, FORGEJO_TOKEN env vars (token of the `swarm-bot` user).
Repo:  BOARD_REPO (default `sarah/gaming-program`).

Usage:
  board.py lanes                      # list lane labels (create missing with --create)
  board.py list [--lane L] [--milestone M0] [--label x] [--state open|closed|all]
  board.py show GB-013
  board.py claim GB-013 [--by swarm-worker-1]
  board.py move GB-013 "In Progress" [--no-comment]
  board.py comment GB-013 -m "text"   (or --file path)
  board.py status                     # lane/milestone rollup (board at a glance)
  board.py lint                       # lane hygiene / blocked-without-reason checks
"""
import argparse
import fcntl
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

API = os.environ.get("FORGEJO_URL", "http://192.168.0.234:3000").rstrip("/") + "/api/v1"
TOKEN = os.environ.get("FORGEJO_TOKEN", "")
REPO = os.environ.get("BOARD_REPO", "sarah/gaming-program")

# §4.1 lanes, in board order.
LANES = [
    "Backlog",
    "Ready",
    "In Progress",
    "In Review",
    "Merge Gate",
    "Done",
    "Icebox",
]
LANE_COLORS = {
    "Backlog": "#888888",
    "Ready": "#0e8a16",
    "In Progress": "#fbca04",
    "In Review": "#ff9800",
    "Merge Gate": "#d93f0b",
    "Done": "#1d76db",
    "Icebox": "#c5def5",
}
WEIGHT_LABELS = {"S": "#bfdadc", "M": "#bfd4f2", "L": "#d4c5f9"}


def _req(method, path, data=None, raw_query=""):
    url = API + path + raw_query
    body = json.dumps(data).encode() if data is not None else None
    r = urllib.request.Request(url, data=body, method=method)
    r.add_header("Authorization", "token " + TOKEN)
    r.add_header("Accept", "application/json")
    if body:
        r.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(r, timeout=30) as resp:
            txt = resp.read().decode()
            return json.loads(txt) if txt.strip() else None
    except urllib.error.HTTPError as e:
        msg = e.read().decode(errors="replace")
        raise SystemExit(f"HTTP {e.code} {method} {url}: {msg[:400]}") from None
    except urllib.error.URLError as e:
        raise SystemExit(f"network error {method} {url}: {e}") from None


def api_get(path, query=""):
    return _req("GET", path, None, query)


def api_post(path, data):
    return _req("POST", path, data)


def api_patch(path, data):
    return _req("PATCH", path, data)


def api_delete(path):
    return _req("DELETE", path)


# ---------------------------------------------------------------- labels
def labels():
    out, page = [], 1
    while True:
        chunk = api_get(f"/repos/{REPO}/labels", f"?limit=50&page={page}")
        out += chunk
        if len(chunk) < 50:
            return out
        page += 1


def ensure_label(name, color="#ededed", desc=""):
    for lab in labels():
        if lab["name"] == name:
            return lab
    return api_post(f"/repos/{REPO}/labels", {"name": name, "color": color.lstrip("#"), "description": desc})


def ensure_lanes(create=False):
    have = {l["name"] for l in labels()}
    missing = [n for n in LANES if n not in have]
    if create:
        for n in missing:
            ensure_label(n, LANE_COLORS[n], "kanban lane (§4.1)")
    return missing


# ---------------------------------------------------------------- issues
def all_issues(state="all"):
    out, page = [], 1
    while True:
        chunk = api_get(f"/repos/{REPO}/issues", f"?state={state}&limit=50&page={page}")
        out += chunk
        if len(chunk) < 50:
            return out
        page += 1


def issue_by_id(gid):
    """Issue whose title starts with the ticket id (GB-nnn, PLAN-nnn, EPIC-...).

    The id is in the title.  PLAN/EPIC tickets were previously unaddressable
    here (`no issue for PLAN-005`), which is how PLAN-003 was left open with no
    lane — the tool could not name the ticket its own loop creates.  Any of the
    three prefixes is accepted, and a bare number is read as GB-nnn.
    """
    gid = gid.upper().strip()
    if gid.isdigit():
        for it in all_issues():
            if it["number"] == int(gid):
                return it
        return None
    if gid.startswith("EPIC") and not re.match(r"^EPIC\b", gid):
        gid = "EPIC"          # EPIC titles carry no number: "EPIC Program-2: …"
    if not re.match(r"^(GB|PLAN|EPIC)-", gid) and not gid.startswith("EPIC"):
        gid = "GB-" + gid
    for it in all_issues():
        # (?![\w-]) is load-bearing: the re-plan loop mints `GB-nnn-V`
        # verification tickets, and a bare `\b` after GB-nnn matches the hyphen.
        # all_issues() returns NEWEST first, so `GB-016-V` would be found before
        # `GB-016` — `move GB-016 Done` would then have laned and closed the
        # verification ticket instead of the implementation one.
        if re.match(rf"^{re.escape(gid)}(?![\w-])", it["title"]):
            return it
    return None


def lane_of(issue):
    names = [l["name"] for l in issue.get("labels", [])]
    for n in LANES:
        if n in names:
            return n
    return None


def set_lane(issue, lane, create=True):
    if lane not in LANES:
        raise SystemExit(f"unknown lane {lane!r}; lanes={LANES}")
    ki = issue["number"]
    if create:
        ensure_label(lane, LANE_COLORS[lane], "kanban lane (§4.1)")
    # GB-074: act on the CURRENT server state, not the (possibly stale) issue
    # object the caller read earlier, so a concurrent lane move is not clobbered
    # and the observed transition is the one we really made.  The swap is a
    # single atomic PUT of the whole label set (Forgejo serializes it
    # server-side), instead of delete-then-add across two requests.
    cur_it = api_get(f"/repos/{REPO}/issues/{ki}")
    cur = lane_of(cur_it)
    names = [l["name"] for l in cur_it.get("labels", []) if l["name"] not in LANES]
    names.append(lane)
    api_put_labels(ki, names)
    return cur


def label_ids(names):
    """Forgejo's CreateIssueOption.labels wants int64 ids, not names."""
    by = {l["name"]: l["id"] for l in labels()}
    return sorted({by[n] for n in names if n in by})


def api_put_labels(index, names):
    """Replace the issue's whole label set (PUT .../labels)."""
    return _req("PUT", f"/repos/{REPO}/issues/{index}/labels", {"labels": names})


def milestone_id(name):
    """name may be 'M0', 'M0-infra', or a full title."""
    for m in api_get(f"/repos/{REPO}/milestones", "?state=all&limit=50"):
        if m["title"] == name or m["title"].startswith(name + "-") or m["title"] == name:
            return m["id"], m["title"]
    return None, None


def comment(gid, text, lane=None, close=False):
    """Post a comment; optionally also move the lane (and close for Done).

    The lane move happens FIRST so the comment lands in the state it describes
    (the webhook/poll inbox then shows the transition with the explanation)."""
    it = issue_by_id(gid)
    if not it:
        raise SystemExit(f"no issue for {gid}")
    if lane:
        set_lane(it, lane)
        if lane == "Done":
            close = True
    if close:
        api_patch(f"/repos/{REPO}/issues/{it['number']}", {"state": "closed"})
    return api_post(f"/repos/{REPO}/issues/{it['number']}/comments", {"body": text})


# ---------------------------------------------------------------- commands
def claimant_of(index):
    """Name the agent behind the first 'claimed by `X`' comment, if any (GB-074
    loser detection: the loser must be able to say who won)."""
    for c in api_get(f"/repos/{REPO}/issues/{index}/comments", "?limit=50"):
        m = re.search(r"claimed by `([^`]+)`", c.get("body") or "")
        if m:
            return m.group(1)
    return None


def cmd_claim(a):
    it = issue_by_id(a.gid)
    if not it:
        raise SystemExit(f"no issue for {a.gid}")
    by = a.by or os.environ.get("BOARD_AGENT", "swarm-bot")
    # GB-074 (claim race): serialize concurrent claims on a lock, then re-read
    # the CURRENT lane before claiming.  A ticket already In Progress was claimed
    # by someone else — refuse and name the winner; never run the gate on a
    # double claim.  The lockfile must sit on the shared store for cross-VM
    # exclusion; on a single host (and in CI) flock alone already serializes.
    lock_path = os.environ.get("BOARD_LOCK", "/tmp/hobbyos-board-claim.lock")
    lockf = open(lock_path, "a")
    fcntl.flock(lockf, fcntl.LOCK_EX)
    try:
        cur = lane_of(api_get(f"/repos/{REPO}/issues/{it['number']}"))
        if cur == "In Progress":
            winner = claimant_of(it["number"])
            raise SystemExit(
                f"{a.gid} #{it['number']} already claimed by `{winner or 'another agent'}` "
                "(lane already In Progress) — not re-claiming (GB-074); run no gate")
        prev = set_lane(it, "In Progress")
        msg = (f"claimed by `{by}` on {a.host or 'swarm-coordinator'} — "
               f"lane: {prev or 'Backlog'} → In Progress")
        comment(a.gid, msg)
    finally:
        fcntl.flock(lockf, fcntl.LOCK_UN)
        lockf.close()
    print(f"{a.gid} #{it['number']} claimed by {by} (was {prev})")


def cmd_move(a):
    it = issue_by_id(a.gid)
    if not it:
        raise SystemExit(f"no issue for {a.gid}")
    prev = set_lane(it, a.lane)
    if a.lane == "Done":
        api_patch(f"/repos/{REPO}/issues/{it['number']}", {"state": "closed"})
    elif it["state"] == "closed":
        api_patch(f"/repos/{REPO}/issues/{it['number']}", {"state": "open"})
    if not a.no_comment:
        comment(a.gid, f"{a.gid}: lane {prev or '-'} → **{a.lane}**")
    print(f"{a.gid} #{it['number']}: {prev or '-'} -> {a.lane}")


def cmd_comment(a):
    text = a.message if a.message is not None else (open(a.file).read() if a.file else sys.stdin.read())
    c = comment(a.gid, text)
    print(f"{a.gid}: comment posted ({c['html_url']})")


def cmd_list(a):
    for it in all_issues(a.state):
        lab = [l["name"] for l in it["labels"]]
        if a.lane and a.lane not in lab:
            continue
        if a.label and a.label not in lab:
            continue
        ms = (it.get("milestone") or {}).get("title", "-")
        if a.milestone and not ms.startswith(a.milestone):
            continue
        print(f"#{it['number']:<4} {it['title'][:64]:<64} {ms:<14} {lane_of(it) or '-':<12} {','.join(l for l in lab if l not in LANES)}")


def cmd_show(a):
    it = issue_by_id(a.gid)
    if not it:
        raise SystemExit(f"no issue for {a.gid}")
    print(json.dumps({k: it[k] for k in ("number", "title", "state", "labels", "milestone", "assignees", "html_url")}, indent=1))
    print("---- body ----")
    print(it.get("body") or "")
    for c in api_get(f"/repos/{REPO}/issues/{it['number']}/comments", "?limit=50"):
        print(f"---- comment {c['user']['login']} {c['created_at']} ----\n{c['body']}")


def cmd_status(a):
    issues = all_issues("all")
    lanes = {n: [] for n in LANES}
    lanes["(no lane)"] = []
    for it in issues:
        lanes[lane_of(it) or "(no lane)"].append(it)
    print(f"BOARD {REPO} @ {API}  — {len(issues)} tickets")
    for n in LANES + ["(no lane)"]:
        row = lanes[n]
        if row or n == "(no lane)":
            print(f"  {n:<12} {len(row):>3}")
    print()
    ms = api_get(f"/repos/{REPO}/milestones", "?state=all&limit=50")
    for m in ms:
        iss = [i for i in issues if (i.get("milestone") or {}).get("id") == m["id"]]
        done = sum(1 for i in iss if i["state"] == "closed")
        pct = (100.0 * done / len(iss)) if iss else 0.0
        print(f"  {m['title']:<16} {done:>2}/{len(iss):<2} open={len(iss)-done:<3} {pct:5.1f}%")


def _has_evidence_comment(body):
    """True when a comment opens with the EVIDENCE marker.

    The marker is a convention, not a token match on the raw bytes: comments
    are written in markdown, so the overwhelmingly common shape is
    ``**EVIDENCE — ...``.  Stripping leading emphasis/space is what makes the
    lint say what it means (a real EVIDENCE comment was being flagged as
    missing because of two asterisks).
    """
    return body.lstrip(" \t\r\n*_#").upper().startswith("EVIDENCE")


def cmd_lint(a):
    rc = 0
    missing = ensure_lanes(create=False)
    if missing:
        print(f"LINT lane labels missing: {missing}")
        rc = 1
    for it in all_issues("open"):
        lab = [l["name"] for l in it["labels"]]
        gid = (re.match(r"^(GB-\d+(?:-V)?)", it["title"]) or [None, it["title"][:10]])[1]
        if lane_of(it) is None:
            print(f"LINT {gid}: no lane label")
            rc = 1
        if "blocked" in lab and "Blocked:" not in (it.get("body") or ""):
            print(f"LINT {gid}: `blocked` label without a 'Blocked:' reason in the body")
            rc = 1
        if lane_of(it) in ("In Review", "Merge Gate") and not any(
            _has_evidence_comment(c["body"]) for c in api_get(f"/repos/{REPO}/issues/{it['number']}/comments", "?limit=50")
        ):
            print(f"LINT {gid}: in {lane_of(it)} without an EVIDENCE comment")
            rc = 1
    # GB-049 / PLAN-006 / blockers deadlock: the verify-lane and dependency
    # checks live in the grooming bot (tools/board/groom.py) and run here so the
    # canonical lint reports the gate debt and any dependency deadlock.  These
    # can provably fail (self-test); a review-lane card whose gate has not
    # posted GREEN is exactly the "lane lies about the gate" case §4.2 step 5
    # exists to catch.
    try:
        import groom
        all_iss = all_issues("all")
        gfind = groom.board_state_lint(
            all_iss,
            lambda n: api_get(f"/repos/{REPO}/issues/{n}/comments", "?limit=100"))
        for kind, gid, detail in gfind:
            print(f"LINT {gid}: [{kind}] {detail}")
            rc = 1
    except ImportError:
        pass
    if rc == 0:
        print("LINT clean")
    return rc


def main(argv=None):
    p = argparse.ArgumentParser(prog="board.py")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("lanes")
    s.add_argument("--create", action="store_true")
    s.set_defaults(fn=lambda a: print(json.dumps(ensure_lanes(a.create), indent=1)))

    s = sub.add_parser("list")
    s.add_argument("--lane")
    s.add_argument("--label")
    s.add_argument("--milestone")
    s.add_argument("--state", default="open")
    s.set_defaults(fn=cmd_list)

    s = sub.add_parser("show")
    s.add_argument("gid")
    s.set_defaults(fn=cmd_show)

    s = sub.add_parser("claim")
    s.add_argument("gid")
    s.add_argument("--by")
    s.add_argument("--host")
    s.set_defaults(fn=cmd_claim)

    s = sub.add_parser("move")
    s.add_argument("gid")
    s.add_argument("lane")
    s.add_argument("--no-comment", action="store_true")
    s.set_defaults(fn=cmd_move)

    s = sub.add_parser("comment")
    s.add_argument("gid")
    s.add_argument("-m", "--message")
    s.add_argument("--file")
    s.set_defaults(fn=cmd_comment)

    s = sub.add_parser("status")
    s.set_defaults(fn=cmd_status)

    s = sub.add_parser("lint")
    s.set_defaults(fn=cmd_lint)

    a = p.parse_args(argv)
    if not TOKEN:
        raise SystemExit("FORGEJO_TOKEN not set")
    r = a.fn(a)
    sys.exit(r if isinstance(r, int) else 0)


if __name__ == "__main__":
    main()
