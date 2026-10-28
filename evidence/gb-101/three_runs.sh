#!/bin/bash
# GB-101 leg A: three consecutive intel T3 runs at origin/main 4626084.
set -u
cd /home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb100 || exit 1
export PATH=/usr/sbin:$PATH
mkdir -p evidence/gb-101
for i in 1 2 3; do
  T3_MONITOR=1 T3_TIMEOUT=900 W0_GATE=0 bash tools/t3_tier.sh intel > "evidence/gb-101/run${i}.log" 2>&1
  echo "run${i} RC=$?" >> evidence/gb-101/runs.rc
  cp obj/t3_intel.log "evidence/gb-101/run${i}.serial.log"
done
echo DONE >> evidence/gb-101/runs.rc