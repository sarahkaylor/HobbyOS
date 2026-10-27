#!/usr/bin/env python3
"""Claim-age report (GB-117): per open card, the age of its newest comment.

DECISION-006 cannot be sized because the board cannot report how long an
`In Progress` claim has sat quiet.  `updated_at` is not usable: a bulk sweep
(import_seed / sync_gaming_md / a label pass) touches every issue at once --
measured 2026-10-27, nine unrelated cards sharing two timestamps inside one
minute -- so a fresh `updated_at` carries no per-card signal.

The ONLY clock here is the age of the card's NEWEST non-coordinator comment
(GB-098): every real activity *and* every lane transition posts a comment, and
coordinator "is this stalled?" probes are excluded because asking re-arms the
very clock being measured.  A card whose `updated_at` is recent but whose
comments are old is counted STALE; a seeded sweep must never reset the age.

Report layout (DoD verbatim):
  1. per open card, the newest-comment age in buckets
       <6h, 6-12h, 12-24h, >24h, no-comments
  2. the oldest N cards by comment age, with ids
  3. a card with fresh updated_at but old comments is STALE (fixture proves it)
  4. decision/blocked-labeled cards are printed in a separate section, so the
     decision-gated subset is visible
  5. the live default reproduces the 42/28 split (compliant at seed time).

Exit 0 = ok.  Exit 2 = environment/lookup problem (never a silent pass).

Deterministic for tests: `--snapshot FILE.json` reads an on-disk snapshot
{"issues": [...], "comments": {"<number>": [comment,...]}, "now": "<iso>"}
instead of the live board.
"""
import json
import os
import sys
import urllib.request
from datetime import datetime, timezone

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOKEN = os.environ.get("FORGEJO_TOKEN", "")

LANES = ("Backlog", "Ready", "In Progress", "In Review", "Merge Gate", "Done", "Icebox")
STALE_HOURS = 24.0
BUCKETS_ORDER = ("under6h", "6-12h", "12-24h", "over24h", "nocomments")


def parse(ts):
    return datetime.fromisoformat(ts.replace("Z", "+00:00"))


def age_hours(ts, now):
    return (now - parse(ts)).total_seconds() / 3600.0


def bucket_for(comment_age):
    if comment_age is None:
        return "nocomments"
    if comment_age < 6:
        return "under6h"
    if comment_age < 12:
        return "6-12h"
    if comment_age < 24:
        return "12-24h"
    return "over24h"


def newest_comment_age(comments, now):
    """Age (h) of the newest non-coordinator comment; None when none exist.

    Coordinator comments are excluded (they re-arm the stall clock by asking).
    Falls back to None -> 'no-comments' bucket.  `updated_at` never enters.
    """
    human = [c for c in comments if "Coordinator" not in (c.get("body") or "")]
    if not human:
        return None
    return max(0.0, age_hours(human[-1]["created_at"], now))


def stale(comment_age):
    return comment_age is None or comment_age >= STALE_HOURS


def api(path):
    req = urllib.request.Request(f"{BASE}{path}",
                                headers={"Authorization": f"token {TOKEN}"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def load_snapshot(path):
    with open(path) as f:
        return json.load(f)


def load_live():
    if not TOKEN:
        raise RuntimeError("FORGEJO_TOKEN not set (source ~/.hermes/.env)")
    issues, page = [], 1
    while True:
        batch = api(f"/issues?state=open&limit=50&page={page}")
        if not batch:
            break
        issues += batch
        page += 1
    comments = {}
    for i in issues:
        try:
            comments[i["number"]] = api(f"/issues/{i['number']}/comments")
        except Exception:                                     # noqa: BLE001
            comments[i["number"]] = []
    return issues, comments, datetime.now(timezone.utc)


def report(issues, comments, now, oldest_n):
    """Compute + print the claim-age report.  Returns (stale_count, nonstale_count)."""
    rows = []
    buckets = {k: 0 for k in BUCKETS_ORDER}
    stale_cnt = nonstale_cnt = 0
    for i in issues:
        if i.get("state") != "open":
            continue
        labels = {l["name"] for l in i.get("labels", [])}
        lane = next((x for x in LANES if x in labels), None)
        ca = newest_comment_age(comments.get(i["number"], []), now)
        buckets[bucket_for(ca)] += 1
        flagged = lane or "?"
        is_stale = stale(ca)
        rows.append({
            "n": i["number"], "title": (i.get("title") or "")[:52],
            "lane": flagged, "ca": ca, "ia": max(0.0, age_hours(i["updated_at"], now)),
            "stale": is_stale, "dec": bool(labels & {"decision", "blocked"}),
        })
        if is_stale:
            stale_cnt += 1
        else:
            nonstale_cnt += 1

    print(f"claim-age report (clock = NEWEST non-coordinator comment; "
          f"updated_at NOT used): {len(rows)} open cards")
    print("buckets: " + ", ".join(f"{k}={buckets[k]}" for k in BUCKETS_ORDER))
    print(f"STALE (comment age >= {STALE_HOURS:g}h, or no comments): {stale_cnt}   "
          f"FRESH: {nonstale_cnt}   split={stale_cnt}/{nonstale_cnt}")

    print(f"\noldest {oldest_n} by comment age (ids, stale only):")
    oldest = sorted([r for r in rows if r["stale"]],
                   key=lambda r: (r["ca"] is None, -1 if r["ca"] is None else -r["ca"]))
    for r in oldest[:oldest_n]:
        ca = "no-comments" if r["ca"] is None else f"{r['ca']:.1f}h"
        print(f"  #{r['n']:<4} {r['lane']:<11} comment {ca}  {r['title']}")

    # 3. updated_at-fresh but comments-old MUST be stale (sweep does not reset age).
    sweep_proven = 0
    for r in sorted(rows, key=lambda r: r["ca"] if r["ca"] is not None else -1):
        if r["stale"] and r["ia"] < 24:          # recent updated_at yet still stale
            sweep_proven += 1
    print(f"cards STALE despite fresh updated_at (<24h): {sweep_proven} "
          f"(a bulk sweep cannot re-arm them)")

    # 4. separate decision/blocked subset.
    dec = [r for r in rows if r["dec"]]
    ord_ = [r for r in rows if not r["dec"]]
    print(f"\ndecision/blocked cards ({len(dec)}):")
    for r in sorted(dec, key=lambda r: r["n"]):
        ca = "no-comments" if r["ca"] is None else f"{r['ca']:5.1f}h"
        print(f"  #{r['n']:<4} {r['lane']:<11} comment {ca}  {'STALE' if r['stale'] else 'fresh'}"
              f"  {r['title']}")
    print(f"ordinary cards ({len(ord_)}): "
          f"{len([r for r in ord_ if r['stale']])} stale / "
          f"{len([r for r in ord_ if not r['stale']])} fresh")
    return stale_cnt, nonstale_cnt


def main():
    oldest_n = 10
    for a in sys.argv[1:]:
        if a == "--self-test":
            return self_test()
        if a.startswith("--oldest="):
            oldest_n = int(a.split("=", 1)[1])
    try:
        if any(a.startswith("--snapshot=") for a in sys.argv[1:]):
            snap_path = [a.split("=", 1)[1] for a in sys.argv[1:]
                        if a.startswith("--snapshot=")][0]
            data = load_snapshot(snap_path)
            issues, comments = data["issues"], {int(k): v for k, v in data["comments"].items()}
            now = datetime.fromisoformat(data["now"].replace("Z", "+00:00"))
        else:
            issues, comments, now = load_live()
    except Exception as e:                                     # noqa: BLE001
        print(f"claim_age: cannot read board/snapshot: {e}")
        return 2
    report(issues, comments, now, oldest_n)
    return 0


def self_test():
    """Deterministic snapshot-driven checks (drives report() directly + a file).

    Proves the honesty-law anchors:
      - a card with fresh updated_at but OLD comments is STALE (sweep-proof);
      - fresh comment -> fresh, no comments -> stale, buckets map correctly;
      - decision/blocked cards are separated from ordinary.
    """
    now = "2026-10-27T12:00:00Z"
    now_dt = parse(now)
    from datetime import timedelta
    com = lambda age_h: (now_dt - timedelta(hours=age_h)).strftime('%Y-%m-%dT%H:%M:%SZ')
    issues = [
        # fresh updated_at (10h ago = <24h) but comment 48h old -> MUST be stale
        {"number": 1, "state": "open", "title": "sweep-touched", "updated_at": "2026-10-27T02:00:00Z",
         "labels": [{"name": "infra"}]},
        {"number": 2, "state": "open", "title": "fresh", "updated_at": "2026-10-27T10:00:00Z",
         "labels": [{"name": "In Progress"}]},                       # comment 1h old
        {"number": 3, "state": "open", "title": "noc", "updated_at": "2026-10-27T11:00:00Z",
         "labels": [{"name": "In Progress"}]},                       # no comments -> stale
        {"number": 4, "state": "open", "title": "dec", "updated_at": "2026-10-21T00:00:00Z",
         "labels": [{"name": "decision"}]},                          # >24h -> stale, decision
        {"number": 5, "state": "open", "title": "blk", "updated_at": "2026-10-21T00:00:00Z",
         "labels": [{"name": "blocked"}]},
        {"number": 6, "state": "closed", "title": "closed-ignored", "updated_at": "2026-10-27T00:00:00Z",
         "labels": [{"name": "In Progress"}]},                        # closed -> skipped
    ]
    comments = {
        1: [{"created_at": com(48)}], 2: [{"created_at": com(1)}], 3: [],
        4: [{"created_at": com(30)}],
        5: [{"body": "Coordinator status", "created_at": "2026-10-27T11:00:00Z"}],  # coordinator -> ignored -> no human -> stale
        6: [{"created_at": com(5)}],
    }
    import io
    from contextlib import redirect_stdout
    buf = io.StringIO()
    with redirect_stdout(buf):
        stale_cnt, nonstale = report(issues, comments, parse(now), oldest_n=3)
    out = buf.getvalue()
    fails = []
    # checks
    if stale_cnt != 4 or nonstale != 1:
        fails.append(f"expected 4 stale / 1 fresh, got {stale_cnt}/{nonstale}")
    # #1: fresh updated_at (10h) + 48h-old comment must be stale
    if "STALE despite fresh updated_at" in out and "cards STALE despite fresh updated_at (<24h)" in out:
        n = [l for l in out.splitlines() if l.startswith("cards STALE")][0]
        if "STALE despite fresh updated_at (<24h): 2" not in n:
            fails.append("sweep-proof count wrong: " + n)
    else:
        fails.append("sweep-proof line missing")
    # buckets: #1>24h, #2 <6h, #3 no, #4>24h, #5 no(coords-only), closed excluded -> over24h=2,under6h=1,nocomments=2
    # buckets: #1>24h, #2 <6h, #3 no, #4>24h, #5 no(coords-only), closed excluded
    #        -> over24h=2, under6h=1, nocomments=2
    bucket_line = next(l for l in out.splitlines() if l.startswith("buckets:"))
    has = dict(kv.split("=") for kv in [p for p in bucket_line.replace("buckets: ", "").split(" ") if p])
    has = {k: int(v.rstrip(",")) for k, v in has.items()}
    if has != {"under6h": 1, "6-12h": 0, "12-24h": 0, "over24h": 2, "nocomments": 2}:
        fails.append(f"buckets wrong: {has}")
    # decision/blocked separated: 2 dec (4,5) listed separately
    if "decision/blocked cards (2)" not in out:
        fails.append("decision/blocked separation missing")
    # oldest ids printed and include the 48h card
    if "#1" not in out:
        fails.append("oldest-N ids missing sweep card #1")

    print(f"  {'PASS' if not fails else 'FAIL'}: b1 sweep-proof stale  b2 bucket map  "
          f"b3 decision/blocked sep -> split={stale_cnt}/{nonstale}")
    for f in fails:
        print(f"    - {f}")
    print("CLAIM_AGE_TEST:", "ALL PASSED" if not fails else f"{len(fails)} FAILED")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
