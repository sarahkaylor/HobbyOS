#!/usr/bin/env bash
# tools/gate/t4_device.sh — GB-066 T4 device-leg gate.
#
# The M1 device leg: run the SAME W0 differential probe (src/user/t4_probe.c,
# built with musl-gcc -static) inside HobbyOS-intel via the real ELF loader
# (GB-013) and Linux-compat syscall dispatch (GB-014), capture its lines from
# the serial console, and diff rc/errno/stdout row-by-row against leg A (the
# same binary on real Linux).  This tests the KERNEL, never a host shim.
#
# DoD clauses this enforces (gaming §7 / the GB-066 mint):
#   1. Leg B runs inside HobbyOS-intel and the diff passes.
#   2. A deliberately-wrong shim makes the same harness FAIL.
#   3. An absent probe FAILS the tier rather than skipping: SKIP must exit
#      non-zero.
#   4. The tier asserts the probe's ARRIVAL in the guest via the probe's OWN
#      console line (t4-probe-arrived) — never an image listing (mdir shows
#      "PIE      BIN" and a grep of it is a tautology, per the mint).
#
# Usage:
#   t4_device.sh          run the device differential (needs staged T4PROBE ELF)
#   t4_device.sh --self-test
#                         prove the gate can fail (DoD clauses 2 + 3) from
#                         synthetic fixtures, exit 0 iff both checks FAIL
#                         as they must.
#
# Env: T4_LEGA_OUT   path to leg A captured output (default: <this>/../../obj/t4_legA.out)
#      T4_SERIAL     path to the HobbyOS serial log (default: obj/t3_intel.log)
#      T4_TIMEOUT    seconds to wait for the probe to arrive (default 120)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LEGA="${T4_LEGA_OUT:-$ROOT/obj/t4_legA.out}"
SERIAL="${T4_SERIAL:-$ROOT/obj/t3_intel.log}"
TIMEOUT="${T4_TIMEOUT:-120}"

# ---------------------------------------------------------------------------
# --self-test: prove the gate can FAIL (clauses 2 and 3).  Uses synthetic
# fixtures so it runs anywhere a shell exists.  Never touches the kernel.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

  # leg A fixture: the real probe's expected output.
  cat >"$TMP/legA" <<'EOF'
t4-probe
hi
row=write3 rc=3 errno=0 out=68690a
t4-probe-arrived
probe-exit
EOF

  # (a) A deliberately-wrong shim (rc=2 instead of 3, and no arrival line
  #     proves nothing about the guest).  The gate MUST fail.
  cat >"$TMP/legB_wrong" <<'EOF'
t4-probe
row=write3 rc=2 errno=0 out=68690a
t4-probe-arrived
probe-exit
EOF

  # (b) An absent probe: serial log has no t4-probe / arrival lines at all
  #     (e.g. the guest never ran it).  The gate MUST fail, not skip.
  cat >"$TMP/legB_absent" <<'EOF'
system boot
some other wave output
System halt
EOF

  fail_count=0

  # --- inline, self-contained verdict logic (mirrors the real diff below) ---
  verdict() { # $1 legA, $2 legB, returns 0 pass / 1 fail
    local a="$1" b="$2" rc=0
    grep -q "t4-probe-arrived" "$b" || { echo "  FAIL: probe never arrived in guest (no t4-probe-arrived)"; return 1; }
    if ! grep -q "row=" "$b"; then echo "  FAIL: no row= lines in guest output"; return 1; fi
    local na nb
    na=$(grep -c "^row=" "$a"); nb=$(grep -c "^row=" "$b")
    [ "$na" = "$nb" ] || { echo "  FAIL: row count differs A=$na B=$nb"; return 1; }
    while IFS= read -r ra; do
      if ! grep -Fqx "$ra" "$b"; then echo "  FAIL: leg A row missing/mismatched in B: $ra"; rc=1; fi
    done < <(grep "^row=" "$a")
    return $rc
  }

  echo "[self-test] clause 2 (wrong shim must FAIL):"
  if verdict "$TMP/legA" "$TMP/legB_wrong" >/dev/null 2>&1; then
    echo "  UNEXPECTED PASS — wrong shim did not fail"; fail_count=$((fail_count+1))
  else
    echo "  OK — wrong shim FAILED as required"; :
  fi

  echo "[self-test] clause 3 (absent probe must FAIL, not skip):"
  if verdict "$TMP/legA" "$TMP/legB_absent" >/dev/null 2>&1; then
    echo "  UNEXPECTED PASS — absent probe did not fail"; fail_count=$((fail_count+1))
  else
    echo "  OK — absent probe FAILED as required"; :
  fi

  echo "self-test result: fail_count=$fail_count"
  [ "$fail_count" = "0" ]
  exit $?
fi

# ---------------------------------------------------------------------------
# real device gate
# ---------------------------------------------------------------------------
echo "=== t4_device: leg A reference: $LEGA ==="
if [ ! -s "$LEGA" ]; then
  echo "t4_device: FATAL — leg A reference '$LEGA' absent/empty. Build leg A first:"
  echo "  musl-gcc -static -O2 src/user/t4_probe.c -o \$TMP/t4.elf && \$TMP/t4.elf > '$LEGA'"
  exit 2
fi

echo "=== t4_device: capture leg B from serial '$SERIAL' ==="
if [ ! -f "$SERIAL" ]; then
  echo "t4_device: FATAL — serial log '$SERIAL' absent. Boot HobbyOS-intel through"
  echo "  tools/t3_tier.sh intel (T3_STAGE staging of T4PROBE) before this gate."
  echo "t4_device: FAIL (environment: no serial log) — not a SKIP."
  exit 1
fi

# Wait (bounded) for the probe to appear in the serial log, i.e. the guest
# actually ran it.  Absent after TIMEOUT == FAIL, never a silent skip.
deadline=$(( $(date +%s) + TIMEOUT ))
arrived=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  if grep -q "t4-probe-arrived" "$SERIAL"; then arrived=1; break; fi
  sleep 2
done
if [ "$arrived" != "1" ]; then
  echo "t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived'"
  echo "  in $SERIAL after ${TIMEOUT}s).  This is a FAIL, not a SKIP (DoD clause 3)."
  echo "  (tail of serial:)"
  tail -20 "$SERIAL" || true
  exit 1
fi
echo "t4_device: probe ARRIVED in guest (t4-probe-arrived present)."

# Per-row diff of rc/errno/stdout against leg A.
fail=0
NA=$(grep -c "^row=" "$LEGA"); NB=$(grep -c "^row=" "$SERIAL")
echo "t4_device: row count A=$NA B=$NB"
[ "$NA" = "$NB" ] || { echo "t4_device: FAIL — row count differs (A=$NA B=$NB)"; fail=1; }
while IFS= read -r ra; do
  if grep -Fqx "$ra" <(grep "^row=" "$SERIAL"); then
    echo "  PASS $ra"
  else
    echo "  FAIL $ra  (not present in guest output; see B rows below)"
    fail=1
  fi
done < <(grep "^row=" "$LEGA")

if [ "$fail" = "0" ]; then
  echo "t4_device: PASS — leg B (HobbyOS-intel) matches leg A (Linux) on all rows."
  exit 0
fi
echo "t4_device: FAIL — differential mismatch.  Guest rows:"
grep "^row=" "$SERIAL" || true
exit 1
