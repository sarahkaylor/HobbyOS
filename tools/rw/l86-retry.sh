#!/usr/bin/env bash
# l86-retry.sh -- run the icewm RimWorld harness until the L86 input driver
# gets through the front end.
#
# Why: the guest has a pre-existing intermittent idle stall during def loading
# ([IDLESTUCK] with every game thread parked in a futex, ~330-400 s of
# presents).  It is not caused by input and not this lane's to fix, but it
# kills the whole boot, so a lane that needs the menu has to retry.  Each
# attempt is a full boot + ~10 min of def loading.
#
# Usage: tools/rw/l86-retry.sh [attempts] [run-timeout]
set -u
cd "$(dirname "$0")/../.." || exit 2
export HOME=/home/sarah

N="${1:-4}"
RT="${2:-2400}"
for i in $(seq 1 "$N"); do
  echo "=== attempt $i of $N  $(date +%H:%M:%S)"
  fuser -k disk.img >/dev/null 2>&1
  sleep 3
  rm -rf obj/rw-desktop/shots obj/rw-desktop/serial-desktop.log
  RW_L86=1 RW_SKIP_BUILD=1 RW_QEMU_GL=1 RW_GLMODE=virgl \
    RW_SCREEN=1280x720 RW_WM_HONOUR=1 \
    bash tools/rw/run-rimworld-desktop.sh --run-timeout "$RT" \
    > "obj/l86/attempt-$i.log" 2>&1
  if grep -q '"ev": "done"' "obj/l86/attempt-$i.log" 2>/dev/null; then
    echo "=== attempt $i: driver finished"
    mkdir -p "obj/l86/attempt-$i"
    cp -a obj/rw-desktop/shots "obj/l86/attempt-$i/shots" 2>/dev/null
    cp -a obj/rw-desktop/serial-desktop.log "obj/l86/attempt-$i/" 2>/dev/null
    cp -a obj/rw-desktop/Player.log "obj/l86/attempt-$i/" 2>/dev/null
    if grep -q 'reached_site=True' "obj/l86/attempt-$i.log"; then
      echo "=== attempt $i: REACHED SITE -- stopping"
      exit 0
    fi
  fi
  if grep -q 'STALLED' "obj/l86/attempt-$i.log"; then
    echo "=== attempt $i: guest idle stall (pre-existing); retrying"
  fi
done
echo "=== no attempt reached the site screen"
exit 1
