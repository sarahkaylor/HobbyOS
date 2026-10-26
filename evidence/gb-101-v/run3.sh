#!/usr/bin/env bash
# GB-101-V clause 3: three consecutive intel waves under the QEMU monitor.
# Run detached:  setsid nohup bash evidence/gb-101-v/run3.sh >/dev/null 2>&1 &
set -u
cd "$(dirname "$0")/../.." || exit 1
OUT=evidence/gb-101-v
export PATH="$PATH:/usr/sbin:/sbin"
for i in 1 2 3; do
  T3_MONITOR=1 T3_TIMEOUT=1500 T3_LOG="obj/t3_gb101v_run$i.log" \
    bash tools/t3_tier.sh intel >"$OUT/1${i}_run$i.log" 2>&1
  echo "RUN${i}_EXIT=$?" >>"$OUT/1${i}_run$i.log"
  sleep 5
done
echo ALL_RUNS_DONE >"$OUT/runs_done.flag"