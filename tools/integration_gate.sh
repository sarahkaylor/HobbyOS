#!/usr/bin/env bash
# tools/integration_gate.sh — GB-057 host-side gate for the userland
# integration tier (T3, MODE=test).
#
# History / bug: `make test_intel` booted the wave and QEMU exited 0 (the
# kernel always prints "System halt." and powers off) REGARDLESS of how many
# `[...] FAIL` lines the test binaries printed.  The tier was therefore not a
# gate: 222 printed failures on intel produced a green exit code.  Every
# printed FAIL must fail the tier, or be explicitly quarantined with a reason
# (gaming.md §6 / §5.3).
#
# This script scans a serial log (the QEMU -serial capture) for test-FAIL
# tokens and fails (exit 1) when any un-quarantined one is present.  It is the
# host-side half of GB-057's DoD: "printed failures fail the tier".
#
# Usage:
#   tools/integration_gate.sh <serial.log>
#   tools/integration_gate.sh --allow='[BROWSER]' --allow='KNOWN-FLAKY' <log>
#
# Options:
#   --allow=REGEX   (repeatable) quarantine: lines matching REGEX that contain
#                   a FAIL token are NOT counted.  Use for a test that prints
#                   "FAIL" in a non-result sense, or a quarantined case with a
#                   documented reason in gaming.md §6.
#   --json          print a one-line JSON summary on stdout.
#   -q              quiet (no per-line dump).
#
# Exit: 0 if the tier passes (no un-quarantined FAIL), 1 if it fails.
set -u

ALLOW=()
JSON=0
QUIET=0
while [ $# -gt 0 ]; do
  case "$1" in
    --allow=*) ALLOW+=("${1#--allow=}"); shift ;;
    --json) JSON=1; shift ;;
    -q) QUIET=1; shift ;;
    --) shift; break ;;
    -*) echo "usage: integration_gate.sh [--allow=RE]... [--json] [-q] <log>" >&2; exit 2 ;;
    *) break ;;
  esac
done

[ $# -ge 1 ] || { echo "usage: integration_gate.sh [--allow=RE]... [--json] [-q] <log>" >&2; exit 2; }
LOG="$1"
[ -f "$LOG" ] || { echo "integration_gate: no such log: $LOG" >&2; exit 2; }

count_quarantined() {
  # exit 0 if the line is quarantined (matches any --allow regex), else 1
  local line="$1"
  for re in "${ALLOW[@]}"; do
    if printf '%s\n' "$line" | grep -aqE "$re"; then
      return 0
    fi
  done
  return 1
}

# A test-FAIL line is one carrying a FAIL verdict token: a bracketed
# "TESTNAME] FAIL" (the first-class form) or a bare " FAIL " / trailing
# "FAILED" result line.  We deliberately do NOT treat every "FAIL" substring
# as a verdict (e.g. "[MAKE] gcc: internal compiler error" won't match), but
# the integrated test binaries all use the bracketed or space-delimited form.
total=0
quarantined=0
fail_lines=""
while IFS= read -r line; do
  if printf '%s\n' "$line" | grep -aqE '\] *FAIL |\] *FAIL\]| FAIL | FAILED'; then
    total=$((total + 1))
    if count_quarantined "$line"; then
      quarantined=$((quarantined + 1))
      [ $QUIET -eq 1 ] || echo "[gate] (quarantined) $line"
    else
      fail_lines+="$line"$'\n'
    fi
  fi
done < <(grep -aE 'FAIL|FAILED' "$LOG" || true)

unquarantined=$((total - quarantined))

if [ $JSON -eq 1 ]; then
  printf '{"tier":"integration","log":"%s","fail_total":%d,"quarantined":%d,"unquarantined":%d,"pass":%s}\n' \
    "$LOG" "$total" "$quarantined" "$unquarantined" \
    "$([ "$unquarantined" -eq 0 ] && echo true || echo false)"
fi

if [ "$unquarantined" -ne 0 ]; then
  if [ $QUIET -eq 0 ]; then
    echo "integration_gate: ${unquarantined} un-quarantined FAIL line(s) -> TIER FAILED"
    printf '%s' "$fail_lines" | sed 's/^/  /'
  fi
  exit 1
fi

if [ $QUIET -eq 0 ]; then
  echo "integration_gate: 0 un-quarantined FAIL lines -> TIER PASS"
fi
exit 0
