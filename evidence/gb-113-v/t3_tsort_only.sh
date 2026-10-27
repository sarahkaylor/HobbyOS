#!/usr/bin/env bash
# t3_tsort_only.sh — GB-113-V focused gate: boot ONE program (TSORTTEST.BIN)
# on intel via a ::WAVE.CFG manifest, so the wait-status evidence is read from
# a small, repeatable serial instead of a 41k-line full wave.
#
# Not a tier verdict: the completeness floor (T3_MIN_LINES) is expected to FAIL
# for a one-program wave.  This script asserts only on TSORTTEST's own rows:
#   * the guest booted and halted,
#   * TSORTTEST.BIN loaded,
#   * every check's wait word is printed (PASS path: `[raw=0x…]`,
#     exit-status FAIL path: `(exit status N != M, raw=0x…)`),
#   * how many exit-status failures vs content failures the wave produced.
#
# Usage: bash tools/gate/t3_tsort_only.sh <log-path> [arch]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG="${1:?usage: t3_tsort_only.sh <log-path> [arch]}"
ARCH="${2:-intel}"
SMP="${T3_SMP:-8}"

case "$LOG" in /*) ;; *) LOG="$ROOT/$LOG" ;; esac
mkdir -p "$(dirname "$LOG")"
: > "$LOG"

SRC="$ROOT/obj/$ARCH/tsort_test.bin"
[ -s "$SRC" ] || { echo "t3_tsort_only: FAIL — $SRC missing/empty (build the tree first)"; exit 1; }

STAGE="mcopy -o -i '$ROOT/disk.img' '$SRC' ::TSORT_T.BIN; \
printf 'TSORT_T.BIN\\n' > '$ROOT/obj/$ARCH/tsortonly.wave'; \
mcopy -o -i '$ROOT/disk.img' '$ROOT/obj/$ARCH/tsortonly.wave' ::WAVE.CFG"

(cd "$ROOT" && T3_SMP=$SMP T3_LOG="$LOG" T3_STAGE="$STAGE" \
   T3_TIMEOUT=1200 bash tools/t3_tier.sh "$ARCH" > "${LOG%.log}.tier.txt" 2>&1)
TIER_RC=$?

echo "=== t3_tsort_only ($ARCH, T3_SMP=$SMP): tier rc=$TIER_RC (1-program wave floor FAIL expected) ==="
[ -f "$LOG" ] || { echo "t3_tsort_only: FAIL — serial '$LOG' missing"; exit 1; }

HALTED=$(grep -ac "System halt" "$LOG")
LOADED=$(grep -ac "\[ARRIVED\] TSORT_T.BIN\|\[TSORTTEST\] starting" "$LOG")
CHECKS=$(grep -ac "\[TSORTTEST\] PASS \|\[TSORTTEST\] FAIL " "$LOG")
WORDS=$(grep -aoc "raw=0x[0-9a-f][0-9a-f]*" "$LOG")
CLEAN=$(grep -ao "raw=0x[0-9a-f]*" "$LOG" | grep -c "raw=0x00000000")
EXITFAIL=$(grep -ac "\[TSORTTEST\] FAIL .*(exit status" "$LOG")
STDERRFAIL=$(grep -ac "\[TSORTTEST\] FAIL .*(stderr)" "$LOG")
SUM=$(grep -a "\[TSORTTEST\] FAILURES:" "$LOG" | tail -1)
KFATAL=$(grep -ac "FATAL kernel-mode\|Synchronous Exception in EL1\|halting this CPU" "$LOG")

echo "  halted=$HALTED loaded=$LOADED check_lines=$CHECKS wait_words=$WORDS clean_0x0=$CLEAN"
echo "  exit_status_FAILs=$EXITFAIL stderr_content_FAILs=$STDERRFAIL kfatal=$KFATAL"
echo "  summary: ${SUM:-<none>}"
echo "  word histogram:"
grep -ao "raw=0x[0-9a-f]*" "$LOG" | sort | uniq -c | sed 's/^/    /'
echo "  first 5 raw-bearing FAIL lines:"
grep -a "\[TSORTTEST\] FAIL .*raw=0x" "$LOG" | head -5 | sed 's/^/    /'
echo "  first 5 raw-bearing PASS lines:"
grep -a "\[TSORTTEST\] PASS .*raw=0x\|\[TSORTTEST\] PASS" "$LOG" | head -5 | sed 's/^/    /'

OK=1
[ "$HALTED" -ge 1 ]  || { echo "t3_tsort_only: FAIL — guest did not halt"; OK=0; }
[ "$LOADED" -ge 1 ]  || { echo "t3_tsort_only: FAIL — TSORTTEST.BIN did not load"; OK=0; }
[ "$WORDS" -ge 3 ]   || { echo "t3_tsort_only: FAIL — wait word not printed (instrument absent)"; OK=0; }
[ "$KFATAL" -eq 0 ]  || { echo "t3_tsort_only: FAIL — kernel faulted instead of the harness running"; OK=0; }
[ "$OK" = "1" ] && { echo "t3_tsort_only: serial usable (assertions above)"; exit 0; }
exit 1