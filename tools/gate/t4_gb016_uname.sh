#!/usr/bin/env bash
# tools/gate/t4_gb016_uname.sh — GB-016: uname(2) must answer from a process that
# carries PROC_FLAG_LINUX_ABI, with a Linux-ish identity.
#
# Why this gate exists at all: GB-016 was marked "not done though a merged PR
# names it" because what landed (PR #61) was the identity FILL and its T2 test —
# `linux_fill_utsname()` — while the dispatch that a real compat process reaches
# was never written.  A host-side differential cannot see that: both host legs
# call the host's uname(2), not the guest's.  So this gate boots the guest, runs
# a static-PIE probe as a compat member, and reads the rows off the console.
#
# Falsifiers, all of which this gate would catch:
#   - no dispatch  -> rc=-38 (ENOSYS), the exact pre-fix signature;
#   - dispatch that copies nothing -> sysname empty, not "Linux";
#   - no user-range check -> the bad-pointer row faults instead of -EFAULT(14);
#   - wrong arch string -> machine != x86_64 on intel.
#
# Usage:  bash tools/gate/t4_gb016_uname.sh [--self-test]
# Env:    ARCH (intel), T4_GB016_OUT, GB016_ELF_PATH
# Exit 0 = uname dispatched, identity correct, bad pointer rejected with EFAULT.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCH="${ARCH:-intel}"
ELF="${GB016_ELF_PATH:-$ROOT/obj/$ARCH/gb016_uname.elf}"
OUT="${T4_GB016_OUT:-$ROOT/obj/t4_gb016_uname.log}"

SELF="${1:-}"
if [ "$SELF" = "--self-test" ]; then
  ok=0
  echo "=== t4_gb016_uname self-test ==="
  cat > "$OUT" <<'EOF'
gb016 BEGIN
gb016 rc=0
gb016 sysname=Linux
gb016 machine=x86_64
gb016 release=5.15.0-hobbyos
gb016 efaul rc=-14
gb016 END
[ARRIVED] GB016.BIN rc>0
v2 loader: PID=9 entry=0x1000001000 ... abi=0x1
EOF
  grep -q "gb016 rc=0" "$OUT" && { echo "  OK — rc=0 (a dispatch exists)"; ok=$((ok+1)); } || echo "  FAIL — rc row"
  grep -q "gb016 sysname=Linux" "$OUT" && { echo "  OK — sysname=Linux"; ok=$((ok+1)); } || echo "  FAIL — sysname"
  grep -q "gb016 machine=x86_64" "$OUT" && { echo "  OK — machine=x86_64"; ok=$((ok+1)); } || echo "  FAIL — machine"
  grep -q "gb016 efaul rc=-14" "$OUT" && { echo "  OK — bad pointer -> -EFAULT"; ok=$((ok+1)); } || echo "  FAIL — EFAULT row"
  grep -q "abi=0x1" "$OUT" && { echo "  OK — Linux-ABI proc"; ok=$((ok+1)); } || echo "  FAIL — abi=0x1"
  # falsifier: the pre-fix console must FAIL this gate
  sed -i.bak 's/gb016 rc=0/gb016 rc=-38/' "$OUT" 2>/dev/null || true
  if grep -q "gb016 rc=0" "$OUT"; then echo "  FAIL — gate accepted the no-dispatch console"; else echo "  OK — -ENOSYS console rejected (falsifier fires)"; ok=$((ok+1)); fi
  rm -f "$OUT" "$OUT.bak"
  echo "t4_gb016_uname self-test: ${ok}/6"
  [ "$ok" -eq 6 ]
  exit $?
fi

# 1. build the probe (the Makefile target asserts ET_DYN/static-PIE).
if [ ! -f "$ELF" ]; then
  echo "t4_gb016_uname: building $ELF..."
  make -C "$ROOT" ARCH="$ARCH" "obj/$ARCH/gb016_uname.elf" >/dev/null 2>&1 || { echo "t4_gb016_uname: FAIL — build"; exit 2; }
fi
[ -f "$ELF" ] || { echo "t4_gb016_uname: FAIL — member ELF absent at $ELF"; exit 2; }

# 2. stage + boot with a one-member manifest, console via the T3_LOG contract.
CFG="$ROOT/tests/fixtures/abi/wave_gb016.cfg"
printf 'GB016.BIN\n' > "$CFG"
STAGE="mcopy -o -i $ROOT/disk.img $ELF ::GB016.BIN; mcopy -o -i $ROOT/disk.img $CFG ::WAVE.CFG"
echo "=== t4_gb016_uname: booting $ARCH guest with GB016-only manifest ==="
TOOLS_SKIP=1 T3_STAGE="$STAGE" bash "$ROOT/tools/t3_tier.sh" "$ARCH" > "$OUT" 2>&1
LOG="$(sed -n 's/^T3_LOG=//p' "$OUT" | tail -1)"
if [ -z "$LOG" ] || [ ! -f "$LOG" ]; then
  echo "t4_gb016_uname: FAIL — t3_tier printed no usable T3_LOG (GB-140 clause 3)"; exit 1
fi

# 3. assert the rows.
echo "=== console rows (T3_LOG=$LOG) ==="
grep -a "gb016 " "$LOG" | head -10
OK=1
grep -q "gb016 rc=0" "$LOG" || { echo "t4_gb016_uname: FAIL — uname did not return 0 (rc=-38 means no dispatch)"; OK=0; }
grep -q "gb016 sysname=Linux" "$LOG" || { echo "t4_gb016_uname: FAIL — sysname is not Linux"; OK=0; }
grep -q "gb016 machine=x86_64" "$LOG" || { echo "t4_gb016_uname: FAIL — machine is not x86_64"; OK=0; }
grep -q "gb016 efaul rc=-14" "$LOG" || { echo "t4_gb016_uname: FAIL — bad pointer did not return -EFAULT"; OK=0; }
grep -q "abi=0x1" "$LOG" || { echo "t4_gb016_uname: FAIL — no Linux-ABI proc (no abi=0x1)"; OK=0; }
[ "$OK" = "1" ] && echo "t4_gb016_uname: PASS — uname(63) dispatched, sysname=Linux machine=x86_64, bad pointer -> EFAULT, from a Linux-ABI proc"
exit $([ $OK -eq 1 ] && echo 0 || echo 1)
