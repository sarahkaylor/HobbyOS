#!/usr/bin/env bash
# GB-103-V clause 3 — the negative control: kill QEMU while probe_guest() samples.
# Uses the INTEL tier because its waves wedge (halted=0), so the sample really is
# taken with QEMU alive; the ARM waves on this box halt before the sample point.
set -u
cd "$(dirname "$0")/../.." || exit 1
export PATH="$PATH:/usr/sbin:/sbin"
ARCH="${1:-intel}"
TIMEOUT="${2:-240}"
LOG=obj/t3_gb103v_sample_$ARCH.log
OUT=evidence/gb-103-v/02_midsample_$ARCH.log
rm -f "$LOG"

T3_MONITOR=1 T3_TIMEOUT="$TIMEOUT" T3_LOG="$LOG" bash tools/t3_tier.sh "$ARCH" >"$OUT" 2>&1 &
TIER=$!

for _ in $(seq 1 400); do
  grep -aq "=== T3 guest sample" "$LOG" 2>/dev/null && break
  sleep 1
done
echo "sample header seen in $LOG at $(date -u +%H:%M:%S)"

QPID=$(pgrep -f "qemu-system-x86_64" | head -1)
echo "SIGTERM -> qemu pid ${QPID:-none} at $(date -u +%H:%M:%S)"
[ -n "${QPID:-}" ] && kill -TERM "$QPID" 2>/dev/null

wait "$TIER"
echo "tier rc = $?"
echo "qemu was alive at the sample: $([ -n "${QPID:-}" ] && echo True || echo False)"
echo "sample header present:      $(grep -aq '=== T3 guest sample' "$LOG" && echo True || echo False)"
echo "'end guest sample' present: $(grep -aq '=== end guest sample' "$LOG" && echo True || echo False)"
echo "INSTRUMENT-FAILURE present: $(grep -aq 'INSTRUMENT-FAILURE' "$LOG" "$OUT" && echo True || echo False)"
echo "verdict line present:       $(grep -aqE "T3_${ARCH}: (PASS|FAIL)|T3_${ARCH} EXIT=" "$OUT" && echo True || echo False)"
echo
echo "--- verdict lines"
grep -aE "T3_${ARCH}:" "$OUT" | head -4
echo "--- INSTRUMENT-FAILURE lines (log then tier output)"
grep -a "INSTRUMENT-FAILURE" "$LOG" "$OUT" 2>/dev/null | head -3 || echo "(none)"