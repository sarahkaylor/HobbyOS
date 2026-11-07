#!/usr/bin/env bash
# tools/rw/l75-ladder.sh — the L75 acceptance windows.
#
# Two 1800 s windows of the desktop harness with the game, back to back, each
# with the QEMU NVMe trace the lane needs (every CC write, every controller
# stop/start, every guest-error tracepoint) written to obj/l75/traceL<N>.log.
#
# Sequential on purpose: one disk.img, one QEMU.  Never run the unit tier at
# the same time (run_unit_tests_intel.sh shares disk.img and the QEMU name).
#
# Usage: tools/rw/l75-ladder.sh [N ...]        (default: 1 2)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
OUT="$ROOT/obj/l75"
mkdir -p "$OUT"

TRACE_ARGS=(
  -trace enable=pci_nvme_mmio_cfg
  -trace enable=pci_nvme_mmio_stopped
  -trace enable=pci_nvme_mmio_start_success
  -trace enable=pci_nvme_pci_reset
  -trace enable=nvme_error
  -trace 'enable=pci_nvme_err_*'
  -trace 'enable=pci_nvme_ub_*'
)

run_one() {
  local n="$1"
  local log="$OUT/runL$n.log"
  local trace="$OUT/traceL$n.log"
  rm -f "$trace"
  echo "== window $n: $(date -Is) -> $log"
  RW_WAIT_SECS=1900 \
  RW_QEMU_EXTRA="-D $trace ${TRACE_ARGS[*]}" \
    tools/rw/run-rimworld-desktop.sh --timeout 600 --run-timeout 1800 \
    >"$log" 2>&1
  local rc=$?
  echo "== window $n done rc=$rc $(date -Is)"
  echo "--- window $n summary ---"
  python3 tools/rw/l75-summary.py "$ROOT/obj/rw-desktop/serial-desktop.log" \
          --json "$OUT/summary-L$n.json" | sed 's/^/  /'
  cp -f "$ROOT/obj/rw-desktop/serial-desktop.log" "$OUT/serial-L$n.log"
  cp -f "$trace" "$OUT/trace-L$n.log" 2>/dev/null
  return $rc
}

if [ $# -eq 0 ]; then
  set -- 1 2
fi
for n in "$@"; do
  run_one "$n"
done
