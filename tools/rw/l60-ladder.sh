#!/usr/bin/env bash
# M6/L60 ladder: three 600 s runs of one build, sequential (never two boots).
# Each run's serial log and shots are copied out before the next run reuses
# obj/rw-desktop.
set -uo pipefail
export HOME=/home/sarah
ROOT=/home/sarah/hobbyos-rw-lanes/l60-ud
cd "$ROOT" || exit 2
mkdir -p obj/l60
for r in 1 2 3; do
  echo "=== ladder run $r $(date -Is)"
  tools/rw/run-rimworld-desktop.sh --run-timeout 600 > "obj/l60/ladder-$r.out" 2>&1
  echo "run $r exit=$?"
  cp -f obj/rw-desktop/serial-desktop.log "obj/l60/serial-l60-$r.log" 2>/dev/null
  cp -f obj/rw-desktop/Player.log "obj/l60/Player-l60-$r.log" 2>/dev/null
  rm -rf "obj/l60/shots-$r"; cp -a obj/rw-desktop/shots "obj/l60/shots-$r" 2>/dev/null
  grep -E 'Vector: 6|\[TXT\]|\[UDX\]|\[BLKQ\] controller|\[MMF\]' \
       "obj/l60/serial-l60-$r.log" | tail -20 > "obj/l60/highlights-$r.txt"
  cat "obj/l60/highlights-$r.txt"
done
echo "=== ladder done $(date -Is)"
