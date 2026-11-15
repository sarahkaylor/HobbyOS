#!/bin/bash
# tools/cp/mmu/castscan_check.sh [--update]
#
# P1 static gate (docs/cp-address-space.md section 3.8 item 3, Appendix A):
# every int<->pointer conversion in the x86_64 kernel (default build and the
# unit-test build) must be either inside src/include/kmem.h (the helpers) or
# a reviewed site in tools/cp/mmu/castscan.allow -- a user VA, a kernel VA used
# as a VA (stacks, IDT/TSS/GS, function pointers), a diagnostic value, or an
# ARM-only address.  A new identity dereference therefore fails this check
# instead of waiting for a [IDMAP-TRIP] at run time.
#
# Sites are keyed by (file, kind, whitespace-normalised source line), not by
# line number, so unrelated edits do not churn the allowlist.  Duplicate
# lines count: the allowlist is a multiset.
#
#   castscan_check.sh            exit 0 = no site outside the allowlist
#   castscan_check.sh --update   rewrite the allowlist from the current tree
#                                (review the diff: every new row is a claim
#                                that the site is not a physical address)
# Needs clang 21 + python3 (linuxci).
set -u
export LC_ALL=C  # sort/comm must agree on collation on every host
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
ALLOW=tools/cp/mmu/castscan.allow
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
bash tools/cp/mmu/runscan.sh intel > "$TMP/x64.raw"
bash tools/cp/mmu/runscan.sh intel -DKERNEL_MODE_UNIT_TEST > "$TMP/x64ut.raw"
cat "$TMP/x64.raw" "$TMP/x64ut.raw" | cut -f1,2 | sort -u > "$TMP/sites"
python3 - "$TMP/sites" > "$TMP/cur" <<'EOF'
import sys
lines = {}
for row in open(sys.argv[1]):
    loc, kind = row.rstrip('\n').split('\t')[:2]
    f, n = loc.rsplit(':', 1)
    if f == 'src/include/kmem.h':
        continue  # the helpers themselves
    if f not in lines:
        lines[f] = open(f, errors='replace').read().split('\n')
    text = ' '.join(lines[f][int(n) - 1].split())
    print(f"{f}\t{kind}\t{text}")
EOF
sort "$TMP/cur" > "$TMP/cur.s"
if [ "${1:-}" = "--update" ]; then
  { echo "# castscan allowlist (tools/cp/mmu/castscan_check.sh --update); file<TAB>kind<TAB>normalised line"
    cat "$TMP/cur.s"; } > "$ALLOW"
  echo "castscan: wrote $(grep -vc '^#' "$ALLOW") sites to $ALLOW"
  exit 0
fi
grep -v '^#' "$ALLOW" | sort > "$TMP/allow.s"
comm -23 "$TMP/cur.s" "$TMP/allow.s" > "$TMP/new"
comm -13 "$TMP/cur.s" "$TMP/allow.s" > "$TMP/gone"
total=$(wc -l < "$TMP/cur.s")
nnew=$(wc -l < "$TMP/new")
ngone=$(wc -l < "$TMP/gone")
echo "castscan: $total int<->pointer sites outside kmem.h; $nnew not in the allowlist; $ngone allowlisted rows no longer present"
if [ "$nnew" -gt 0 ]; then
  echo "--- NEW sites (convert to phys_to_virt/virt_to_phys/kimg_pa, or review + --update):"
  cat "$TMP/new"
  exit 1
fi
exit 0
