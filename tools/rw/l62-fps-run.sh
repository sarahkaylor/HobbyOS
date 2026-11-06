#!/usr/bin/env bash
# tools/rw/l62-fps-run.sh -- one measured fps run: harness + present timeline.
#
#   l62-fps-run.sh <name> <WxH> <run-seconds> [honour]
#
# Starts the [XSW] timeline follower on the serial log first, then drives the
# usual desktop harness, then keeps the run's evidence under obj/l62/<name>/:
# the serial log, Player.log and the timestamped present rows.  Nothing is
# rebuilt unless RW_SKIP_BUILD is cleared (the caller builds once).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
NAME="${1:?name}"; SCREEN="${2:?WxH}"; SECS="${3:?run seconds}"
HONOUR="${4:-1}"
OUT="obj/l62/$NAME"
mkdir -p "$OUT"

python3 tools/rw/l62-xsw-follow.py obj/rw-desktop/serial-desktop.log \
        "$OUT/xsw.jsonl" &
FOLLOW=$!
sleep 0.5

RW_SKIP_BUILD="${RW_SKIP_BUILD:-1}" RW_SCREEN="$SCREEN" \
  RW_WM_HONOUR="$HONOUR" RW_SHOT_EVERY="${RW_SHOT_EVERY:-60}" \
  tools/rw/run-rimworld-desktop.sh --timeout 420 --run-timeout "$SECS" \
  >"$OUT/driver.log" 2>&1
RC=$?

sleep 0.5
kill "$FOLLOW" 2>/dev/null
wait "$FOLLOW" 2>/dev/null

cp -f obj/rw-desktop/serial-desktop.log "$OUT/serial.log" 2>/dev/null
cp -f obj/rw-desktop/Player.log "$OUT/Player.log" 2>/dev/null
echo "== $NAME rc=$RC  serial=$(stat -c %s "$OUT/serial.log" 2>/dev/null) bytes"
grep -a "screen request\|WMSIZE" "$OUT/serial.log" | head -3
python3 tools/rw/l62-fps.py "$OUT/xsw.jsonl" --window 120 --label "$NAME ($SCREEN)"
exit "$RC"
