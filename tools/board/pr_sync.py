#!/usr/bin/env python3
"""tools/board/pr_sync.py — map open PRs to their board cards (split-aware).

WORK POLICY §2: a card with an open PR belongs in `In Review`, never
`In Progress`.  This file is the versioned source of the hourly timer that
enforces that rule (the coordinator deploys a copy; the API shapes here are the
same as `tools/board/board.py`).

Ticket ids in PR titles take any of these shapes:

    GB-<n>                 implementation card
    GB-<n>-<A>             split card            (GB-146-A)
    GB-<n>-<A>-<Vk>        gate on a split card  (GB-146-A-V2)
    GB-<n>-<Vk>            gate card             (GB-047-V2)

Mapping rule (GB-237; most specific wins):

    * a split suffix is kept and any gate suffix is stripped:
          GB-146-A-V2       -> GB-146-A
          GB-146-B-LOCKDIAG -> GB-146-B
    * with no split suffix the gate suffix is part of the card id:
          GB-047-V2         -> GB-047-V2
          GB-146            -> GB-146

A title with no GB id maps to nothing, and nothing is moved for it.

Why the old rule was wrong: the timer's regex was
    re.findall(r"\\bGB-(\\d+)(?:-(V\\d*))?\\b", title)
so `GB-146-A` collapsed to `GB-146` and PR #410 (the GB-146-A PR) moved the
umbrella card #345 instead of the split card #563.  A split card is the normal
shape of the M1 wall, so every split PR inherited the wrong parent.

Usage:
    python3 tools/board/pr_sync.py --digest-only   # print PR -> card, move nothing
    python3 tools/board/pr_sync.py --apply         # the timer's form (apply is the default)
    python3 tools/board/pr_sync.py                 # move mapped open-PR cards to In Review
"""
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request

API = os.environ.get("FORGEJO_URL", "http://192.168.0.234:3000").rstrip("/") + "/api/v1"
TOKEN = os.environ.get("FORGEJO_TOKEN", "")
REPO = os.environ.get("BOARD_REPO", "sarah/gaming-program")

# PRs live on GitHub, cards live on Forgejo.  The two are separate clients; the
# board helpers below keep board.py's API shapes.
GH_API = os.environ.get("GITHUB_API", "https://api.github.com").rstrip("/")
GH_TOKEN = os.environ.get("GITHUB_TOKEN", "")
GH_REPO = os.environ.get("GITHUB_REPO", "sarahkaylor/HobbyOS")

LANES = ["Backlog", "Ready", "In Progress", "In Review", "Merge Gate", "Done", "Icebox"]
LANE_COLORS = {
    "Backlog": "#888888",
    "Ready": "#0e8a16",
    "In Progress": "#fbca04",
    "In Review": "#ff9800",
    "Merge Gate": "#d93f0b",
    "Done": "#1d76db",
    "Icebox": "#c5def5",
}

# GB-<n> [ -<split letter> if it is a whole token ] [ -V<digits> if present ].
# The split letter must be followed by a non-alnum/end token boundary; that is
# what keeps `GB-146-B-LOCKDIAG` a split id (B) instead of a split named `L`.
_TICKET_RE = re.compile(
    r"\bGB-(\d+)(?:-([A-Z])(?=[^A-Za-z0-9]|$))?(?:-(V\d*))?(?![A-Za-z0-9])"
)


def ticket_of(title):
    """Return the card id a PR title names, or None.

    Pure function (the unit test calls it): no network, no environment.
    """
    m = _TICKET_RE.search(title or "")
    if not m:
        return None
    number, split, gate = m.group(1), m.group(2), m.group(3)
    if split:
        # A gate on a split card belongs to the split card: GB-146-A-V2 -> GB-146-A.
        return "GB-%s-%s" % (number, split)
    if gate:
        # No split: the gate id is itself a card: GB-047-V2 -> GB-047-V2.
        return "GB-%s-%s" % (number, gate)
    return "GB-%s" % number


# ---------------------------------------------------------------- HTTP, same shape as board.py
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
        raise SystemExit("HTTP %d %s %s: %s" % (e.code, method, url, msg[:400])) from None
    except urllib.error.URLError as e:
        raise SystemExit("network error %s %s: %s" % (method, url, e)) from None


def api_get(path, query=""):
    return _req("GET", path, None, query)


def api_post(path, data):
    return _req("POST", path, data)


def api_put(path, data):
    return _req("PUT", path, data)


def _gh_get(path, query=""):
    url = GH_API + path + query
    r = urllib.request.Request(url, method="GET")
    r.add_header("Authorization", "Bearer " + GH_TOKEN)
    r.add_header("Accept", "application/vnd.github+json")
    try:
        with urllib.request.urlopen(r, timeout=30) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        msg = e.read().decode(errors="replace")
        raise SystemExit("GitHub HTTP %d GET %s: %s" % (e.code, url, msg[:400])) from None
    except urllib.error.URLError as e:
        raise SystemExit("GitHub network error GET %s: %s" % (url, e)) from None


# ---------------------------------------------------------------- issues
def all_issues(state="all"):
    out, page = [], 1
    while True:
        chunk = api_get("/repos/%s/issues" % REPO,
                        "?state=%s&limit=50&page=%d" % (state, page)) or []
        out += chunk
        if len(chunk) < 50:
            return out
        page += 1


def issue_by_id(gid):
    """Issue whose title starts with the exact ticket id.

    `(?![\\w-])` is load-bearing: a bare boundary matches the hyphen in
    GB-146-A, so `GB-146` would resolve to the split card.  Ids are looked up
    newest-first, matching board.py.
    """
    gid = gid.upper().strip()
    for it in all_issues():
        if re.match(r"^%s(?![\w-])" % re.escape(gid), it["title"]):
            return it
    return None


def lane_of(issue):
    names = [l["name"] for l in (issue.get("labels") or [])]
    for n in LANES:
        if n in names:
            return n
    return None


def set_lane(issue, lane):
    if lane not in LANES:
        raise SystemExit("unknown lane %r" % (lane,))
    # Act on the current server state, like board.py (GB-074).
    cur_it = api_get("/repos/%s/issues/%d" % (REPO, issue["number"])) or {}
    cur = lane_of(cur_it)
    names = [l["name"] for l in (cur_it.get("labels") or []) if l["name"] not in LANES]
    names.append(lane)
    api_put("/repos/%s/issues/%d/labels" % (REPO, issue["number"]), {"labels": names})
    return cur


def comment(issue, text):
    api_post("/repos/%s/issues/%d/comments" % (REPO, issue["number"]), {"body": text})


# ---------------------------------------------------------------- pulls
def open_pull_requests():
    """Open PRs from GitHub (the board repo is Forgejo; PRs are not)."""
    out, page = [], 1
    while True:
        chunk = _gh_get("/repos/%s/pulls" % GH_REPO,
                        "?state=open&per_page=50&page=%d" % page) or []
        out += chunk
        if len(chunk) < 50:
            return out
        page += 1


def sync(digest_only=False):
    moved = 0
    for pr in open_pull_requests():
        title = pr.get("title") or ""
        gid = ticket_of(title)
        if not gid:
            print("PR #%s -> (no GB id; not moved): %s" % (pr["number"], title[:80]))
            continue
        it = issue_by_id(gid)
        if not it:
            print("PR #%s -> %s (no such card; not moved)" % (pr["number"], gid))
            continue
        head = (pr.get("head") or {}).get("sha", "")[:8]
        lane = lane_of(it)
        print("PR #%s head %s -> %s (#%d) lane=%s" % (pr["number"], head, gid, it["number"], lane))
        if digest_only:
            continue
        if lane != "In Review":
            set_lane(it, "In Review")
            comment(it, "pr_sync: PR #%s (head `%s`) -> `In Review`" % (pr["number"], head))
            moved += 1
    print("pr_sync: %d card(s) moved%s" % (moved, " (digest only)" if digest_only else ""))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description="map open PRs to board cards (§4.2 step 2)")
    # `--apply` is the deployed timer's spelling (hobbyos-pr-sync.service runs
    # `ExecStart=... --apply`).  Applying is already the default, so the flag is
    # a no-op alias: without it the versioned copy cannot replace the stopgap
    # the unit runs (it exits 2 on "unrecognized arguments: --apply").
    p.add_argument("--apply", action="store_true",
                   help="move mapped cards (the default; the timer passes this)")
    p.add_argument("--digest-only", action="store_true",
                   help="print the PR -> card mapping and move nothing")
    a = p.parse_args(argv)
    if not TOKEN:
        raise SystemExit("FORGEJO_TOKEN is not set")
    if not GH_TOKEN:
        raise SystemExit("GITHUB_TOKEN is not set")
    return sync(digest_only=a.digest_only)


if __name__ == "__main__":
    sys.exit(main())
