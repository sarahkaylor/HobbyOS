#!/usr/bin/env bash
# tools/cp/run-wine-p2.sh - Gate P2 inside HobbyOS (lane C2-wine2,
# docs/cyberpunk.md §6 P2):
#
#   in QEMU/KVM, `wine cmd /c echo hello` prints `hello`; `winprobe.exe`
#   prints every marker and exits 0; `wineserver -k` leaves no processes.
#   Evidence: serial log + `ps` before/after.
#
# What it does (sibling of run-wine-smoke.sh, the same staging pattern):
#   1. builds the intel MODE=test kernel + disk, src/user/winep2.c (the native
#      launcher, the only wave member) and src/user/cp/winprobe.c
#      (tools/cp/build-winprobe.sh, PE x86_64, no MinGW);
#   2. stages the Wine tree from strix:~/cp/wine-build/stage over a TEMPORARY
#      NFS export that is localhost-only and read-only, exactly
#        <dir> 127.0.0.1(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)
#      QEMU user networking reaches it as 10.0.2.2.  /etc/exports is backed up
#      first and put back BYTE-FOR-BYTE on every exit path (EXIT/INT/TERM/HUP
#      trap, then `cmp`); the export directory is removed after the run.  The
#      export is never widened; an export dir that already exists is refused
#      (it is not this run's to delete);
#   3. stages the glibc 2.31 runtime on the FAT disk (/LIB64 + /lib64/ld);
#   4. boots once under `flock /tmp/cp-qemu.lock` and the launcher runs
#      ps, wineboot -i, wine cmd /c echo hello, wine winprobe.exe (from Z: and
#      from C:), wineserver -k, ps with WINEPREFIX=/tmp/pfx on the compat tmpfs;
#   5. grades the serial log: G1 hello, G2 every marker in
#      tools/cp/winprobe-markers.txt + "[WP] done rc=0" + exit 0, G3 the kill
#      step exits 0 and the after-census has no process the before list lacked.
#
# Exit 0 only when G1, G2 and G3 all pass.
# Env: CP_WINE_STAGE (default ~/cp/wine-build/stage/CP/WINE),
#      GLIBC231_LIB (default ~/cp/wine-build/glibc231/lib/x86_64-linux-gnu),
#      WINE_EXPORT (default /srv/nfs/cp-wine2-p2), TIMEOUT_S (default 2400),
#      P2_WINEDEBUG (default err+all,fixme-all), QEMU_MEM (default unset).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
STAGE="${CP_WINE_STAGE:-$HOME/cp/wine-build/stage/CP/WINE}"
LDIR="${GLIBC231_LIB:-$HOME/cp/wine-build/glibc231/lib/x86_64-linux-gnu}"
EXPORT="${WINE_EXPORT:-/srv/nfs/cp-wine2-p2}"
TIMEOUT_S="${TIMEOUT_S:-2400}"
WDBG="${P2_WINEDEBUG:-err+all,fixme-all}"
OUT="$ROOT/obj/wine-p2"; mkdir -p "$OUT"
LOG="$OUT/serial.log"
IMG="$ROOT/disk.img"
MARKERS="$ROOT/tools/cp/winprobe-markers.txt"
die() { echo "wine-p2: $*" >&2; exit 2; }
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP="${TOOLS_SKIP:-1}"

for t in clang ld.lld llvm-objcopy mcopy mmd qemu-system-x86_64 exportfs cmp; do
  command -v "$t" >/dev/null 2>&1 || die "missing $t"
done
[ -x "$STAGE/bin/wine" ] || die "no staged Wine at $STAGE (tools/cp/build-wine.sh all)"
[ -f "$LDIR/libc-2.31.so" ] || die "no glibc 2.31 at $LDIR"
[ -f "$MARKERS" ] || die "no marker list $MARKERS"
case "$EXPORT" in /srv/nfs/?*) ;; *) die "WINE_EXPORT must be under /srv/nfs/ ($EXPORT)";; esac
[ -e "$EXPORT" ] && die "$EXPORT already exists: not this run's to reuse or delete; remove it by hand"

# ---- /etc/exports: back up, add ONE localhost-only ro line, restore always ----
OPTS="ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534"
EXP_BAK="$(mktemp /tmp/cp-wine2-exports.XXXXXX)"
sudo -n cp -a /etc/exports "$EXP_BAK" || die "cannot back up /etc/exports"
MADE_DIR=""
CLEANED=""
cleanup() {
  [ -n "$CLEANED" ] && return 0
  CLEANED=1
  sudo -n cp "$EXP_BAK" /etc/exports && sudo -n exportfs -ra
  if sudo -n cmp -s "$EXP_BAK" /etc/exports; then
    echo "== /etc/exports restored (cmp identical to the pre-run copy)"
    sudo -n rm -f "$EXP_BAK"
  else
    echo "== WARNING: /etc/exports differs from the backup $EXP_BAK (kept)"
  fi
  if [ -n "$MADE_DIR" ]; then
    sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"
  fi
  sudo -n exportfs -v 2>/dev/null | grep -F "$EXPORT" && echo "== WARNING: $EXPORT still exported"
  return 0
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP

echo "== export: $EXPORT (Wine stage + winprobe.exe), 127.0.0.1 only, ro"
sudo -n mkdir -p "$EXPORT/WINE" "$EXPORT/WPROBE" && MADE_DIR=1
sudo -n rsync -a --delete "$STAGE/" "$EXPORT/WINE/" || die "rsync stage"

echo "== build: kernel + disk (intel, test), winep2.bin, winprobe.exe"
make ARCH=intel MODE=test hobbyos.elf disk.img >"$OUT/build.log" 2>&1 ||
  { echo "BUILD FAILED"; tail -30 "$OUT/build.log"; exit 2; }
make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || die "libc.a"
RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large)
clang "${UCFLAGS[@]}" -DWINE_EXPORT="\"$EXPORT\"" -DP2_WINEDEBUG="\"$WDBG\"" \
  -c src/user/winep2.c -o "$OUT/winep2.o" || die "winep2.c"
ld.lld -T src/user/linker.ld -e _start -o "$OUT/winep2.elf" "$OUT/winep2.o" obj/intel/libc.a || die "ld winep2"
llvm-objcopy -O binary "$OUT/winep2.elf" "$OUT/winep2.bin"
bash tools/cp/build-winprobe.sh "$OUT/winprobe.exe" >"$OUT/winprobe-build.log" 2>&1 ||
  { cat "$OUT/winprobe-build.log"; die "winprobe.exe"; }
sudo -n cp "$OUT/winprobe.exe" "$EXPORT/WPROBE/winprobe.exe"
sudo -n chmod -R a+rX "$EXPORT"

if ! grep -qsE "^[[:space:]]*${EXPORT//./\\.}[[:space:]]" /etc/exports; then
  printf '%s 127.0.0.1(%s)\n' "$EXPORT" "$OPTS" | sudo -n tee -a /etc/exports >/dev/null
fi
sudo -n exportfs -ra
sudo -n exportfs -v | grep -F "$EXPORT" | sed 's/^/== exported: /'

echo "== stage: glibc 2.31 under /LIB64 + /lib64/ld + WINEP2.BIN + WAVE.CFG"
mmd -i "$IMG" ::/LIB64 ::/lib64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy failed $2"; }
for so in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
          libresolv.so.2 libgcc_s.so.1 libutil.so.1 libnss_files.so.2 libnss_dns.so.2; do
  [ -e "$LDIR/$so" ] && stage "$(readlink -f "$LDIR/$so")" "::/LIB64/$so"
done
stage "$(readlink -f "$LDIR/ld-linux-x86-64.so.2")" ::/lib64/ld-linux-x86-64.so.2
stage "$OUT/winep2.bin" ::/WINEP2.BIN
printf 'WINEP2.BIN\n' > "$OUT/wave.cfg"
stage "$OUT/wave.cfg" ::/WAVE.CFG

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || die "no OVMF_CODE_4M.fd"
OVMF="$OUT/OVMF_CODE_4M.fd"; [ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

echo "== boot (deadline ${TIMEOUT_S}s, under the shared QEMU lock)"
: >"$LOG"
QA="-display none -enable-kvm${QEMU_MEM:+ -m $QEMU_MEM}"
flock /tmp/cp-qemu.lock bash -c '
  ( make -o disk.img ARCH=intel MODE=test EDK2_X86_64='"$OVMF"' run \
         QEMU_ARGS="'"$QA"'" >>'"$LOG"' 2>&1 & )
  for _ in $(seq 1 '"$TIMEOUT_S"'); do
    grep -aq "System halt" '"$LOG"' && break
    grep -aq "\[P2\] done" '"$LOG"' && sleep 3 && break
    sleep 1
  done
  # Only the QEMU holding THIS tree'"'"'s disk.img: other lanes share the box.
  for qp in $(fuser '"$IMG"' 2>/dev/null); do kill "$qp" 2>/dev/null; done'

# ---- grade -------------------------------------------------------------------
tr -d '\r' < "$LOG" > "$OUT/serial.txt"
S="$OUT/serial.txt"
span() { awk -v t="$1" '$0 ~ "\\[P2\\] begin "t" :" {on=1; next} $0 ~ "\\[P2\\] end "t" " {on=0} on' "$S"; }
endrow() { grep -a "\[P2\] end $1 " "$S" | tail -1; }
echo
echo "== the P2 rows"
grep -aE '^\[P2\]|\[WP\] (FAIL|done)|^hello$' "$S" | grep -av ' ps-' || echo "(no [P2] rows)"
echo
G1=FAIL; G2=FAIL; G3=FAIL
span echo | grep -qx 'hello' && endrow echo | grep -q 'status=0 ' && G1=PASS
miss=0
while IFS= read -r m; do
  [ -n "$m" ] || continue
  span winprobe | grep -qF "[WP] PASS $m (" || { echo "missing marker: $m"; miss=$((miss+1)); }
done < "$MARKERS"
span winprobe | grep -q '\[WP\] FAIL' && miss=$((miss+1))
[ $miss -eq 0 ] && span winprobe | grep -q '^\[WP\] done rc=0$' &&
  endrow winprobe | grep -q 'status=0 ' && G2=PASS
endrow kill | grep -q 'status=0 ' && grep -aq '^\[P2\] ps-extra count=0$' "$S" && G3=PASS
echo "G1 wine cmd /c echo hello prints hello : $G1   ($(endrow echo))"
echo "G2 winprobe.exe every marker + exit 0  : $G2   ($(endrow winprobe); $(wc -l < "$MARKERS") markers, $miss missing/failed)"
echo "G3 wineserver -k leaves no processes   : $G3   ($(endrow kill); $(grep -a 'ps-extra' "$S" | tail -1))"
echo "extra: winprobe from C:\\ (tmpfs)       : $(span winprobe-c | grep -a '^\[WP\] done' | tail -1) ($(endrow winprobe-c))"
echo
echo "== ps before / after"
grep -aE '^\[P2\] ps-(before|after)' "$S"
echo
echo "raw serial: $LOG"
[ "$G1$G2$G3" = PASSPASSPASS ]
