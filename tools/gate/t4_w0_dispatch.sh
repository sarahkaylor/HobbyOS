#!/usr/bin/env bash
# tools/gate/t4_w0_dispatch.sh — GB-138: dispatch the W0PIE static-PIE ELF member in
# a guest wave and read the abi=0x1 line out of the serial.
#
# GB-138's DoD clause 1/2/3 need a Linux-ABI member that RUNS: the build target
# exists ($(W0_PIE_ELF), musl -static-pie, ET_DYN asserted) but a staged member no
# wave dispatches is the "SKIP that prints PASS" trap.  This gate follows the
# t4_proctest/t3_pollbrk pattern exactly: build the member, stage it + its
# manifest onto disk.img via T3_STAGE (NOT the default disk.img recipe, so every
# other tier's image is untouched — GB-138-V2 clause 5), boot the intel guest once,
# and report the [ARRIVED] W0PIE.BIN line and the loader's abi= flags.
#
# Env:  ARCH            intel (default).  Compat is x64-only, so there is no ARM leg.
#       T4_W0_CFG       path to the W0 manifest (default tests/fixtures/abi/WAVE.CFG)
#       T4_W0_OUT       serial capture path (default obj/t4_w0_dispatch.log)
#       W0_PIE_PATH     where the member ELF lives (default obj/ARCH/w0_hello_pie.elf)
#
# Exits 0 when [ARRIVED] W0PIE.BIN and an abi=0x1 loader line are both observed;
# 1 when either is absent (a staged member that never runs is a FAIL, not a skip).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCH="${ARCH:-intel}"
OUT="${T4_W0_OUT:-$ROOT/obj/t4_w0_dispatch.log}"
CFG="${T4_W0_CFG:-$ROOT/tests/fixtures/abi/WAVE.CFG}"
PIE="${W0_PIE_PATH:-$ROOT/obj/$ARCH/w0_hello_pie.elf}"

# 1. build the static-PIE member (ET_DYN asserted by the Makefile target).
if [ ! -f "$PIE" ]; then
  echo "t4_w0_dispatch: building $PIE..."
  # the Makefile's recipe for $(OBJ_DIR)/w0_hello_pie.elf (philosophy: a file
  # target named by path, not a phony).  Run that file's recipe directly to stay
  # out of the disk.img dependency graph — clause 5: staging happens HERE, on
  # disk.img, only for this gate's boot.
  TGT="$ROOT/obj/$ARCH/w0_hello_pie.elf"
  if ! make -C "$ROOT" ARCH="$ARCH" "$TGT" >/dev/null 2>&1; then
    echo "t4_w0_dispatch: FAIL — W0_PIE_ELF build failed (see 'make $TGT ARCH=$ARCH')"; exit 2
  fi
fi
[ -f "$PIE" ] || { echo "t4_w0_dispatch: FAIL — member ELF absent at $PIE after build"; exit 2; }

# 2. stage the member + its manifest onto disk.img via T3_STAGE, then boot.
STAGE="mcopy -o -i $ROOT/disk.img $PIE ::W0PIE.BIN; mcopy -o -i $ROOT/disk.img $CFG ::WAVE.CFG"
echo "=== t4_w0_dispatch: booting $ARCH guest with W0PIE-only manifest (GB-138) ==="
T3_STAGE="$STAGE" bash "$ROOT/tools/t3_tier.sh" "$ARCH" > "$OUT" 2>&1
boot_rc=$?

# 3. read the two observations the DoD names.
arrived=$(grep -ac "\[ARRIVED\] W0PIE.BIN" "$OUT" || true)
abi=$(grep -ac "abi=0x1" "$OUT" || true)
echo "=== t4_w0_dispatch: [ARRIVED] W0PIE.BIN x$arrived, abi=0x1 x$abi (boot rc=$boot_rc) ==="
grep -aE "\[ARRIVED\] W0PIE.BIN|abi=0x1|v2 loader: PID=" "$OUT" | tail -6

if [ "$arrived" -lt 1 ]; then
  echo "t4_w0_dispatch: FAIL — W0PIE.BIN never arrived in the guest (staged but not dispatched; GB-138)" ; exit 1
fi
if [ "$abi" -lt 1 ]; then
  echo "t4_w0_dispatch: FAIL — no abi=0x1 on the W0PIE loader line (PROC_FLAG_LINUX_ABI not set; GB-138 clause 2)"; exit 1
fi
echo "t4_w0_dispatch: PASS — W0PIE.BIN dispatched and carried PROC_FLAG_LINUX_ABI in the guest"
exit 0
