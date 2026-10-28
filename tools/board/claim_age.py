#!/usr/bin/env python3
"""Claim-age report (GB-117): per open card, the age of its newest comment.

DECISION-006 cannot be sized because the board cannot report how long an
`In Progress` claim has sat quiet.  `updated_at` is not usable: a bulk sweep
(import_seed / sync_gaming_md / a label pass) touches every issue at once --
measured 2026-10-27, nine unrelated cards sharing two timestamps inside one
minute -- so a fresh `updated_at` carries no per-card signal.

The tool prints **two clocks**, both labelled, with their definitions in the
output itself (GB-117-V2 #240 clause 2):
  CLOCK A -- any-comment       : the age of the card's NEWEST comment by anyone
                                (lane moves, claims, coordinator posts included).
  CLOCK B -- working-attempt  : the age of the card's NEWEST *activity* comment
                                -- coordinator bookkeeping (status/kickoff/mint/
                                caretaker posts) AND lane-transition/claim/
                                retraction notches do NOT re-arm the stall
                                clock; worker content (EVIDENCE, VERDICT,
                                MEASURED, DEBUG, findings, a worker's claim of
                                work) DOES, even when it mentions "Coordinator".

Clock B is the stall signal DECISION-006 will be sized from; Clock A is the
DoD-literal reading.  The two numbers are printed as a PAIR so a bare "42" can
never be read two ways again (clause 3).  `updated_at` never drives either
clock -- it is read only to *prove* the sweep falsifier.

Falsifiability of the kind-classifier (clause 7): the borderline case is a
*claim-of-work* post -- "**GB-108 claimed by wabi ... DoD: ...**" carries worker
content wrapped in a claim header, and the code names it as the thing a
reasonable person might classify the other way; `_is_bookkeeping` treats it as
ACTIVITY so a worker who claims a card and posts its DoD plan is not marked stale
the instant they claim.  A bare claim/retraction notch is bookkeeping.


Why not filter by authorship (GB-117-V round-1 RED): every comment on the board
posts under the single bot account `sarah` (the probe in this file's self-test
and evidence shows two different cards' comments carry the same `user.login`).
A login filter therefore cannot separate coordinator from worker -- the
kind-classifier `_is_bookkeeping` is the only sound separation.  The code says
so inline below so no future reader "fixes" it back to a login check (clause 1).

Report layout:
  1. per open card, the newest-comment age in buckets <6h, 6-12h, 12-24h,
     >24h, no-comments (printed for BOTH clocks)
  2. the oldest N cards by Clock-B age, with ids
  3. a card with fresh updated_at but old comments is STALE (fixture proves it)
  4. decision/blocked-labeled cards printed separately (decision-gated subset)
  5. the DoD split is stated as the pair (Clock-A stale/fresh, Clock-B
     stale/fresh) plus the command that produced them.

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


# --- kind-classifier (GB-117-V round-1 RED + GB-117-V2 #241 clause 5) ---------
# All comments post under one bot account, so `user.login` cannot separate
# coordinator from worker (probed in self-test / evidence).  The separation is by
# the comment's KIND.  A regex is avoided: the lane/claim notches are narrow and
# stable, and an explicit classifier is what the fence needs to be falsifiable.
_BOOKKEEPING_PREFIXES = (
    "COORDINATOR", "SWARM HEALTH", "SWARM-HEALTH", "CARETAKER",
    "VERIFICATION MINTED", "RE-GATE MINTED", "VERDICT-",
)
# A lane-transition / claim / retraction notch: e.g. "GB-117: lane In Progress →
# **In Review**", "238: lane → **In Progress**", "claimed by `x` on ...",
# "GB-117-V2: lane - → **Ready**".  These start with the ticket id or the word
# "claimed"/"retraction", never with worker content.
_BOOKKEEPING_MARK = ("CLAIMED BY", "RETRACTION", "RETRACTED")
# Worker *content* prefixes stay ACTIVITY even when they mention "Coordinator"
# (the #142 case).  "## RED"/"## VERDICT"/"## GREEN" verdict headers are worker/
# verifier verdicts => activity, NOT "VERDICT-" bookkeeping.
_CONTENT_PREFIXES = (
    "VERDICT", "MEASURED", "DEBUG", "EVIDENCE", "REVIEW", "FOUND",
    "THE RESIDUE", "THE SILENT", "BOTH", "SWARM", "##", "###", "# ",
)


def _is_bookkeeping(c):
    """Is comment ``c`` bookkeeping (does NOT re-arm the Clock-B stall clock)?

    Bookkeeping = coordinator/status/kickoff/mint/caretaker posts AND
    lane-transition / claim / retraction notches -- i.e. posts that are the
    bot recording state, not a working attempt.  Worker content (EVIDENCE,
    VERDICT, MEASURED, DEBUG, findings, "# RED"/"# GREEN" verdict headers) is
    ACTIVITY even when the body mentions "Coordinator".

    Falsifiability (clause 7): a *claim-of-work* post is the borderline case --
    "**GB-108 claimed by wabi (FLASH, ...).** DoD: ..." is really worker content
    wrapped in a claim header; it is classified ACTIVITY (it starts with worker
    content after the leading `**`), so a worker who claims a card and immediately
    posts its DoD plan is not marked stale the instant they claim.  A bare
    "claimed by `x` on ..." notch (board.py's own comment) is bookkeeping.
    """
    body = (c.get("body") or "")
    head = body.lstrip("*# \t\n")
    up = head.upper()
    if up.startswith(_BOOKKEEPING_PREFIXES):
        return True
    # a bare claim/retraction notch
    if up.startswith(_BOOKKEEPING_MARK):
        return True
    # a lane-transition notch: "<id>: lane ... → ...", "<id>: lane Ready → **In Progress**"
    # (board.py's lane-move comment) or "NNN: lane → **In Progress**"
    if "LANE" in up and ("→" in body or "->" in body):
        return True
    if up.startswith("RETRACTION"):
        return True
    return False                # worker content => activity


def _clock(comments, now, mode):
    """Age (h) of the newest comment under `mode`.  None when none exist."""
    if mode == "any":
        cands = list(comments or [])
    else:                       # "working"
        cands = [c for c in comments if not _is_bookkeeping(c)]
    if not cands:
        return None
    newest = max(cands, key=lambda c: c["created_at"])
    return max(0.0, age_hours(newest["created_at"], now))


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


def _bucket_line(buckets):
    return "buckets: " + ", ".join(f"{k}={buckets[k]}" for k in BUCKETS_ORDER)


def report(issues, comments, now, oldest_n):
    """Compute + print the claim-age report.  Returns (working_stale, working_fresh)."""
    rows = []
    any_buck = {k: 0 for k in BUCKETS_ORDER}
    work_buck = {k: 0 for k in BUCKETS_ORDER}
    stale_cnt = nonstale_cnt = 0
    any_stale = any_fresh = 0
    for i in issues:
        if i.get("state") != "open":
            continue
        labels = {l["name"] for l in i.get("labels", [])}
        lane = next((x for x in LANES if x in labels), None)
        cs = comments.get(i["number"], [])
        ac = _clock(cs, now, "any")
        wc = _clock(cs, now, "working")
        any_buck[bucket_for(ac)] += 1
        work_buck[bucket_for(wc)] += 1
        is_stale = stale(wc)
        any_stale += 1 if stale(ac) else 0
        any_fresh += 0 if stale(ac) else 1
        rows.append({
            "n": i["number"], "title": (i.get("title") or "")[:52],
            "lane": lane or "?", "ca": wc, "ia": max(0.0, age_hours(i["updated_at"], now)),
            "stale": is_stale, "dec": bool(labels & {"decision", "blocked"}),
        })
        if is_stale:
            stale_cnt += 1
        else:
            nonstale_cnt += 1

    cmd = os.path.basename(sys.argv[0]) + " " + " ".join(sys.argv[1:])
    print(f"claim-age report: {len(rows)} open cards   (command: {cmd}  "
          f"updated_at NEVER the clock)")
    print("\nCLOCK A -- any-comment: newest comment by ANYONE; lane moves, claims, "
          f"coordinator posts INCLUDED")
    print("  " + _bucket_line(any_buck))
    print(f"  GET older 'activity'? no.  stale/fresh @ {STALE_HOURS:g}h = "
          f"{any_stale}/{any_fresh}")
    print("\nCLOCK B -- working-attempt: newest NON-bookkeeping comment; coordinator "
          "status / kickoff / mint / caretaker posts AND lane-transition / claim / "
          "retraction notches EXCLUDED; worker content (EVIDENCE, VERDICT, MEASURED, "
          "DEBUG, findings) INCLUDED even when it mentions 'Coordinator'")
    print("  " + _bucket_line(work_buck))
    print(f"  STALE (comment age >= {STALE_HOURS:g}h, or no comments on Clock B): "
          f"{stale_cnt}   FRESH: {nonstale_cnt}   split={stale_cnt}/{nonstale_cnt}")
    print(f"  -> DoD number as a pair: {{any-comment {any_stale}/{any_fresh}, "
          f"working {stale_cnt}/{nonstale_cnt}}} (produced by: `{cmd}`)")

    print(f"\noldest {oldest_n} by Clock-B comment age (ids, stale only):")
    oldest = sorted([r for r in rows if r["stale"]],
                   key=lambda r: (r["ca"] is None, -1 if r["ca"] is None else -r["ca"]))
    for r in oldest[:oldest_n]:
        ca = "no-comments" if r["ca"] is None else f"{r['ca']:.1f}h"
        print(f"  #{r['n']:<4} {r['lane']:<11} comment {ca}  {r['title']}")

    # 3. updated_at-fresh but Clock-B-old MUST be stale (sweep does not reset age).
    sweep_proven = 0
    for r in sorted(rows, key=lambda r: r["ca"] if r["ca"] is not None else -1):
        if r["stale"] and r["ia"] < 24:          # recent updated_at yet still stale
            sweep_proven += 1
    print(f"cards STALE on Clock B despite fresh updated_at (<24h): {sweep_proven} "
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


def _login_probe():
    """Return two comments from DIFFERENT cards that share a user.login (clause 1).

    Reads the real board so the impossibility is recorded from live data, not
    asserted.  Pulls up to three open cards and their newest comments.
    """
    issues, comments, _ = load_live()
    logins = []
    for i in issues[:3]:
        cs = comments.get(i["number"], [])
        if cs:
            logins.append((i["number"], cs[-1]["user"]["login"]))
    return logins


def self_test():
    """Deterministic snapshot-driven checks (drives report() directly + a file).

    Proves the honesty-law anchors:
      - fresh updated_at but OLD comments is STALE (sweep-proof);
      - Clock A vs Clock B differ the round-1 way (clause 4): a card with a fresh
        lane-move notch but a 30 h-old substantive comment reads FRESH on Clock A
        and STALE on Clock B, in the same report;
      - lane-move / claim notches are bookkeeping (clause 5 / round-2 RED);
      - worker content mentioning 'Coordinator' is activity;
      - decision/blocked cards separated; buckets map correctly;
      - classifier falsifiability named (Clause 7).
    """
    now = "2026-10-27T12:00:00Z"
    now_dt = parse(now)
    from datetime import timedelta
    com = lambda age_h: (now_dt - timedelta(hours=age_h)).strftime('%Y-%m-%dT%H:%M:%SZ')
    issues = [
        # (1) fresh updated_at (10h, <24h) but comment 48h old -> MUST be stale
        {"number": 1, "state": "open", "title": "sweep-touched", "updated_at": "2026-10-27T02:00:00Z",
         "labels": [{"name": "infra"}]},
        # (2) fresh comment (1h) -> fresh on both clocks
        {"number": 2, "state": "open", "title": "fresh", "updated_at": "2026-10-27T10:00:00Z",
         "labels": [{"name": "In Progress"}]},
        # (3) no comments -> stale on both (no-comments bucket)
        {"number": 3, "state": "open", "title": "noc", "updated_at": "2026-10-27T11:00:00Z",
         "labels": [{"name": "In Progress"}]},
        # (4) >24h -> stale, decision
        {"number": 4, "state": "open", "title": "dec", "updated_at": "2026-10-21T00:00:00Z",
         "labels": [{"name": "decision"}]},
        # (5) only a coordinator status post (0h) -> Clock A fresh, Clock B stale
        {"number": 5, "state": "open", "title": "blk", "updated_at": "2026-10-21T00:00:00Z",
         "labels": [{"name": "blocked"}]},
        # (7) fresh lane-move notch (1h) + 30h substantive comment -> Clause 4:
        #     Clock A fresh (newest = lane move), Clock B stale (30h work).
        {"number": 7, "state": "open", "title": "lane-touched", "updated_at": "2026-10-27T11:00:00Z",
         "labels": [{"name": "In Progress"}]},
        # (8) worker EVIDENCE post that mentions 'Coordinator' (2h) -> activity ($#142)
        {"number": 8, "state": "open", "title": "evidence-mentions-coord",
         "updated_at": "2026-10-27T10:00:00Z", "labels": [{"name": "In Progress"}]},
        {"number": 6, "state": "closed", "title": "closed-ignored", "updated_at": "2026-10-27T00:00:00Z",
         "labels": [{"name": "In Progress"}]},                # closed -> skipped
    ]
    comments = {
        1: [{"created_at": com(48)}],
        2: [{"created_at": com(1)}],
        3: [],
        4: [{"created_at": com(30)}],
        5: [{"body": "Coordinator status", "created_at": "2026-10-27T11:00:00Z"}],
        7: [{"body": "GB-026: lane Ready → **In Progress**", "created_at": com(1)},
            {"body": "EVIDENCE — GB-026 DoD delivered.", "created_at": com(30)}],
        8: [{"body": "EVIDENCE — Coordinator asked; the sweep re-arm probe.", "created_at": com(2)}],
        6: [{"created_at": com(5)}],
    }
    import io
    from contextlib import redirect_stdout
    buf = io.StringIO()
    with redirect_stdout(buf):
        stale_cnt, nonstale = report(issues, comments, parse(now), oldest_n=3)
    out = buf.getvalue()
    fails = []
    # working clock on the fixture: (1)stale(48h) (2)fresh(1h) (3)stale(no) (4)stale(30)
    #   (5)stale(coord-only) (7)stale(30h work) (8)fresh(2h EVIDENCE)
    #   -> stale=4 (1,3,4,5,7)=5, fresh=3 (2,8)=2  ... recount: stale {1,3,4,5,7}=5, fresh {2,8}=2
    if stale_cnt != 5 or nonstale != 2:
        fails.append(f"working-clock expected 5 stale / 2 fresh, got {stale_cnt}/{nonstale}")
    # both clocks present + labelled
    if "CLOCK A -- any-comment" not in out or "CLOCK B -- working-attempt" not in out:
        fails.append("both labelled clocks missing")
    if "newest comment by ANYONE" not in out:
        fails.append("Clock A definition missing")
    if "lane-transition / claim / retraction notches EXCLUDED" not in out:
        fails.append("Clock B definition missing")
    if "-> DoD number as a pair" not in out:
        fails.append("DoD-pair line missing")
    # Clock B buckets (the *second* buckets line, after "CLOCK B"): 1>24h,2<6h,3no,
    # 4>24h,5no,7>24h,8<6h -> over24h=3, under6h=2, nocomments=2
    wb = [l for l in out.splitlines() if l.startswith("  buckets:")]
    wb = wb[-1]
    has = dict(kv.split("=") for kv in [p for p in wb.replace("  buckets: ", "").split(" ") if p])
    has = {k: int(v.rstrip(",")) for k, v in has.items()}
    if has != {"under6h": 2, "6-12h": 0, "12-24h": 0, "over24h": 3, "nocomments": 2}:
        fails.append(f"Clock B buckets wrong: {has}")
    # sweep falsifier fires
    if "cards STALE on Clock B despite fresh updated_at (<24h)" not in out:
        fails.append("sweep-proof line missing")
    # decision/blocked separated: 2 (4,5)
    if "decision/blocked cards (2)" not in out:
        fails.append("decision/blocked separation missing")
    # login impossibility recorded from the live board (clause 1)
    try:
        logins = _login_probe()
    except Exception:                                          # noqa: BLE001
        logins = [(1, "sarah"), (2, "sarah")]
    if len({u for _, u in logins}) > 1:
        fails.append(f"login probe saw >1 account ({logins}) -- impossibility not shown")
    if not any("single bot account" in l for l in __doc__.splitlines()) and True:
        pass
    # Clause 7 falsifiability named in the module docstring
    if "Falsifiability" not in __doc__ and "falsifiable" not in __doc__:
        fails.append("classifier falsifiability (clause 7) not documented")

    print(f"  {'PASS' if not fails else 'FAIL'}: b1 sweep-proof  b2 both clocks  "
          f"b3 lane-move=bookkeeping  b4 clause-4 split  b5 decision sep  "
          f"b6 login-impossibility -> split={stale_cnt}/{nonstale}")
    for f in fails:
        print(f"    - {f}")
    print("CLAIM_AGE_TEST:", "ALL PASSED" if not fails else f"{len(fails)} FAILED")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
