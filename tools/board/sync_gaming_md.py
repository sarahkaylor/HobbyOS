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
re-runs import_seed.py to push plan-side fields back onto the board;
`--adopt-orphans` appends a §8 row for each open board-only mint (GB-193).

  sync_gaming_md.py [--fix | --adopt-orphans] [-v]
"""
import argparse
import re
import subprocess
import sys

HERE = __file__.rsplit("/", 1)[0]
sys.path.insert(0, HERE)
import board            # noqa: E402
import import_seed as S  # noqa: E402

# The seed range imported from the original plan: GB-001..GB-147.  Ids above it
# are mints and may be sparse — GB-148..GB-150 went to gate/scratch cards, never
# plan rows — so contiguity is required ONLY inside the seed (GB-185).
SEED_MAX = 147


def plan_id_problems(order, seed_max=SEED_MAX):
    """Validate the plan's GB id set: the seed range complete, no id repeated.

    GB-185: the old check required set(ids) == 1..len(ids), so adding one row
    for a card minted after GB-147 reported a false drift ("missing=[148]") and
    froze §8.  A sparse id above the seed is legitimate; a gap inside it is not.
    """
    ids = [int(g.split("-")[1]) for g in order]
    dups = sorted({i for i in ids if ids.count(i) > 1})
    gaps = sorted(set(range(1, seed_max + 1)) - set(ids))
    if not gaps and not dups:
        return []
    return [f"plan: GB id set is not a clean GB-001..GB-{seed_max:03d} seed "
            f"(missing={gaps}, duplicated={dups})"]


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
            "body": it.get("body") or "",
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


def _flatten(s):
    return re.sub(r"\s+", " ", s or "").strip()


def _cell(s):
    """A §8 table cell: one line, literal pipes escaped for _cells()."""
    return _flatten(s).replace("|", "\\|")


def row_from_issue(gid, issue):
    """Build a §8 table row for a board-only mint from its own issue fields.

    GB-193: every coordinator mint above GB-147 lands without a row, so the
    sync check is red until someone hand-writes one.  The issue body already
    carries the three fields a row needs (weight label, Depends on, DoD).
    Returns None (and names the card) when a required field is missing.
    """
    labs = issue.get("labels") or []
    wt = next((l[3] for l in labs if re.fullmatch(r"wt-[SML]", l)), None)
    if wt is None:
        print(f"CANNOT ADOPT {gid}: no wt-S/M/L label")
        return None
    title = re.sub(r"^GB-\d{3}\s+", "", issue.get("title") or "").strip()
    body = issue.get("body") or ""

    m = re.search(r"^\|\s*Depends on\s*\|\s*(.+?)\s*\|", body, re.M)
    depends = _flatten(m.group(1)) if m else ""
    depends = depends.replace("**", "").replace("`", "")
    if not depends or depends in ("—", "-", "none", "None"):
        depends = "—"

    d = re.search(r"^###\s*DoD[^\n]*\n(.*?)(?=^###\s|\Z)", body, re.M | re.S)
    items, cur = [], None
    if d:
        for line in d.group(1).splitlines():
            s = line.strip()
            if not s:
                continue
            if re.match(r"^(?:\d+[.)]|[-*])\s+", s):
                if cur:
                    items.append(cur)
                cur = re.sub(r"^(?:\d+[.)]|[-*])\s+", "", s)
            elif cur is not None:
                # a wrapped continuation of the current item, not a new one
                cur += " " + s
            else:
                cur = s
        if cur:
            items.append(cur)
    dod = _flatten("; ".join(items)) or f"see board #{issue.get('number')}"
    return f"| {gid} | **{_cell(title)}** | {wt} | {_cell(depends)} | {_cell(dod)} |"


def insert_plan_row(text, milestone, row):
    """Append `row` to the §8 table of section `milestone` (e.g. M4-acceptance)."""
    key = next((k for k, v in S.MS_KEY.items() if v == milestone), None)
    if key is None:
        raise ValueError(f"unknown milestone {milestone!r}")
    m = re.search(r"^### Milestone %s\b.*$" % re.escape(key), text, re.M)
    if not m:
        raise ValueError(f"no §8 section for {milestone}")
    start = m.end()
    nxt = re.search(r"^#{1,3} ", text[start:], re.M)
    end = start + (nxt.start() if nxt else len(text) - start)
    rows = list(re.finditer(r"^\|.*\|[ \t]*$", text[start:end], re.M))
    if not rows:
        raise ValueError(f"no table in §8 section {milestone}")
    at = start + rows[-1].end()
    return text[:at] + "\n" + row + text[at:]


def adopt_orphans(bd, plan, path=None):
    """Append a §8 row for every OPEN board mint missing from the plan."""
    path = path or S.PLAN
    text = open(path, encoding="utf-8").read()
    adopted = []
    for gid, b in sorted(bd.items()):
        if gid in plan or b.get("state") == "closed":
            continue
        ms = b.get("milestone")
        if ms not in S.MS_KEY.values():
            print(f"CANNOT ADOPT {gid}: milestone {ms!r} is not a known MS_KEY")
            continue
        row = row_from_issue(gid, b)
        if row is None:
            continue
        try:
            text = insert_plan_row(text, ms, row)
        except ValueError as e:
            print(f"CANNOT ADOPT {gid}: {e}")
            continue
        adopted.append(row)
    if adopted:
        open(path, "w", encoding="utf-8").write(text)
    return adopted


def find_problems(plan, order, bd, text, verbose=False):
    problems = []

    # plan shape. Integrity = the SEED range GB-001..GB-147 is complete and no
    # id repeats.  §8 is organised by milestone, and mints above the seed are
    # sparse by design — see plan_id_problems() (GB-185).
    problems += plan_id_problems(order)
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
        if verbose:
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
    for key, title in S.MS_KEY.items():
        if f"**{key} " not in text and f"**{key}" not in text:
            problems.append(f"plan §7: milestone {key} missing from the timeline table")

    return problems


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--fix", action="store_true", help="push plan-side fields back to the board")
    p.add_argument("--adopt-orphans", action="store_true",
                   help="append a §8 row for each open board-only mint")
    p.add_argument("-v", "--verbose", action="store_true")
    a = p.parse_args()

    plan, order = S.parse_plan(S.PLAN)
    bd = board_snapshot()

    if a.adopt_orphans:
        adopted = adopt_orphans(bd, plan)
        if adopted:
            print(f"ADOPTED {len(adopted)} row(s):")
            for row in adopted:
                print("  " + row)
            plan, order = S.parse_plan(S.PLAN)

    text = open(S.PLAN, encoding="utf-8").read()
    problems = find_problems(plan, order, bd, text, verbose=a.verbose)

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
