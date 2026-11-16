#!/usr/bin/env bash
# tools/cp/qemu-slot.sh -- the gate host's QEMU slots (docs/cyberpunk.md §7.4).
#
#   bash tools/cp/qemu-slot.sh CMD [ARGS...]   run CMD holding one slot
#   . tools/cp/qemu-slot.sh; cp_qemu_slot_acquire
#                                              take a slot for the rest of
#                                              the current shell (fd 197)
#
# The gate host allows CP_QEMU_SLOTS boots at once (default 4: the user allowed
# up to four 12 GiB guests on strix).  Slot 1 is /tmp/cp-qemu.lock, the lock
# every script used before slots existed, so a legacy `flock /tmp/cp-qemu.lock
# ...` still counts as one slot and never runs beside a fifth guest.  Slots
# 2..N are /tmp/cp-qemu.lock.<n>.
#
# A command that already holds a slot (CP_QEMU_SLOT_HELD set by an outer
# qemu-slot.sh) runs directly, so nesting never takes two slots or deadlocks.
# The lock fd is inherited by CMD and everything it starts (QEMU included), so
# the slot stays held until the last of them exits -- same as flock(1).

cp_qemu_slot_acquire() {
  [ -n "${CP_QEMU_SLOT_HELD:-}" ] && return 0
  local n="${CP_QEMU_SLOTS:-4}" i L waited=0
  while :; do
    for i in $(seq 1 "$n"); do
      if [ "$i" = 1 ]; then L=/tmp/cp-qemu.lock; else L=/tmp/cp-qemu.lock.$i; fi
      exec 197>>"$L" || continue
      if flock -n 197; then
        export CP_QEMU_SLOT_HELD="$i"
        echo "qemu-slot: slot $i of $n held by pid $$ $(date -u +%T)" >&2
        return 0
      fi
      exec 197>&-
    done
    [ "$waited" = 1 ] || echo "qemu-slot: all $n slots busy; waiting" >&2
    waited=1
    sleep 3
  done
}

# Sourced: only define the function.
if [ "${BASH_SOURCE[0]}" != "$0" ]; then return 0 2>/dev/null; fi

set -u
[ $# -gt 0 ] || { echo "usage: qemu-slot.sh CMD [ARGS...]" >&2; exit 2; }
cp_qemu_slot_acquire
"$@"
