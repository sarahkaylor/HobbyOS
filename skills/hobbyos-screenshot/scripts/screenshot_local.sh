#!/bin/bash
# One-shot local screenshot of a running HobbyOS qemu via QMP screendump.
#
# Usage: screenshot_local.sh [qmp-sock] [out.png]
#   qmp-sock : path to the QMP unix socket (default ./qmp-sock)
#   out.png  : output PNG (default /tmp/hobbyos.png)
#
# Requires: socat, python3. QEMU must have been started with e.g.
#   make run QEMU_ARGS="-qmp unix:./qmp-sock,server,nowait"
set -euo pipefail

SOCK="${1:-./qmp-sock}"
OUT="${2:-/tmp/hobbyos.png}"
PPM="$(mktemp -t hobbyos-shot.XXXXXX).ppm"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -S "$SOCK" ]; then
  echo "error: QMP socket '$SOCK' not found. Start qemu with -qmp unix:$SOCK,server,nowait" >&2
  exit 1
fi
command -v socat >/dev/null || { echo "error: socat not installed (brew install socat)" >&2; exit 1; }

# screendump writes on the qemu host (here, this Mac) — use an absolute path.
( printf '{"execute":"qmp_capabilities"}\n'; sleep 0.2; \
  printf '{"execute":"screendump","arguments":{"filename":"%s"}}\n' "$PPM"; \
  sleep 1 ) | socat - "UNIX-CONNECT:$SOCK" 2>&1 | tr -d '\r'

if [ ! -s "$PPM" ]; then
  echo "error: screendump produced no PPM at $PPM" >&2
  exit 1
fi

python3 "$SCRIPT_DIR/ppm2png.py" "$PPM" "$OUT"
rm -f "$PPM"
echo "screenshot: $OUT"
