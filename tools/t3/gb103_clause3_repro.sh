#!/usr/bin/env bash
# Replicate the GB-103 clause-3 RED: T3_MONITOR=1 run, SIGTERM the live qemu at
# the sample header; the fix must emit INSTRUMENT-FAILURE and still grade a verdict.
set -u
cd /home/sarah/Documents/GitHub/HobbyOS/.worktrees/hermes-830a858d
T3_LOG_ARM="${T3_LOG:-obj/t3_arm.log}"
T3_MONITOR=1 W0_GATE=0 T3_TIMEOUT=150 bash tools/t3_tier.sh arm >/tmp/gb103_repro_run.log 2>&1 &
TIPID=$!
echo "repro: tier pid $TIPID; watching $T3_LOG_ARM for the sample header..."
for i in $(seq 1 200); do
  [ -f "$T3_LOG_ARM" ] && grep -q "=== T3 guest sample" "$T3_LOG_ARM" && break
  sleep 0.3
done
Q=$(pgrep -f 'qemu-system' | head -1)
echo "repro: qemu pid=${Q:-none}"
if [ -n "$Q" ]; then kill -TERM "$Q"; echo "repro: SIGTERM sent to qemu $Q"; fi
wait "$TIPID"
echo "repro: tier-exit=$?"
echo "=== sample header present? ==="; grep -n "=== T3 guest sample" "$T3_LOG_ARM" | head -3
echo "=== INSTRUMENT-FAILURE present? ==="; grep -n "INSTRUMENT-FAILURE" "$T3_LOG_ARM" | head -3 || echo "(none)"
echo "=== verdict ==="; grep "T3_arm EXIT=" "$T3_LOG_ARM" || echo "(no verdict line!)"
