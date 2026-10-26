#!/usr/bin/env bash
# GB-101-V2 clauses 1 and 2: inject torn-frame records into a real 0-FAIL halted
# wave log and require the police verdict to flip PASS -> FAIL, naming the line.
set -u
OUT=evidence/gb-101-v2
BASE=$OUT/clean_wave.log

inject() { # $1 dst  $2 line
  awk -v line="$2" '{ if ($0 ~ /^System halt/ && !done) { print line; done=1 } print }' \
      "$BASE" >"$1"
}

case_run() { # $1 label  $2 log
  echo "=== $1"
  T3_MIN_LINES=0 T3_MIN_PROGRAMS=0 bash tools/t3_tier.sh police "$2" >"$OUT/police_$1.txt" 2>&1
  echo "POLICE_EXIT=$?"
  grep -aE "TORNFRAME|T3_intel \(police|T3_intel EXIT" "$OUT/police_$1.txt" | head -4
  echo
}

inject "$OUT/inj_below32.log" \
 '[TORNFRAME] FAIL - interrupt/fault handler read a torn frame: vector=6 rip=0x7 cs=0x0'
case_run below32 "$OUT/inj_below32.log"

inject "$OUT/inj_4E20.log" \
 '[TORNFRAME] FAIL - interrupt/fault handler read a torn frame: vector=20000 rip=0x97 cs=0x08'
case_run above32_4E20 "$OUT/inj_4E20.log"

inject "$OUT/inj_7703A33E.log" \
 '[TORNFRAME] FAIL - interrupt/fault handler read a torn frame: vector=1996485438 rip=0x7003A33E cs=0x08'
case_run above32_7703A33E "$OUT/inj_7703A33E.log"

case_run baseline_again "$BASE"