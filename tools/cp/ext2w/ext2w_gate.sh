#!/usr/bin/env bash
# tools/cp/ext2w/ext2w_gate.sh - lane C3-ext2w's storage gate (docs/cp-ext2w.md).
#
#   bench   boot 1: linux_probe_ext2w "bench" on a fresh data disk (perf rows +
#           write-back correctness rows); e2fsck -fn.  Boot 2: "verify" (the
#           files boot 1 wrote WITHOUT fsync survived the halt); e2fsck -fn.
#   crash   the pull-the-plug test: "crashs" (write s<N>, sync(2), print
#           "[LP] SYNCED n=<N>") beside "crashw" (an unsynced writer that never
#           stops).  The host sends QMP `quit` the moment SYNCED n=<K> appears
#           (after an optional extra delay), then e2fsck -fn must be clean and
#           s0..s<K> must hold their bytes.  CRASH_POINTS="K:delay_ms ..."
#
# Usage: bash tools/cp/ext2w/ext2w_gate.sh [bench] [crash]   (default: both)
# Env: GATE_DIR (default <tree>/obj/ext2w-gate), LP_TIMEOUT (default 900),
#      CRASH_POINTS (default "1:0 3:0 6:0 2:400 4:1300 5:2600 8:0 3:900").
# Every boot holds one QEMU slot (tools/cp/qemu-slot.sh).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
D="${GATE_DIR:-$ROOT/obj/ext2w-gate}"
PATH="/usr/sbin:/sbin:$PATH"
WHAT="${*:-bench crash}"
fails=0
say() { printf '%s\n' "$*"; }
bad() { say "[HOST] FAIL $*"; fails=$((fails + 1)); }
good() { say "[HOST] ok   $*"; }
mkdir -p "$D"

stage_mode() { # stage_mode IMG MODE
  printf '%s\n' "$2" > "$D/mode.txt"
  debugfs -w -R "rm ext2w.mode" "$1" >/dev/null 2>&1
  debugfs -w -R "write $D/mode.txt ext2w.mode" "$1" >/dev/null 2>&1
}

boot_probe() { # boot_probe TAG IMG [extra wave lines] -> $D/TAG.console.log
  local tag="$1" img="$2" extra="${3:-}"
  rm -f "$D/$tag.raw.log"
  CPDATA="$img" T3_LOG="$D/$tag.raw.log" LP_OUT="$D/$tag.tier.log" LP_TIMEOUT="${LP_TIMEOUT:-900}" \
    LP_WAVE_EXTRA="$extra" \
    bash "$ROOT/tools/cp/qemu-slot.sh" bash "$ROOT/tools/gate/t3_linux_probe.sh" ext2w \
    > "$D/$tag.gate.log" 2>&1
  local rc=$?
  cp "$D/$tag.raw.log" "$D/$tag.console.log" 2>/dev/null
  return $rc
}

fsck_clean() { # fsck_clean IMG LABEL
  e2fsck -fn "$1" > "$D/$2.fsck.log" 2>&1
  local frc=$?
  if [ "$frc" = 0 ]; then good "$2: e2fsck -fn rc=0 ($(tail -1 "$D/$2.fsck.log"))"
  else bad "$2: e2fsck -fn rc=$frc"; sed -n '1,30p' "$D/$2.fsck.log"; fi
}

if [[ " $WHAT " == *" bench "* ]]; then
  IMG="$D/cpdata-bench.img"
  CPDATA_FORCE=1 bash "$ROOT/tools/cp/mkcpdata.sh" "$IMG" 8G >/dev/null || exit 2
  stage_mode "$IMG" bench
  boot_probe bench "$IMG"; rc=$?
  grep -a '\[LP\] \(perf\|FAIL\|SKIP\|info\|done\)' "$D/bench.console.log"
  say "bench: $(grep -ac '\[LP\] PASS' "$D/bench.console.log") PASS rows, rc=$rc"
  [ "$rc" = 0 ] || bad "bench probe rc=$rc"
  grep -a '^\[E2W\]' "$D/bench.console.log" | tail -5
  fsck_clean "$IMG" bench
  stage_mode "$IMG" verify
  boot_probe verify "$IMG"; rc=$?
  grep -a '\[LP\] \(PASS\|FAIL\|SKIP\|info\|done\)' "$D/verify.console.log"
  [ "$rc" = 0 ] || bad "verify probe rc=$rc"
  fsck_clean "$IMG" verify
  rm -f "$IMG"
fi

qmp_quit() { # qmp_quit SOCK
  python3 - "$1" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
f = s.makefile("rw")
f.readline()
for cmd in ({"execute": "qmp_capabilities"}, {"execute": "quit"}):
    f.write(json.dumps(cmd) + "\n"); f.flush()
    try: f.readline()
    except Exception: pass
EOF
}

# Expected bytes of /cs/s<N> (the probe's pat(), key 9000+N, 65536 bytes).
pat_file() { # pat_file N OUT
  python3 - "$1" "$2" <<'EOF'
import sys
n, out = int(sys.argv[1]), sys.argv[2]
k = 9000 + n
b = bytearray(65536)
M = (1 << 64) - 1
for i in range(65536):
    x = (((i >> 2) * 2654435761) + k * 40503 + (i & 3) * 97) & M
    b[i] = (x ^ (x >> 11) ^ (x >> 19)) & 0xFF
open(out, "wb").write(bytes(b))
EOF
}

if [[ " $WHAT " == *" crash "* ]]; then
  run=0
  for pt in ${CRASH_POINTS:-1:0 3:0 6:0 2:400 4:1300 5:2600 8:0 3:900}; do
    run=$((run + 1))
    K="${pt%%:*}"; DLY="${pt##*:}"
    IMG="$D/cpdata-crash$run.img"
    CPDATA_FORCE=1 bash "$ROOT/tools/cp/mkcpdata.sh" "$IMG" 2G >/dev/null || exit 2
    stage_mode "$IMG" crashs
    SOCK="$D/qmp-crash$run.sock"; rm -f "$SOCK"
    tag="crash$run"
    say "=== crash run $run: quit after SYNCED n=$K + ${DLY} ms ==="
    ( export T3_QEMU_ARGS="-qmp unix:$SOCK,server=on,wait=off"; boot_probe "$tag" "$IMG" "PROBE.ELF /EXT crashw" ) &
    bpid=$!
    con="$D/$tag.raw.log"; t0=$(date +%s); hit=0
    while kill -0 "$bpid" 2>/dev/null; do
      if grep -aqE "\[LP\] SYNCED n=$K([^0-9]|\$)" "$con" 2>/dev/null; then
        [ "$DLY" != 0 ] && sleep "$(awk "BEGIN{print $DLY/1000}")"
        qmp_quit "$SOCK" && hit=1
        break
      fi
      [ $(( $(date +%s) - t0 )) -gt "${LP_TIMEOUT:-900}" ] && break
      sleep 0.05
    done
    wait "$bpid" 2>/dev/null
    cp "$con" "$D/$tag.console.log" 2>/dev/null
    if [ "$hit" != 1 ]; then bad "run $run: never saw SYNCED n=$K (or QMP quit failed)"; continue; fi
    last=$(grep -ao '\[LP\] SYNCED n=[0-9]*' "$D/$tag.console.log" | tail -1 | tr -dc '0-9')
    wi=$(grep -ao '\[LP\] info crashw i=[0-9]*' "$D/$tag.console.log" | tail -1 | tr -dc '0-9')
    say "[HOST] info run $run: last SYNCED n=$last on the console, crashw reached i>=${wi:-?}"
    fsck_clean "$IMG" "$tag"
    okn=0
    for n in $(seq 0 "$K"); do
      pat_file "$n" "$D/want.bin"
      debugfs -R "dump /cs/s$n $D/got.bin" "$IMG" >/dev/null 2>&1
      if cmp -s "$D/want.bin" "$D/got.bin"; then okn=$((okn + 1)); else bad "run $run: /cs/s$n lost or wrong after the plug pull"; fi
    done
    [ "$okn" = $((K + 1)) ] && good "run $run: s0..s$K (synced before the quit) intact"
    ncw=$(debugfs -R "ls -p /cw" "$IMG" 2>/dev/null | awk -F/ 'NF>5 && $6!="." && $6!=".."' | wc -l)
    say "[HOST] info run $run: /cw holds $ncw entries on disk"
    rm -f "$IMG" "$SOCK"
  done
fi

if [ "$fails" = 0 ]; then say "ext2w_gate: PASS"; else say "ext2w_gate: FAIL ($fails)"; fi
[ "$fails" = 0 ]
