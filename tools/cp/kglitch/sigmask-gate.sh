#!/usr/bin/env bash
# tools/cp/kglitch/sigmask-gate.sh -- one boot: the native driver KGMASK.BIN
# (tools/cp/kglitch/kgmask.c) spawn_ex()es linux_probe_sigmask "fill" (600
# forks that die with SIGINT blocked), then 8 x "check" on recycled pid slots.
# PASS = "[LP] done rc=0" and 8 "[LP] M1 ... PASS" rows (H4-kglitch round 2).
#   LP_TIMEOUT=300 bash tools/cp/qemu-slot.sh bash tools/cp/kglitch/sigmask-gate.sh [LOG]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
OUT="${1:-$ROOT/obj/cp-kglitch/sigmask.log}"
B="$ROOT/obj/cp-kglitch"; mkdir -p "$B"
RESINC="$(clang -print-resource-dir)/include"
make ARCH=intel obj/intel/probe_sigmask.elf obj/intel/libc.a > "$B/build.log" 2>&1 || { echo "sigmask-gate: build failed"; tail -5 "$B/build.log"; exit 2; }
clang -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include \
  --target=x86_64-none-elf -ffreestanding -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large \
  -c tools/cp/kglitch/kgmask.c -o "$B/kgmask.o" || exit 2
ld.lld -T src/user/linker.ld -e _start -o "$B/kgmask.elf" "$B/kgmask.o" obj/intel/libc.a || exit 2
llvm-objcopy -O binary "$B/kgmask.elf" "$B/kgmask.bin" || exit 2
printf 'KGMASK.BIN\n' > "$B/WAVE.CFG"
STAGE="mcopy -o -i \$ROOT/disk.img $ROOT/obj/intel/probe_sigmask.elf ::PROBE.ELF && mcopy -o -i \$ROOT/disk.img $B/kgmask.bin ::KGMASK.BIN && mcopy -o -i \$ROOT/disk.img $B/WAVE.CFG ::WAVE.CFG"
T3_TIMEOUT="${LP_TIMEOUT:-300}" T3_STAGE="$STAGE" T3_MIN_LINES=1 TOOLS_SKIP="${TOOLS_SKIP:-1}" \
  bash "$ROOT/tools/t3_tier.sh" intel > "$OUT" 2>&1
CONSOLE="$(sed -n 's/^T3_LOG=//p' "$OUT" | tail -1)"
[ -r "$CONSOLE" ] || { echo "sigmask-gate: no console"; exit 1; }
cp "$CONSOLE" "${OUT%.log}.console.log"
grep -aoE '\[LP\] .*' "$CONSOLE"
ok=$(grep -aoE '\[LP\] M1 .*-> PASS' "$CONSOLE" | wc -l)
if grep -aqE '\[LP\] done rc=0([^0-9]|$)' "$CONSOLE" && [ "$ok" -ge 8 ]; then echo "sigmask-gate: PASS ($ok/8)"; exit 0; fi
echo "sigmask-gate: FAIL ($ok/8 checks with an empty mask)"; exit 1
