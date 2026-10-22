#!/usr/bin/env bash
# run_evidence_xnibbler.sh — demo of the GB-040 evidence rig against XNIBBLER.
#
# Builds the disk image (if needed), then captures a full evidence bundle
# (serial log + boot/launch/gameplay screendumps + input replay + manifest)
# for XNIBBLER via tools/evidence_rig.py.  Run from the repo root.
#
#   bash run_evidence_xnibbler.sh [arm|intel]
set -euo pipefail
ARCH="${1:-arm}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "[evidence] ensuring disk.img (${ARCH}) ..."
if [ ! -f disk.img ]; then
  make disk.img ARCH="$ARCH"
fi

echo "[evidence] capturing XNIBBLER bundle via evidence_rig.py ..."
ARCH="$ARCH" python3 tools/evidence_rig.py \
  --game xnibbler --bin XNIBBLER.BIN --slot 18 --arch "$ARCH" \
  --checkpoints boot,launch,gameplay --run-seconds 6 --input "up,left,up,left"

echo "[evidence] done. Bundle:"
ls -la evidence/xnibbler/*/ | head -30
