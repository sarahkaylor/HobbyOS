#!/usr/bin/env bash
# tools/cp/ext2w/reg_gate.sh - lane C3-ext2w's registry gate (docs/cp-ext2w.md
# section 5): console Wine on a prefix on /EXT,
#   boot 1  wineboot -i; wine reg add HKCU\Software\CpTest /v X /d Y /f;
#           wineserver -k (timed); NO sync(2) -- the halt comes after the
#           kernel's write-back has committed the cache
#   boot 2  wine reg query HKCU\Software\CpTest /v X  must print X REG_SZ Y
# and after each boot e2fsck -fn rc 0; after boot 1 the hive (pfx/user.reg,
# read with debugfs) holds the key and no reg*.tmp is left beside it.
#
# The Wine stage reaches the guest as in run-wine-p2.sh: a localhost-only
# read-only NFS export, added and removed only through tools/cp/exports.sh
# (removed from an EXIT trap).  Every boot runs under one QEMU slot.
# Env: CP_WINE_STAGE (default ~/cp/wine-build/stage/CP/WINE), GLIBC231_LIB
#      (default ~/cp/wine-build/glibc231/lib/x86_64-linux-gnu), REG_EXPORT
#      (default /srv/nfs/cp-ext2w-reg), TIMEOUT_S (default 2400 per boot),
#      REG_OUT (default <tree>/obj/ext2w-reg).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
STAGE="${CP_WINE_STAGE:-$HOME/cp/wine-build/stage/CP/WINE}"
LDIR="${GLIBC231_LIB:-$HOME/cp/wine-build/glibc231/lib/x86_64-linux-gnu}"
EXPORT="${REG_EXPORT:-/srv/nfs/cp-ext2w-reg}"
TIMEOUT_S="${TIMEOUT_S:-2400}"
OUT="${REG_OUT:-$ROOT/obj/ext2w-reg}"; mkdir -p "$OUT"
IMG="$ROOT/disk.img"
PIMG="$OUT/cpdata-reg.img"
PATH="/usr/sbin:/sbin:$PATH"
die() { echo "reg_gate: $*" >&2; exit 2; }
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP="${TOOLS_SKIP:-1}"
case "$EXPORT" in /srv/nfs/cp-?*) ;; *) die "REG_EXPORT must be /srv/nfs/cp-<name>";; esac
[ -e "$EXPORT" ] && die "$EXPORT already exists: not this run's to reuse or delete"
[ -x "$STAGE/bin/wine" ] || die "no staged Wine at $STAGE"
[ -f "$LDIR/libc-2.31.so" ] || die "no glibc 2.31 at $LDIR"

echo "== build: kernel + disk (intel, test), regp launchers"
make ARCH=intel MODE=test hobbyos.elf disk.img >"$OUT/build.log" 2>&1 ||
  { tail -30 "$OUT/build.log"; die "build"; }
make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || die "libc.a"
RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large)
for ph in 1 2; do
  clang "${UCFLAGS[@]}" -DWINE_EXPORT="\"$EXPORT\"" -DREGP_PHASE=$ph \
    -c tools/cp/ext2w/regp.c -o "$OUT/regp$ph.o" || die "regp.c"
  ld.lld -T src/user/linker.ld -e _start -o "$OUT/regp$ph.elf" "$OUT/regp$ph.o" obj/intel/libc.a || die "ld"
  llvm-objcopy -O binary "$OUT/regp$ph.elf" "$OUT/regp$ph.bin"
done

MADE_DIR="" ADDED="" CLEANED=""
cleanup() {
  [ -n "$CLEANED" ] && return 0
  CLEANED=1
  [ -n "$ADDED" ] && bash "$ROOT/tools/cp/exports.sh" remove "$EXPORT" | sed 's/^/== /'
  [ -n "$MADE_DIR" ] && sudo -n rm -rf "$EXPORT" && echo "== removed $EXPORT"
  return 0
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP
echo "== export: $EXPORT (Wine stage), 127.0.0.1 only, ro"
sudo -n mkdir -p "$EXPORT/WINE" && MADE_DIR=1
sudo -n rsync -a --delete "$STAGE/" "$EXPORT/WINE/" || die "rsync stage"
sudo -n chmod -R a+rX "$EXPORT"
ADDED=1
bash "$ROOT/tools/cp/exports.sh" add "$EXPORT" | sed 's/^/== /' || die "exports.sh add"

echo "== stage: glibc 2.31 under /LIB64 + /lib64/ld"
mmd -i "$IMG" ::/LIB64 ::/lib64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy $2"; }
for so in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
          libresolv.so.2 libgcc_s.so.1 libutil.so.1 libnss_files.so.2 libnss_dns.so.2; do
  [ -e "$LDIR/$so" ] && stage "$(readlink -f "$LDIR/$so")" "::/LIB64/$so"
done
stage "$(readlink -f "$LDIR/ld-linux-x86-64.so.2")" ::/lib64/ld-linux-x86-64.so.2
printf 'REGP.BIN\n' > "$OUT/wave.cfg"
stage "$OUT/wave.cfg" ::/WAVE.CFG
OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || die "no OVMF_CODE_4M.fd"
OVMF="$OUT/OVMF_CODE_4M.fd"; [ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

rg_boot() { # rg_boot <log>: one boot, ends at System halt
  local log="$1"
  : >"$log"
  bash "$ROOT/tools/cp/qemu-slot.sh" bash -c '
    ( make -o disk.img ARCH=intel MODE=test EDK2_X86_64='"$OVMF"' CPDATA='"$PIMG"' run \
           QEMU_ARGS="-display none -enable-kvm" >>'"$log"' 2>&1 & )
    for i in $(seq 1 '"$TIMEOUT_S"'); do
      grep -aq "System halt" '"$log"' && sleep 1 && break
      [ $i -gt 30 ] && ! fuser '"$IMG"' >/dev/null 2>&1 && break
      sleep 1
    done
    for qp in $(fuser '"$IMG"' 2>/dev/null); do kill "$qp" 2>/dev/null; done
    for i in $(seq 1 30); do fuser '"$IMG"' >/dev/null 2>&1 || break; sleep 1; done'
  tr -d '\r' < "$log" > "${log%.log}.txt"
}
fails=0
bad() { echo "[HOST] FAIL $*"; fails=$((fails + 1)); }
good() { echo "[HOST] ok   $*"; }
fsck_img() {
  e2fsck -fn "$PIMG" > "$OUT/fsck-$1.log" 2>&1
  local rc=$?
  [ "$rc" = 0 ] && good "e2fsck -fn after $1 rc=0 ($(tail -1 "$OUT/fsck-$1.log"))" ||
    { bad "e2fsck -fn after $1 rc=$rc"; head -30 "$OUT/fsck-$1.log"; }
}

CPDATA_FORCE=1 bash tools/cp/mkcpdata.sh "$PIMG" 8G >/dev/null || die "mkcpdata"
echo "== boot 1: wineboot -i, reg add, wineserver -k, no sync"
stage "$OUT/regp1.bin" ::/REGP.BIN
rg_boot "$OUT/serial-b1.log"
B1="$OUT/serial-b1.txt"
grep -aE '^\[RG\]|^\[E2W\]' "$B1"
grep -aq '^\[RG\] end regadd status=0 ' "$B1" && good "reg add exit 0" || bad "reg add did not exit 0"
kms=$(sed -n 's/^\[RG\] end kill status=\([0-9-]*\) ms=\([0-9]*\).*/\2/p' "$B1" | tail -1)
grep -aq '^\[RG\] end kill status=0 ' "$B1" && good "wineserver -k exit 0 in ${kms} ms (Wine SIGKILLs the server at 10.5 s)" || bad "wineserver -k"
grep -aq 'System halt' "$B1" && good "boot 1 halted" || bad "boot 1 never halted"
fsck_img b1
debugfs -R "dump /pfx/user.reg $OUT/user-b1.reg" "$PIMG" >/dev/null 2>&1
if grep -aq '^\[Software\\\\CpTest\]' "$OUT/user-b1.reg" && grep -aq '^"X"="Y"' "$OUT/user-b1.reg"; then
  good "pfx/user.reg on the disk holds [Software\\\\CpTest] \"X\"=\"Y\" ($(stat -c %s "$OUT/user-b1.reg") bytes)"
else
  bad "pfx/user.reg on the disk lacks the key"
fi
debugfs -R "dump /pfx/system.reg $OUT/system-b1.reg" "$PIMG" >/dev/null 2>&1
if grep -aq '^\[Software\\\\CpTest\]' "$OUT/system-b1.reg" && grep -aq '^"X"="Y"' "$OUT/system-b1.reg"; then
  good "pfx/system.reg on the disk holds the HKLM key ($(stat -c %s "$OUT/system-b1.reg") bytes)"
else
  bad "pfx/system.reg on the disk lacks the HKLM key"
fi
grep -aq '^\[RG\] end regadd-hklm status=0 ' "$B1" && good "HKLM reg add exit 0" || bad "HKLM reg add"
tmps=$(debugfs -R "ls -p /pfx" "$PIMG" 2>/dev/null | awk -F/ '$6 ~ /^reg.*\.tmp$/ {print $6}' | tr '\n' ' ')
[ -z "$tmps" ] && good "no reg*.tmp left in pfx/" || bad "hive temp files left: $tmps"

echo "== boot 2: wine reg query"
stage "$OUT/regp2.bin" ::/REGP.BIN
rg_boot "$OUT/serial-b2.log"
B2="$OUT/serial-b2.txt"
grep -aE '^\[RG\]' "$B2"
# reg's console output is interleaved with kernel rows ("    X[MR] s=...",
# whole "[XYZ] ..." lines) and other Wine processes' output (winemenubuilder
# paths): cut a row glued onto a fragment, keep reg's own line shapes (the
# key "HKEY_..." and the 4-space-indented value fields), join, and look for
# "X REG_SZ Y".
qv=$(awk '/^\[RG\] begin regquery/{on=1;next} /^\[RG\] end regquery/{on=0} on' "$B2" |
     sed 's/\[[A-Z][A-Z0-9_-]*\] .*$//' | grep -aE '^(HKEY_| {4})' | tr '\n' ' ')
echo "== reg query output (kernel rows removed): $(printf '%s' "$qv" | tr -s ' ' | cut -c1-200)"
printf '%s' "$qv" | grep -aqE 'HKEY_CURRENT_USER\\Software\\CpTest +X +REG_SZ +Y( |$)' &&
  good "boot 2: reg query prints X REG_SZ Y" || bad "boot 2: the value is not there"
grep -aq '^\[RG\] end regquery status=0 ' "$B2" && good "reg query exit 0" || bad "reg query exit status"
qv=$(awk '/^\[RG\] begin regquery-hklm/{on=1;next} /^\[RG\] end regquery-hklm/{on=0} on' "$B2" |
     sed 's/\[[A-Z][A-Z0-9_-]*\] .*$//' | grep -aE '^(HKEY_| {4})' | tr '\n' ' ')
printf '%s' "$qv" | grep -aqE 'HKEY_LOCAL_MACHINE\\Software\\CpTest +X +REG_SZ +Y( |$)' &&
  good "boot 2: HKLM reg query prints X REG_SZ Y" || bad "boot 2: the HKLM value is not there"
fsck_img b2
if [ "$fails" = 0 ]; then echo "reg_gate: PASS"; else echo "reg_gate: FAIL ($fails)"; fi
[ "$fails" = 0 ]
