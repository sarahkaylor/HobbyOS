#!/bin/bash
# Break/revert demonstration for tools/nv/check_wpr_bus_provenance.py.
#
# Two single-line mutations, one in each audited region:
#   1. src/kernel/nv_gsp.c -- the metadata block's signature address is built
#      from the image mapping's PHYSICAL base instead of its bus base.
#   2. src/kernel/nv_fw.c  -- the radix3 level-2 PTE holds the image's physical
#      address instead of its bus address.
# Both are the exact regressions the guard exists for, and both are invisible
# while VT-d translation is off (bus == phys).  The files are restored at the
# end (and on any exit, via the trap), then the gate is re-run green.
#
# Run from anywhere: bash tools/nv/demo-wpr-bus-guard.sh
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
GSP=src/kernel/nv_gsp.c
FW=src/kernel/nv_fw.c

tmp=$(mktemp -d) || exit 1
cp "$GSP" "$tmp/gsp"
cp "$FW" "$tmp/fw"
restore() { cp "$tmp/gsp" "$GSP"; cp "$tmp/fw" "$FW"; }
trap restore EXIT

echo '$ sed -i "s/img_bus + sig_off/rm.map.phys + sig_off/" src/kernel/nv_gsp.c'
sed -i 's/img_bus + sig_off/rm.map.phys + sig_off/' "$GSP"
echo '$ sed -n 4889p src/kernel/nv_gsp.c   # the mutation in place'
sed -n 4889p "$GSP"

echo '$ sed -i "s/image_bus + i \* NV_RADIX3_PAGE/image_phys + i * NV_RADIX3_PAGE/" src/kernel/nv_fw.c'
sed -i 's/image_bus + i \* NV_RADIX3_PAGE/image_phys + i * NV_RADIX3_PAGE/' "$FW"
echo '$ sed -n 709p src/kernel/nv_fw.c     # the mutation in place'
sed -n 709p "$FW"

echo '$ make wpr_bus_provenance   # expect FAIL'
make wpr_bus_provenance
echo "exit=$?"

restore
echo '$ # both files restored'
grep -n 'img_bus + sig_off' "$GSP"
grep -n 'image_bus + i \* NV_RADIX3_PAGE' "$FW"

echo '$ make wpr_bus_provenance   # expect ok'
make wpr_bus_provenance
echo "exit=$?"
