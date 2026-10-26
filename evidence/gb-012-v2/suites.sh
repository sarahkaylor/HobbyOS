#!/usr/bin/env bash
# GB-012-V2 clause 3: did the four named compat suites actually execute?
# The T2 console prints a suite banner ("<name>_suite:") and one
# "  Running test_<something>..." line per case.
set -u
LOG=evidence/gb-012-v2/03_unit_intel.log
echo "=== suite banners present in the intel T2 console"
grep -aoE "[A-Za-z0-9_]+_suite:" "$LOG" | sort -u | tr '\n' ' '; echo
echo
echo "=== the four the card names"
for pat in 'vm_test_suite' 'process_test' 'memfd' 'thrd_test'; do
  banner=$(grep -ac "$pat" "$LOG" || true)
  cases=$(grep -ac "Running test_.*${pat%%_*}" "$LOG" || true)
  printf '%-18s banner/hits=%-4s\n' "$pat" "$banner"
done
echo
echo "=== case lines for the four families (first 10)"
grep -aE "Running test_(vm_|process_|memfd|thrd)" "$LOG" | head -10
echo
echo "=== totals"
grep -ac "Running test_" "$LOG"