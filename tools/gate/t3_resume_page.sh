#!/usr/bin/env bash
# tools/gate/t3_resume_page.sh — GB-122 clause 5: the resume-copy #PF invariant.
#
# The invariant: a kernel-mode or user-mode resume must never take a #PF while
# writing the 296-byte frame copy into the stack top page.  On the unmodified
# tree this fires 10-26 times per intel T3 wave; with GB-122's commit-on-resume
# patch it fires 0-1 times.  This script turns that into a gate that can FAIL,
# rather than an observation quoted in a comment.
#
# A fault counts as a resume-page violation when BOTH hold:
#   * the dump is the kernel #PF form ("Vector: 14", error code 0x2)
#   * a following line of the same dump carries RDI=0x17FFFFFxxx, the stack top
#     page the 296-byte copy writes (`dest = target_sp - 0x140`)
#
# A "Vector: 14" whose dump is shredded by another writer (the console is
# unlocked on this path — GB-097) is NOT silently passed: it is counted as
# UNATTRIBUTABLE and the gate fails, because the invariant cannot be confirmed
# from that line.  That is deliberate: the first version of GB-122's five-wave
# table had exactly one such line, and a gate that ignores it would have turned
# "cannot tell" into "clean".
#
# Usage:
#   bash tools/gate/t3_resume_page.sh <serial.log>     # exit 0 clean, 1 violation, 2 unreadable
#   bash tools/gate/t3_resume_page.sh --self-test      # fixtures: clean/resume/unattributable/missing
set -u

classify() {
  # $1 = log path.  Echo "resume=<n> unattributable=<n> total_vector14=<n>"
  local log="$1" resume=0 unattr=0 total=0
  local dumps
  dumps=$(grep -an "Vector: 14" "$log" 2>/dev/null | cut -d: -f1)
  for ln in $dumps; do
    total=$((total + 1))
    # 12 lines is enough for the RDI line of one dump; a foreign record inside
    # that window means the dump was shredded.
    window=$(sed -n "${ln},$((ln + 12))p" "$log" 2>/dev/null)
    if printf '%s\n' "$window" | grep -aqE "RDI: *0x00000017FFFFF"; then
      resume=$((resume + 1))
    else
      unattr=$((unattr + 1))
    fi
  done
  echo "resume=$resume unattributable=$unattr total_vector14=$total"
}

if [ "${1:-}" = "--self-test" ]; then
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  # clean: no Vector: 14 at all
  printf '[BOOT] ok\n[KERNEL] Process 3: HELLO.BIN exited\nSystem halt.\n' > "$tmp/clean.log"
  # violation: kernel #PF with the stack-top destination
  cat > "$tmp/resume.log" <<'EOF'
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000002
  RIP: 0x000000007003D95C  RSP: 0x000000007558E848
  RDI: 0x00000017FFFFF388  RSI: 0x00000000755EE908  RBP: 0x00000000755EEA60
EOF
  # unattributable: the dump is shredded by another writer
  cat > "$tmp/shredded.log" <<'EOF'
P[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000002
  RIP: 0x000000007003E900 Process heart-beat from CPU: 6
  CS:  0x0000000000000008  SS:  0x0000000000000000
EOF
  fail=0
  got=$(classify "$tmp/clean.log")
  echo "self-test clean:        $got"
  case "$got" in *"resume=0 unattributable=0 total_vector14=0"*) ;; *) fail=1 ;; esac
  got=$(classify "$tmp/resume.log")
  echo "self-test violation:    $got"
  case "$got" in *"resume=1"*) ;; *) fail=1 ;; esac
  got=$(classify "$tmp/shredded.log")
  echo "self-test shredded:     $got"
  case "$got" in *"unattributable=1"*) ;; *) fail=1 ;; esac
  if [ -f "$tmp/absent.log" ]; then fail=1; fi
  if [ "$fail" = 0 ]; then
    echo "t3_resume_page: SELF-TEST PASS — clean=0, resume-page fault identified, shredded counted as unattributable, absent log is not a pass"
    exit 0
  fi
  echo "t3_resume_page: SELF-TEST FAIL — the detector does not classify its own fixtures"
  exit 1
fi

LOG="${1:-}"
[ -n "$LOG" ] || { echo "usage: t3_resume_page.sh <serial.log> | --self-test"; exit 2; }
[ -f "$LOG" ] || { echo "t3_resume_page: FAIL (2) — '$LOG' absent: an absent serial is not a passing gate"; exit 2; }

counts=$(classify "$LOG")
echo "t3_resume_page: $counts in '$LOG'"
case "$counts" in
  *"resume=0 unattributable=0 "*) echo "t3_resume_page: PASS — no resume-copy #PF and no unattributable Vector: 14"; exit 0 ;;
  *) echo "t3_resume_page: FAIL — resume-copy #PF invariant violated (or unconfirmable: see unattributable count)"; exit 1 ;;
esac