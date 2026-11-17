#!/usr/bin/env bash
# tools/cp/pfx/pfx_gate.sh - the C3-pfx probe gate (docs/cp-pfx.md).
#
# Boots each probe once with a fresh ext2 data disk attached (NVMe namespace
# 2, mounted at /EXT), then judges the image with the host's e2fsck -fn:
#   pfx     Wine prefix operations on /EXT          (linux_probe_pfx.c)
#   bigobj  200 MiB memfd / shared anon / ext2 maps  (linux_probe_bigobj.c)
#   fatcoh  FAT16 private mappings see write()s      (linux_probe_fatcoh.c)
#   ext2w   ext2 write-back: speed rows + write-back correctness
#           (linux_probe_ext2w.c, cp/ext2w; its reboot/plug-pull halves
#           are tools/cp/ext2w/ext2w_gate.sh)
# A probe that SKIPs a row here is a failure: the disk is attached.
#
# Usage: bash tools/cp/pfx/pfx_gate.sh [probe...]   (default: all four)
# Env: LP_TIMEOUT (default 600), GATE_DIR (default <tree>/obj/pfx-gate).
# Every boot holds one QEMU slot (tools/cp/qemu-slot.sh), one slot per boot.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
D="${GATE_DIR:-$ROOT/obj/pfx-gate}"
PATH="/usr/sbin:/sbin:$PATH"
PROBES="$*"
if [ -z "$PROBES" ]; then
  for p in pfx bigobj fatcoh ext2w; do [ -f "$ROOT/src/user/linux_probe_$p.c" ] && PROBES="$PROBES $p"; done
fi
fails=0
rm -rf "$D" && mkdir -p "$D"
for p in $PROBES; do
  IMG="$D/cpdata-$p.img"
  CPDATA_FORCE=1 bash "$ROOT/tools/cp/mkcpdata.sh" "$IMG" 2G >/dev/null || exit 2
  CPDATA="$IMG" LP_OUT="$D/$p.tier.log" LP_TIMEOUT="${LP_TIMEOUT:-600}" \
    bash "$ROOT/tools/cp/qemu-slot.sh" bash "$ROOT/tools/gate/t3_linux_probe.sh" "$p" > "$D/$p.gate.log" 2>&1
  rc=$?
  con="$(sed -n 's/^T3_LOG=//p' "$D/$p.tier.log" | tail -1)"
  [ -n "$con" ] && cp "$con" "$D/$p.console.log"
  okn=$(grep -ac '\[LP\] PASS' "$D/$p.console.log" 2>/dev/null)
  grep -a '\[LP\] \(FAIL\|SKIP\|info\|done\)' "$D/$p.console.log" 2>/dev/null
  e2fsck -fn "$IMG" > "$D/$p.fsck.log" 2>&1
  frc=$?
  skip=$(grep -ac '\[LP\] SKIP' "$D/$p.console.log" 2>/dev/null)
  echo "pfx_gate: $p probe rc=$rc PASS rows=$okn skips=$skip; e2fsck -fn rc=$frc ($(tail -1 "$D/$p.fsck.log"))"
  [ "$rc" = 0 ] && [ "$frc" = 0 ] && [ "$skip" = 0 ] || fails=$((fails + 1))
  rm -f "$IMG"
done
[ "$fails" = 0 ] && echo "pfx_gate: PASS" || echo "pfx_gate: FAIL ($fails)"
[ "$fails" = 0 ]
