#!/usr/bin/env bash
# tools/gate/t4_w0.sh — GB-070 W0-corpus differential gate.
#
# The M1 gate names a corpus (W0: hello / cat / sh / coreutils-static) that the
# INTEL wave's *exited-programs set* is supposed to carry.  Measured on a full
# intel wave (63 programs exited): only HELLO.BIN is a W0 member — cat/sh do not
# auto-run in the boot wave, so the corpus the M1 sentence cites is not the one
# the harness checks.  A gate over the wrong set reports green by construction.
#
# DoD (this gate):
#   1. every W0 member appears in the intel wave's exited-programs set (serial
#      log), and
#   2. its output matches the Linux leg (static-musl, run on the host) — or the
#      member's own 'arrived' line is present, proving it ran in the guest; and
#   3. a member missing from the set, or an absent probe, FAILS the gate (never
#      a silent skip); a deliberately-broken member FAILS too.
#
# Usage:
#   t4_w0.sh --serial <intel-wave-log> [--members 'HELLO CAT SH LS']
#       run the gate over an already-captured intel wave serial log.
#   t4_w0.sh --self-test
#       prove it can fail: a missing member FAILs, a present+matched member
#       PASSes, a deliberately-broken (mismatched output) member FAILs.
#
# Env: T4_LEGA_OUT  dir of Linux-leg outputs (default obj/w0_linux/)
#      W0_TIMEOUT   optional
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# GB-138 (2026-10-30): W0PIE is the Linux-ABI static-PIE ELF member (builds from
# src/user/w0_pie_probe.c, ET_DYN, carried through elf_loader_load which sets
# PROC_FLAG_LINUX_ABI).  It is the member that makes the compat table measurable;
# the gate asserts it AND its abi= flag (clause 2: an ELF member's load line shows
# abi=0x1, a native .bin abi=0).  The other four stay the native flat corpus.
W0_MEMBERS="W0PIE HELLO SH CAT LS"
W0_OUT_DIR="${T4_LEGA_OUT:-$ROOT/tests/fixtures/abi/w0_output}"

# --- self-test: prove the gate can report both PASS and FAIL ----------------
if [ "${1:-}" = "--self-test" ]; then
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  FAILS=0

  # a serial log that shows a full W0 run: every member's arrived + exit lines
  cat >"$TMP/good.log" <<'EOF'
t4-wave
[W0-ARRIVED] HELLO
[W0-ARRIVED] W0PIE
v2 loader: PID=1 abi=0x1 resident=1
[W0-ARRIVED] SH
[W0-ARRIVED] CAT
[W0-ARRIVED] LS
System halt
EOF
  # a log where CAT never ran (even though others did) -> must FAIL
  cat >"$TMP/missing_cat.log" <<'EOF'
t4-wave
[W0-ARRIVED] HELLO
[W0-ARRIVED] SH
[W0-ARRIVED] LS
System halt
EOF
  # a log with HELLO output that does NOT match the Linux leg -> must FAIL
  cat >"$TMP/broken_hello.log" <<'EOF'
t4-wave
[W0] HELLO linux=WRONG
[W0-ARRIVED] HELLO
System halt
EOF
  # GB-138 clause-3 mirror: a wave where W0PIE arrived but its load line lacks the
  # compat flag ('abi=0x1' absent) -> must FAIL (the flag is not proven at runtime)
  cat >"$TMP/failed_abi.log" <<'EOF'
t4-wave
[W0-ARRIVED] W0PIE
v2 loader: PID=1 abi=0 resident=1
System halt
EOF

  verdict() { # $1 log, $2 member -> PASS/FAIL (echoes verdict, returns 0/1)
    local log="$1" m="$2" rc=0
    # DoD 3: absent probe fails, never skips
    if ! grep -q "System halt" "$log"; then echo "FAIL(no halt)"; return 1; fi
    if ! grep -q "\[W0-ARRIVED\] $m" "$log"; then echo "FAIL($m absent)"; return 1; fi
    # DoD 2: output match against Linux leg (or 'linux=' marker equal)
    local line; line=$(grep "\[W0\] $m linux=" "$log" | head -1)
    if [ -n "$line" ]; then
      local expected; expected=$(grep "\[W0\] $m linux=" "$log" | sed 's/.*linux=//')
      local linuxref; linuxref=$(cat "$W0_OUT_DIR/$m.ref" 2>/dev/null || true)
      if [ -n "$linuxref" ] && [ "$expected" != "$linuxref" ]; then echo "FAIL($m output)"; return 1; fi
    fi
    echo "PASS($m)"; return 0
  }

  # helper linux-leg reference for the self-test (built in a THROWAWAY dir,
  # never $W0_OUT_DIR the real committed corpus — GB-024: the selftest must
  # not clobber the genuine Linux-leg .ref fixtures with '-ref' placeholders)
  SCRATCH_OUT="$TMP/w0out"
  mkdir -p "$SCRATCH_OUT"
  for m in W0PIE HELLO SH CAT LS; do printf '%s-ref\n' "$m" > "$SCRATCH_OUT/$m.ref"; done
  # verdict() reads $W0_OUT_DIR; the self-test pins it to the scratch corpus
  W0_OUT_DIR="$SCRATCH_OUT"

  echo "[self-test] good wave (all members arrived):"
  ok=1; for m in $W0_MEMBERS; do verdict "$TMP/good.log" "$m" | grep -q PASS || ok=0; done
  if [ "$ok" = "1" ]; then echo "  OK — all W0 members PASS (matched Linux leg)"; else echo "  UNEXPECTED FAIL on good wave"; FAILS=$((FAILS+1)); fi

  echo "[self-test] missing CAT (must FAIL):"
  if verdict "$TMP/missing_cat.log" CAT >/dev/null 2>&1; then echo "  UNEXPECTED PASS — absent member did not fail"; FAILS=$((FAILS+1));
  else echo "  OK — absent member FAILED as required"; fi

  echo "[self-test] broken HELLO output (must FAIL):"
  if verdict "$TMP/broken_hello.log" HELLO >/dev/null 2>&1; then echo "  UNEXPECTED PASS — broken member did not fail"; FAILS=$((FAILS+1));
  else echo "  OK — broken member FAILED as required"; fi

  echo "[self-test] W0PIE-compat flag absent (must FAIL):"
  # The abi=0x1 requirement lives in the real-gate member loop (verdict() judges
  # arrival/output only), so exercise THAT on the failed_abi fixture.
  if bash "${BASH_SOURCE[0]}" --serial "$TMP/failed_abi.log" --members W0PIE >/dev/null 2>&1; then echo "  UNEXPECTED PASS — missing abi=0x1 did not fail the gate"; FAILS=$((FAILS+1));
  else echo "  OK — gate FAILED a member whose compat flag (abi=0x1) is absent"; fi
  echo "[self-test] absent .ref for a member that emits a marker (must FAIL):"
  # A wave whose member emits '[W0] X linux=...' but whose .ref is not committed
  # must FAIL (an absent reference is a missing comparison, not a pass).  Point a
  # scratch corpus at an empty dir so the member's marker can never match a ref.
  printf 'marker-writer\n' > "$TMP/marker_only.log"
  printf '[W0] NOENTS linux=hello\n[W0-ARRIVED] NOENTS\nSystem halt\n' >> "$TMP/marker_only.log"
  if W0_OUT_DIR="$TMP/emptydir" bash "${BASH_SOURCE[0]}" --serial "$TMP/marker_only.log" --members NOENTS >/dev/null 2>&1; then echo "  UNEXPECTED PASS — member with an absent .ref did not fail"; FAILS=$((FAILS+1));
  else echo "  OK — member emitting a marker with no .ref FAILED (absent ref is no longer a pass)"; fi

  echo "self-test result: fails=$FAILS"
  [ "$FAILS" = "0" ]; exit $?
fi

# --- real gate over a captured intel wave serial log -------------------------
LOG=""
MEMS="$W0_MEMBERS"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --serial) LOG="$2"; shift 2;;
    --members) MEMS="$2"; shift 2;;
    *) echo "usage: t4_w0.sh --serial <log> [--members '...']"; exit 2;;
  esac
done
[ -n "$LOG" ] || { echo "t4_w0: FATAL — --serial <intel-wave-log> required"; exit 2; }
[ -f "$LOG" ] || { echo "t4_w0: FATAL — serial log '$LOG' absent"; exit 1; }

fail=0
echo "=== t4_w0: W0 corpus gate over $LOG ==="
for m in $MEMS; do
  # Arrival is proven by either marker: the [W0-ARRIVED] form the gate was
  # written around, or the loader's own [ARRIVED] <name>.BIN line, which is
  # what any real wave emits since the arrival instrument landed (2bab246).
  # Without the second form this gate could never pass on a real wave: no
  # program in the tree has ever printed [W0-ARRIVED], so a correct corpus was
  # reported as "never ran".
  if grep -q "\\[W0-ARRIVED\\] $m" "$LOG" || grep -q "\\[ARRIVED\\] $m\\.BIN" "$LOG" \
     || grep -q "\\[W0\\] $m linux=" "$LOG"; then
    echo "  PASS $m (arrived in guest)"
  else
    echo "  FAIL $m — not in the intel wave's exited-programs/arrival set (W0 corpus member never ran)"
    fail=1
  fi
  # GB-138 clause 2: an ELF (Linux-ABI) member must show the compat flag on its
  # loader line.  A native .bin shows 'abi=0'; a static-pie ELF shows 'abi=0x1'.
  # A gate that cannot tell the difference is the GB-007 zero-row defect one
  # level up (comparing the wrong kind of binary), so require the flag line for
  # members built as ELF (W0PIE is the one by construction today).
  if [ "$m" = "W0PIE" ] && ! grep -q "abi=0x1" "$LOG"; then
    echo "  FAIL $m — no 'abi=0x1' on its loader line: the Linux-ABI flag is not proven at runtime (GB-138 clause 2)"
    fail=1
  fi
  # The output half of the DoD, exercised live: if this member emits a
  # '[W0] <member> linux=<output>' marker (GB-024, hello.c now does), compare
  # it against the Linux-leg .ref corpus and FAIL on mismatch — so "writes
  # expected output" genuinely passes/fails on a real intel run, not only in
  # --self-test.  Members with no marker and no .ref are still judged on
  # arrival (their output is covered by the same-diff baseline, stated below).
  line=$(grep "\[W0\] $m linux=" "$LOG" | head -1)
  if [ -n "$line" ]; then
    expected=$(grep "\[W0\] $m linux=" "$LOG" | sed 's/.*linux=//' | head -1 | tr -d '\r')
    linuxref=$(cat "$W0_OUT_DIR/$m.ref" 2>/dev/null | tr -d '\r' || true)
    # GB-024-V (10-25, RED): a member that emits a '[W0] <m> linux=' marker but has
    # NO committed .ref used to print "PASS ... == ''" — an absent reference turned
    # the verification off without failing the gate.  A marker names Linux-leg
    # output that MUST be compared; if the reference is missing, that is a failing
    # corpus (the gate cannot verify the very thing its member claims to emit),
    # not a pass-by-construction.  Absent ref -> FAIL, never a silent skip.
    if [ -z "$linuxref" ]; then
      echo "  FAIL $m — emits a '[W0] $m linux=' marker but '$W0_OUT_DIR/$m.ref' is absent: the Linux-leg reference its output must match does not exist (GB-024-V)"
      fail=1
    elif [ "$expected" != "$linuxref" ]; then
      echo "  FAIL $m — guest output marker '$expected' != Linux-leg '$linuxref' (writes expected output: NO)"
      fail=1
    else
      echo "  PASS $m output — guest '$expected' == Linux-leg '$linuxref'"
    fi
  fi
done
# DoD 3: a wave with no W0 arrival at all must fail, not skip
if ! grep -q "\[W0-ARRIVED\]" "$LOG" && ! grep -qE "\[ARRIVED\] (HELLO|CAT|SH|LS)\.BIN" "$LOG"; then
  echo "t4_w0: FAIL — no W0 member arrived in the guest (absent probe is a failure, never a skip)"
  fail=1
fi
if [ "$fail" = "0" ]; then
  echo "t4_w0: PASS — every W0 corpus member arrived in the intel wave; members emitting a"
  echo "               '[W0] <member> linux=' marker had their output verified against the"
  echo "               Linux-leg .ref corpus (a mismatched marker FAILs)."
  exit 0
fi
echo "t4_w0: FAIL — W0 corpus incomplete or output mismatch"
exit 1
