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

print()
if fails:
    print(f"SYNC_GAMING_MD_TEST: FAIL — {len(fails)}")
    sys.exit(1)
print("SYNC_GAMING_MD_TEST: PASS — seed gaps/duplicates fail, sparse mints pass")
sys.exit(0)
