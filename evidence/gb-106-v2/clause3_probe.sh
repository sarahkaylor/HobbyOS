#!/usr/bin/env bash
# GB-106-V (#202) clause 3 probe: does the #194 fixture guard the MOTIVATING incident?
# The incident was a MISMATCH (gate graded 7de6dde, merge landed e8c7dc89).
# Mutate decide() so a mismatch is allowed, and see which self-test lines go red.
# If the #194 fixture line stays PASS, the incident is unguarded — exactly the
# failure mode the card warns about.
set -u
cd "$(dirname "$0")/../.." || exit 1
F=tools/board/merge_pr.py
OUT=evidence/gb-106-v2
cp "$F" /tmp/mp_graded.bak

cp /tmp/mp_graded.bak "$F"
python3 "$F" --self-test > "$OUT/04_selftest_clean.log" 2>&1
echo "clean: self-test exit=$?  (want 0)"
grep -a "#194" "$OUT/04_selftest_clean.log"

echo
echo "=== mutation: a MISMATCHED graded sha is allowed (the incident's shape)"
python3 - "$F" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '    if graded != head:\n        return False,'
new = '    if graded != head:\n        return True, "ok: MUTANT allows a mismatched graded sha"'
assert old in s, "pattern not found"
open(p, "w").write(s.replace(old, new, 1))
print("[mutant] mismatch now allowed")
PY
python3 "$F" --self-test > "$OUT/05_selftest_mismatch_allowed.log" 2>&1
echo "mutant: self-test exit=$?  (want non-zero)"
echo "--- every assertion under the mutant:"
cat "$OUT/05_selftest_mismatch_allowed.log" | sed 's/^/    /'
cp /tmp/mp_graded.bak "$F"
echo
echo "=== restored: $(python3 "$F" --self-test > /dev/null 2>&1; echo exit=$?)"
echo "--- does either file name the incident's shas?"
grep -c "7de6dde\|e8c7dc8" "$F" tools/board/merge_pr_test.py