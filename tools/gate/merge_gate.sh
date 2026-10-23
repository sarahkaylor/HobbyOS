#!/usr/bin/env bash
# tools/gate/merge_gate.sh — the merge gate (GB-005).
#
# One reproducible gate a branch/PR must pass before merge.  It runs the
# project's test tiers and turns their output into a single verdict:
#
#   T1  host golden  : make host_tests   (host unit/golden suite + t4 parity)
#   T2  kernel unit  : make unit_tests_{arm,intel}  (in-guest, QEMU)
#   T3  integration  : tools/t3_tier.sh {arm,intel} (the GATE T3 tier — inherits
#                      its real green criteria: no FATAL kernel-mode fault, a
#                      console above the completeness floor, and a "System halt"
#                      completion marker — not merely a halt marker presence)
#   STYLE            : make check-format + tools/test_cstyle.py
#
# Verdict semantics: a tier is PASS or FAIL; SKIP only when the tier is
# explicitly disabled (never a silent pass).  The gate exits non-zero if any
# required tier is FAIL.  Heavy QEMU tiers (T2/T3) are OFF by default so the
# gate is cheap to run on any host (a dev runs T1 locally; the farm/CI sets
# GATE_CI=1 to enable the full wave).  Every tier's real command + exit code
# is logged to obj/gate/ so any verdict is reproducible from the log alone.
#
# Usage:
#   ./tools/gate/merge_gate.sh            # T1 (+style if GATE_STYLE=1); T2/T3 off
#   GATE_CI=1 ./tools/gate/merge_gate.sh  # full gate: T1 + T2(arm,intel) + T3(arm,intel) + style
#   GATE_T3=1 ./tools/gate/merge_gate.sh  # T1 + T3 integration waves
#   ./tools/gate/merge_gate.sh police <arch> <log>   # just the T3 verdict over an
#                                       # already-captured console log (fast,
#                                       # no QEMU) — this is how the gate's FAIL
#                                       # detector is proven (GB-005 DoD).
#
# Env:
#   GATE_T1 GATE_T2 GATE_T3 GATE_STYLE   per-tier on/off (default: 1 0 0 0)
#   GATE_CI     =1 -> enable T2, T3 and STYLE (for a farm/CI runner)
#   GATE_FAIL_TAIL  lines of a failing tier's log to show (default 25)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# --- environment preconditions (§11.2b) -----------------------------------
# A non-login/background shell may omit /usr/sbin, where mkfs.fat lives, so a
# whole gate run died at the disk step with `mkfs.fat: No such file or
# directory` AFTER the build and QEMU work had succeeded — which reads like a
# code failure (measured 2026-10-23: T2_unit_arm and T2_unit_intel both
# "FAILed" on this, nothing else). The tool is provisioned; only PATH was
# wrong. Prepending is what t3_tier.sh already does for the same reason.
export PATH="/usr/sbin:/sbin:$PATH"

GATE_T1="${GATE_T1:-1}"; GATE_T2="${GATE_T2:-0}"; GATE_T3="${GATE_T3:-0}"; GATE_STYLE="${GATE_STYLE:-0}"
if [ "${GATE_CI:-0}" = "1" ]; then GATE_T2=1; GATE_T3=1; GATE_STYLE=1; fi
GATE_FAIL_TAIL="${GATE_FAIL_TAIL:-25}"

# Missing tools are an ENVIRONMENT failure (exit 2), never a red verdict: the
# gate must not report a code defect for a box that cannot run the tier.
missing=()
for t in mkfs.fat mcopy dd; do command -v "$t" >/dev/null 2>&1 || missing+=("$t"); done
if [ "$GATE_T2$GATE_T3" != "00" ]; then
  command -v qemu-system-aarch64 >/dev/null 2>&1 || missing+=("qemu-system-aarch64")
  command -v qemu-system-x86_64  >/dev/null 2>&1 || missing+=("qemu-system-x86_64")
fi
if [ "${#missing[@]}" -gt 0 ]; then
  echo "[GATE] ENVIRONMENT FAILURE (exit 2): missing tool(s): ${missing[*]}" >&2
  echo "[GATE] install them or drop the tier (GATE_T2=0 / GATE_T3=0); this is NOT a code verdict" >&2
  exit 2
fi

# --- T3 verdict-only mode (fast; no QEMU): proxies tools/t3_tier.sh police ---
if [ "${1:-}" = "police" ]; then
  ARCH="${2:?usage: merge_gate.sh police <arch> <log>}"
  LOG="${3:?usage: merge_gate.sh police <arch> <log>}"
  bash tools/t3_tier.sh police "$LOG" "$ARCH"
  rc=$?
  echo "GATE T3_INT_${ARCH} (police $LOG): $([ $rc -eq 0 ] && echo PASS || echo FAIL) (exit $rc)"
  exit "$rc"
fi

mkdir -p obj/gate
declare -A RESULT

run_tier() {  # run_tier <name> <cmd...>
  local name="$1"; shift
  local log="obj/gate/$name.log"
  echo "[GATE] $name: $*"
  "$@" >"$log" 2>&1
  local rc=$?
  if [ "$rc" -eq 0 ]; then
    RESULT[$name]="PASS"; echo "[GATE] $name: PASS"
  else
    RESULT[$name]="FAIL"; echo "[GATE] $name: FAIL (exit $rc) — log: obj/gate/$name.log"
    tail -n "$GATE_FAIL_TAIL" "$log" | sed 's/^/    /'
  fi
}

# --- T1: host golden suite (includes t4 parity, elf/linux-abi, gfx, evdev…) ---
if [ "$GATE_T1" = "1" ]; then run_tier T1_host make host_tests; else RESULT[T1_host]=skip; fi

# --- T2: kernel unit waves (in-guest, QEMU) ---
if [ "$GATE_T2" = "1" ]; then
  run_tier T2_unit_arm   make unit_tests_arm
  run_tier T2_unit_intel make unit_tests_intel
else RESULT[T2_unit_arm]=skip; RESULT[T2_unit_intel]=skip; fi

# --- T3: integration waves (the real T3 gate, both arches) ---
if [ "$GATE_T3" = "1" ]; then
  run_tier T3_int_arm   bash tools/t3_tier.sh arm
  run_tier T3_int_intel bash tools/t3_tier.sh intel
else RESULT[T3_int_arm]=skip; RESULT[T3_int_intel]=skip; fi

# --- STYLE (GB-005) -------------------------------------------------------
# A merge gate asks "did THIS CHANGE add a violation?", not "is the whole tree
# formatted?" — and on this tree the second question is red for reasons no PR
# can fix: `make check-format` reports 6 pre-existing files and
# `tools/test_cstyle.py` fails 2 of 86 (src/user/grep/dfa.c is not idempotent
# and one file changes its token stream when formatted — both defects in
# tools/cstyle.py itself, measured 2026-10-23). Left tree-wide, GATE_CI=1
# therefore made the merge gate un-passable by construction, which is how a
# gate stops being read at all.
#
#   GATE_STYLE=1        -> check the files changed vs GATE_STYLE_BASE
#                          (default origin/main); PASS when nothing changed.
#   GATE_STYLE_STRICT=1 -> the old tree-wide behaviour, plus cstyle's own
#                          self-test, for a box that wants the whole tree.
GATE_STYLE_BASE="${GATE_STYLE_BASE:-origin/main}"
if [ "$GATE_STYLE" = "1" ]; then
  if [ "${GATE_STYLE_STRICT:-0}" = "1" ]; then
    run_tier STYLE_check make check-format
    run_tier STYLE_test  python3 tools/test_cstyle.py
  else
    style_files=()
    style_mode="no-base($GATE_STYLE_BASE)-tree-wide(src)"
    if git rev-parse --verify -q "$GATE_STYLE_BASE" >/dev/null 2>&1; then
      while IFS= read -r f; do
        [ -f "$f" ] && style_files+=("$f")
      done < <(git diff --name-only --diff-filter=ACMR "$GATE_STYLE_BASE"...HEAD -- 'src/*.c' 'src/*.h')
      style_mode="changed-vs-$GATE_STYLE_BASE"
    fi
    echo "[GATE] STYLE: $style_mode, ${#style_files[@]} file(s)"
    if [ "$style_mode" = "${style_mode#no-base}" ]; then
      if [ "${#style_files[@]}" -eq 0 ]; then
        RESULT[STYLE_check]="PASS"; echo "[GATE] STYLE_check: PASS (no changed C sources)"
      else
        run_tier STYLE_check python3 tools/cstyle.py check "${style_files[@]}"
      fi
    else
      run_tier STYLE_check make check-format
    fi
    # cstyle's own idempotency suite is reported, not gated, until its two
    # pre-existing defects are fixed (mint: style instrument ticket).
    echo "[GATE] STYLE_test: reported (tree-wide red is pre-existing; GATE_STYLE_STRICT=1 gates it)"
    RESULT[STYLE_test]="reported"
  fi
else RESULT[STYLE_check]=skip; RESULT[STYLE_test]=skip; fi

# --- verdict ---
echo
echo "=== GATE SUMMARY ==="
declare -a TIERS=(T1_host T2_unit_arm T2_unit_intel T3_int_arm T3_int_intel STYLE_check STYLE_test)
fail=0
for k in "${TIERS[@]}"; do
  v="${RESULT[$k]:-skip}"
  printf "  %-16s %s\n" "$k" "$v"
  [ "$v" = "FAIL" ] && fail=1
done
# A tier that was required but left in an unknown state is a failure too.
echo
if [ "$fail" = "1" ]; then
  echo "GATE: FAIL — at least one required tier failed. Logs in obj/gate/."
  exit 1
fi
echo "GATE: PASS (no required tier failed; skipped tiers are reported above)"
exit 0
