#!/usr/bin/env bash
# tools/cp/k5/run-thrstorm.sh - C1-k5: thrstorm.exe (256 Windows threads, each
# on its own event + pipe / named pipe, for K5_SECS seconds) under console Wine
# inside HobbyOS (docs/cp-k5.md).  tools/cp/run-wine-p2.sh's staging, cut down:
#
#   1. builds the intel MODE=test kernel + disk, the native launcher
#      tools/cp/k5/k5wine.c (the only wave member) and thrstorm.exe
#      (tools/cp/k5/build-thrstorm.sh: PE x86_64, clang + Wine's import libs);
#   2. stages the console Wine tree (~/cp/wine-build/stage) and thrstorm.exe
#      over a TEMPORARY localhost-only read-only NFS export, added and removed
#      only through tools/cp/exports.sh (EXIT/INT/TERM/HUP trap);
#   3. stages the glibc 2.31 runtime on the FAT disk (/LIB64 + /lib64/ld);
#   4. boots once under a QEMU slot (tools/cp/qemu-slot.sh): wineboot -i,
#      wine thrstorm.exe K5_SECS, wineserver -k (WINEPREFIX=/tmp/pfx);
#   5. grades the serial log:
#        S1 thrstorm printed "[TS] done rc=0" and exited 0
#        S2 its T2 storm row ran >= K5_SECS s with rounds > 0
#        S3 wineserver -k exits 0 and nothing new is left
#        S4 no fault line (page fault / memory fault / FATAL)
#
# Exit 0 only when S1..S4 pass.
# Env: CP_WINE_STAGE (default ~/cp/wine-build/stage/CP/WINE),
#      GLIBC231_LIB (default ~/cp/wine-build/glibc231/lib/x86_64-linux-gnu),
#      K5_EXPORT (default /srv/nfs/cp-k5-ts), K5_SECS (default 60),
#      TIMEOUT_S (default 3600), K5_WINEDEBUG (default err+all,fixme-all),
#      K5_OUT (default obj/cp-k5/ts), K5_GRADE_ONLY=1 (re-grade, no boot).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
STAGE="${CP_WINE_STAGE:-$HOME/cp/wine-build/stage/CP/WINE}"
LDIR="${GLIBC231_LIB:-$HOME/cp/wine-build/glibc231/lib/x86_64-linux-gnu}"
EXPORT="${K5_EXPORT:-/srv/nfs/cp-k5-ts}"
SECS="${K5_SECS:-60}"
TIMEOUT_S="${TIMEOUT_S:-3600}"
WDBG="${K5_WINEDEBUG:-err+all,fixme-all}"
OUT="${K5_OUT:-$ROOT/obj/cp-k5/ts}"; mkdir -p "$OUT"
LOG="$OUT/serial.log"
IMG="$ROOT/disk.img"
die() { echo "k5-ts: $*" >&2; exit 2; }
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP="${TOOLS_SKIP:-1}"

if [ -z "${K5_GRADE_ONLY:-}" ]; then
for t in clang ld.lld llvm-objcopy mcopy mmd qemu-system-x86_64 exportfs; do
  command -v "$t" >/dev/null 2>&1 || die "missing $t"
done
[ -x "$STAGE/bin/wine" ] || die "no staged Wine at $STAGE"
[ -f "$LDIR/libc-2.31.so" ] || die "no glibc 2.31 at $LDIR"
case "$EXPORT" in /srv/nfs/cp-?*) ;; *) die "K5_EXPORT must be /srv/nfs/cp-<name>";; esac
[ -e "$EXPORT" ] && die "$EXPORT already exists: not this run's to reuse or delete"

echo "== build: kernel + disk (intel, test), k5wine.bin, thrstorm.exe"
make ARCH=intel MODE=test hobbyos.elf disk.img >"$OUT/build.log" 2>&1 ||
  { echo "BUILD FAILED"; tail -30 "$OUT/build.log"; exit 2; }
make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || die "libc.a"
RESINC="$(clang -print-resource-dir)/include"
clang -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include \
  -Isrc/libc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -nostdinc \
  -isystem "$RESINC" -mcmodel=large -DWINE_EXPORT="\"$EXPORT\"" -DK5_WINEDEBUG="\"$WDBG\"" \
  -DK5_SECS="\"$SECS\"" -c tools/cp/k5/k5wine.c -o "$OUT/k5wine.o" || die "k5wine.c"
ld.lld -T src/user/linker.ld -e _start -o "$OUT/k5wine.elf" "$OUT/k5wine.o" obj/intel/libc.a || die "ld k5wine"
llvm-objcopy -O binary "$OUT/k5wine.elf" "$OUT/k5wine.bin"
bash tools/cp/k5/build-thrstorm.sh "$OUT/thrstorm.exe" >"$OUT/thrstorm-build.log" 2>&1 ||
  { cat "$OUT/thrstorm-build.log"; die "thrstorm.exe"; }
cat "$OUT/thrstorm-build.log"

MADE_DIR="" ADDED="" CLEANED=""
cleanup() {
  [ -n "$CLEANED" ] && return 0
  CLEANED=1
  [ -n "$ADDED" ] && bash "$ROOT/tools/cp/exports.sh" remove "$EXPORT" | sed 's/^/== /'
  [ -n "$MADE_DIR" ] && sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"
  sudo -n exportfs -v 2>/dev/null | grep -F "$EXPORT " && echo "== WARNING: $EXPORT still exported"
  return 0
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP

echo "== export: $EXPORT (Wine stage + thrstorm.exe), 127.0.0.1 only, ro"
sudo -n mkdir -p "$EXPORT/WINE" "$EXPORT/K5" && MADE_DIR=1
sudo -n rsync -a --delete "$STAGE/" "$EXPORT/WINE/" || die "rsync stage"
sudo -n cp "$OUT/thrstorm.exe" "$EXPORT/K5/thrstorm.exe"
sudo -n chmod -R a+rX "$EXPORT"
ADDED=1
bash "$ROOT/tools/cp/exports.sh" add "$EXPORT" | sed 's/^/== /' || die "exports.sh add $EXPORT"

echo "== stage: glibc 2.31 under /LIB64 + /lib64/ld + K5WINE.BIN + WAVE.CFG"
mmd -i "$IMG" ::/LIB64 ::/lib64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy failed $2"; }
for so in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
          libresolv.so.2 libgcc_s.so.1 libutil.so.1 libnss_files.so.2 libnss_dns.so.2; do
  [ -e "$LDIR/$so" ] && stage "$(readlink -f "$LDIR/$so")" "::/LIB64/$so"
done
stage "$(readlink -f "$LDIR/ld-linux-x86-64.so.2")" ::/lib64/ld-linux-x86-64.so.2
stage "$OUT/k5wine.bin" ::/K5WINE.BIN
printf 'K5WINE.BIN\n' > "$OUT/wave.cfg"
stage "$OUT/wave.cfg" ::/WAVE.CFG

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
    grep -aq "System halt" '"$LOG"' && sleep 1 && break
    grep -aq "\[K5\] done" '"$LOG"' && sleep 3 && break
    [ $i -gt 30 ] && ! fuser '"$IMG"' >/dev/null 2>&1 && break
    sleep 1
  done
  for qp in $(fuser '"$IMG"' 2>/dev/null); do kill "$qp" 2>/dev/null; done
  for i in $(seq 1 30); do fuser '"$IMG"' >/dev/null 2>&1 || break; sleep 1; done'
fi # K5_GRADE_ONLY

set +o pipefail
tr -d '\r' < "$LOG" > "$OUT/serial.txt"
S="$OUT/serial.txt"
span() { awk -v t="$1" '$0 ~ "\\[K5\\] begin "t" :" {on=1; next} $0 ~ "\\[K5\\] end "t" " {on=0} on' "$S"; }
endrow() { grep -a "\[K5\] end $1 " "$S" | tail -1; }
echo
echo "== rows"
grep -aE '^\[K5\]|\[TS\]' "$S" | grep -av 'ps-before' | head -80
echo
S1=FAIL S2=FAIL S3=FAIL S4=FAIL
span thrstorm | grep -aq '^\[TS\] done rc=0$' && endrow thrstorm | grep -q 'status=0 ' && S1=PASS
st=$(span thrstorm | grep -a '^\[TS\] PASS T2 storm' | sed -n 's/.*rounds=\([0-9]*\) ms=\([0-9]*\).*/\1 \2/p' | tail -1)
[ -n "$st" ] && [ "${st% *}" -gt 0 ] && [ "${st#* }" -ge $((SECS * 1000)) ] && S2=PASS
endrow kill | grep -q 'status=0 ' && grep -aq '^\[K5\] ps-extra count=0$' "$S" && S3=PASS
nf=$(grep -acE 'memory fault|page fault|FATAL' "$S")
[ "$nf" = 0 ] && S4=PASS
echo "S1 thrstorm done rc=0, exit 0        : $S1   ($(endrow thrstorm))"
echo "S2 storm >= ${SECS}s, rounds > 0          : $S2   (rounds/ms: ${st:-none})"
echo "S3 wineserver -k, nothing left        : $S3   ($(endrow kill); $(grep -a 'ps-extra' "$S" | tail -1))"
echo "S4 no fault line                      : $S4   ($nf lines$( [ "$nf" != 0 ] && echo ": $(grep -aE 'memory fault|page fault|FATAL' "$S" | head -1 | cut -c1-140)"))"
echo "raw serial: $LOG"
[ "$S1$S2$S3$S4" = PASSPASSPASSPASS ]
