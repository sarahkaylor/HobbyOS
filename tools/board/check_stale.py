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


def last_activity(issue, comments, now):
    """Age (hours) of the last activity that is NOT the coordinator's own.

    Coordinator comments must not count: asking "is this stalled?" stamps the
    ticket and re-arms the very clock being measured.  GB-029 was correctly
    flagged as stalled, my status question made it look active, and it stayed
    In Progress with no work as a result.  Every coordinator comment carries
    "(Coordinator" in its footer, so that is the discriminator.

    Staleness is measured from COMMENTS ONLY (GB-098): every lane transition
    is itself a comment (board.py move posts "GB-xxx: lane A -> B"), so a
    comment's `created_at` captures both real activity AND every lane move.
    The issue object's raw `updated_at` is NEVER used -- a board-wide sync
    (import_seed / sync_gaming_md / a label sweep) bumps `updated_at` on every
    issue at once, so a fresh `updated_at` carries no information about a
    single card's activity and must not re-arm its stall clock.

    When a card has no human/agent comments at all, fall back to `created_at`
    (creation is itself an activity event that bulk writes never touch).
    """
    human = [c for c in comments
             if "Coordinator" not in (c.get("body") or "")]
    if human:
        return age_hours(human[-1]["created_at"], now)
    return age_hours(issue.get("created_at") or issue["updated_at"], now)


def classify(lane, comment_age_h, hours):
    """Is this claim stale?  Isolated so --self-test can exercise it.

    The ONLY clock is the last non-coordinator comment's age (which, because
    every lane move is a comment, is the lane-transition clock too).  The
    issue's `updated_at` deliberately plays no part -- see last_activity.
    """
    if lane != "In Progress":
        return False
    return comment_age_h > hours


def self_test():
    # Each case is (lane, comment_age_h, issue_age_h, expected_stale).
    # issue_age_h is only used to print the "issue clock" so a reader can SEE
    # that it no longer drives the verdict — the stale decision must be the
    # comment clock alone (GB-098).  The case that fails today:
    #   old comment + fresh updated_at must STILL be stale (it was not before),
    #   proving updated_at no longer erases a genuinely parked card.
    cases = [
        # (lane, comment_age, issue_age) -> stale
        ("In Progress", 20.0, 30.0, True),
        ("In Progress", 1.0, 1.0, False),
        ("In Progress", 13.0, 40.0, True),
        ("Ready", 99.0, 99.0, False),
        ("In Review", 99.0, 99.0, False),
        ("In Progress", 11.9, 11.9, False),
        # GB-098: old last comment, FRESH updated_at (a bulk touch) -> STALE.
        ("In Progress", 40.0, 0.10, True),
        # fresh comment always clears, whatever updated_at says.
        ("In Progress", 0.50, 99.0, False),
        ("In Progress", 0.50, 0.10, False),
    ]
    bad = 0
    for lane, c, i, want in cases:
        got = classify(lane, c, DEFAULT_HOURS)
        ok = got == want
        if not ok:
            bad += 1
        print(f"  [{'OK  ' if ok else 'FAIL'}] lane={lane:<11} comment_age={c:<5} "
              f"issue_age={i:<5} -> stale={got} (want {want})")
    print(f"self-test result: fails={bad}")
    return 0 if bad == 0 else 1


def fleet_live(hosts=("192.168.0.230", "192.168.0.231", "192.168.0.232")):
    """Count live `hermes` agents on the farms.  Returns (total, per_host).

    The stale-claim rule needs this: a quiet ticket while agents are running is
    a worker mid-edit; the same ticket with ZERO agents up is an abandoned
    claim.  Without the probe, the tool can only report age, and age alone must
    never move a ticket (that is how a live claim gets demoted).
    """
    import subprocess
    per, total = {}, 0
    for h in hosts:
        try:
            r = subprocess.run(
                ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=3", f"sarah@{h}",
                 "pgrep -c hermes || true"],
                capture_output=True, text=True, timeout=8)
            n = int((r.stdout or "0").strip() or 0)
        except Exception:                                     # noqa: BLE001
            n = -1                                            # unreachable != zero
        per[h] = n
        total += max(n, 0)
    return total, per


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
    checked = 0
    comment_ages = []          # distribution of last-comment age for In-Progress cards
    for i in issues:
        # Count only OPEN claims (RED-F1, GB-098): a closed card that still
        # carries a stale `In Progress` label is not a parked claim, and a
        # board-wide `state=all` read was inflating `checked`/`stale` past an
        # independent `state=open` read.  Closed scratch cards must never move
        # the stale number a farm decides on.
        if i.get("state") != "open":
            continue
        labels = {l["name"] for l in i["labels"]}
        lane = next((x for x in LANES if x in labels), None)
        if lane != "In Progress":
            continue
        checked += 1
        # Display-only issue clock: what the raw updated_at says.  It MUST NOT
        # drive the stale verdict (GB-098) -- a board-wide sync bumps it on
        # every issue at once.  We print it so 3 and 37 can never be confused.
        issue_age = age_hours(i["updated_at"], now)
        try:
            cs = api(f"/issues/{i['number']}/comments")
        except Exception:                                     # noqa: BLE001
            cs = []
        comment_age = last_activity(i, cs, now)
        comment_ages.append(comment_age)
        if classify(lane, comment_age, hours):
            stale.append((i["number"], (i.get("title") or "")[:56],
                          comment_age, issue_age))

    print(f"checked {checked} in-progress tickets, threshold {hours:g}h "
          f"(stale clock = last non-coordinator comment; updated_at NOT used)")
    print(f"stale (no comment or lane-move on the card past threshold): {len(stale)}")
    for n, t, ca, ia in sorted(stale, key=lambda r: -min(r[2], r[3])):
        print(f"  #{n:<4} last comment {ca:5.1f}h ago, issue updated {ia:5.1f}h ago   {t}")
    if comment_ages:
        import statistics
        comment_ages.sort()
        print(f"comment-age distribution across the {len(comment_ages)} in-progress cards: "
              f"min={comment_ages[0]:.1f}h "
              f"median={statistics.median(comment_ages):.1f}h "
              f"max={comment_ages[-1]:.1f}h")
    if "--fleet" in sys.argv:
        total, per = fleet_live()
        unreachable = [h for h, v in per.items() if v < 0]
        print(f"fleet live agents: {total}  "
              + " ".join(f"{h.split('.')[-1]}={v}" for h, v in per.items()))
        if unreachable:
            print(f"  WARNING: {len(unreachable)} host(s) unreachable — "
                  "unreachable is NOT the same as zero agents")
        if total == 0 and not unreachable:
            print("  => every stale claim above is abandoned (zero agents everywhere)")
        elif total > 0:
            print("  => agents are running; a stale ticket may be mid-edit. "
                  "Do NOT move it on age alone.")
    return 0 if not stale else 1


if __name__ == "__main__":
    sys.exit(main())
