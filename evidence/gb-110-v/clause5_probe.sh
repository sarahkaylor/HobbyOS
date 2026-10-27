#!/usr/bin/env bash
# GB-110-V (#212) clause 5: the self-test must fail when the board read is broken
# (i.e. when graded_sha_of falls back to the local file — the very bug GB-110 fixes).
set -u
cd "$(dirname "$0")/../.." || exit 1
F=tools/board/merge_pr.py
OUT=evidence/gb-110-v
cp "$F" /tmp/mp110.bak

echo "=== as shipped"
python3 tools/board/merge_pr_test.py > "$OUT/08_test_clean.log" 2>&1
echo "  merge_pr_test.py exit=$?  (want 0)"
tail -1 "$OUT/08_test_clean.log"

echo
echo "=== mutation: graded_sha_of() returns the LOCAL FILE (the bug GB-110 exists to kill)"
python3 - "$F" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "    board = graded_sha_from_board(pr, env)\n    if board is None:\n        return None"
new = ("    board = graded_sha_from_board(pr, env)\n"
       "    if board is None:\n"
       "        p0 = os.path.join(TREE, 'obj', 'gate', f'graded-{pr}.sha')\n"
       "        if os.path.exists(p0):\n"
       "            v0 = open(p0).read().strip()\n"
       "            if re.fullmatch(r'[0-9a-f]{40}', v0):\n"
       "                return v0   # MUTANT: silent fallback to the local file\n"
       "        return None")
assert old in s, "pattern not found"
open(p, "w").write(s.replace(old, new, 1))
print("  [mutant] silent fallback to the local file installed")
PY
python3 tools/board/merge_pr_test.py > "$OUT/09_test_mutant.log" 2>&1
echo "  merge_pr_test.py exit=$?  (want non-zero)"
grep -a "FAIL" "$OUT/09_test_mutant.log" | head -4
tail -1 "$OUT/09_test_mutant.log"

cp /tmp/mp110.bak "$F"
echo
echo "=== restored"
python3 tools/board/merge_pr_test.py > /dev/null 2>&1
echo "  merge_pr_test.py exit=$?  (want 0)"