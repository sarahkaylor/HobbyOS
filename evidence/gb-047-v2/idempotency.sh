#!/usr/bin/env bash
# GB-047-V (#223) — is the fail-closed check defeated by the pack's own manifest?
#
# evidence_pack.py writes S0-S1-EVIDENCE-MANIFEST.json INTO --root (line 98), and the
# artifact scan globs --root.  So run 2 over the same tree may count run 1's manifest
# as an artifact and exit 0 — which is exactly the state criterion 2 forbids.
set -u
cd "$(dirname "$0")/../.." || exit 1
OUT=evidence/gb-047-v2
NEW=tools/t5/evidence_pack.py.new
cp "$NEW" tools/t5/evidence_pack.py

D=/tmp/gb047v2_fresh_$$
mkdir -p "$D"
echo "=== fresh empty dir: $D"
echo "--- run 1 (new code)"
python3 tools/t5/evidence_pack.py --root "$D" --out /tmp/gb047v2_p1.md 2>&1 | tail -2
echo "run1 exit=${PIPESTATUS[0]}  (want 1)"
echo "--- what the run left in the tree:"
ls -la "$D" | sed 's/^/    /'

echo "--- run 2, same tree, same code"
python3 tools/t5/evidence_pack.py --root "$D" --out /tmp/gb047v2_p2.md 2>&1 | tail -2
echo "run2 exit=${PIPESTATUS[0]}  (want 1 — an empty tree is still an empty tree)"

echo
echo "=== and a third time, to be sure it is not a one-off"
python3 tools/t5/evidence_pack.py --root "$D" --out /tmp/gb047v2_p3.md 2>&1 | head -2
echo "run3 exit=${PIPESTATUS[0]}"

echo
echo "=== is the manifest itself one of the scan's markers?"
grep -n "MANIFEST\|S0-S1" "$NEW" | head -5