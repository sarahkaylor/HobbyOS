#!/usr/bin/env python3
"""Stale-claim check: In Progress tickets with no visible activity.

An `In Progress` ticket with no comment, no branch and no PR is
indistinguishable from a dropped one, and it blocks its dependents by
dependency -- GB-029 held GB-051 that way, and I only noticed by reading the
board by hand.  This is the automated version, in the spirit of GB-049.

REPORT ONLY.  A quiet ticket is not necessarily a wrong one: a worker may be
mid-edit for hours, and moving it because a clock passed is how a live ticket
gets demoted (I did that once to a claimed ticket).  The tool names them; a
human or the coordinator decides.

Exit 0 = nothing stale.  Exit 1 = at least one stale claim (printed).
Exit 2 = environment/lookup problem (never a silent pass).
"""
import json
import os
import sys
import urllib.request
from datetime import datetime, timezone

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOKEN = os.environ.get("FORGEJO_TOKEN", "")
LANES = ("Backlog", "Ready", "In Progress", "In Review", "Merge Gate", "Done", "Icebox")
DEFAULT_HOURS = 12.0


def api(path):
    req = urllib.request.Request(f"{BASE}{path}",
                                headers={"Authorization": f"token {TOKEN}"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def all_issues():
    out, page = [], 1
    while True:
        batch = api(f"/issues?state=all&limit=50&page={page}")
        if not batch:
            return out
        out.extend(batch)
        page += 1


def parse(ts):
    return datetime.fromisoformat(ts.replace("Z", "+00:00"))


def age_hours(ts, now):
    return (now - parse(ts)).total_seconds() / 3600.0


def classify(lane, comment_age_h, issue_age_h, hours):
    """Is this claim stale?  Isolated so --self-test can exercise it."""
    if lane != "In Progress":
        return False
    return min(comment_age_h, issue_age_h) > hours


def self_test():
    cases = [
        ("In Progress", 20.0, 30.0, True),
        ("In Progress", 1.0, 1.0, False),
        ("In Progress", 13.0, 40.0, True),
        ("Ready", 99.0, 99.0, False),
        ("In Review", 99.0, 99.0, False),
        ("In Progress", 11.9, 11.9, False),
    ]
    bad = 0
    for lane, c, i, want in cases:
        got = classify(lane, c, i, DEFAULT_HOURS)
        ok = got == want
        if not ok:
            bad += 1
        print(f"  [{'OK  ' if ok else 'FAIL'}] lane={lane:<11} comment_age={c:<5} "
              f"issue_age={i:<5} -> stale={got} (want {want})")
    print(f"self-test result: fails={bad}")
    return 0 if bad == 0 else 1


def main():
    if "--self-test" in sys.argv:
        return self_test()
    hours = DEFAULT_HOURS
    for a in sys.argv[1:]:
        if a.startswith("--hours="):
            hours = float(a.split("=", 1)[1])
    if not TOKEN:
        print("check_stale: FORGEJO_TOKEN not set (source ~/.hermes/.env)")
        return 2
    try:
        issues = all_issues()
    except Exception as e:                                    # noqa: BLE001
        print(f"check_stale: cannot read the board: {e}")
        return 2

    now = datetime.now(timezone.utc)
    stale = []
    for i in issues:
        labels = {l["name"] for l in i["labels"]}
        lane = next((x for x in LANES if x in labels), None)
        if lane != "In Progress":
            continue
        issue_age = age_hours(i["updated_at"], now)
        try:
            cs = api(f"/issues/{i['number']}/comments")
        except Exception:                                     # noqa: BLE001
            cs = []
        comment_age = age_hours(cs[-1]["created_at"], now) if cs else issue_age
        if classify(lane, comment_age, issue_age, hours):
            stale.append((i["number"], (i.get("title") or "")[:56],
                          comment_age, issue_age))

    print(f"in-progress tickets checked, threshold {hours:g}h")
    print(f"stale (no activity on either the issue or its last comment): {len(stale)}")
    for n, t, ca, ia in sorted(stale, key=lambda r: -min(r[2], r[3])):
        print(f"  #{n:<4} last comment {ca:5.1f}h ago, issue updated {ia:5.1f}h ago   {t}")
    return 0 if not stale else 1


if __name__ == "__main__":
    sys.exit(main())
