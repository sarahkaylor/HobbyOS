#!/usr/bin/env bash
#
# tools/selfhost_bench.sh — reproducible in-OS self-host build harness (A/B).
#
# Boots a given HobbyOS disk copy with the canonical ARM QEMU line, MODE
# baked into the disk (the disk's /boot/hobbyos.bin carries the kernel whose
# KERNEL_MODE_SELFHOST branch launches SELFHOST.BIN -> /SRC/SELFHOST.RUN),
# and times the in-OS build by recording wall-clock at every [SELFHOST]
# marker hit in the serial log.  Verdicts come from MARKERS, never from the
# qemu exit code.
#
# Baseline run + protocol: docs/selfhost/selfhost-bench-baseline.md
# (also the pointer for the cache A/B arm; the frozen starting disk copies
# and raw serial logs live under /home/sarah/hobbyos-lanes/blkcache-evidence/
# selfhost/ -- NOT committed).
#
# Usage:
#   tools/selfhost_bench.sh <disk-copy> <out-log>                \
#                           [--timeout <sec>] [--tag <name>]     \
#                           [--qemu /path/to/qemu-system-aarch64]
#       Boots <disk-copy> (a COPY — the run mutates it: the in-OS build
#       writes objects into /SRC), serial into <out-log>, times every
#       marker, kills ONLY its own qemu on exit (trap on PID).
#       --timeout bounds the whole boot+build (default 14400 s = 4 h; a
#       fresh full loop measured 2.5-4 h, an incremental resume ~23-90 min).
#       On timeout: kill qemu, dump the serial tail, verdict=TIMEOUT
#       (report-with-evidence), rc 2.
#       Verdict pass = '[SELFHOST] ACCEPTANCE-COMPLETE' present (rc 0).
#       Staged failure (FAILED-AT / phase=*-fail / ABORT) = rc 1.
#   tools/selfhost_bench.sh --swap-kernel <disk-copy> <flat-kernel.bin>
#       Replace /boot/hobbyos.bin on a disk COPY via mcopy (the cache A/B
#       arm swaps in the cache-enabled kernel this way).  Does NOT run
#       anything.  Guard: refuses to run on a path containing 'pristine'
#       or 'abcopy' (the frozen copies are never mutated).
#
# Evidence emitted next to <out-log>:
#   <out-log>.timing   marker table:  <label> <elapsed-s> <raw line>
#   <out-log>.verdict   one-line verdict + FAIL-token count + md5 of disk
#   <out-log>.qemu.cmd   the exact qemu invocation (reproducibility)
#
# Fail-token scan (the driver's own '-fail'/FAILED-AT stage lines are part
# of its contract and are excluded — same regex as the runner):
#   (?<!-)\bFAIL(?:ED)?\b(?!-AT)
# written in grep -P.  Count is matching LINES (conservative).
#
# Conservative bash: no bashisms beyond [[ ]] and arrays where unavoidable;
# set -u only (no set -e — we handle error paths explicitly).

set -u

# ---------------------------------------------------------------- headers --
VERDICT_PASS="PASS"
VERDICT_FAIL="FAIL"
VERDICT_TIMEOUT="TIMEOUT"
DEFAULT_TIMEOUT=14400

usage() {
  cat <<EOF
usage: $0 <disk-copy> <out-log> [--timeout <sec>] [--tag <name>] [--qemu <path>]
       $0 --swap-kernel <disk-copy> <flat-kernel.bin>
EOF
  exit 3
}

# ------------------------------------------------------------ infrastructure
now_s() { date +%s; }

die() { echo "selfhost_bench: ERROR: $*" >&2; exit 2; }

# --------------------------------------------------------------- swap-kernel
if [ "${1:-}" = "--swap-kernel" ]; then
  [ $# -eq 3 ] || usage
  DISK="$2"
  BIN="$3"
  case "$DISK" in
    *pristine*|*abcopy*) die "refusing to swap kernel into a frozen copy: $DISK";;
  esac
  [ -f "$DISK" ] || die "disk copy not found: $DISK"
  [ -f "$BIN" ] || die "kernel .bin not found: $BIN"
  mcopy -o -i "$DISK" "$BIN" ::/boot/hobbyos.bin || die "mcopy failed"
  echo "selfhost_bench: swapped $BIN -> $DISK ::/boot/hobbyos.bin"
  echo "--- ::/boot ---"
  mdir -i "$DISK" ::/boot 2>&1 | tail -6
  exit 0
fi

[ $# -ge 2 ] || usage
DISK="$1"
LOG="$2"
shift 2

TIMEOUT="$DEFAULT_TIMEOUT"
TAG=""
QEMU_BIN="qemu-system-aarch64"
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT="${2:-}"; [ -n "$TIMEOUT" ] || usage; shift 2;;
    --tag)     TAG="${2:-}";     [ -n "$TAG" ]     || usage; shift 2;;
    --qemu)    QEMU_BIN="${2:-}"; [ -n "$QEMU_BIN" ] || usage; shift 2;;
    *) usage;;
  esac
done

[ -f "$DISK" ] || die "disk copy not found: $DISK"
case "$DISK" in
  *pristine*|*abcopy*) die "refusing to boot a frozen copy directly: $DISK (make a run copy)";;
esac
command -v "$QEMU_BIN" >/dev/null 2>&1 || die "qemu not found: $QEMU_BIN"
command -v mcopy >/dev/null 2>&1 || die "mtools mcopy not found"
command -v grep >/dev/null 2>&1 || die "grep not found"

# ---------------------------------------------------------------- constants
FW="$HOME/.local/share/AAVMF/AAVMF_CODE.fd"
[ -f "$FW" ] || die "AAVMF firmware missing at $FW"

# The canonical ARM QEMU line (Makefile:275 / acceptance.md §5.1) with
# -serial stdio replaced by -serial file:$LOG and -display none (non-desktop
# modes resolve GX_GPU_DISPLAY to none + soft virtio-gpu-device).
QEMU_CMD=(
  "$QEMU_BIN" -M virt -cpu cortex-a53 -smp 8 -m 16384M
  -accel tcg,thread=multi -bios "$FW"
  -display none -serial "file:$LOG"
  -drive "if=none,file=$DISK,format=raw,id=hd0" -device virtio-blk-device,drive=hd0
  -device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device
  -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56
  -semihosting -action shutdown=poweroff
)

# Marker regexes (first occurrence of each distinct marker is timed).
# One regex per line: <label>|grep -P pattern ... handled by a case table.
pick_label() {
  # echo the label for a matching raw line, or empty
  case "$1" in
    *"Mode: SELFHOST"*)                    echo "boot-mode-selfhost";;
    *"DRIVER-UP"*)                         echo "driver-up";;
    *"phase=s0-check-start"*)              echo "s0-check-start";;
    *"phase=s0-check-done"*)               echo "s0-check-done";;
    *"phase=s1-user-start"*)               echo "s1-user-start";;
    *"phase=s1-user-done"*)                echo "s1-user-done";;
    *"phase=s2-kernel-start"*)             echo "s2-kernel-start";;
    *"phase=s2-kernel-done"*)              echo "s2-kernel-done";;
    *"phase=s3-disk-start"*)               echo "s3-disk-start";;
    *"phase=s3-disk-done"*)                echo "s3-disk-done";;
    *"ACCEPTANCE-COMPLETE"*)               echo "acceptance-complete";;
    *"[TAR] done:")                        echo "tar-done";;
    *"System halt."*)                      echo "system-halt";;
    *"FAILED-AT"*)                         echo "failed-at";;
    *"phase="*"-fail"*)                    echo "stage-fail";;
    *"ABORT"*)                             echo "abort";;
    *)                                     echo "";;
  esac
}

# ------------------------------------------------------------ preflight log
rm -f "$LOG" "$LOG.timing" "$LOG.verdict" "$LOG.qemu.cmd"
printf '%s\n' "${QEMU_CMD[@]}" > "$LOG.qemu.cmd"

DISK_MD5="$(md5sum "$DISK" | awk '{print $1}')"
echo "selfhost_bench: booting $DISK (md5 $DISK_MD5) tag=$TAG timeout=${TIMEOUT}s"
echo "selfhost_bench: qemu line -> $LOG.qemu.cmd"

# ---------------------------------------------------------------- run qemu
"${QEMU_CMD[@]}" &
QPID=$!
T0=$(now_s)
DEADLINE=$((T0 + TIMEOUT))

cleanup() {
  if kill -0 "$QPID" 2>/dev/null; then
    kill "$QPID" 2>/dev/null
    # brief grace for the kill to land; no wait on purpose
    sleep 1
    if kill -0 "$QPID" 2>/dev/null; then kill -9 "$QPID" 2>/dev/null; fi
  fi
}
trap cleanup EXIT

# ------------------------------------------------------------- marker poll
# Line-incremental poll: complete lines only (wc -l counts newlines, so the
# possibly-unterminated last line is naturally deferred to the next poll).
# This avoids the command-substitution newline-stripping trap of byte-based
# reads (a chunk boundary newline was being dropped, gluing two lines).
POS=0           # lines consumed so far
MARKERS=()      # "elapsed label rawline" entries, first occurrence per label
RECORDED=""     # space-separated labels already recorded (dedupe)

poll_once() {
  local now_lines new_lines skip
  now_lines=$(wc -l < "$LOG" 2>/dev/null || echo 0)
  [ "$now_lines" -gt "$POS" ] || return 0
  new_lines=$((now_lines - POS))
  skip=$((POS + 1))
  POS=$now_lines
  while IFS= read -r line; do
    label=$(pick_label "$line")
    [ -n "$label" ] || continue
    # first occurrence per label only (the driver AND the make recipe echo
    # the same stage names; the earliest hit is the stage timing datum)
    case " ${RECORDED:-} " in
      *" $label "*) continue;;
    esac
    RECORDED="$RECORDED $label"
    MARKERS+=("$(( $(now_s) - T0 ))|$label|$line")
  done < <(tail -n +"$skip" "$LOG" 2>/dev/null | head -n "$new_lines")
}

# ---------------------------------------------------------------- the loop
ACCEPT_SEEN=0
VERDICT=""
QEMU_EXITED=0

while :; do
  poll_once
  if ! kill -0 "$QPID" 2>/dev/null; then
    QEMU_EXITED=1
    wait "$QPID" 2>/dev/null
    QRC=$?
    # final poll once more after exit (drains buffered output)
    sleep 1
    poll_once
    break
  fi
  # acceptance complete: give the guest time to drain + poweroff (bounded)
  if [ "$ACCEPT_SEEN" -eq 0 ]; then
    if [ -n "${MARKERS[*]:-}" ] && printf '%s\n' "${MARKERS[@]}" | grep -q 'acceptance-complete'; then
      ACCEPT_SEEN=1
      echo "selfhost_bench: ACCEPTANCE-COMPLETE seen, waiting up to 180s for guest halt"
      DEADLINE=$(( $(now_s) + 180 ))
    fi
  fi
  NOW=$(now_s)
  if [ "$NOW" -ge "$DEADLINE" ]; then
    break
  fi
  sleep 5
done

ELAPSED=$(( $(now_s) - T0 ))

# ------------------------------------------------------------- the verdict
# Acceptance is derived from the marker table (not from the in-loop flag,
# which can be skipped when qemu exits in the same poll as the marker).
if [ "$ACCEPT_SEEN" -eq 1 ] || printf '%s\n' "${MARKERS[@]:-}" | grep -q 'acceptance-complete'; then
  ACCEPT_SEEN=1
fi
if [ "$ACCEPT_SEEN" -eq 1 ]; then
  VERDICT="$VERDICT_PASS"
elif printf '%s\n' "${MARKERS[@]:-}" | grep -qE 'failed-at|stage-fail|abort'; then
  VERDICT="$VERDICT_FAIL"
elif [ "$QEMU_EXITED" -eq 1 ]; then
  VERDICT="$VERDICT_FAIL"     # qemu ended without acceptance marker
else
  VERDICT="$VERDICT_TIMEOUT"
fi

# Fail-token scan (runner regex; excludes the driver's own -fail/FAILED-AT)
# grep -Pc prints 0 AND exits 1 when there are no matches; capture the
# count without a stray second "0" from ||.
FAIL_COUNT=$(grep -Pc '(?<!-)\bFAIL(?:ED)?\b(?!-AT)' "$LOG" 2>/dev/null)
FAIL_COUNT=${FAIL_COUNT:-0}

# ------------------------------------------------------------------ print
echo
echo "=== selfhost_bench: $VERDICT (tag=$TAG) in ${ELAPSED}s ==="
echo "  disk md5  : $DISK_MD5"
echo "  fail tokens (runner regex, lines): $FAIL_COUNT"

{
  echo "label|elapsed_s|line"
  printf '%s\n' "${MARKERS[@]:-}" | sort -t'|' -k2 -n
} > "$LOG.timing"

echo "=== marker timing table (first occurrence), absolute-from-boot s ==="
printf '%s\n' "${MARKERS[@]:-}" | sort -t'|' -k2 -n

if [ "$VERDICT" = "$VERDICT_TIMEOUT" ]; then
  echo
  echo "=== TIMEOUT after ${ELAPSED}s (deadline ${TIMEOUT}s) — serial tail (last 40 lines) ==="
  tail -n 40 "$LOG" 2>/dev/null
  echo "=== qemu state ==="
  ps -o pid,stat,etime,cmd -p "$QPID" 2>/dev/null || echo "  qemu not running (exited on its own before the deadline)"
fi

if [ "$FAIL_COUNT" -gt 0 ]; then
  echo
  echo "=== FAIL-token lines (non-driver) ==="
  grep -Pn '(?<!-)\bFAIL(?:ED)?\b(?!-AT)' "$LOG" 2>/dev/null | head -20
fi

{
  echo "verdict=$VERDICT elapsed=${ELAPSED}s tag=$TAG"
  echo "disk_md5=$DISK_MD5"
  echo "fail_token_lines=$FAIL_COUNT"
  echo "acceptance_complete=$ACCEPT_SEEN"
  echo "qemu_exited=$QEMU_EXITED qemu_rc=${QRC:-n/a}"
  echo "timeout_sec=$TIMEOUT"
} > "$LOG.verdict"
echo "selfhost_bench: evidence in $LOG.verdict $LOG.timing $LOG.qemu.cmd"

case "$VERDICT" in
  "$VERDICT_PASS")     exit 0;;
  "$VERDICT_FAIL")     exit 1;;
  *)                   exit 2;;
esac
