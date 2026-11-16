#!/usr/bin/env bash
# tools/cp/p4/run-p4.sh - Gate P4 inside HobbyOS (lane C4-x2, docs/cyberpunk.md
# §6 P4):
#
#   winecfg opens in an IceWM window and its tabs respond to QMP clicks;
#   notepad accepts typed text; a raw-input probe prints relative mouse deltas
#   from QMP-injected motion.  Evidence: screendumps + serial log.
#
# What it does (the run-wine-p2.sh pattern; that script belongs to C3-pfx):
#   1. builds the intel MODE=desktop DESKTOP=icewm kernel + disk (XSESSION ->
#      XHOBBY.BIN + IceWM), tools/cp/p4/winep4.c (the native launcher that
#      XSESSION starts from /XSTART.TXT) and src/user/cp/rawinput.c
#      (tools/cp/p4/build-rawinput.sh);
#   2. exports, over a TEMPORARY localhost-only read-only NFS line
#        <dir> 127.0.0.1(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)
#      the X variant of Wine (/CP/WINE), the C4-x client libraries + FreeType
#      (/CP/X11) and the probe (/CP/P4).  /etc/exports is edited only under
#      /tmp/cp-exports.lock, taken OUTER to the QEMU lock, from before the
#      backup until after the restore; the exit path removes exactly this
#      run's line and cmp's the file against the pre-run copy;
#   3. stages, on a COPY of disk.img: glibc 2.31 (/LIB64 + /lib64/ld),
#      /WINEP4.BIN, /XSTART.TXT, and /X11/XSESSION.ENV + HB_SECOND_SCREEN=0
#      (the one-screen DDX mode: Composite and RandR's Xinerama come back);
#   4. boots once under `flock /tmp/cp-qemu.lock`: tools/cp/p4/p4_gate.py
#      follows the serial log, injects QMP input, screendumps and grades.
#
# Exit 0 only when G1 (winecfg), G2 (notepad) and G3 (raw deltas) pass.
# Env: CP_WINE_STAGE (default ~/cp/wine-build-x/stage/CP/WINE),
#      CP_X11_STAGE (default ~/cp/x11-build-x2/stage/CP/X11),
#      GLIBC231_LIB (default ~/cp/wine-build/glibc231/lib/x86_64-linux-gnu),
#      WINE_EXPORT (default /srv/nfs/cp-x2-p4), P4_OUT (default obj/p4/run),
#      P4_WINEDEBUG (default err+all,fixme-all), QEMU_MEM (default 12288M),
#      P4_BOOT_ONLY=1 (iteration aid: wineboot only; never a gate result),
#      P4_SKIP_BUILD=1 (reuse obj/ and disk.img from the previous run).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
STAGE="${CP_WINE_STAGE:-$HOME/cp/wine-build-x/stage/CP/WINE}"
X11STAGE="${CP_X11_STAGE:-$HOME/cp/x11-build-x2/stage/CP/X11}"
LDIR="${GLIBC231_LIB:-$HOME/cp/wine-build/glibc231/lib/x86_64-linux-gnu}"
EXPORT="${WINE_EXPORT:-/srv/nfs/cp-x2-p4}"
WDBG="${P4_WINEDEBUG:-err+all,fixme-all}"
OUT="${P4_OUT:-$ROOT/obj/p4/run}"; mkdir -p "$OUT"
die() { echo "p4: $*" >&2; exit 2; }
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP="${TOOLS_SKIP:-1}"

for t in clang ld.lld llvm-objcopy mcopy mmd qemu-system-x86_64 exportfs cmp python3; do
  command -v "$t" >/dev/null 2>&1 || die "missing $t"
done
[ -x "$STAGE/bin/wine" ] || die "no staged Wine at $STAGE (CP_WINE_VARIANT=x tools/cp/build-wine.sh all)"
[ -f "$X11STAGE/lib/libX11.so.6" ] || die "no /CP/X11 stage at $X11STAGE (tools/cp/x11/build.sh all)"
[ -f "$LDIR/libc-2.31.so" ] || die "no glibc 2.31 at $LDIR"
case "$EXPORT" in /srv/nfs/?*) ;; *) die "WINE_EXPORT must be under /srv/nfs/ ($EXPORT)";; esac
[ -e "$EXPORT" ] && die "$EXPORT already exists: not this run's to reuse or delete; remove it by hand"

if [ -z "${P4_SKIP_BUILD:-}" ]; then
  echo "== build: kernel + disk (intel, desktop, icewm), winep4.bin, rawinput.exe"
  rm -f hobbyos.elf
  make ARCH=intel MODE=desktop DESKTOP=icewm hobbyos.elf disk.img >"$OUT/build.log" 2>&1 ||
    { echo "BUILD FAILED"; tail -30 "$OUT/build.log"; exit 2; }
  make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || die "libc.a"
  RESINC="$(clang -print-resource-dir)/include"
  UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
           -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
           -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large)
  clang "${UCFLAGS[@]}" -DWINE_EXPORT="\"$EXPORT\"" -DP4_WINEDEBUG="\"$WDBG\"" \
    ${P4_BOOT_ONLY:+-DP4_BOOT_ONLY=1} -c tools/cp/p4/winep4.c -o "$OUT/winep4.o" || die "winep4.c"
  ld.lld -T src/user/linker.ld -e _start -o "$OUT/winep4.elf" "$OUT/winep4.o" obj/intel/libc.a || die "ld winep4"
  llvm-objcopy -O binary "$OUT/winep4.elf" "$OUT/winep4.bin"
  WINE_BUILD="$(dirname "$(dirname "$(dirname "$STAGE")")")" \
    bash tools/cp/p4/build-rawinput.sh "$OUT/rawinput.exe" >"$OUT/rawinput-build.log" 2>&1 ||
    { cat "$OUT/rawinput-build.log"; die "rawinput.exe"; }
fi
[ -f "$OUT/winep4.bin" ] && [ -f "$OUT/rawinput.exe" ] && [ -f disk.img ] || die "nothing built"

# ---- /etc/exports: ONE localhost-only ro line, removed again on every exit ---
OPTS="ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534"
MYLINE="$EXPORT 127.0.0.1($OPTS)"
exec 9>>/tmp/cp-exports.lock
echo "== waiting for /tmp/cp-exports.lock"
flock 9
EXP_BAK="$(mktemp /tmp/cp-x2-exports.XXXXXX)"
sudo -n cat /etc/exports > "$EXP_BAK" || die "cannot copy /etc/exports"
ADDED=""; MADE_DIR=""; CLEANED=""
cleanup() {
  [ -n "$CLEANED" ] && return 0
  CLEANED=1
  if [ -n "$ADDED" ]; then
    local t; t="$(mktemp /tmp/cp-x2-exports-new.XXXXXX)"
    sudo -n cat /etc/exports | grep -vxF -- "$MYLINE" > "$t"
    sudo -n tee /etc/exports < "$t" >/dev/null
    rm -f "$t"
  fi
  sudo -n exportfs -ra
  if cmp -s "$EXP_BAK" <(sudo -n cat /etc/exports); then
    echo "== /etc/exports: our line removed (cmp identical to the pre-run copy)"
    rm -f "$EXP_BAK"
  else
    echo "== WARNING: /etc/exports differs from the pre-run copy $EXP_BAK (kept)"
  fi
  [ -n "$MADE_DIR" ] && sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"
  sudo -n exportfs -v 2>/dev/null | grep -F "$EXPORT" && echo "== WARNING: $EXPORT still exported"
  return 0
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP

echo "== export: $EXPORT (WINE + X11 + P4), 127.0.0.1 only, ro"
sudo -n mkdir -p "$EXPORT/WINE" "$EXPORT/X11" "$EXPORT/P4" && MADE_DIR=1
sudo -n rsync -a --delete "$STAGE/" "$EXPORT/WINE/" || die "rsync wine"
sudo -n rsync -a --delete "$X11STAGE/" "$EXPORT/X11/" || die "rsync x11"
sudo -n cp "$OUT/rawinput.exe" "$EXPORT/P4/rawinput.exe"
sudo -n chmod -R a+rX "$EXPORT"
if ! grep -qsE "^[[:space:]]*${EXPORT//./\\.}[[:space:]]" /etc/exports; then
  printf '%s\n' "$MYLINE" | sudo -n tee -a /etc/exports >/dev/null && ADDED=1
fi
sudo -n exportfs -ra
sudo -n exportfs -v | grep -F "$EXPORT" | sed 's/^/== exported: /'

echo "== stage (on a copy of disk.img): glibc 2.31, WINEP4.BIN, XSTART.TXT, XSESSION.ENV"
IMG="$OUT/disk.img"
cp disk.img "$IMG"
mmd -i "$IMG" ::/LIB64 ::/lib64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy failed $2"; }
for so in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
          libresolv.so.2 libgcc_s.so.1 libutil.so.1 libnss_files.so.2 libnss_dns.so.2; do
  [ -e "$LDIR/$so" ] && stage "$(readlink -f "$LDIR/$so")" "::/LIB64/$so"
done
stage "$(readlink -f "$LDIR/ld-linux-x86-64.so.2")" ::/lib64/ld-linux-x86-64.so.2
stage "$OUT/winep4.bin" ::/WINEP4.BIN
printf '# Gate P4 (tools/cp/p4/run-p4.sh)\n/WINEP4.BIN\n' > "$OUT/xstart.txt"
stage "$OUT/xstart.txt" ::/XSTART.TXT
mcopy -o -i "$IMG" ::/X11/XSESSION.ENV "$OUT/xsession.env.orig" >/dev/null 2>&1 || : > "$OUT/xsession.env.orig"
{ grep -v '^HB_SECOND_SCREEN=' "$OUT/xsession.env.orig"; echo "HB_SECOND_SCREEN=0"; } > "$OUT/xsession.env"
stage "$OUT/xsession.env" ::/X11/XSESSION.ENV

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || die "no OVMF_CODE_4M.fd"
cp "$OVMF_SRC" "$OUT/ovmf.fd"

echo "== boot + drive (under the shared QEMU lock)"
flock /tmp/cp-qemu.lock python3 tools/cp/p4/p4_gate.py --out "$OUT" --disk "$IMG" \
  --ovmf "$OUT/ovmf.fd" --mem "${QEMU_MEM:-12288M}" 2>&1 | tee "$OUT/gate.log"
rc=${PIPESTATUS[0]}

tr -d '\r' < "$OUT/serial.log" > "$OUT/serial.txt" 2>/dev/null
echo
echo "== the P4 rows"
grep -aE '\[P4\]|\[RI\] (PASS|FAIL|phase|window|mode|modes|Change|restore|done)' "$OUT/serial.txt" | grep -av ' ps-' | head -80
echo "== X server rows"
grep -aE 'hobbyos: (one screen|two screens|randr)|Composite|XINERAMA' "$OUT/serial.txt" | head -20
for f in "$OUT"/*.ppm; do
  [ -f "$f" ] && command -v convert >/dev/null && convert "$f" "${f%.ppm}.png" 2>/dev/null
done
echo "raw serial: $OUT/serial.log; screendumps: $OUT/*.png"
exit "$rc"
