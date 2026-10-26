#!/usr/bin/env bash
# GB-101-V clause 1/2 control: police-mode classifier on one log, clean vs torn.
#
# `tools/t3_tier.sh police <log>` is a log CLASSIFIER (it applies the tier's
# verdict rules to an already-captured console).  The control the card asks for
# is: the same log PASSes clean, and FAILs by name once a [TORNFRAME] line is
# injected.  A synthetic baseline is used because no captured intel log on this
# box is both >= the 1000-line floor and free of the known GB-093/095 FAIL rows;
# the log's shape (halt, boot count, line floor) is the classifier's contract.
#
# The injected lines carry the vector/rip pairs measured on 2026-10-26:
#   vector=20000 (=0x4E20), vector=1996485438 (=0x7703A33E), vector=6 rip=0x7
set -u
OUT="${1:-evidence/gb-101-v}"
mkdir -p "$OUT"

mkbase() { # $1 path
  : >"$1"
  for i in $(seq 1 1200); do printf 'guest line %04d\n' "$i" >>"$1"; done
  printf 'Controller reset complete!\n' >>"$1"
  printf 'System halt from CPU 3.\n' >>"$1"
}

inject() { # $1 src $2 dst $3 line
  cp "$1" "$2"
  # insert just before the halt line, like a real record would appear
  awk -v line="$3" '{ if ($0 ~ /^System halt/ && !done) { print line; done=1 } print }' \
      "$1" >"$2"
}

BASE="$OUT/ctl_base.log"
mkbase "$BASE"

run_case() { # $1 label, $2 log
  echo "=== $1"
  bash tools/t3_tier.sh police "$2" >"$OUT/police_$1.txt" 2>&1
  echo "EXIT=$?"
  grep -aE "TORNFRAME|police .*lines|EXIT=" "$OUT/police_$1.txt" | head -4
  echo
}

run_case clean "$BASE"

inject "$BASE" "$OUT/ctl_torn_4E20.log" \
  '[TORNFRAME] FAIL - interrupt/fault handler read an impossible vector=20000 (x86 IDT has 256 gates; >0xFF means the trap frame is corrupt, not a real exception) rip=0x97'
run_case torn_4E20 "$OUT/ctl_torn_4E20.log"

inject "$BASE" "$OUT/ctl_torn_7703A33E.log" \
  '[TORNFRAME] FAIL - interrupt/fault handler read an impossible vector=1996485438 (x86 IDT has 256 gates; >0xFF means the trap frame is corrupt, not a real exception) rip=0x7003A33E'
run_case torn_7703A33E "$OUT/ctl_torn_7703A33E.log"

inject "$BASE" "$OUT/ctl_torn_v6.log" \
  '[TORNFRAME] FAIL - interrupt/fault handler read an impossible vector=6 (x86 IDT has 256 gates; >0xFF means the trap frame is corrupt, not a real exception) rip=0x7'
run_case torn_v6_BELOW32 "$OUT/ctl_torn_v6.log"