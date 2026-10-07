#!/usr/bin/env bash
# run_rp.sh — render-pipeline meta-runner.
#
# Re-runs any render-pipeline probe against ANY WebProcess build, so rp-f's
# checkpoints get re-verified with the SAME probes and the same baseline
# disks.  Pin the ELF with --wp-elf (its sha256 is recorded in every
# report and the baselines doc).
#
# Usage:
#   run_rp.sh <probe> --wp-elf PATH [probe args...]
#              [--instance ID] [--port N]
#
#   probe:   perf          tile-bleed perf scenario (HOME+TALL receipts)
#            tile-bleed    tile-bleed probe (default scenario gentle)
#            resp-close    close-responsiveness probe (default midload)
#
#   --instance ID  instance-scope id for /tmp sockets (default rp-h).
#                  Two lanes CAN run in parallel if they use distinct
#                  --instance AND distinct --port (socket/lock scoping).
#   --port N       host port base for the run (scope token; no listener on
#                  the host unless a probe adds one).
#
# Environment:
#   RP_OS_TREE      OS scratch with hobbyos.elf + obj/arm/*.bin built
#                   (default /home/sarah/hobbyos-scratch/rp-h-os)
#   RP_FIXTURES     fixture dir with HOME.HTM/TALL.HTM
#                   (default .../wk5/fixtures)
#   RP_SMP / RP_MEM_MB   QEMU sizing (default 4 / 4096)
#
# Exit codes: 0 = probe completed (verdict in report.json), 1 = probe
# step/analysis failure, 2 = usage error, 3 = lock/instance error.
#
# Example:
#   ./run_rp.sh perf --wp-elf ~/webkit-hobbyos/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess
#   ./run_rp.sh tile-bleed --wp-elf PATH --scenario load --instance rp-h --port 8811
set -u

PROBE="${1:-}"
[ -n "$PROBE" ] || { echo "usage: run_rp.sh <probe> --wp-elf PATH [--instance ID --port N] [probe args...]"; exit 2; }
shift

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PIPE="$REPO/tools/render_pipeline"
WP_ELF=""
INSTANCE="rp-h"
PORT=""

# --- parse the common runner args (rest passes through to the probe) -----
REST=()
while [ $# -gt 0 ]; do
  case "$1" in
    --wp-elf) WP_ELF="$2"; shift 2 ;;
    --instance) INSTANCE="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    *) REST+=("$1"); shift ;;
  esac
done
[ -n "$WP_ELF" ] || { echo "run_rp.sh: --wp-elf PATH is required"; exit 2; }
[ -f "$WP_ELF" ] || { echo "run_rp.sh: WebProcess ELF not found: $WP_ELF"; exit 2; }

case "$PROBE" in
  perf) PY="$PIPE/tile_bleed_probe.py"; EXTRA=(--scenario perf) ;;
  tile-bleed) PY="$PIPE/tile_bleed_probe.py"; EXTRA=() ;;
  resp-close) PY="$PIPE/resp_close_probe.py"; EXTRA=() ;;
  *) echo "run_rp.sh: unknown probe '$PROBE' (perf|tile-bleed|resp-close)"; exit 2 ;;
esac

# --- instance scoping: distinct --instance AND --port -> parallel safe ----
LK="/tmp/rp-${INSTANCE}${PORT:+-}${PORT}.lock"
exec 9>"$LK" || { echo "run_rp.sh: cannot open lock $LK"; exit 3; }
flock -n 9 || { echo "run_rp.sh: instance '$INSTANCE' locked (another run active)"; exit 3; }

TS="$(date +%Y%m%d-%H%M%S)"
EVDIR="${RP_EVDIR:-$PIPE/evidence/$PROBE-$INSTANCE-$TS}"
mkdir -p "$EVDIR"

: "${RP_OS_TREE:=/home/sarah/hobbyos-scratch/rp-h-os}"
: "${RP_FIXTURES:=/home/sarah/webkit-hobbyos/HobbyOS/continuation/wk5/fixtures}"
: "${RP_SMP:=4}"
: "${RP_MEM_MB:=4096}"

echo "== run_rp: probe=$PROBE instance=$INSTANCE port=${PORT:-none}"
echo "== run_rp: wp-elf=$WP_ELF"
echo "== run_rp: sha256=$(sha256sum "$WP_ELF" | cut -d' ' -f1)"
echo "== run_rp: evdir=$EVDIR"
echo "== run_rp: os_tree=$RP_OS_TREE fixtures=$RP_FIXTURES"

python3 "$PY" --wp-elf "$WP_ELF" --os-tree "$RP_OS_TREE" \
    --fixtures-root "$RP_FIXTURES" --evdir "$EVDIR" --instance "$INSTANCE" \
    --smp "$RP_SMP" --mem-mb "$RP_MEM_MB" "${EXTRA[@]}" "${REST[@]}"
RC=$?

echo "== run_rp: rc=$RC report=$EVDIR/report.json"
exit $RC
