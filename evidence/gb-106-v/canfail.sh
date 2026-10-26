#!/usr/bin/env bash
# GB-106-V can-fail control: mutate the guard's decide() and prove --self-test
# catches each mutation, then restore.  A gate that cannot fail is not a gate.
set -u
cd "$(dirname "$0")/../.." || exit 1
F=tools/board/merge_pr.py
OUT=evidence/gb-106-v
cp "$F" /tmp/merge_pr.bak

mutate() { # $1 = label, $2 = python-literal old, $3 = new
  local label="$1" old="$2" new="$3"
  cp /tmp/merge_pr.bak "$F"
  python3 - "$F" "$old" "$new" <<'PY'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
if old not in s:
    print("  [mutant] PATTERN NOT FOUND:", old[:40]); sys.exit(1)
open(p, "w").write(s.replace(old, new, 1))
print("  [mutant] applied")
PY
  python3 "$F" --self-test > "$OUT/05_selftest_$label.log" 2>&1
  local rc=$?
  echo "  mutant '$label': self-test exit=$rc  (want non-zero)"
  grep -a "FAIL" "$OUT/05_selftest_$label.log" | head -3
}

echo "=== mutation 1: a GREEN with NO graded sha is allowed (the fail-open bug)"
mutate "no_sha_allowed" \
  '    if not graded:
        return False, f"REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head {head[:12]}"' \
  '    if not graded:
        return True, "ok: MUTANT allows a GREEN with no graded sha"'

echo
echo "=== mutation 2: a mismatched graded sha is allowed"
mutate "mismatch_allowed" \
  '    if graded != head:
        return False, f"REFUSE: graded sha {graded[:12]} != current head {head[:12]} — refusing to merge a tree that was not graded"' \
  '    if graded != head:
        return True, "ok: MUTANT allows a mismatched graded sha"'

cp /tmp/merge_pr.bak "$F"
echo
echo "=== restored"
python3 "$F" --self-test > "$OUT/06_selftest_restored.log" 2>&1
echo "  self-test exit after restore=$?  (want 0)"
tail -1 "$OUT/06_selftest_restored.log"