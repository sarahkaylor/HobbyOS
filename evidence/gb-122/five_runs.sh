#!/bin/bash
# GB-122 clause 3: five consecutive intel T3 runs at the fix head.
set -u
cd /home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb122 || exit 1
export PATH=/usr/sbin:$PATH
mkdir -p evidence/gb-122
: > evidence/gb-122/runs.txt
for i in 1 2 3 4 5; do
  T3_MONITOR=0 T3_TIMEOUT=300 W0_GATE=0 bash tools/t3_tier.sh intel > "evidence/gb-122/fix-run${i}.log" 2>&1
  rc=$?
  cp obj/t3_intel.log "evidence/gb-122/fix-run${i}.serial.log"
  fatal=$(grep -ac "FATAL kernel-mode fault" "evidence/gb-122/fix-run${i}.serial.log" || true)
  summ=$(grep -a "T3_intel:" "evidence/gb-122/fix-run${i}.log" | head -1)
  echo "run${i} rc=${rc} fatals=${fatal} :: ${summ}" >> evidence/gb-122/runs.txt
  # a stale frozen guest must not block the next run
  for p in $(pgrep -f "qemu-system-x86_64.*gb122" || true); do kill -9 "$p" 2>/dev/null; done
  sleep 2
done
echo DONE >> evidence/gb-122/runs.txt