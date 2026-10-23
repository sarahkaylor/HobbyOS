#!/usr/bin/env bash
# tools/gate/t4_glued.sh — GB-082 glued-console-line counter.
#
# A "glued console line" is one serial line carrying output from TWO different
# writers with no separ~-newline between them — the observable of non-atomic
# console writes.  Measured (2026-10-23): the intel full-wave serial contains
# lines like `21 [NLTEST] PASS UNEXPAND.BINmulti-file numbering/COMM.TXT
# CMPTEST.BIN` — two programs' records fused on one line — so writes are
# per-call atomic but not whole-line atomic.
#
# This gate COUNTS glued lines in a captured serial log and FAILs (exit 1) when
# any exist, because a tier that gets interleaved records cannot cleanly
# attribute a FAIL to the tag in front of it (the DoD's third clause).  It is
# the instrument the ticket names; the kernel whole-line-atomicity fix is the
# other half (gated on the counter, exactly like GB-079's two halves).
#
# Detection rule — a serial line is "glued" if it contains TWO OR MORE
# distinct bracketed record tags, i.e. 2+ matches of [TAG_PREFIX] in
# identifiable record-start positions, OR text fuses two records with no
# newline (a soft separator like "multi-file" appearing mid-line after a tag's
# output).  Concretely we count two predicates and require 0 total:
#   P1: 2+ distinct `[...]` tags on one line (e.g. [KERNEL]...[KERNEL]).
#   P2: one tag followed (not after whitespace) by a second record start, seen
#       as `]<letter>` or `]0x` immediately after a closing bracket mid-line.
# Both are host-cheap greps; the DoD's "can-fail self-test" uses a synthetic
# fixture with a known glued line and a known clean one.
#
# Env: T4_SERIAL   path to the serial log to scan (default obj/t3_<arch>.log)
# Arg: <log>       same, and it WINS over T4_SERIAL.  (Coordinator 2026-10-23:
#                  argv used to be ignored, so `t4_glued.sh some/log` silently
#                  scanned the default path — and a missing file counted 0.)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERIAL="${1:-${T4_SERIAL:-$ROOT/obj/t3_intel.log}}"

# --- counter ---------------------------------------------------------------
count_glued() { # $1 log -> prints count
  local log="$1"
  # An ABSENT log is not a clean log: the caller decides (see the real gate).
  [ -f "$log" ] || return 1
  local l1 l2
  # P1: 2+ distinct bracketed tags on one line (dedupe within the line).
  l1=$(grep -aE '\[[A-Za-z0-9_-]+\].*\[[A-Za-z0-9_-]+' "$log" \
         | grep -avc 'STACK\[' || true)
  # P2: a record end fused straight into another (no whitespace): `]<LETTER`
  # or `]0x` mid-line — the classic two-writers-one-line fusion.
  l2=$(grep -aE '\][A-Za-z0-9_].*\[[A-Za-z0-9_-]+\]' "$log" \
         | grep -avc 'STACK\[' || true)
  echo $(( ${l1:-0} + ${l2:-0} ))
}

# --- self-test -------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  # (a) a glued line — two writers' records fused on one line (measured shape:
  #     `[ARRIVED] [TEST] 'help'...` and `[WAVE-TRY][ALIVE-SET]` both appear in
  #     the real intel serial, i.e. two distinct bracket tags back-to-back).
  cat >"$TMP/glued" <<'EOF'
[ARRIVED] [TEST] 'help' output validated successfully.
[WAVE-TRY] [ALIVE-SET] no READY work
[KERNEL] pid=28 memory fault VA=0x17FFFFFE5C
[ARRIVED] /BLKT2.TXT
EOF
  # (b) a clean line — one tag per line.
  cat >"$TMP/clean" <<'EOF'
[MD5TEST] PASS multi-file order
[KERNEL] pid=3 exited gracefully
[ARRIVED] HELLO.BIN
EOF
  cg=$(count_glued "$TMP/glued")
  cc=$(count_glued "$TMP/clean")
  echo "self-test: glued fixture count=$cg (want >0), clean fixture count=$cc (want 0)"
  # (c) an ABSENT log must NOT be a pass — the defect class this gate exists for.
  T4_MISSING_RC=0
  bash "${BASH_SOURCE[0]}" "$TMP/does_not_exist.log" >"$TMP/missing.out" 2>&1 || T4_MISSING_RC=$?
  echo "self-test: absent-log exit=$T4_MISSING_RC (want non-zero): $(tail -1 "$TMP/missing.out")"
  [ "${cg:-0}" -gt 0 ] && [ "${cc:-1}" = "0" ] && [ "$T4_MISSING_RC" != "0" ] && {
    echo "self-test: OK — counter can fail, stays 0 on clean, and refuses a missing log"; exit 0; }
  echo "self-test: FAIL"; exit 1
fi

# --- real gate -------------------------------------------------------------
if [ ! -f "$SERIAL" ]; then
  echo "t4_glued: FAIL (exit 2) — serial log '$SERIAL' absent. A missing log is not a"
  echo "          clean log: 0 glued lines cannot be measured over nothing."
  exit 2
fi
g=$(count_glued "$SERIAL") || { echo "t4_glued: FAIL (exit 2) — cannot read '$SERIAL'"; exit 2; }
echo "t4_glued: $g glued console lines in '$SERIAL'"
if [ "$g" = "0" ]; then
  echo "t4_glued: PASS — every console line is a single writer (whole-line atomic)."
  exit 0
fi
echo "t4_glued: FAIL — $g glued lines (writes not whole-line atomic); the next"
echo "          GB-057 FAIL-attribution ratio cannot be trusted until this is 0."
echo "  sample glued lines:"
grep -aE '\[[A-Za-z0-9_]+\].*\[[A-Za-z0-9_]+' "$SERIAL" 2>/dev/null | grep -av 'STACK\[' | head -3
exit 1
