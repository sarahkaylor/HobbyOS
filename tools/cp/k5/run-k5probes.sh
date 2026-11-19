#!/usr/bin/env bash
# tools/cp/k5/run-k5probes.sh - C1-k5: C8-game's P8 kernel probes in one boot
# (docs/cp-k5.md §8; probes src/user/cp/p8{env,scm,kill}probe.c and
# p8fdcount.c by lane C8-game).  No NFS, no Wine: the ELFs go on the FAT disk
# and tools/cp/k5/k5probes.c (the only wave member) runs them.
#
# Graded rows (each must say PASS):
#   E1/E2 compat execve with 40 env entries      (p8envprobe child)
#   E3/E4 native spawn_ex -> compat, 40 entries  (p8envprobe spawned40)
#   E5    spawn_ex with an env past HO_ENV_LEN is E2BIG, not truncated
#   S1-S3 SCM_RIGHTS into a full fd table         (p8scmprobe)
# Reported, not graded (K5_GRADE_KILL=1 grades them): K1/K2 (p8killprobe).
# Env: K5_OUT (default obj/cp-k5/probes), TIMEOUT_S (default 600).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
OUT="${K5_OUT:-$ROOT/obj/cp-k5/probes}"; mkdir -p "$OUT"
LOG="$OUT/serial.log"
IMG="$ROOT/disk.img"
TIMEOUT_S="${TIMEOUT_S:-600}"
die() { echo "k5-probes: $*" >&2; exit 2; }
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP="${TOOLS_SKIP:-1}"

if [ -z "${K5_GRADE_ONLY:-}" ]; then
echo "== build: kernel + disk (intel, test), k5probes.bin, the p8 probes"
make ARCH=intel MODE=test hobbyos.elf disk.img >"$OUT/build.log" 2>&1 ||
  { echo "BUILD FAILED"; tail -30 "$OUT/build.log"; exit 2; }
make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || die "libc.a"
RESINC="$(clang -print-resource-dir)/include"
clang -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include \
  -Isrc/libc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -nostdinc \
  -isystem "$RESINC" -mcmodel=large -c tools/cp/k5/k5probes.c -o "$OUT/k5probes.o" || die "k5probes.c"
ld.lld -T src/user/linker.ld -e _start -o "$OUT/k5probes.elf" "$OUT/k5probes.o" obj/intel/libc.a || die "ld"
llvm-objcopy -O binary "$OUT/k5probes.elf" "$OUT/k5probes.bin"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/p8env.elf" src/user/cp/p8envprobe.c || die "p8envprobe"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/p8scm.elf" src/user/cp/p8scmprobe.c || die "p8scmprobe"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/p8kill.elf" src/user/cp/p8killprobe.c -lpthread || die "p8killprobe"
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/p8fd.elf" src/user/cp/p8fdcount.c || die "p8fdcount"
stage() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy $2"; }
stage "$OUT/k5probes.bin" ::/K5PROBE.BIN
stage "$OUT/p8env.elf" ::/P8ENV.ELF
stage "$OUT/p8scm.elf" ::/P8SCM.ELF
stage "$OUT/p8kill.elf" ::/P8KILL.ELF
stage "$OUT/p8fd.elf" ::/P8FD.ELF
printf 'K5PROBE.BIN\n' > "$OUT/wave.cfg"; stage "$OUT/wave.cfg" ::/WAVE.CFG
OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || die "no OVMF_CODE_4M.fd"
OVMF="$OUT/OVMF_CODE_4M.fd"; [ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"
echo "== boot (deadline ${TIMEOUT_S}s, under a QEMU slot)"
: >"$LOG"
QA="-display none -enable-kvm"
bash "$ROOT/tools/cp/qemu-slot.sh" bash -c '
  ( make -o disk.img ARCH=intel MODE=test EDK2_X86_64='"$OVMF"' run QEMU_ARGS="'"$QA"'" >>'"$LOG"' 2>&1 & )
  for i in $(seq 1 '"$TIMEOUT_S"'); do
    grep -aq "\[K5P\] done" '"$LOG"' && sleep 2 && break
    grep -aq "System halt" '"$LOG"' && sleep 1 && break
    [ $i -gt 30 ] && ! fuser '"$IMG"' >/dev/null 2>&1 && break
    sleep 1
  done
  for qp in $(fuser '"$IMG"' 2>/dev/null); do kill "$qp" 2>/dev/null; done
  for i in $(seq 1 30); do fuser '"$IMG"' >/dev/null 2>&1 || break; sleep 1; done'
fi

set +o pipefail
tr -d '\r' < "$LOG" > "$OUT/serial.txt"
S="$OUT/serial.txt"
echo "== rows"
grep -aE '^\[K5P\]|\[P8ENV\]|\[P8SCM\]|\[P8KILL\]|\[P8FD\]' "$S"
fail=0
for r in "E1 execve env" "E2 malformed" "E3 spawn_ex env" "E4 malformed" "E5 spawn_ex env > HO_ENV_LEN" "S1 recvmsg" "S2 MSG_CTRUNC" "S3 second recvmsg"; do
  if grep -a "$r" "$S" | grep -q -- '-> PASS'; then echo "PASS  $r"; else echo "FAIL  $r"; fail=$((fail+1)); fi
done
for r in "K1 reaped" "K2 status"; do
  if grep -a "$r" "$S" | grep -q -- '-> PASS'; then echo "PASS  $r (not graded)"; else
    echo "FAIL  $r (not graded unless K5_GRADE_KILL=1)"; [ -n "${K5_GRADE_KILL:-}" ] && fail=$((fail+1)); fi
done
grep -aq '^\[K5P\] done' "$S" || { echo "FAIL  no [K5P] done row"; fail=$((fail+1)); }
echo "k5-probes: $fail failure(s); raw serial $LOG"
[ "$fail" = 0 ]
