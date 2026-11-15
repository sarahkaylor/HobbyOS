#!/usr/bin/env bash
# tools/cp/p0host/batch-perf.sh <prefix> — the measured perf runs: 720p and 1080p, twice each,
# alternating.  Each run takes ~/cp/measure.lock itself (run.sh), so another lane can interleave.
set -u
. "$(dirname "$0")/env.sh"
p=${1:-perf}
for rep in a b; do
  for res in "1280 720" "1920 1080"; do
    set -- $res
    MANGO=1 "$T/run.sh" "${p}-${2}p-${rep}" "$1" "$2" --bench --menu-log 30 --timeout 900 \
      > "$P0/runs/${p}-${2}p-${rep}.out" 2>&1
    sleep 5
  done
done
echo "batch $p done $(date -Is)" >> "$P0/runs/batch.log"
