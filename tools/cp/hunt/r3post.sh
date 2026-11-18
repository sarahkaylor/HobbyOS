#!/usr/bin/env bash
# tools/cp/hunt/r3post.sh RUN... -- round 3's per-boot post-processing (lane
# H6-hunt3, docs/cp-hunt.md): re-run classify.py on run-hunt.sh's copies (the
# classifier gained rows while the batch ran), shotcheck.py --json over the
# boot's screendumps (numbers only), then r3table.py over all the RUNs and
# classify.py --aggregate.  Output: LOGS/r3-table.txt, LOGS/r3-aggregate.txt.
# The screendumps themselves are deleted only by r3post.sh --purge RUN...
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
LOGS="${HUNT_LOGS:-$ROOT/obj/hunt-logs}"
PURGE=0
[ "${1:-}" = --purge ] && { PURGE=1; shift; }
for r in "$@"; do
  [ -f "$LOGS/$r.serial.txt" ] || { echo "r3post: no $LOGS/$r.serial.txt" >&2; continue; }
  python3 -I "$ROOT/tools/cp/hunt/classify.py" --run "$r" --log "$LOGS/$r.log" --host "$LOGS/$r.host.txt" \
    --json "$LOGS/$r.class.json" "$LOGS/$r.serial.txt" > "$LOGS/$r.class.txt" 2>&1
  OUT="$ROOT/obj/cp-p9/$r"
  if [ -d "$OUT/shots" ]; then
    python3 -I "$ROOT/tools/cp/hunt/shotcheck.py" --json "$OUT" > "$LOGS/$r.shotcheck.json" 2>/dev/null
    python3 -I "$ROOT/tools/cp/hunt/shotcheck.py" "$OUT" > "$LOGS/$r.shotcheck.txt" 2>/dev/null
    [ "$PURGE" = 1 ] && rm -rf "$OUT/shots" "$OUT/vnc-sock"
  fi
done
python3 -I "$ROOT/tools/cp/hunt/r3table.py" "$LOGS" "$@" > "$LOGS/r3-table.txt"
js=(); for r in "$@"; do [ -f "$LOGS/$r.class.json" ] && js+=("$LOGS/$r.class.json"); done
python3 -I "$ROOT/tools/cp/hunt/classify.py" --aggregate "${js[@]}" > "$LOGS/r3-aggregate.txt"
cat "$LOGS/r3-table.txt"
