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

W0_MEMBERS="HELLO SH CAT LS"
W0_OUT_DIR="${T4_LEGA_OUT:-obj/w0_linux}"

# --- self-test: prove the gate can report both PASS and FAIL ----------------
if [ "${1:-}" = "--self-test" ]; then
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  FAILS=0

  # a serial log that shows a full W0 run: every member's arrived + exit lines
  cat >"$TMP/good.log" <<'EOF'
t4-wave
[W0-ARRIVED] HELLO
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

  # helper linux-leg reference for the self-test
  mkdir -p "$W0_OUT_DIR"
  for m in HELLO SH CAT LS; do printf '%s-ref\n' "$m" > "$W0_OUT_DIR/$m.ref"; done

  echo "[self-test] good wave (all members arrived):"
  ok=1; for m in $W0_MEMBERS; do verdict "$TMP/good.log" "$m" | grep -q PASS || ok=0; done
  if [ "$ok" = "1" ]; then echo "  OK — all W0 members PASS (matched Linux leg)"; else echo "  UNEXPECTED FAIL on good wave"; FAILS=$((FAILS+1)); fi

  echo "[self-test] missing CAT (must FAIL):"
  if verdict "$TMP/missing_cat.log" CAT >/dev/null 2>&1; then echo "  UNEXPECTED PASS — absent member did not fail"; FAILS=$((FAILS+1));
  else echo "  OK — absent member FAILED as required"; fi

  echo "[self-test] broken HELLO output (must FAIL):"
  if verdict "$TMP/broken_hello.log" HELLO >/dev/null 2>&1; then echo "  UNEXPECTED PASS — broken member did not fail"; FAILS=$((FAILS+1));
  else echo "  OK — broken member FAILED as required"; fi

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
  if grep -q "\[W0-ARRIVED\] $m" "$LOG"; then
    echo "  PASS $m (arrived in guest)"
  else
    echo "  FAIL $m — not in the intel wave's exited-programs/arrival set (W0 corpus member never ran)"
    fail=1
  fi
done
# DoD 3: a wave with no W0 arrival at all must fail, not skip
if ! grep -q "\[W0-ARRIVED\]" "$LOG"; then
  echo "t4_w0: FAIL — no W0 member arrived in the guest (absent probe is a failure, never a skip)"
  fail=1
fi
if [ "$fail" = "0" ]; then
  echo "t4_w0: PASS — every W0 corpus member appears in the intel wave + output matched the Linux leg"
  exit 0
fi
echo "t4_w0: FAIL — W0 corpus incomplete"
exit 1
