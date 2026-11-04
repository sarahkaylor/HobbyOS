#!/usr/bin/env python3
"""sync_gaming_md_test.py — GB-185: the plan id-set check.

The plan seed is GB-001..GB-147; ids above it are mints and may be sparse
(GB-148..GB-150 went to gate/scratch cards, never plan rows).  The old check
required set(ids) == 1..len(ids), so adding one post-147 row produced
`missing=[148]` — a false drift that froze §8.

Cases:
  seed alone is valid;
  sparse ids above the seed are valid (the false-positive case);
  a gap inside GB-001..GB-147 fails, naming the missing id;
  a duplicate fails, naming the id.

No board/network: plan_id_problems() is pure.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sync_gaming_md as S  # noqa: E402

fails = []


def check(name, cond):
    print(f"  {'PASS' if cond else 'FAIL'}: {name}")
    if not cond:
        fails.append(name)


seed = [f"GB-{i:03d}" for i in range(1, S.SEED_MAX + 1)]

check("seed GB-001..GB-147 alone is valid", S.plan_id_problems(seed) == [])

sparse = seed + ["GB-182", "GB-184", "GB-185"]
check("sparse ids above the seed are valid", S.plan_id_problems(sparse) == [])

gap = [g for g in seed if g != "GB-146"] + ["GB-182"]
probs = S.plan_id_problems(gap)
check("a gap inside the seed fails, naming the missing id",
      len(probs) == 1 and "missing=[146]" in probs[0])

dup = seed + ["GB-050"]
probs = S.plan_id_problems(dup)
check("a duplicate fails, naming the id",
      len(probs) == 1 and "duplicated=[50]" in probs[0])

# The exact regression the old check produced: 148 ids (1..147 + one mint) must
# not report `missing=[148]`.
regression = S.plan_id_problems(sparse)
check("no false 'missing=[148]' for the 148th (minted, sparse) row",
      regression == [])

# --- GB-193: --adopt-orphans row generation/insertion (pure, no board) -------

fixture_plan = """### Milestone M0 — Infrastructure

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-001 | one | S | — | d |

### Milestone M4 — Integration

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-043 | four | M | — | d |
"""

issue = {
    "number": 900,
    "title": "GB-900 a fixture mint with a | pipe",
    "milestone": "M4-acceptance",
    "labels": ["wt-M"],
    "state": "open",
    "body": ("| Depends on | GB-053 (PR #314) |\n\n"
             "### DoD\n\n1. first check | with pipe\n2. second check\n\n"
             "### Evidence slot\nx\n"),
}
row = S.row_from_issue("GB-900", issue)
assert row is not None, "fixture row must be generated"
check("row carries id, weight and depends",
      "| GB-900 |" in row and "| M |" in row and "GB-053" in row)
check("row escapes a literal pipe", "\\|" in row)
check("row includes both DoD items", "first check" in row and "second check" in row)
check("row stops at the next section", "Evidence slot" not in row)

out = S.insert_plan_row(fixture_plan, "M4-acceptance", row)
check("row lands in the M4 table", out.index("GB-900") > out.index("### Milestone M4"))
check("row lands after the M4 rows", out.index("GB-900") > out.index("GB-043"))

out0 = S.insert_plan_row(fixture_plan, "M0-infra", row)
check("row lands in M0's table", out0.index("GB-900") < out0.index("### Milestone M4"))
check("a following heading stays on its own line", "|\n\n### Milestone M4" in out0)

bad = dict(issue, labels=[])
check("a mint with no wt label is refused", S.row_from_issue("GB-901", bad) is None)

try:
    S.insert_plan_row(fixture_plan, "M9-nope", row)
    refused = False
except ValueError as e:
    refused = "M9-nope" in str(e)
check("an unknown milestone is refused by name", refused)

print()
if fails:
    print(f"SYNC_GAMING_MD_TEST: FAIL — {len(fails)}")
    sys.exit(1)
print("SYNC_GAMING_MD_TEST: PASS — seed gaps/duplicates fail, sparse mints pass")
sys.exit(0)
