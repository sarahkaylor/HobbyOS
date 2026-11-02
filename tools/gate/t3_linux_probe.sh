#!/usr/bin/env bash
# tools/gate/t3_linux_probe.sh <probe-name> — run ONE Linux-ABI probe ELF in the
# intel guest and print what it said.
#
# A probe is src/user/linux_probe_<name>.c: an ordinary musl Linux program (no
# HobbyOS header), built static-PIE by the Makefile's probe pattern rule.  The
# guest boots in test mode with the probe as the ONLY wave member, so its stdout
# is the console and the run costs one boot (~30-60 s under KVM).
#
# The probe's own protocol: print rows prefixed "[LP] " and finish with
# "[LP] done rc=<n>".  This gate exits 0 only when that final marker says rc=0,
# so a probe that dies mid-way (fault, halt, ENOSYS loop) can never read as a
# pass.  Everything else in the console is printed for the operator: the
# [LCOMPAT] ENOSYS records name the syscall that is still missing.
#
# Env:  LP_OUT     console path     (default obj/linux_probe_<name>.log)
#       LP_TIMEOUT deadline seconds (default 300)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NAME="${1:?usage: t3_linux_probe.sh <probe-name>   (src/user/linux_probe_<name>.c)}"
ELF="$ROOT/obj/intel/probe_$NAME.elf"
OUT="${LP_OUT:-$ROOT/obj/linux_probe_$NAME.log}"
TIMEOUT_S="${LP_TIMEOUT:-300}"

[ -f "$ROOT/src/user/linux_probe_$NAME.c" ] || {
  echo "t3_linux_probe: no such probe src/user/linux_probe_$NAME.c"; exit 2; }
if [ ! -f "$ELF" ]; then
  echo "t3_linux_probe: building $ELF"
  make -C "$ROOT" ARCH=intel "obj/intel/probe_$NAME.elf" || {
    echo "t3_linux_probe: BUILD FAILED"; exit 2; }
fi

CFG="$(mktemp)"
printf 'PROBE.ELF\n' > "$CFG"
# Stage AFTER the tier's disk build (t3_tier regenerates disk.img first; a file
# copied onto it beforehand is wiped -- the measured "probe tested nothing" mode).
STAGE="mcopy -o -i \$ROOT/disk.img $ELF ::PROBE.ELF && mcopy -o -i \$ROOT/disk.img $CFG ::WAVE.CFG"
echo "=== t3_linux_probe: $NAME (deadline ${TIMEOUT_S}s, log $OUT) ==="
T3_TIMEOUT="$TIMEOUT_S" T3_STAGE="$STAGE" T3_MIN_LINES=1 \
  bash "$ROOT/tools/t3_tier.sh" intel > "$OUT" 2>&1
RC=$?
CONSOLE="$(sed -n 's/^T3_LOG=//p' "$OUT" | tail -1)"
rm -f "$CFG"
if [ -z "$CONSOLE" ] || [ ! -r "$CONSOLE" ]; then
  echo "t3_linux_probe: FAIL — the tier printed no readable T3_LOG (boot itself failed)"
  tail -20 "$OUT"
  exit 1
fi

echo "--- probe rows ---"
grep -a '^\[LP\]' "$CONSOLE" || echo "(none: the probe produced no [LP] row)"
echo "--- unhandled Linux syscalls (name the next handler) ---"
grep -a '\[LCOMPAT\]' "$CONSOLE" | head -20 || true
echo "--- faults / exits ---"
grep -aE 'memory fault|\[EXITPC\]|pid=[0-9]+ \(PROBE' "$CONSOLE" | tail -8 || true
if grep -aq '^\[LP\] done rc=0' "$CONSOLE"; then
  echo "t3_linux_probe: PASS — $NAME reached its done marker"
  exit 0
fi
echo "t3_linux_probe: FAIL — $NAME never printed '[LP] done rc=0' (tier rc=$RC)"
exit 1
