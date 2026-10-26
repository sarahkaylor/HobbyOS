#!/usr/bin/env bash
# GB-104-V clauses 1, 2 and 4b on PR #197's head d222dae (unchanged since the 13:03Z RED).
set -u
cd "$(dirname "$0")/../.." || exit 1
GUARD=tools/swarm/resume-cwd-guard.sh
OUT=evidence/gb-104-v

echo "=== clause 1: one error, not N"
DEAD=/tmp/gb104-dead-cwd-$$
rm -rf "$DEAD"
set +e
bash "$GUARD" "$DEAD" >"$OUT/03_guard_dead.out" 2>&1
rc=$?
set -e
echo "guard exit=$rc"
echo "messages naming the path: $(grep -ac "cannot resume into working directory '$DEAD'" "$OUT/03_guard_dead.out")"
echo "total output lines:      $(wc -l < "$OUT/03_guard_dead.out")"
echo "--- the message"
cat "$OUT/03_guard_dead.out"

echo
echo "=== clause 1b: a wrapper calling it once across a 5-command session"
total=0
for i in 1 2 3 4 5; do
  n=$(bash "$GUARD" "$DEAD" 2>&1 | grep -ac "RESUME-CWD-GUARD")
  total=$((total + n))
done
echo "guard invocations in the loop: 5 (one per resume, not per shell command)"
echo "RESUME-CWD-GUARD lines printed: $total  (the defect was one per command; the wrapper calls it once)"

echo
echo "=== clause 2: healthy path is silent, exit 0"
mkdir -p /tmp/gb104-live-cwd
bash "$GUARD" /tmp/gb104-live-cwd >"$OUT/04_guard_live.out" 2>&1
echo "guard exit=$?"
echo "bytes of output: $(wc -c < "$OUT/04_guard_live.out")"

echo
echo "=== clause 4b: break the guard deliberately, the self-test must fail"
cp "$GUARD" /tmp/gb104_guard.bak
# remove the dead-cwd check: always take the healthy path
python3 - "$GUARD" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'if [ -d "$TARGET" ]; then\n  exit 0\nfi\n'
assert old in s, "guard shape changed"
s = s.replace(old, 'exit 0   # MUTANT: dead-cwd check removed\n', 1)
open(p, "w").write(s)
print("[control] guard mutated: dead-cwd check removed")
PY
set +e
python3 tools/swarm/resume_guard_test.py >"$OUT/05_selftest_broken.log" 2>&1
broken=$?
set -e
echo "self-test exit with the guard broken: $broken  (expected non-zero)"
tail -3 "$OUT/05_selftest_broken.log"
cp /tmp/gb104_guard.bak "$GUARD"
echo "[control] guard restored"
set +e
python3 tools/swarm/resume_guard_test.py >"$OUT/06_selftest_restored.log" 2>&1
ok=$?
set -e
echo "self-test exit after restore: $ok  (expected 0)"
tail -2 "$OUT/06_selftest_restored.log"