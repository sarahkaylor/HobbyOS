#!/usr/bin/env python3
"""Verdict propagation check (§4.2 step 5).

A `GB-nnn-V` verification ticket posts a GREEN/RED verdict that *governs the
parent's lane*:

    GREEN -> the implementation ticket goes to Merge Gate
    RED   -> the implementation ticket goes back to In Progress

Today that propagation is done by hand, and it was missed twice in one loop:
GB-010 and GB-070 both sat In Review with a RED verdict already posted against
them, reading as "awaiting review" when a gate had in fact been run and failed.
A lane that lies about the gate is the same defect class as an instrument that
lies about the machine, so this check is a tool, not a habit.

Exit 0 = every parent's lane agrees with its verdict.
Exit 1 = at least one mismatch (printed, with the verdict line).
Exit 2 = environment/lookup problem (never a silent pass).
"""
import json
import os
import re
import sys
import urllib.request

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOKEN = os.environ.get("FORGEJO_TOKEN", "")
LANE_LABELS = ("Backlog", "Ready", "In Progress", "In Review", "Merge Gate",
               "Done", "Icebox")


def api(path):
    url = f"{BASE}{path}"
    req = urllib.request.Request(url, headers={"Authorization": f"token {TOKEN}"})
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


def lane_of(issue):
    names = {l["name"] for l in issue["labels"]}
    return next((x for x in LANE_LABELS if x in names), None)


def base_id(title):
    return title.split()[0] if title else ""


def verdict_of(number):
    """Newest GREEN/RED verdict posted on a verification ticket."""
    cs = api(f"/issues/{number}/comments")
    for c in reversed(cs):
        body = (c.get("body") or "").replace("\r", "")
        m = re.search(r"VERDICT[^\n]{0,40}?\b(GREEN|RED)\b", body, re.I)
        if m:
            line = next((ln.strip() for ln in body.split("\n")
                         if re.search(r"VERDICT", ln, re.I)), "")
            return m.group(1).upper(), line[:160]
    return None, None


def classify(verdict, parent_lane):
    """The rule, isolated so a self-test can exercise it without the API."""
    if verdict == "GREEN":
        return parent_lane in ("Merge Gate", "Done"), "Merge Gate"
    return parent_lane in ("In Progress", "Backlog", "Ready"), "In Progress"


def self_test():
    cases = [
        ("RED", "In Review", False),
        ("RED", "In Progress", True),
        ("GREEN", "In Review", False),
        ("GREEN", "Merge Gate", True),
        ("GREEN", "In Progress", False),
        ("RED", "Done", False),
    ]
    bad = 0
    for verdict, lane, want_ok in cases:
        ok, want = classify(verdict, lane)
        mark = "OK  " if ok == want_ok else "FAIL"
        if ok != want_ok:
            bad += 1
        print(f"  [{mark}] verdict={verdict:<5} parent in {lane!r:<13} "
              f"-> ok={ok} (expected {want_ok}, want-lane {want!r})")
    print(f"self-test result: fails={bad}")
    return 0 if bad == 0 else 1


def main():
    if "--self-test" in sys.argv:
        return self_test()
    if not TOKEN:
        print("check_verdicts: FORGEJO_TOKEN not set (source ~/.hermes/.env)")
        return 2
    try:
        issues = all_issues()
    except Exception as e:                                    # noqa: BLE001
        print(f"check_verdicts: cannot read the board: {e}")
        return 2

    by_id = {}
    for i in issues:
        by_id.setdefault(base_id(i.get("title") or ""), i)

    checked = mismatched = 0
    for vid, vi in sorted(by_id.items()):
        if not vid.endswith("-V"):
            continue
        parent_id = vid[:-2]
        parent = by_id.get(parent_id)
        if parent is None:
            print(f"  [{vid}] no parent ticket {parent_id} on the board")
            mismatched += 1
            continue
        verdict, line = verdict_of(vi["number"])
        if verdict is None:
            continue
        checked += 1
        pl = lane_of(parent)
        ok, want = classify(verdict, pl)
        if not ok:
            mismatched += 1
            print(f"  MISMATCH {vid}#{vi['number']} = {verdict} "
                  f"but {parent_id}#{parent['number']} is in {pl!r} (want {want!r})")
            print(f"           verdict line: {line}")

    print(f"verdicts checked: {checked}, mismatched parents: {mismatched}")
    return 0 if mismatched == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
