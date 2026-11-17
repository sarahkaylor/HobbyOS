#!/usr/bin/env bash
# tools/jdk/boot.sh -- boot the CURRENT disk.img with a deadline and capture the
# serial log.  Never rebuilds: staging is a separate step, because a guest must
# not read a disk while it is still being written (tools/rw/mkdisk-rimworld.sh).
#
# Usage: tools/jdk/boot.sh [--timeout S] [--log PATH] [--marker REGEX]
# Exits 0 when the marker (default "System halt") appears in the log, 3 on the
# deadline.  Always kills only ITS OWN qemu pid: other lanes may be running.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# --- refuse to boot a kernel that predates its sources --------------------
# Three measurement rounds in this lane were polluted by booting a stale kernel:
# boot.sh used to load the ELF from inside disk.img (a rebuild that never
# repacked was invisible), a failed build left the previous ELF in place (the
# fd_cloexec array assignment), and a broken refactor left 11 compile errors.
# Every one of those boots looked NORMAL: rc=0, "System halt", plausible output.
# So: if any kernel source is newer than the built ELF, this is not a rebuild and
# the run is not evidence.  --allow-stale / ALLOW_STALE_KERNEL=1 overrides, for
# the deliberate case of booting an older image on purpose.
KERNEL_ELF="$ROOT/hobbyos.elf"
allow_stale="${ALLOW_STALE_KERNEL:-0}"
for _arg in "$@"; do [ "$_arg" = "--allow-stale" ] && allow_stale=1; done
if [ -f "$KERNEL_ELF" ] && [ "$allow_stale" != "1" ]; then
  NEWER="$(find "$ROOT/src" \( -name '*.c' -o -name '*.h' -o -name '*.S' \) \
             -newer "$KERNEL_ELF" -print -quit 2>/dev/null)"
  if [ -n "$NEWER" ]; then
    echo "boot.sh: REFUSING to boot -- a kernel source is newer than hobbyos.elf:" >&2
    echo "  $NEWER" >&2
    echo "  The build did not run or did not finish; anything this boot prints" >&2
    echo "  would describe the PREVIOUS kernel.  Run make first, or pass" >&2
    echo "  --allow-stale if booting the old image is the point." >&2
    exit 4
  fi
fi
cd "$ROOT" || exit 2

TIMEOUT_S=900
LOG="$ROOT/obj/jdk/serial.log"
MARKER='System halt'
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT_S=$2; shift 2 ;;
    --log)     LOG=$2; shift 2 ;;
    --marker)  MARKER=$2; shift 2 ;;
    --allow-stale) shift ;;   # handled above; accepted here so it is not "unknown"
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done
mkdir -p "$(dirname "$LOG")"
: > "$LOG"

# The x86_64 command line from the Makefile (QEMU_CMD), minus make's plumbing.
# -enable-kvm is deliberately absent: it is FATAL on a QEMU without KVM, and
# this host is an Apple-silicon Mac (TCG only).
EDK2="${EDK2_X86_64:-/opt/homebrew/share/qemu/edk2-x86_64-code.fd}"
[ -f "$EDK2" ] || { echo "no edk2 x86_64 firmware at $EDK2"; exit 2; }
SMP="${QEMU_SMP:-8}"
MEM="${QEMU_MEM:-12288M}"

qemu-system-x86_64 -M q35,vmport=on -smp "$SMP" -m "$MEM" \
  -pflash "$EDK2" -display none -serial "file:$LOG" \
  -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
  -drive file=disk.img,format=raw,id=disk0,if=none \
  -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
  -device virtio-tablet-pci \
  ${QEMU_DEBUG:+-d "$QEMU_DEBUG" -D "${QEMU_DEBUG_LOG:-obj/jdk/qemu-debug.log}"} \
  -action shutdown=poweroff &
QPID=$!
trap 'kill -TERM $QPID 2>/dev/null; wait $QPID 2>/dev/null' EXIT

deadline=$(( $(date +%s) + TIMEOUT_S ))
rc=3
while kill -0 $QPID 2>/dev/null; do
  if grep -aqE "$MARKER" "$LOG" 2>/dev/null; then rc=0; break; fi
  [ "$(date +%s)" -ge "$deadline" ] && { rc=3; break; }
  sleep 1
done
# A guest that halts cleanly exits QEMU by itself (-action shutdown=poweroff),
# so losing the pid is not by itself a failure: judge on the marker, which is
# the only thing that says the run got where it was going.
if grep -aqE "$MARKER" "$LOG" 2>/dev/null; then rc=0; fi
kill -TERM $QPID 2>/dev/null
# A TCG guest that is spinning inside the kernel does not always act on TERM:
# the script then blocks in `wait` forever and the deadline means nothing.
# Escalate, so every boot really ends at its deadline.
for _ in 1 2 3 4 5 6; do
  kill -0 $QPID 2>/dev/null || break
  sleep 1
done
kill -KILL $QPID 2>/dev/null
wait $QPID 2>/dev/null
trap - EXIT
echo "boot: rc=$rc log=$LOG lines=$(wc -l < "$LOG")"
exit $rc
