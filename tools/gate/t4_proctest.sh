#!/usr/bin/env bash
# tools/gate/t4_proctest.sh — GB-077 reproducible-victim gate.
#
# GB-077: default-list harness programs corrupt their OWN stacks after entry —
# measured split: W0-corpus manifest = 0 faults / halted=1; default 97-program
# wave = 5 execute-at-stack + 3 parked in __stack_chk_fail.  The coordinator
# directed (2026-10-23) "make it small": reproduce with a ::WAVE.CFG manifest
# naming ONLY PROC_TEST.BIN, then read the atomic fault line.
#
# This stages that minimal wave, boots the intel guest once, and REPORTS the
# atomic [KERNEL] ... memory fault ... line + the fault ledger.  It does not
# pass/fail the kernel — GB-077's DoD is about the default wave, and this is a
# reproduction loop for the named victim, not a verdict.
#
# Verified behavior: with PROC_TEST.BIN only, the guest boots, PROC_TEST runs
# its fork/exec battery, and pid=2 faults at VA=PC=0x17FFFFFF84 (a stack
# address, 0x1C0 below the 8 MiB stack top) — execute-at-stack, not overflow.
# Reproduces on every boot (measured x2).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CFG="${T4_PROCTEST_CFG:-$ROOT/tests/fixtures/abi/wave_proctest.cfg}"
BIN="${T4_PROCTEST_BIN:-$ROOT/obj/intel/proc_test.bin}"
OUT="${T4_PROCTEST_OUT:-/tmp/t4_proctest.log}"

[ -f "$BIN" ] || { echo "t4_proctest: FATAL — PROC_TEST.BIN not built ($BIN)"; exit 2; }
{ echo "PROC_TEST.BIN"; } > "$CFG"

STAGE="mcopy -o -i $ROOT/disk.img $BIN ::PROC_TEST.BIN; mcopy -o -i $ROOT/disk.img $CFG ::WAVE.CFG"
echo "=== t4_proctest: booting intel guest with PROC_TEST-only manifest ==="
T3_STAGE="$STAGE" bash "$ROOT/tools/t3_tier.sh" intel > "$OUT" 2>&1

# GB-140: the guest console is T3_LOG, NOT the captured stdout.  Derive it from
# the machine contract and fail loudly if absent — never grep $OUT for markers.
CONSOLE="$(sed -n 's/^T3_LOG=//p' "$OUT" | tail -1)"
if [ -z "$CONSOLE" ] || [ ! -r "$CONSOLE" ]; then
  echo "t4_proctest: FATAL — t3_tier did not print a readable T3_LOG (GB-140 clause 3)"; exit 1
fi

echo "=== fault ledger ==="
grep -E "T3_intel: [0-9]+ console" "$OUT" | tail -1
echo "=== atomic fault line ==="
grep -E "memory fault|FATAL kernel-mode|FATAL: Exception|IDLESTUCK|EXITPC" "$CONSOLE" | tail -6
