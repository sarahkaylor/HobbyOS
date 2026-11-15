#!/usr/bin/env bash
# tools/cp/p0-wine-evidence.sh — the game-independent half of P0's Wine
# measurements (docs/cyberpunk.md §3.3 items 3 and 5; P2's syscall list).
# Runs on the Linux host (linuxci) against the staged Wine and the pinned glibc
# 2.31 runtime (tools/cp/run-wine-host.sh), and writes raw output to $OUT:
#
#   wineboot-cold.txt           stdout/stderr of `wineboot -i` into a fresh prefix
#   echo-warm.txt               stdout/stderr of `wine cmd /c echo hello`, warm prefix
#   strace-wineboot.raw         strace -f -tt -T -yy, fresh prefix, every process
#   strace-wineboot.c.txt       strace -f -c -w, fresh prefix
#   strace-echo.raw / .c.txt    the same for `wine cmd /c echo hello`, warm prefix
#   maps/                       /proc/<pid>/maps of every Wine process while
#                               `wine cmd /c pause` blocks on a pipe
#   summary-*.md                tools/cp/strace-summary.py over the raw traces
#
# Usage: bash tools/cp/p0-wine-evidence.sh [OUT]   (default $CP_WINE_WORK/evidence)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${CP_WINE_WORK:-$HOME/cp/wine-build}"
OUT="${1:-$WORK/evidence}"
RUN="bash $ROOT/tools/cp/run-wine-host.sh"
rm -rf "$OUT"; mkdir -p "$OUT"
say() { printf '== %s\n' "$*"; }

say "1. wineboot -i, fresh prefix (WINEDEBUG=err+all,fixme-all)"
WINEDEBUG=err+all,fixme-all $RUN -- wineboot -i > "$OUT/wineboot-cold.txt" 2>&1 || true
tail -5 "$OUT/wineboot-cold.txt"
find "$WORK/hostroot/tmp/pfx" -maxdepth 1 | sort >> "$OUT/wineboot-cold.txt"

say "2. wine cmd /c echo hello, warm prefix"
$RUN --warm -- wine cmd /c echo hello > "$OUT/echo-warm.txt" 2>&1 || true
cat "$OUT/echo-warm.txt"

say "3. strace raw: wineboot -i, fresh prefix"
$RUN --strace "$OUT/strace-wineboot.raw" -- wineboot -i > "$OUT/strace-wineboot.out" 2>&1 || true
say "4. strace -c: wineboot -i, fresh prefix"
$RUN --strace-c "$OUT/strace-wineboot.c.txt" -- wineboot -i > /dev/null 2>&1 || true
say "5. strace raw: wine cmd /c echo hello, warm prefix"
$RUN --warm --strace "$OUT/strace-echo.raw" -- wine cmd /c echo hello > "$OUT/strace-echo.out" 2>&1 || true
say "6. strace -c: wine cmd /c echo hello, warm prefix"
$RUN --warm --strace-c "$OUT/strace-echo.c.txt" -- wine cmd /c echo hello > /dev/null 2>&1 || true

say "7. /proc/<pid>/maps while cmd /c pause blocks (warm prefix)"
( sleep 12 ) | $RUN --warm --maps "$OUT/maps" --maps-delay 6 -- wine cmd /c pause > "$OUT/pause.out" 2>&1 || true
ls "$OUT/maps"

say "8. summaries"
for t in wineboot echo; do
  python3 -I "$ROOT/tools/cp/strace-summary.py" --tree "$ROOT" "$OUT/strace-$t.raw" > "$OUT/summary-$t.md"
done
python3 -I "$ROOT/tools/cp/strace-summary.py" --tree "$ROOT" \
  "$OUT/strace-wineboot.raw" "$OUT/strace-echo.raw" > "$OUT/summary-union.md"
wc -l "$OUT"/*.raw
say "done: $OUT"
