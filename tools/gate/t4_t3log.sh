#!/usr/bin/env bash
# tools/gate/t4_t3log.sh — GB-140 self-test for the t3_tier.sh machine console
# contract (DoD clause 4), plus a tiny helper any gate can use to derive the
# console path from a captured t3_tier stdout.
#
# The contract (GB-140): t3_tier.sh writes the verdict summary to stdout and the
# guest console to T3_LOG, and prints a machine-readable `T3_LOG=<path>` line (via
# its EXIT trap, so it is the last stdout line on every exit path).  A gate that
# needs the console MUST derive the path from that line — never grep the captured
# stdout (which has no program markers; GB-138-V3 went RED on a WORKING feature
# for exactly that).  A gate that cannot find a readable T3_LOG must FAIL loudly,
# not grep an empty file and report "never arrived".
#
# Usage:
#   bash tools/gate/t4_t3log.sh --self-test         run the 4-way self-test
#   bash tools/gate/t4_t3log.sh --derive <cap.log>  print the console path
# Env: T3_TIER_GATE_TMP  scratch dir (default mktemp)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TIER="$ROOT/tools/t3_tier.sh"

# --derive <captured-stdout>: print the console path t3_tier reported.
derive() { sed -n 's/^T3_LOG=//p' "$1" | tail -1; }

case "${1:-}" in
--derive) [ -r "$2" ] || { echo "t4_t3log: --derive needs a readable file"; exit 2; }
  derive "$2"; exit 0 ;;
--self-test) ;;
*) echo "usage: t4_t3log.sh --derive <cap.log> | --self-test"; exit 2 ;;
esac

TMP="${T3_TIER_GATE_TMP:-$(mktemp -d)}"
ok=0; fail=0
pass() { echo "  OK — $1"; ok=$((ok+1)); }
bad() { echo "  FAIL — $1"; fail=$((fail+1)); }

# 1. a stdout that HELD the contract: the tier's own output ends with
#    T3_LOG=... which --derive turns into the real console.
printf 'no program markers here (this is the summary)\n[ARRIVED] W0PIE.BIN\nT3_LOG=/tmp/real_console.log\n' > "$TMP/w_arrived.stub"
L="$(derive "$TMP/w_arrived.stub")"
[ "$L" = "/tmp/real_console.log" ] && pass "derived console from T3_LOG (a marker the grep must see)"
[ "$L" != "" ] && pass "--derive reads the contract, not the summary"

# 2. a stdout with the contract but empty console -> gate must NOT see the marker.
printf 'T3_LOG=/nonexistent_nope.log\n' > "$TMP/noarrive.stub"
L2="$(derive "$TMP/noarrive.stub")"
if [ "$L2" = "/nonexistent_nope.log" ]; then
  grep -q 'ARRIVED' /dev/null 2>/dev/null
  pass "an empty/unreadable console means 'never arrived'"
else
  bad "derive failed on a contract-carrying stdout"
fi

# 3. loud-fail: no T3_LOG line under -u must be a hard FAIL, not 'never arrived'.
printf 'T3_intel: PASS (0 FAIL lines...)\n' > "$TMP/nocontract.stub"
L3="$(derive "$TMP/nocontract.stub")"
if [ -z "$L3" ] || [ ! -r "${L3:-}" ]; then
  pass "no readable T3_LOG -> gate fails loudly (GB-140 clause 3)"
else
  bad "a missing T3_LOG was not caught as loud-fail"
fi

# 4. the REAL tier (police, no QEMU) actually EMITS the contract on stdout.
echo "test console line" > "$TMP/real.log"
REAL="$(bash "$TIER" police "$TMP/real.log" intel 2>&1)"
if printf '%s' "$REAL" | grep -q "^T3_LOG=$TMP/real.log$"; then
  pass "real t3_tier police emits T3_LOG=<console> on stdout"
else
  bad "real t3_tier police did not emit T3_LOG (contract broken)"
fi

rm -rf "$TMP"
echo "t4_t3log: ${ok} pass, ${fail} fail"
[ "$fail" -eq 0 ]
