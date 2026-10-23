#!/usr/bin/env python3
"""tools/board/sync_gaming_md.py — GB-008: gaming.md (plan) <-> board (Forgejo) sync + lint.

The plan is the single source of truth (§ preamble); the board must not drift from
it. This checks, both directions:

  plan -> board   every GB-nnn in gaming.md §8 exists as an issue, with the same
                  milestone, weight label and workstream labels
  board -> plan   every GB-nnn issue on the board exists in §8 (no orphan tickets)
  plan shape      IDs are contiguous GB-001..GB-NNN, weights ∈ S/M/L, every row has
                  a DoD cell, milestone ranges match the §7 timeline

Exit 0 = in sync. Exit 1 = drift (the printed lines are the repair list). `--fix`
re-runs import_seed.py to push plan-side fields back onto the board.

  sync_gaming_md.py [--fix] [-v]
"""
import argparse
import re
import subprocess
import sys

HERE = __file__.rsplit("/", 1)[0]
sys.path.insert(0, HERE)
import board            # noqa: E402
import import_seed as S  # noqa: E402


def board_snapshot():
    out = {}
    for it in board.all_issues("all"):
        m = re.match(r"^(GB-\d{3})\b", it["title"])
        if not m:
            continue
        labs = [l["name"] for l in it["labels"]]
        out[m.group(1)] = {
            "number": it["number"],
            "title": it["title"],
            "milestone": (it.get("milestone") or {}).get("title"),
            "labels": labs,
            "lane": board.lane_of(it),
            "state": it["state"],
        }
    return out


def expected_labels(gid, weight):
    labs = list(S.WORKSTREAM.get(gid, []))
    if weight in "SML":
        labs.append(f"wt-{weight}")
    if gid in S.DEEP_IDS:
        labs.append("deep")
    return set(labs)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--fix", action="store_true", help="push plan-side fields back to the board")
    p.add_argument("-v", "--verbose", action="store_true")
    a = p.parse_args()

    plan, order = S.parse_plan(S.PLAN)
    bd = board_snapshot()
    problems = []

    # plan shape. Integrity = "every id GB-001..GB-N present exactly once", NOT
    # "sorted": §8 is organised by milestone, so a minted ticket belongs in the
    # section it serves (e.g. an intel-tier defect in M1) even though its number
    # is higher than the ids around it.
    ids = [int(g.split("-")[1]) for g in order]
    want = set(range(1, len(ids) + 1))
    if set(ids) != want or len(ids) != len(set(ids)):
        gaps = sorted(want - set(ids))
        dups = sorted({i for i in ids if ids.count(i) > 1})
        problems.append(f"plan: GB id set is not a clean GB-001..GB-{len(ids):03d} "
                        f"(missing={gaps}, duplicated={dups})")
    for gid in order:
        t = plan[gid]
        if t["weight"] not in ("S", "M", "L"):
            problems.append(f"{gid}: bad weight {t['weight']!r}")
        if not t["dod"].strip():
            problems.append(f"{gid}: empty DoD cell")
        if t["ms"] not in S.MS_KEY.values():
            problems.append(f"{gid}: unmapped milestone {t['ms']!r}")

    # plan -> board
    for gid in order:
        t = plan[gid]
        b = bd.get(gid)
        if not b:
            problems.append(f"{gid}: in gaming.md §8 but NOT on the board")
            continue
        if b["milestone"] != t["ms"]:
            problems.append(f"{gid}: milestone drift — plan {t['ms']} vs board {b['milestone']}")
        missing = expected_labels(gid, t["weight"]) - set(b["labels"])
        if missing:
            problems.append(f"{gid}: missing labels {sorted(missing)}")
        if b["lane"] is None:
            problems.append(f"{gid}: no lane label")
        if a.verbose:
            print(f"ok  {gid:8} #{b['number']:<3} {t['ms']:<15} lane={b['lane']:<12} wt={t['weight']}")

    # board -> plan
    for gid, b in sorted(bd.items()):
        # A CLOSED ticket is not work, so it can never legitimately earn a §8 row
        # (scratch tickets exist precisely to be closed and never planned). Counting
        # those as orphans makes the check unable to pass for a legitimate case --
        # the same defect as a test that cannot fail, and it trains people to ignore
        # the lint. Only OPEN tickets can be orphans.
        if gid not in plan and b.get("state") != "closed":
            problems.append(f"{gid}: on the board but NOT in gaming.md §8 (orphan #{b['number']}) — "
                            f"add it to §8 or close it")

    # §7 milestone table must name the same milestone ids the issues carry
    text = open(S.PLAN, encoding="utf-8").read()
    for key, title in S.MS_KEY.items():
        if f"**{key} " not in text and f"**{key}" not in text:
            problems.append(f"plan §7: milestone {key} missing from the timeline table")

    for pr in problems:
        print("DRIFT " + pr)
    if problems:
        if a.fix:
            print("\n--fix: re-importing §8 onto the board (idempotent) ...")
            subprocess.run([sys.executable, f"{HERE}/import_seed.py"], check=True)
            return 0
        print(f"\n{len(problems)} drift item(s); run with --fix to push plan fields to the board")
        return 1
    print(f"IN SYNC — {len(order)} plan tickets, {len(bd)} board tickets, "
          f"milestones {sorted({v['ms'] for v in plan.values()})}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
