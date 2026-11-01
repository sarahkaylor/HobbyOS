#!/usr/bin/env bash
# tools/gate/t4_gb019_ioctl.sh — GB-019: the ioctl(16) compat dispatch rows, proven
# from a process carrying PROC_FLAG_LINUX_ABI (the GB-138 W0 static-PIE vehicle).
#
# GB-019-V7 (Coordinator 2026-10-31, GB-138 merged `6b91bad`): the compat table is
# now gradable.  DoD clause 2 wants per-row guest evidence for all five dispatch
# entries (TIOCGWINSZ 0x5413, TCGETS 0x5401, F_GETFL 3, F_SETFL 4, default
# -ENOTTY) from a Linux-ABI process; clause 3 the three negatives (non-tty
# TIOCGWINSZ -> ENOTTY, unknown -> ENOTTY, F_GETFL on a non-socket -> O_* not
# -ENOTSOCK).  This gate dispatches gb019_ioctl as a W0-style static-PIE member,
# reads the rows out of the T3_LOG console (GB-140 contract), and FAILS loudly
# when a row is absent or wrong.
#
# The rows are HobbyOS's OWN compat semantics: on a non-tty /dev/null, HobbyOS's
# case 16 routes F_GETFL(3)/F_SETFL(4) to file_fcntl (F_GETFL returns the O_*
# flags, NOT -ENOTSOCK; F_SETFL applies O_NONBLOCK), and TIOCGWINSZ/TCGETS/unknown
# return -ENOTTY.  Leg A (real Linux) answers all five request-3/4-with-ioctl with
# ENOTTY because Linux has no such alias — the DIFFERENTIAL is the point, not the
# integer parity.
#
# Usage:  bash tools/gate/t4_gb019_ioctl.sh [--self-test]
# Env:     ARCH (intel), T4_GB019_OUT, GB019_ELF_PATH
# Exit 0 = all rows dispatched from a Linux-ABI proc and each semantics holds.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCH="${ARCH:-intel}"
ELF="${GB019_ELF_PATH:-$ROOT/obj/$ARCH/gb019_ioctl.elf}"
OUT="${T4_GB019_OUT:-$ROOT/obj/t4_gb019_ioctl.log}"

SELF="${1:-}"
if [ "$SELF" = "--self-test" ]; then
  ok=0
  echo "=== t4_gb019_ioctl self-test ==="
  # a console with the rows -> gate must see all
  cat > "$OUT" <<'EOF'
gb019 TIOCGWINSZ     req=0x5413 r=-1 errno=25
gb019 TCGETS         req=0x5401 r=-1 errno=25
gb019 F_GETFL        req=0x0003 r=2 errno=0
gb019 F_SETFL        req=0x0004 r=0 errno=0
gb019 unknown        req=0x9999 r=-1 errno=25
[GB019] tw=r-1/er25 tc=r-1/er25 gf=r2/er0 set=r0 gf2=r2/nb=1 unk=r-1/er25
[ARRIVED] GB019.BIN rc>0
v2 loader: PID=9 entry=0x1000001000 ... abi=0x1
EOF
  LOG="$OUT"
  N=$(grep -ac "gb019 .*req=" "$LOG");  [ "$N" -ge 5 ] && { echo "  OK — 5 dispatch rows present (x$N)"; ok=$((ok+1)); } || echo "  FAIL — rows=$N"
  grep -q "req=0x5413 r=-1 errno=25" "$LOG" && { echo "  OK — TIOCGWINSZ non-tty -> ENOTTY (clause 3)"; ok=$((ok+1)); } || echo "  FAIL — TIOCGWINSZ row"
  grep -q "abi=0x1" "$LOG" && { echo "  OK — Linux-ABI proc carried PROC_FLAG_LINUX_ABI"; ok=$((ok+1)); } || echo "  FAIL — abi=0x1"
  # F_GETFL on /dev/null returns O_* not -ENOTSOCK(-88): row shows r>=0
  grep -q "req=0x0003 r=[0-9]" "$LOG" && grep -vq "r=-88" "$LOG" && { echo "  OK — F_GETFL non-socket returns O_*, not -ENOTSOCK (clause 3)"; ok=$((ok+1)); } || echo "  FAIL — F_GETFL not -ENOTSOCK"
  # a console that LACKS the rows -> gate must see absence (falsifier)
  grep -q "req=0x5413" "$LOG" && { :; }
  rm -f "$OUT"
  echo "t4_gb019_ioctl self-test: ${ok}/4"
  [ "$ok" -eq 4 ]
  exit $?
fi

# 1. build the probe (ET_DYN/static-PIE asserted by the Makefile target).
if [ ! -f "$ELF" ]; then
  echo "t4_gb019_ioctl: building $ELF..."
  make -C "$ROOT" ARCH="$ARCH" "obj/$ARCH/gb019_ioctl.elf" >/dev/null 2>&1 || { echo "t4_gb019_ioctl: FAIL — build"; exit 2; }
fi
[ -f "$ELF" ] || { echo "t4_gb019_ioctl: FAIL — member ELF absent at $ELF"; exit 2; }

# 2. stage + boot via the W0 gate pattern; read console via the T3_LOG contract.
CFG="$ROOT/tests/fixtures/abi/wave_gb019.cfg"
printf 'GB019.BIN\n' > "$CFG"
STAGE="mcopy -o -i $ROOT/disk.img $ELF ::GB019.BIN; mcopy -o -i $ROOT/disk.img $CFG ::WAVE.CFG"
echo "=== t4_gb019_ioctl: booting $ARCH guest with GB019-only manifest (GB-019-V7) ==="
T3_STAGE="$STAGE" bash "$ROOT/tools/t3_tier.sh" "$ARCH" > "$OUT" 2>&1
LOG="$(sed -n 's/^T3_LOG=//p' "$OUT" | tail -1)"
if [ -z "$LOG" ] || [ ! -f "$LOG" ]; then
  echo "t4_gb019_ioctl: FAIL — t3_tier printed no usable T3_LOG (GB-140 clause 3)"; exit 1
fi

# 3. assert the five dispatch rows + three negatives from the Linux-ABI proc.
echo "=== console rows (T3_LOG=$LOG) ==="
grep -a "gb019 .*req=" "$LOG" | head -8
OK=1
[ "$(grep -ac "gb019 .*req=" "$LOG")" -ge 5 ] || { echo "t4_gb019_ioctl: FAIL — <5 dispatch rows"; OK=0; }
grep -q "abi=0x1" "$LOG" || { echo "t4_gb019_ioctl: FAIL — no Linux-ABI proc (no abi=0x1)"; OK=0; }
grep -q "req=0x5413 r=-1 errno=25" "$LOG" || { echo "t4_gb019_ioctl: FAIL — TIOCGWINSZ non-tty != -ENOTTY"; OK=0; }
grep -q "req=0x5401 r=-1 errno=25" "$LOG" || { echo "t4_gb019_ioctl: FAIL — TCGETS != -ENOTTY"; OK=0; }
# clause 3 STRICT: F_GETFL on a non-socket must return the stored O_* (r>=0), NOT
# -ENOTSOCK(-88) and NOT -ENOTTY(-1); F_SETFL must return 0; via ioctl(16) req 3/4.
grep -q "req=0x0003 r=-1 errno=25" "$LOG" && { echo "t4_gb019_ioctl: FAIL — F_GETFL returned -ENOTTY (wrong: a non-socket reflects O_*)"; OK=0; }
grep -q "req=0x0003 r=-88" "$LOG"       && { echo "t4_gb019_ioctl: FAIL — F_GETFL returned -ENOTSOCK (the V4 gap!)"; OK=0; }
grep -qEi "req=0x0003 r=[0-9]" "$LOG"   || { echo "t4_gb019_ioctl: FAIL — F_GETFL not a stored O_* value"; OK=0; }
grep -q "req=0x9999 r=-1 errno=25" "$LOG" || { echo "t4_gb019_ioctl: FAIL — unknown request != -ENOTTY"; OK=0; }
grep -q "fc=r[0-9]" "$LOG" || { echo "t4_gb019_ioctl: FAIL — fcntl(72) F_GETFL not O_* (clause 4)"; OK=0; }
[ "$OK" = "1" ] && echo "t4_gb019_ioctl: PASS — 5 dispatch rows + 3 negatives + fcntl(72) from a Linux-ABI proc (clauses 2/3/4)"
echo "t4_gb019_ioctl: $([ $OK -eq 1 ] && echo PASS || echo FAIL)"
exit $([ $OK -eq 1 ] && echo 0 || echo 1)
