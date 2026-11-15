#!/usr/bin/env bash
# tools/cp/ext2b/probe_parity.sh <tree> <outdir> [probe...]
# Run linux probes (default: every src/user/linux_probe_*.c except ext2/ext2b,
# which tools/cp/ext2_gate.sh covers) in <tree>, one boot each under
# flock /tmp/cp-qemu.lock, no data disk attached (CPDATA=).  One summary line
# per probe in <outdir>/summary.txt: "<name> rc=<n> <gate verdict>".
set -u
T="$(cd "$1" && pwd)"; O="$2"; shift 2
mkdir -p "$O"; O="$(cd "$O" && pwd)"
cd "$T"
names=("$@")
if [ ${#names[@]} = 0 ]; then
  for f in src/user/linux_probe_*.c; do
    n=$(basename "$f" .c); n=${n#linux_probe_}
    case "$n" in ext2|ext2b) continue ;; esac
    names+=("$n")
  done
fi
: > "$O/summary.txt"
for n in "${names[@]}"; do
  CPDATA= LP_OUT="$O/$n.console.log" LP_TIMEOUT="${LP_TIMEOUT:-300}" TOOLS_SKIP=1 \
    flock /tmp/cp-qemu.lock bash tools/gate/t3_linux_probe.sh "$n" > "$O/$n.gate.log" 2>&1
  echo "$n rc=$? $(tail -1 "$O/$n.gate.log")" | tee -a "$O/summary.txt"
done
