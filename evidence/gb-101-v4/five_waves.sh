#!/bin/bash
# GB-101-V4 clause 4: five intel T3 waves at the graded head.
set -u
cd /home/sarah/Documents/GitHub/HobbyOS/.worktrees/verify-gb101v4 || exit 1
export PATH=/usr/sbin:$PATH
: > evidence/gb-101-v4/waves.txt
for i in 1 2 3 4 5; do
  T3_MONITOR=0 T3_TIMEOUT=300 W0_GATE=0 bash tools/t3_tier.sh intel > "evidence/gb-101-v4/wave${i}.log" 2>&1
  cp obj/t3_intel.log "evidence/gb-101-v4/wave${i}.serial.log"
  echo "wave${i} rc=$?" >> evidence/gb-101-v4/waves.txt
  for p in $(pgrep -f "qemu-system-x86_64.*verify-gb101v4" || true); do kill -9 "$p" 2>/dev/null; done
  sleep 2
done
echo DONE >> evidence/gb-101-v4/waves.txt