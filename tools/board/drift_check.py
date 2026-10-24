#!/usr/bin/env python3
"""drift_check.py - find board tickets whose work has already landed.

Coordinator tool (GB-049 territory).  Every night several tickets sit in a review
lane with their PR already merged, and I have moved them by hand repeatedly
(GB-025, GB-027, GB-028, GB-030, GB-042, GB-048, GB-050, GB-052, GB-064).  A
merged ticket in a review lane tells the next reader that work is pending when it
is finished -- the same class of lie as an instrument that reports the wrong
state, so it is worth a tool rather than a habit.

How it decides, and what it refuses to decide:

  * a merged PR whose title or body names `GB-<id>` is evidence that work landed;
  * a ticket for that id that is still OPEN and sitting in a review lane
    (In Review / Merge Gate) is therefore drift;
  * it reports and does NOT move anything.  A PR that references a ticket does not
    prove the ticket's DoD is met -- GB-033's contract test is merged while its
    milestone is not, and GB-048 needed the cron *observed running*, not merely
    present.  Closing is a judgement; this only finds the candidates.

Usage:  python3 tools/board/drift_check.py [--json]
Env:    GITHUB_TOKEN (for the PR list), FORGEJO_TOKEN (for the board)
"""
import json
import os
import re
import sys
import urllib.request

GITHUB = "https://api.github.com/repos/sarahkaylor/HobbyOS"
BOARD = os.environ.get("FORGEJO_BASE", "http://192.168.0.234:3000") + "/api/v1/repos/sarah/gaming-program"
REVIEW_LANES = {"In Review", "Merge Gate"}
GB_RE = re.compile(r"\bGB-(\d{3})\b")


def api(url, token):
    req = urllib.request.Request(url, headers={
        "Authorization": "token " + token,
        "Accept": "application/vnd.github+json",
        "User-Agent": "drift-check",
    })
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def board_api(path, token):
    req = urllib.request.Request(BOARD + path, headers={
        "Authorization": "token " + token,
        "User-Agent": "drift-check",
    })
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def landed_ids(prs, use_bodies=False):
    """{GB-id: [PR numbers]} for merged PRs.

    Title-only by default, and that is a deliberate precision choice: a PR *body*
    mentions other tickets freely (PR #25's body references GB-016/017/019/020,
    and the first run of this tool attributed GB-007 to it purely from prose).
    A title is a deliberate claim about what the PR is for.

    Pure function of the PR list so it can be tested without the network.
    """
    out = {}
    for pr in prs:
        if not pr.get("merged_at"):
            continue
        text = pr.get("title") or ""
        if use_bodies:
            text += " " + (pr.get("body") or "")
        for m in GB_RE.findall(text):
            out.setdefault("GB-" + m, []).append(pr["number"])
    return out


def merged_pr_ids(gh_token, use_bodies=False):
    prs = []
    for page in (1, 2, 3):
        batch = api(f"{GITHUB}/pulls?state=closed&per_page=100&page={page}", gh_token)
        if not batch:
            break
        prs.extend(batch)
    return landed_ids(prs, use_bodies)


def main():
    gh = os.environ.get("GITHUB_TOKEN")
    fj = os.environ.get("FORGEJO_TOKEN")
    if not gh or not fj:
        raise SystemExit("GITHUB_TOKEN and FORGEJO_TOKEN must be set (source ~/.hermes/.env)")

    landed = merged_pr_ids(gh, use_bodies="--bodies" in sys.argv)
    issues = board_api("/issues?state=open&limit=200", fj)

    drift, gated = [], []
    for it in issues:
        title = it.get("title") or ""
        m = GB_RE.search(title)
        if not m:
            continue
        gid = "GB-" + m.group(1)
        lanes = {l["name"] for l in it.get("labels", [])}
        if not (lanes & REVIEW_LANES):
            continue
        prs = landed.get(gid)
        if prs:
            drift.append({"id": gid, "issue": it["number"], "lane": sorted(lanes & REVIEW_LANES),
                          "prs": sorted(set(prs)), "title": title[:70]})
        else:
            gated.append({"id": gid, "issue": it["number"], "title": title[:70]})

    if "--json" in sys.argv:
        print(json.dumps({"drift": drift, "in_review_no_merged_pr": gated}, indent=2))
    else:
        print(f"drift_check: {len(drift)} ticket(s) in a review lane whose PR is already merged")
        for d in drift:
            print(f"  {d['id']}  #{d['issue']}  lane={'/'.join(d['lane'])}  PR{','.join(map(str, d['prs']))}  {d['title']}")
        print(f"drift_check: {len(gated)} ticket(s) in a review lane with no merged PR (nothing to conclude)")
        for g in gated:
            print(f"  {g['id']}  #{g['issue']}  {g['title']}")
        print("drift_check: report only -- a referencing PR does not prove the DoD is met.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
