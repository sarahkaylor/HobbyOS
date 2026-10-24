#!/usr/bin/env bash
# tools/gate/t4_glued.sh — GB-082 glued-console-line counter (v2, GB-092).
#
# A "glued console line" is one serial line carrying output from TWO different
# writers with no separator-newline between them — the observable of non-atomic
# console writes.  Measured (2026-10-23): the intel full-wave serial contains
# lines like `21 [NLTEST] PASS UNEXPAND.BINmulti-file numbering/COMM.TXT
# CMPTEST.BIN` — two programs' records fused on one line — so writes are
# per-call atomic but not whole-line atomic.
#
# v2 (GB-092): v1 counted its OWN syntax as a defect.  P1's `\[[A-Za-z0-9_-]+\]`
# treated the array subscript in `tf->regs[0]` as a second bracketed RECORD TAG,
# so a SINGLE record line
#     [DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x...
# was counted glued.  Of ARM's 417 "glued" lines, 379 were this [0]-subscript
# false positive.  The discriminating rule (per the GB-092 dispatch):
#
#   A RECORD TAG is a LETTER-LED bracketed identifier — [KERNEL], [WAVE-TRY],
#   [ARRIVED], [ALIVE-SET], ...  An array subscript ([0],[1],...) is DIGIT-led
#   and is NOT a tag.
#
# Two fused-writer shapes are counted:
#   P1  two+ letter-led record tags on one line
#       ([ARRIVED] ... [ALIVE-SET], [WAVE-TRY][ALIVE-SET], ...)
#   P2  a letter-led record tag that is NOT at a record start — immediately
#       preceded by non-space text from another writer (BLKTEST.BIN[ARRIVED],
#       HEL[ARRIVED]...,  ...]X.BIN[KERNEL]...).  A lone [0] subscript never
#       triggers this because it is digit-led and so is not a record tag.
#
# We report TWO numbers:
#   raw     the v1 superset: any line with 2+ bracketed items (record tags OR
#           [0]-style subscripts) OR any 1-bracket fusion.  Retained for
#           continuity; includes the [0]-subscript FPs v1 miscounted.
#   genuine the GB-092 measurement: only record-tag fusions (P1+P2 above).
# GB-082's DoD is graded on the GENUINE count (dispatch rule: publishing both,
# and grading on the genuine one, is the fix — never making the number fall).
#
# Env: T4_SERIAL   path to the serial log to scan (default obj/t3_<arch>.log)
# Arg: <log>       same, and it WINS over T4_SERIAL.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERIAL="${1:-${T4_SERIAL:-$ROOT/obj/t3_intel.log}}"

# --- core detector (awk) ------------------------------------------------------
# Per line, classify every bracketed token: letter-led = record tag, digit-led =
# subscript.  Emit two flags:
#   fused_any  = 2+ bracketed tokens, OR any token (tag or subscript) not at a
#                record start (preceded by non-space text) => raw superset
#   fused_tag  = 2+ record tags, OR a record tag not at a record start => genuine
# At-EOF, print both totals as "raw genuine".
AWK_PROG='
function scanline(line,   n,i,ch,pre,presp,tok,is_tag,ntags,nbr,anyfl_tag,anyfl_br,fl) {
  n=0
  while (match(line, /\[[A-Za-z0-9_][A-Za-z0-9_-]*\]/)) {
    tok=substr(line,RSTART,RLENGTH)
    ch=substr(tok,2,1)
    is_tag=(ch ~ /[A-Za-z]/)
    pre=(RSTART>1)?substr(line,RSTART-1,1):""
    presp=(pre=="" || pre ~ /[[:space:]]/)
    # fusion: this token is not at a record start
    jo=!presp
    if (is_tag) { ntags++; if (jo) anyfl_tag=1 }
    if (jo) anyfl_br=1
    nbr++
    line=substr(line,RSTART+RLENGTH)
  }
  fused_any = (nbr>=2 || anyfl_br) ? 1 : 0
  fused_tag = (ntags>=2 || anyfl_tag) ? 1 : 0
}
{
  scanline($0)
  R += fused_any
  G += fused_tag
}
END { print R, G }
'

detect() { # $1 log -> prints "raw genuine" on one line
  local log="$1"
  [ -f "$log" ] || return 1
  awk "$AWK_PROG" "$log"
}

# --- self-test ---------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  # (a) genuinely glued — two RECORD tags fused on one line.
  cat >"$TMP/glued" <<'EOF'
[ARRIVED] [TEST] 'help' output validated successfully.
[WAVE-TRY] [ALIVE-SET] no READY work
EOF
  # (b) CLEAN — one record tag per line, INCLUDING the v1-false-positive
  # /regs[0]/ shape that GB-092 demotes (a single [DEBUG] record whose body
  # carries an array subscript is NOT glued).
  cat >"$TMP/clean" <<'EOF'
[MD5TEST] PASS multi-file order
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000002A78
[KERNEL] pid=3 exited gracefully
[ARRIVED] HELLO.BIN
EOF
  # (c) injected two-writer fusions exactly as the DoD names them.
  printf 'HEL[ARRIVED] X.BIN\n' >"$TMP/fusion"
  printf '  PASS [WAVE-TRY]  CXXSMOKE.BIN[KERNEL] process_create\n' >"$TMP/fusion2"
  cg=$(detect "$TMP/glued"); cc=$(detect "$TMP/clean")
  cf=$(detect "$TMP/fusion"); cf2=$(detect "$TMP/fusion2")
  echo "self-test: glued=[$cg] clean=[$cc] fusion=[$cf] fusion2=[$cf2] (want glue>0 / 0,0 / >=1 / >=1)"
  T4_MISSING_RC=0
  bash "${BASH_SOURCE[0]}" "$TMP/does_not_exist.log" >"$TMP/missing.out" 2>&1 || T4_MISSING_RC=$?
  echo "self-test: absent-log exit=$T4_MISSING_RC (want non-zero)"
  g1=$(echo "$cg"|awk '{print $2}'); c1=$(echo "$cc"|awk '{print $2}')
  f1=$(echo "$cf"|awk '{print $2}'); f21=$(echo "$cf2"|awk '{print $2}')
  [ "${g1:-0}" -gt 0 ] && [ "${c1:-9}" = "0" ] && [ "${f1:-0}" -ge 1 ] && [ "${f21:-0}" -ge 1 ] && [ "$T4_MISSING_RC" != "0" ] && {
    echo "self-test: OK — genuine FAILs on glue, 0 on clean (incl [0]-subscript single records), catches injected fusions, refuses a missing log"; exit 0; }
  echo "self-test: FAIL"; exit 1
fi

# --- real gate ---------------------------------------------------------------
if [ ! -f "$SERIAL" ]; then
  echo "t4_glued: FAIL (exit 2) — serial log '$SERIAL' absent. A missing log is not a clean log: 0 glued lines cannot be measured over nothing."
  exit 2
fi
DG=$(detect "$SERIAL") || { echo "t4_glued: FAIL (exit 2) — cannot read '$SERIAL'"; exit 2; }
RAW=$(echo "$DG"|awk '{print $1}')
GEN=$(echo "$DG"|awk '{print $2}')
echo "t4_glued: raw=$RAW genuine=$GEN glued lines in '$SERIAL'"
echo "          (GB-092: GB-082 DoD is graded on the GENUINE count; RAW is the v1"
echo "           superset incl. [0]-array-subscript single records v1 misread.)"
if [ "$GEN" = "0" ]; then
  echo "t4_glued: PASS — every console line is a single writer (whole-line atomic)."
  exit 0
fi
echo "t4_glued: FAIL — $GEN genuine glued lines (writes not whole-line atomic)."
exit 1
