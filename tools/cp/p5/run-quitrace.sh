#!/usr/bin/env bash
# tools/cp/p5/run-quitrace.sh - H3-winefix: targeted repro of the wineserver
# SIGQUIT vs. thread-exit race (P5's "services.exe memory fault VA=0x3A8
# PC=quit_handler+0x40", docs/cp-winefix.md §2).  tools/cp/k5/run-thrstorm.sh's
# staging, with tools/cp/p5/qrwine.c as the only wave member and
# tools/cp/p5/quitrace.c as the PE program:
#
#   1. builds the intel MODE=test kernel + disk, qrwine.bin and quitrace.exe;
#   2. stages CP_WINE_STAGE + quitrace.exe over a TEMPORARY localhost-only
#      read-only NFS export (tools/cp/exports.sh add/remove, EXIT trap);
#   3. boots once under a QEMU slot: wineboot -i, then QR_REPEAT x the runs in
#      QR_PLAN ("thr:<rounds>:<pairs>" / "proc:<n>:<ms_max>"), wineserver -k;
#   4. grades the serial log:
#        Q1 every quitrace run printed "[QR] done rc=0" and exited 0
#        Q2 no fault line at all (page fault / memory fault / FATAL)
#        Q3 wineserver -k exits 0 and nothing new is left
#      and prints the k/n line "[QRG] runs=<n> ok=<k> faults=<f> va3a8=<q> vaa8=<a>"
#      (va3a8 = W1, quit_handler on a NULL TEB: "memory fault VA=0x3A8";
#       vaa8 = W1b, user_mode_abort_thread on a NULL syscall frame: "VA=0xA8").
#
# Exit 0 only when Q1..Q3 pass.
# Env: CP_WINE_STAGE (default ~/cp/wine-cp/stage/CP/WINE), WINE_BUILD (the
#      build tree for the PE headers/import libs; default the stage's work dir),
#      GLIBC231_LIB (default ~/cp/sysroot-glibc231/lib/x86_64-linux-gnu),
#      QR_EXPORT (default /srv/nfs/cp-winefix-qr), QR_PLAN (default
#      "thr:2000:4,proc:40:900"; more thr rounds exhaust the TEB area, docs/cp-winefix.md §4b), QR_REPEAT (default 3), TIMEOUT_S (default 5400),
#      QR_WINEDEBUG (default err+all,fixme-all), QR_OUT (default obj/cp-qr/run),
#      QR_GRADE_ONLY=1 (re-grade QR_OUT/serial.log, no boot).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
STAGE="${CP_WINE_STAGE:-$HOME/cp/wine-cp/stage/CP/WINE}"
WB="${WINE_BUILD:-${STAGE%/stage/CP/WINE}}"
LDIR="${GLIBC231_LIB:-$HOME/cp/sysroot-glibc231/lib/x86_64-linux-gnu}"
EXPORT="${QR_EXPORT:-/srv/nfs/cp-winefix-qr}"
PLAN="${QR_PLAN:-thr:2000:4,proc:40:900}"
REPEAT="${QR_REPEAT:-3}"
TIMEOUT_S="${TIMEOUT_S:-5400}"
WDBG="${QR_WINEDEBUG:-err+all,fixme-all}"
OUT="${QR_OUT:-$ROOT/obj/cp-qr/run}"; mkdir -p "$OUT"
LOG="$OUT/serial.log"
IMG="$ROOT/disk.img"
die() { echo "quitrace: $*" >&2; exit 2; }
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP="${TOOLS_SKIP:-1}"

if [ -z "${QR_GRADE_ONLY:-}" ]; then
for t in clang ld.lld llvm-objcopy mcopy mmd qemu-system-x86_64 exportfs; do
  command -v "$t" >/dev/null 2>&1 || die "missing $t"
done
[ -x "$STAGE/bin/wine" ] || die "no staged Wine at $STAGE"
[ -f "$LDIR/libc-2.31.so" ] || die "no glibc 2.31 at $LDIR"
case "$EXPORT" in /srv/nfs/cp-?*) ;; *) die "QR_EXPORT must be /srv/nfs/cp-<name>";; esac
[ -e "$EXPORT" ] && die "$EXPORT already exists: not this run's to reuse or delete"
echo "== stage $STAGE ($(cat "$STAGE/share/wine/cp-wine-patches.txt" 2>/dev/null | awk '{printf "%s ", $2}' || true))"
echo "== plan $PLAN x $REPEAT"

echo "== build: kernel + disk (intel, test), qrwine.bin, quitrace.exe"
make ARCH=intel MODE=test hobbyos.elf disk.img >"$OUT/build.log" 2>&1 ||
  { echo "BUILD FAILED"; tail -30 "$OUT/build.log"; exit 2; }
make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || die "libc.a"
RESINC="$(clang -print-resource-dir)/include"
clang -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include \
  -Isrc/libc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -nostdinc \
  -isystem "$RESINC" -mcmodel=large -DWINE_EXPORT="\"$EXPORT\"" -DQR_WINEDEBUG="\"$WDBG\"" \
  -DQR_PLAN="\"$PLAN\"" -DQR_REPEAT="$REPEAT" -c tools/cp/p5/qrwine.c -o "$OUT/qrwine.o" || die "qrwine.c"
ld.lld -T src/user/linker.ld -e _start -o "$OUT/qrwine.elf" "$OUT/qrwine.o" obj/intel/libc.a || die "ld qrwine"
llvm-objcopy -O binary "$OUT/qrwine.elf" "$OUT/qrwine.bin"
# quitrace.exe: tools/cp/build-winprobe.sh's recipe (kernel32 + ntdll + ucrtbase imports only)
WSRC="$WB/src/wine-11.0" WOBJ="$WB/build-x86_64"
[ -x "$WOBJ/tools/winegcc/winegcc" ] || die "no Wine build tree at $WB (WINE_BUILD)"
clang -c -o "$OUT/quitrace.o" tools/cp/p5/quitrace.c \
  -I"$WOBJ/include" -I"$WSRC/include" -I"$WSRC/include/msvcrt" -D_UCRT -D__WINE_PE_BUILD \
  -target x86_64-windows -fuse-ld=lld --no-default-config -fno-strict-aliasing \
  -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers || die "quitrace.c"
(cd "$WOBJ" && tools/winegcc/winegcc -o "$OUT/quitrace.exe" --wine-objdir . -b x86_64-windows -mconsole \
  "$OUT/quitrace.o" dlls/winecrt0/x86_64-windows/libwinecrt0.a \
  libs/compiler-rt/x86_64-windows/libcompiler-rt.a dlls/ucrtbase/x86_64-windows/libucrtbase.a \
  dlls/kernel32/x86_64-windows/libkernel32.a dlls/ntdll/x86_64-windows/libntdll.a \
  --no-default-config -Wl,-debug:dwarf) || die "link quitrace.exe"
echo "== quitrace.exe $(stat -c %s "$OUT/quitrace.exe") B sha256 $(sha256sum "$OUT/quitrace.exe" | cut -c1-16)"

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

echo "== export: $EXPORT (Wine stage + quitrace.exe), 127.0.0.1 only, ro"
sudo -n mkdir -p "$EXPORT/WINE" "$EXPORT/QR" && MADE_DIR=1
sudo -n rsync -a --delete "$STAGE/" "$EXPORT/WINE/" || die "rsync stage"
sudo -n cp "$OUT/quitrace.exe" "$EXPORT/QR/quitrace.exe"
sudo -n chmod -R a+rX "$EXPORT"
ADDED=1
bash "$ROOT/tools/cp/exports.sh" add "$EXPORT" | sed 's/^/== /' || die "exports.sh add $EXPORT"

echo "== stage: glibc 2.31 under /LIB64 + /lib64/ld + QRWINE.BIN + WAVE.CFG"
mmd -i "$IMG" ::/LIB64 ::/lib64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy failed $2"; }
for so in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
          libresolv.so.2 libgcc_s.so.1 libutil.so.1 libnss_files.so.2 libnss_dns.so.2; do
  [ -e "$LDIR/$so" ] && stage "$(readlink -f "$LDIR/$so")" "::/LIB64/$so"
done
stage "$(readlink -f "$LDIR/ld-linux-x86-64.so.2")" ::/lib64/ld-linux-x86-64.so.2
stage "$OUT/qrwine.bin" ::/QRWINE.BIN
printf 'QRWINE.BIN\n' > "$OUT/wave.cfg"
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
    grep -aq "\[QRL\] done" '"$LOG"' && sleep 3 && break
    [ $i -gt 30 ] && ! fuser '"$IMG"' >/dev/null 2>&1 && break
    sleep 1
  done
  for qp in $(fuser '"$IMG"' 2>/dev/null); do kill "$qp" 2>/dev/null; done
  for i in $(seq 1 30); do fuser '"$IMG"' >/dev/null 2>&1 || break; sleep 1; done'
fi # QR_GRADE_ONLY

set +o pipefail
tr -d '\r' < "$LOG" > "$OUT/serial.txt"
S="$OUT/serial.txt"
echo
echo "== rows"
grep -aE '^\[QRL\]|\[QR\] (thr|proc) (begin|end|HUNG|STUCK)|\[QR\] done|\[QR\] proc #' "$S" | grep -av 'ps-before' | head -120
echo
runs=$(grep -ac '^\[QRL\] begin \(thr\|proc\)#' "$S")
ok=0
for tag in $(sed -n 's/^\[QRL\] begin \(\(thr\|proc\)#[0-9]*\) :.*/\1/p' "$S"); do
  sp=$(awk -v t="$tag" '$0 ~ "\\[QRL\\] begin "t" :" {on=1; next} $0 ~ "\\[QRL\\] end "t" " {on=0} on' "$S")
  e=$(grep -a "^\[QRL\] end $tag " "$S" | tail -1)
  if printf '%s\n' "$sp" | grep -q '\[QR\] done rc=0$' && printf '%s\n' "$e" | grep -q 'status=0 '; then ok=$((ok + 1))
  else echo "== run $tag NOT ok: ${e:-no end row}"; fi
done
nf=$(grep -acE 'memory fault|page fault|FATAL' "$S")
nq=$(grep -acE 'memory fault VA=0x0*3A8 ' "$S")
na=$(grep -acE 'memory fault VA=0x0*A8 ' "$S")
Q1=FAIL Q2=FAIL Q3=FAIL
[ "$runs" -gt 0 ] && [ "$ok" = "$runs" ] && Q1=PASS
[ "$nf" = 0 ] && Q2=PASS
grep -a "^\[QRL\] end kill " "$S" | tail -1 | grep -q 'status=0 ' && grep -aq '^\[QRL\] ps-extra count=0$' "$S" && Q3=PASS
echo "Q1 every run done rc=0, exit 0   : $Q1   ($ok/$runs)"
echo "Q2 no fault line                 : $Q2   ($nf lines)"
grep -aE 'memory fault|page fault|FATAL' "$S" | head -5 | cut -c1-200 | sed 's/^/     /'
echo "Q3 wineserver -k, nothing left   : $Q3   ($(grep -a '^\[QRL\] end kill ' "$S" | tail -1); $(grep -a 'ps-extra' "$S" | tail -1))"
nh=$(grep -acE '\[QR\] thr (HUNG|STUCK)' "$S")
ne=$(for tag in $(sed -n 's/^\[QRL\] begin \(\(thr\|proc\)#[0-9]*\) :.*/\1/p' "$S"); do
       sp=$(awk -v t="$tag" '$0 ~ "\\[QRL\\] begin "t" :" {on=1; next} $0 ~ "\\[QRL\\] end "t" " {on=0} on' "$S")
       printf '%s\n' "$sp" | grep -q '\[QR\] done rc=' || echo x; done | grep -c x)
echo "[QRG] runs=$runs ok=$ok faults=$nf va3a8=$nq vaa8=$na hung=$nh silent_exit=$ne"
echo "raw serial: $LOG"
[ "$Q1$Q2$Q3" = PASSPASSPASS ]
