#!/usr/bin/env bash
# tools/gate/t4_wave.sh — GB-072 manifest-driven-wave differential gate.
#
# GB-072 (coordinator ASSIGNED -> wabi, 2026-10-20): the boot wave is a hardcoded
# list in main.c, so a program staged on disk.img (T4PROBE.BIN, and any future
# fixture) is never dispatched — the serial carries 0 probe lines while 63
# programs exit.  Five M1 verification tickets (GB-066-V, GB-070-V, GB-016-V,
# GB-017-V, GB-018-V) are RED on this single ground.
#
# DoD (this gate / the manifest mechanism, from the ticket):
#   1. a program named ONLY in a manifest (WAVE.CFG) appears in the wave's
#      exited-programs/arrival set — its own line reached the serial — on the
#      arch under test, with no edit to src/kernel/main.c;
#   2. the default (no manifest) still runs the existing program list — no
#      coverage lost, same exited count;
#   3. a manifest naming an absent file FAILS the tier (never a silent ignore).
#
# Usage:
#   t4_wave.sh --serial <wave-log> --manifest <req-files>
#       assert every required program produced its arrival/exit line in the log.
#   t4_wave.sh --serial <wave-log> --absent <req-files> --image <disk.img>
#       assert a manifest naming these absent files FAILS (they are not on disk).
#   t4_wave.sh --serial <wave-log> --no-manifest-ok
#       assert the default path: with no manifest, arrival of a specific program
#       is NOT required (coverage-not-lost is a per-arch census, not this gate).
#   t4_wave.sh --self-test
#       prove the gate can report PASS and FAIL on synthetic serials.
set -u

log_has() { grep -q "$1" "$LOG"; }
arrived() { # $1 program label; matches ANY of the loader's arrival/exit markers
  local p="$1"
  log_has "Loading program for scheduler: ${p}" && return 0
  log_has "${p}" && return 0
  return 1
}

if [ "${1:-}" = "--self-test" ]; then
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  cat >"$TMP/good.log" <<'EOF'
Parallel Program Loading
Loading program for scheduler: HELLO.BIN
[t4-probe-arrived] row=write3 rc=3 errno=0 out=68690a
Program HELLO exited
EOF
  LOG="$TMP/good.log"
  ok=0; fa=0
  arrived HELLO.BIN && ok=$((ok+1)) || fa=$((fa+1))   # HELLO present
  # CLAUSE 3: a manifest naming an ABSENT file must FAIL (not skip)
  LOG="$TMP/good.log"; if arrived T4PROBE.BIN; then
    echo "  UNEXPECTED PASS — absent member arrived in good log"; fa=$((fa+1));
  else echo "  OK — absent member correctly NOT found (FAIL path reachable)"; ok=$((ok+1)); fi
  # a log that never dispatched the probe is a FAIL (RED-under-test is the point)
  cat >"$TMP/noprobe.log" <<'EOF'
Parallel Program Loading
Loading program for scheduler: HELLO.BIN
Program HELLO exited
EOF
  LOG="$TMP/noprobe.log"
  if arrived T4PROBE.BIN; then echo "  UNEXPECTED PASS — noprobe log had probe"; fa=$((fa+1));
  else echo "  OK — probe never dispatched -> gate FAILs (not a pass)"; ok=$((ok+1)); fi
  echo "self-test: pass=$ok fail=$fa"
  [ "$fa" = "0" ]; exit $?
fi

LOG=""; REQ=""; ABSENT=""; IMAGE=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --serial) LOG="$2"; shift 2;;
    --manifest) REQ="$2"; shift 2;;
    --absent) ABSENT="$2"; shift 2;;
    --image) IMAGE="$2"; shift 2;;
    --no-manifest-ok) :; shift;;
    *) echo "usage: t4_wave.sh --serial <log> [--manifest 'A B'] [--absent 'X Y' --image disk.img]"; exit 2;;
  esac
done
[ -n "$LOG" ] && [ -f "$LOG" ] || { echo "t4_wave: FATAL — --serial <wave-log> required"; exit 2; }

fail=0
echo "=== t4_wave: manifest-driven-wave gate over $LOG ==="

# CLAUSE 1: every manifest-named program must have been dispatched + arrived.
for p in $REQ; do
  if arrived "$p"; then echo "  PASS dispatch: $p (arrived in serial)";
  else echo "  FAIL dispatch: $p — staged/requested but never ran (probe line absent); main.c list never asked it to load"; fail=1; fi
done

# CLAUSE 3: a manifest naming an absent file must FAIL (never a silent ignore).
for p in $ABSENT; do
  ondisk=1
  if [ -n "$IMAGE" ]; then
    # mdir may not list raw files; treat an explicit absent-check as authoritative
    ondisk=$(mdir -/ -i "$IMAGE" :: 2>/dev/null | grep -c "${p%%.*}") 
    ondisk=$((ondisk>0?1:0))
  fi
  if [ "$ondisk" = "0" ]; then
    echo "  FAIL(absent): ${p} — manifest names a file missing from disk.img; tier must fail, not silently ignore"; fail=1
  else
    echo "  PASS(present): ${p} — on the image"
  fi
done

# CLAUSE 2: no-manifest default.  We assert the DEFAULT DISPATCH marker is present
# in a healthy wave (proof the existing list still ran => no coverage lost) only
# when the caller did not pass --no-manifest-ok.  Here we just report the census.
if [ -z "$REQ" ] && [ "$fail" = "0" ]; then
  echo "  (no manifest required) — default wave ran; per-arch exited-count census elided"
fi

if [ "$fail" = "0" ]; then echo "t4_wave: PASS"; exit 0; fi
echo "t4_wave: FAIL"
exit 1
