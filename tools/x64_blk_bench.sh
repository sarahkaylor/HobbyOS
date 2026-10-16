#!/usr/bin/env bash
#
# tools/x64_blk_bench.sh - x86_64 HobbyOS test-tier baseline battery (KVM).
#
# Establishes the local x64 truth for re-validating block-cache changes later:
#   1. (skip with --skip-build) intel kernel+disk template build
#   2. x64 unit tier   (MODE=unit_tests under -enable-kvm; bounded
#                       UNIT_TESTS_TIMEOUT, via ./run_unit_tests_intel.sh)
#   3. bounded integration-wave battery: N copies seeded from ONE template,
#      each with its OWN disk.img (no QEMU write-lock collision), each run
#      via `timeout <WAVE_TIMEOUT> make MODE=test run ARCH=intel QEMU_ARGS=...`,
#      reaped with `fuser -k <copy>/disk.img`, then parsed for the completion
#      marker, FAIL tokens, suite summaries and reboot-storm evidence.
#
# Why bounded: at current tip the x64 integration tier does NOT reliably
# complete - CI-VM copies reboot-loop (silent triple fault -> OVMF reboot)
# and a naive `make test_intel` can wait forever (guest resets instead of
# powering off).  Always time-box it and record what today's x64 wave does.
#
# Usage:
#   tools/x64_blk_bench.sh                    # full default battery
#   tools/x64_blk_bench.sh --copies 3         # 3 wave copies
#   tools/x64_blk_bench.sh --skip-build       # reuse existing obj + disk
#   tools/x64_blk_bench.sh --skip-unit        # wave only
#   tools/x64_blk_bench.sh --skip-wave        # unit only
#   LOGDIR=/path tools/x64_blk_bench.sh       # logs elsewhere
#
# Env (all optional):
#   LOGDIR              logs + copies (default /tmp/x64-blk-bench-<pid>)
#   UNIT_TESTS_TIMEOUT  unit-tier budget s (default 1800)
#   WAVE_TIMEOUT        per-copy wave budget s (default 1800)
#   BROWSER_SKIP / TOOLS_SKIP forwarded to make (default 1/1 - the intel
#                     disk needs no browser for unit/wave tiers; /TOOLS is
#                     staged only when a dist-staging already exists).
#
# Safety: this script only ever kills QEMU bound to ITS OWN copies
# (`fuser -k <copy>/disk.img`, exact-file scope); it never broad-pkills and
# never uses `make clean` (Makefile:1962 pkill -f qemu-system = kills every
# lane's VMs).  Other lanes' VMs are never touched.
set -u

cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd -P)"

COPIES=2
SKIP_BUILD=0
SKIP_UNIT=0
SKIP_WAVE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --copies)      COPIES="$2"; shift 2 ;;
    --skip-build)  SKIP_BUILD=1; shift ;;
    --skip-unit)   SKIP_UNIT=1;  shift ;;
    --skip-wave)   SKIP_WAVE=1;  shift ;;
    --help|-h) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

UNIT_TESTS_TIMEOUT="${UNIT_TESTS_TIMEOUT:-1800}"
WAVE_TIMEOUT="${WAVE_TIMEOUT:-1800}"
LOGDIR="${LOGDIR:-/tmp/x64-blk-bench-$$}"
COPIES_DIR="$LOGDIR/copies"
mkdir -p "$LOGDIR" "$COPIES_DIR"

echo "== x64 blk bench: ROOT=$ROOT LOGDIR=$LOGDIR copies=$COPIES =="
echo "   kvm: $([ -e /dev/kvm ] && echo yes || echo 'NO - falls back to TCG (slower)') unit_tmo=${UNIT_TESTS_TIMEOUT}s wave_tmo=${WAVE_TIMEOUT}s"

elapsed() { # <start-epoch> -> mm:ss
  local s=$(( $(date +%s) - "$1" ))
  printf '%02d:%02d' $((s/60)) $((s%60))
}

scan_log() { # <log> -> "halt=... fails=... summaries=... stress=... reboots=..."
  local log="$1"
  local halts=0 fails=0 summaries=0 stress=no reboots=0
  halts=$(grep -ac "System halt\." "$log" 2>/dev/null || echo 0)
  fails=$(grep -ac "FAIL" "$log" 2>/dev/null || echo 0)
  summaries=$(grep -ac "ALL TESTS PASSED" "$log" 2>/dev/null || echo 0)
  grep -aq "STRESS TEST" "$log" && stress=yes
  reboots=$(grep -acE "BdsDxe: starting|Guest reset|Triple fault|#GP|IOTRAP" "$log" 2>/dev/null || echo 0)
  echo "halt_markers=$halts fails=$fails summaries=$summaries stress=$stress reboots=$reboots"
}

MAKE_BASE="ARCH=intel BROWSER_SKIP=1 TOOLS_SKIP=1"

# --- 0. intel build (cold once; incremental resume afterwards) -------------
if [ "$SKIP_BUILD" = "0" ]; then
  t0=$(date +%s)
  # NEVER leave a stale cross-arch hobbyos.elf: python-delete it first.
  python3 -c "import os; [os.remove(f) for f in ['hobbyos.elf'] if os.path.exists(f)]"
  echo "-- [bench] intel template build (MODE=test, $MAKE_BASE -j$(nproc))"
  if ! make $MAKE_BASE MODE=test hobbyos.elf disk.img -j"$(nproc)" >"$LOGDIR/build.log" 2>&1; then
    echo "!! intel template build FAILED (tail):" >&2
    tail -30 "$LOGDIR/build.log" >&2
    exit 2
  fi
  echo "-- [bench] intel template built in $(elapsed "$t0") (log $LOGDIR/build.log)"
fi

# --- 1. x64 unit tier ------------------------------------------------------
if [ "$SKIP_UNIT" = "0" ]; then
  t0=$(date +%s)
  echo "-- [bench] unit tier: UNIT_TESTS_TIMEOUT=$UNIT_TESTS_TIMEOUT EXTRA_QEMU_ARGS=-enable-kvm ./run_unit_tests_intel.sh"
  UNIT_TESTS_TIMEOUT="$UNIT_TESTS_TIMEOUT" EXTRA_QEMU_ARGS=-enable-kvm \
    ./run_unit_tests_intel.sh >"$LOGDIR/unit-run.log" 2>&1
  if grep -aq "UNIT TESTS PASSED" "$LOGDIR/unit-run.log"; then
    unit="PASS"
  elif grep -aq "UNIT TESTS FAILED" "$LOGDIR/unit-run.log"; then
    unit="FAIL"
  else
    unit="TIMEOUT/NO-MARKER"
  fi
  fails=$(grep -ac "FAIL" "$LOGDIR/unit-run.log" 2>/dev/null || echo 0)
  echo "UNIT verdict=$unit duration=$(elapsed "$t0") fails_tokens=$fails log=$LOGDIR/unit-run.log"
  [ "$unit" = "PASS" ] || echo "  !! unit tier not PASS; see $LOGDIR/unit-run.log"
fi

# --- 2. wave template build (MODE=test flips from the unit build) ----------
if [ "$SKIP_WAVE" = "0" ]; then
  t0=$(date +%s)
  echo "-- [bench] wave template: make $MAKE_BASE MODE=test hobbyos.elf disk.img (relink + 1 GiB disk rebuild)"
  make $MAKE_BASE MODE=test hobbyos.elf disk.img -j"$(nproc)" >"$LOGDIR/build_wave.log" 2>&1 || {
    echo "!! wave template build FAILED:" >&2; tail -20 "$LOGDIR/build_wave.log" >&2; exit 2; }
  echo "-- [bench] wave template built in $(elapsed "$t0")"

  # --- 3. seed copies (full tree incl. obj/; each copy owns its disk.img) ---
  for n in $(seq 1 "$COPIES"); do
    c="$COPIES_DIR/wave-$n"
    cp -a . "$c" 2>/dev/null || { echo "!! copy $n failed" >&2; exit 2; }
    if [ ! -f "$c/disk.img" ] || [ ! -f "$c/hobbyos.elf" ]; then
      echo "!! copy $n missing disk/elf" >&2; exit 2
    fi
    echo "-- [bench] seeded copy $n at $c"
  done

  # --- 4. run each copy boundedly, in parallel, from its own dir ------------
  pids=()
  starts=()
  for n in $(seq 1 "$COPIES"); do
    c="$COPIES_DIR/wave-$n"
    starts[$n]=$(date +%s)
    ( cd "$c" && timeout "$WAVE_TIMEOUT" make MODE=test run ARCH=intel \
        QEMU_ARGS='-enable-kvm -display none' >"$LOGDIR/wave-$n.log" 2>&1
      echo "$?" >"$LOGDIR/wave-$n.rc" ) &
    pids+=("$!")
    echo "-- [bench] wave-$n started (pid $! timeout ${WAVE_TIMEOUT}s)"
  done
  for p in "${pids[@]}"; do wait "$p"; done

  # --- 5. reap any QEMU still holding a copy's disk (a timeout-killed make
  #        can orphan its qemu child); scoped by exact file, never broad.
  for n in $(seq 1 "$COPIES"); do
    fuser -k "$COPIES_DIR/wave-$n/disk.img" 2>/dev/null
  done

  # --- 6. verdicts -----------------------------------------------------------
  echo "== WAVE VERDICTS =="
  wave_green=0
  for n in $(seq 1 "$COPIES"); do
    log="$LOGDIR/wave-$n.log"
    rc=$(cat "$LOGDIR/wave-$n.rc" 2>/dev/null || echo 254)
    dur=$(elapsed "${starts[$n]}")
    v=$(scan_log "$log")
    echo "wave-$n rc=$rc duration=$dur $v log=$log"
    if echo "$v" | grep -q "halt_markers=[1-9]"; then
      wave_green=$((wave_green+1))
      echo "  suite summaries:"
      grep -a "ALL TESTS PASSED" "$log" | sed 's/^/    /' | head -15
      grep -a "STRESS TEST" "$log" | head -2 | sed 's/^/    /'
    else
      echo "  !! wave-$n did NOT print 'System halt.'"
      grep -aE "BdsDxe: starting|Guest reset|Triple fault|no free (process )?slot|LOCKFOREVER|IDLESTUCK|WATCHDOG" "$log" | head -6 | sed 's/^/    /'
    fi
  done
  echo "== wave-green copies (System halt.): $wave_green/$COPIES =="
fi

echo "== X64 BLK BENCH DONE - logs in $LOGDIR =="
