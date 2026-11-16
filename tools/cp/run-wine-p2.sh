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
#      QEMU user networking reaches it as 10.0.2.2.  The line is added and
#      removed only through tools/cp/exports.sh (docs/cyberpunk.md §7.4: it
#      holds /tmp/cp-exports.lock only for the edit, never across a boot);
#      every exit path (EXIT/INT/TERM/HUP trap) removes it.  The export
#      directory is removed after the run.  The export is never widened; an
#      export dir that already exists is refused (it is not this run's to
#      delete);
#   3. stages the glibc 2.31 runtime on the FAT disk (/LIB64 + /lib64/ld);
#   4. boots once under a QEMU slot (tools/cp/qemu-slot.sh) and the launcher runs
#      ps, wineboot -i, wine cmd /c echo hello, wine winprobe.exe (from Z: and
#      from C:), wineserver -k, ps with WINEPREFIX=/tmp/pfx on the compat tmpfs;
#   5. grades the serial log: G1 hello, G2 every marker in
#      tools/cp/winprobe-markers.txt + "[WP] done rc=0" + exit 0, G3 the kill
#      step exits 0 and the after-census has no process the before list lacked.
#
# Exit 0 only when G1, G2 and G3 all pass.
# Env: CP_WINE_STAGE (default ~/cp/wine-build/stage/CP/WINE),
#      GLIBC231_LIB (default ~/cp/wine-build/glibc231/lib/x86_64-linux-gnu),
#      WINE_EXPORT (default /srv/nfs/cp-wine2-p2), TIMEOUT_S (default 7200),
#      P2_WINEDEBUG (default err+all,fixme-all), QEMU_MEM (default unset),
#      P2_BOOT_ONLY=1 (iteration aid: only wineboot -i, then wineserver -k;
#      never a gate result), P2_GRADE_ONLY=1 (re-grade $OUT, no boot).
#
# P2_PFX=1: the prefix-on-ext2 mode (lane C3-pfx, Gate P3 "the prefix survives
# a reboot", docs/cp-pfx.md).  WINEPREFIX=/EXT/pfx on a fresh
# tools/cp/mkcpdata.sh image attached as NVMe namespace 2 (CPDATA); /tmp stays
# the compat tmpfs.  TWO boots of the same disk:
#   boot 1  wineboot -i, the whole tree (wineserver too) exits on its own,
#           sync, the kernel halts -- nothing is killed;
#   boot 2  wine cmd /c echo hello, winprobe.exe from Z: and from C:\ (copied
#           into the ext2 prefix), wineserver -k -- WITHOUT wineboot -i.
# After each boot: e2fsck -fn, and a manifest of the prefix (type, mtime,
# size, path, link target) read by the HOST's ext2 driver (read-only loop
# mount in a private mount namespace).  Graded: P1 boot 1 built the prefix
# (system.reg/user.reg/userdef.reg, dosdevices/c: -> ../drive_c, z: -> /,
# system32/kernel32.dll, .update-timestamp; wineboot's "has been updated"
# trace), P2 clean shutdown (tree gone without a kill, System halt), P3 boot
# 2 did not re-initialise it (no "created the configuration directory", no
# "has been updated", .update-timestamp and every pre-existing file outside
# the set a second start rewrites on Linux unchanged -- that set, measured by
# tools/cp/pfx/host_pfx_ref.sh, is the three *.reg hives, the prefix and
# dosdevices directories and the dosdevices/com* links), P4 G1+G2+G3 in boot
# 2, P5 e2fsck -fn rc=0 after both boots.
# P2_PFX_ONLY=2 reuses $OUT/cpdata-pfx.img from an earlier boot 1 and runs boot
# 2 only (iteration aid; the gate is the two-boot run).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
STAGE="${CP_WINE_STAGE:-$HOME/cp/wine-build/stage/CP/WINE}"
LDIR="${GLIBC231_LIB:-$HOME/cp/wine-build/glibc231/lib/x86_64-linux-gnu}"
EXPORT="${WINE_EXPORT:-/srv/nfs/cp-wine2-p2}"
TIMEOUT_S="${TIMEOUT_S:-7200}"
PFX="${P2_PFX:-}"
if [ -n "$PFX" ]; then WDBG="${P2_WINEDEBUG:-err+all,fixme-all,+wineboot}"; else WDBG="${P2_WINEDEBUG:-err+all,fixme-all}"; fi
OUT="$ROOT/obj/wine-p2"; mkdir -p "$OUT"
LOG="$OUT/serial.log"
IMG="$ROOT/disk.img"
MARKERS="$ROOT/tools/cp/winprobe-markers.txt"
die() { echo "wine-p2: $*" >&2; exit 2; }
export MAKEFLAGS="${MAKEFLAGS:--j16}" TOOLS_SKIP="${TOOLS_SKIP:-1}"

# P2_GRADE_ONLY=1: re-grade the files of the last run in $OUT (no build, no
# export, no boot) -- e.g. after a grader fix.
if [ -z "${P2_GRADE_ONLY:-}" ]; then
for t in clang ld.lld llvm-objcopy mcopy mmd qemu-system-x86_64 exportfs cmp; do
  command -v "$t" >/dev/null 2>&1 || die "missing $t"
done
[ -x "$STAGE/bin/wine" ] || die "no staged Wine at $STAGE (tools/cp/build-wine.sh all)"
[ -f "$LDIR/libc-2.31.so" ] || die "no glibc 2.31 at $LDIR"
[ -f "$MARKERS" ] || die "no marker list $MARKERS"
case "$EXPORT" in /srv/nfs/cp-?*) ;; *) die "WINE_EXPORT must be /srv/nfs/cp-<name> (tools/cp/exports.sh; got $EXPORT)";; esac
[ -e "$EXPORT" ] && die "$EXPORT already exists: not this run's to reuse or delete; remove it by hand"

echo "== build: kernel + disk (intel, test), winep2.bin, winprobe.exe"
make ARCH=intel MODE=test hobbyos.elf disk.img >"$OUT/build.log" 2>&1 ||
  { echo "BUILD FAILED"; tail -30 "$OUT/build.log"; exit 2; }
make -s ARCH=intel obj/intel/libc.a >>"$OUT/build.log" 2>&1 || die "libc.a"
RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large)
launcher() { # launcher <out-stem> [extra cflags...]
  local o="$1"; shift
  clang "${UCFLAGS[@]}" -DWINE_EXPORT="\"$EXPORT\"" -DP2_WINEDEBUG="\"$WDBG\"" \
    ${P2_BOOT_ONLY:+-DP2_BOOT_ONLY=1} ${P2_LS_ONLY:+-DP2_LS_ONLY=1} ${P2_STEP_WINEDEBUG:+-DP2_STEP_WINEDEBUG="\"$P2_STEP_WINEDEBUG\""} \
    "$@" -c src/user/winep2.c -o "$o.o" || die "winep2.c"
  ld.lld -T src/user/linker.ld -e _start -o "$o.elf" "$o.o" obj/intel/libc.a || die "ld winep2"
  llvm-objcopy -O binary "$o.elf" "$o.bin"
}
if [ -n "$PFX" ]; then
  launcher "$OUT/winep2-b1" -DP2_PFX_PHASE=1
  launcher "$OUT/winep2-b2" -DP2_PFX_PHASE=2
else
  launcher "$OUT/winep2"
fi
bash tools/cp/build-winprobe.sh "$OUT/winprobe.exe" >"$OUT/winprobe-build.log" 2>&1 ||
  { cat "$OUT/winprobe-build.log"; die "winprobe.exe"; }
# ---- /etc/exports: ONE localhost-only ro line, removed again on every exit ---
# docs/cyberpunk.md §7.4 (four QEMU slots): every edit goes through
# tools/cp/exports.sh, which holds /tmp/cp-exports.lock only while it edits
# the file (never across a boot), writes only the approved localhost ro line
# for this one directory and checks that every non-cp line is unchanged.  The
# EXIT/INT/TERM/HUP trap removes the line (exports.sh remove is a no-op when
# it is absent) and the export directory this run populated.
MADE_DIR=""
ADDED=""
CLEANED=""
cleanup() {
  [ -n "$CLEANED" ] && return 0
  CLEANED=1
  if [ -n "$ADDED" ]; then
    bash "$ROOT/tools/cp/exports.sh" remove "$EXPORT" | sed 's/^/== /'
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
sudo -n cp "$OUT/winprobe.exe" "$EXPORT/WPROBE/winprobe.exe"
sudo -n chmod -R a+rX "$EXPORT"
ADDED=1 # set before the call: a half-done add is undone by the trap's remove
bash "$ROOT/tools/cp/exports.sh" add "$EXPORT" | sed 's/^/== /' || die "exports.sh add $EXPORT"
sudo -n exportfs -v | grep -F "$EXPORT" | sed 's/^/== exported: /'

echo "== stage: glibc 2.31 under /LIB64 + /lib64/ld + WINEP2.BIN + WAVE.CFG"
mmd -i "$IMG" ::/LIB64 ::/lib64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy failed $2"; }
for so in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
          libresolv.so.2 libgcc_s.so.1 libutil.so.1 libnss_files.so.2 libnss_dns.so.2; do
  [ -e "$LDIR/$so" ] && stage "$(readlink -f "$LDIR/$so")" "::/LIB64/$so"
done
stage "$(readlink -f "$LDIR/ld-linux-x86-64.so.2")" ::/lib64/ld-linux-x86-64.so.2
musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/pfxls.elf" tools/cp/p2/pfxls.c || die "pfxls"
stage "$OUT/pfxls.elf" ::/PFXLS.ELF
if [ -n "$PFX" ]; then
  musl-gcc -static-pie -fPIE -O1 -Wall -o "$OUT/pfxsync.elf" tools/cp/pfx/pfxsync.c || die "pfxsync"
  stage "$OUT/pfxsync.elf" ::/PFXSYNC.ELF
fi
[ -z "$PFX" ] && stage "$OUT/winep2.bin" ::/WINEP2.BIN
printf 'WINEP2.BIN\n' > "$OUT/wave.cfg"
stage "$OUT/wave.cfg" ::/WAVE.CFG

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || die "no OVMF_CODE_4M.fd"
OVMF="$OUT/OVMF_CODE_4M.fd"; [ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

# p2_boot <log> [CPDATA image]: one boot under one QEMU slot (qemu-slot.sh).  In the
# pfx mode the boot ends at "System halt" (the clean shutdown after the
# launcher's "[P2] done"), not 3 s after "[P2] done".
p2_boot() {
  local log="$1" cpd="${2:-}"
  echo "== boot (deadline ${TIMEOUT_S}s, under a QEMU slot, tools/cp/qemu-slot.sh)${cpd:+, CPDATA=$cpd}"
  : >"$log"
  local QA="-display none -enable-kvm${QEMU_MEM:+ -m $QEMU_MEM}"
  local donewait='grep -aq "\[P2\] done" '"$log"' && sleep 3 && break'
  [ -n "$PFX" ] && donewait='grep -aq "\[P2\] done" '"$log"' && { d=$((d+1)); [ $d -gt 120 ] && break; }'
  bash "$ROOT/tools/cp/qemu-slot.sh" bash -c '
    ( make -o disk.img ARCH=intel MODE=test EDK2_X86_64='"$OVMF"' CPDATA='"$cpd"' run \
           QEMU_ARGS="'"$QA"'" >>'"$log"' 2>&1 & )
    d=0
    for i in $(seq 1 '"$TIMEOUT_S"'); do
      grep -aq "System halt" '"$log"' && sleep 1 && break
      '"$donewait"'
      # QEMU gone (crashed or stopped by hand): nothing more will arrive.
      [ $i -gt 30 ] && ! fuser '"$IMG"' >/dev/null 2>&1 && break
      sleep 1
    done
    # Only the QEMU holding THIS tree'"'"'s disk.img: other lanes share the box.
    for qp in $(fuser '"$IMG"' 2>/dev/null); do kill "$qp" 2>/dev/null; done
    for i in $(seq 1 30); do fuser '"$IMG"' >/dev/null 2>&1 || break; sleep 1; done'
}
# ---- pfx mode: two boots of one ext2 disk, judged by the host -------------
PIMG="$OUT/cpdata-pfx.img"
# manifest <tag>: the prefix as Linux's ext2 driver sees it (ro loop mount in
# a private mount namespace; nothing is written to the image)
manifest() {
  local tag="$1"
  sudo -n unshare -m --propagation private sh -c '
    M=$(mktemp -d /tmp/cp-pfx-mnt.XXXXXX)
    mount -t ext2 -o ro,loop "$1" "$M" || exit 3
    ( cd "$M" && find pfx -printf "%p\t%y\t%T@\t%s\t%l\n" | LC_ALL=C sort )
    cat "$M/pfx/.update-timestamp" > "$2.update-timestamp" 2>/dev/null
    umount "$M"; rmdir "$M"' sh "$PIMG" "$OUT/manifest-$tag" > "$OUT/manifest-$tag.txt"
  echo "== manifest $tag: $(wc -l < "$OUT/manifest-$tag.txt") entries, $(awk -F'\t' '$2=="f"{s+=$4} END{print s+0}' "$OUT/manifest-$tag.txt") file bytes"
}
fsck_img() {
  e2fsck -fn "$PIMG" > "$OUT/fsck-$1.log" 2>&1
  local rc=$?
  echo "== e2fsck -fn after $1: rc=$rc ($(tail -1 "$OUT/fsck-$1.log"))"
  echo "$rc" > "$OUT/fsck-$1.rc"
}
if [ -n "$PFX" ]; then
  PATH="/usr/sbin:/sbin:$PATH"
  if [ "${P2_PFX_ONLY:-}" = 2 ]; then
    [ -f "$OUT/cpdata-pfx-b1.img" ] || die "P2_PFX_ONLY=2 needs $OUT/cpdata-pfx-b1.img from an earlier boot 1"
    cp --sparse=always "$OUT/cpdata-pfx-b1.img" "$PIMG"
    echo "== boot 2 only, on a copy of the saved boot-1 disk (an iteration aid, never a gate result)"
  else
    CPDATA_FORCE=1 bash tools/cp/mkcpdata.sh "$PIMG" 8G || die "mkcpdata"
    stage "$OUT/winep2-b1.bin" ::/WINEP2.BIN
    p2_boot "$OUT/serial-b1.log" "$PIMG"
    tr -d '\r' < "$OUT/serial-b1.log" > "$OUT/serial-b1.txt"
    fsck_img b1
    manifest b1
    # keep the boot-1 disk: P2_PFX_ONLY=2 re-runs boot 2 on a copy of it
    cp --sparse=always "$PIMG" "$OUT/cpdata-pfx-b1.img"
  fi
  stage "$OUT/winep2-b2.bin" ::/WINEP2.BIN
  p2_boot "$OUT/serial-b2.log" "$PIMG"
  tr -d '\r' < "$OUT/serial-b2.log" > "$OUT/serial-b2.txt"
  fsck_img b2
  manifest b2
  cp "$OUT/serial-b2.log" "$LOG"
else
  p2_boot "$LOG"
fi

fi # P2_GRADE_ONLY
PIMG="$OUT/cpdata-pfx.img"

# ---- grade -------------------------------------------------------------------
# No pipefail here: `span ... | grep -q` exits early, awk takes SIGPIPE, and
# under pipefail a found marker reads as missing (measured: "hello" printed,
# G1 graded FAIL).
set +o pipefail
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
echo "extra: winprobe from C:\\ (the prefix) : $(span winprobe-c | grep -a '^\[WP\] done' | tail -1) ($(endrow winprobe-c))"
echo
echo "== ps before / after"
grep -aE '^\[P2\] ps-(before|after)' "$S"
echo
echo "raw serial: $LOG"
[ -z "$PFX" ] && { [ "$G1$G2$G3" = PASSPASSPASS ]; exit $?; }

# ---- pfx grading (two boots) -----------------------------------------------
B1="$OUT/serial-b1.txt" B2="$OUT/serial-b2.txt"
M1="$OUT/manifest-b1.txt" M2="$OUT/manifest-b2.txt"
P1=FAIL P2=FAIL P3=FAIL P4=FAIL P5=FAIL
mf() { awk -F'\t' -v p="$2" '$1==p' "$1"; } # path type mtime size link
echo
echo "== pfx: boot 1 rows"
grep -aE '^\[P2\]' "$B1" | grep -av ' ps-' | head -20
p1miss=""
for f in pfx/system.reg pfx/user.reg pfx/userdef.reg pfx/.update-timestamp \
         pfx/drive_c/windows/system32/kernel32.dll pfx/drive_c/windows/system32/ntdll.dll; do
  mf "$M1" "$f" | awk -F'\t' '$2=="f"' | grep -q . || p1miss="$p1miss $f"
done
mf "$M1" pfx/dosdevices/c: | awk -F'\t' '$2=="l" && $5=="../drive_c"' | grep -q . || p1miss="$p1miss c:->../drive_c"
mf "$M1" pfx/dosdevices/z: | awk -F'\t' '$2=="l" && $5=="/"' | grep -q . || p1miss="$p1miss z:->/"
upd1=$(grep -ac 'has been updated' "$B1")
cre1=$(grep -ac 'created the configuration directory' "$B1")
[ -z "$p1miss" ] && [ "$upd1" -ge 1 ] && [ "$cre1" -ge 1 ] && P1=PASS
echo "P1 boot 1 built the prefix on ext2       : $P1   (missing:${p1miss:- none}; created-config rows $cre1, has-been-updated rows $upd1; $(wc -l < "$M1") entries)"
# clean shutdown: the tree exited by itself (no timeout, no kill, no leftover)
# and the kernel halted after the launcher's done row
q1=$(grep -a '^\[P2\] quiesce after wineboot' "$B1" | tail -1)
k1=$(grep -ac '^\[P2\] \(timeout\|leftover\)' "$B1")
grep -aq '^\[P2\] ps-extra count=0$' "$B1" && [ "$k1" = 0 ] && echo "$q1" | grep -q 'extra=0' &&
  grep -aq '^\[P2\] sync done' "$B1" && grep -aq 'System halt' "$B1" && P2=PASS
echo "P2 boot 1 shut down cleanly             : $P2   ($q1; timeouts/leftovers $k1; $(grep -ac 'System halt' "$B1") halt rows)"
# no re-initialisation in boot 2
upd2=$(grep -ac 'has been updated' "$B2")
cre2=$(grep -ac 'created the configuration directory' "$B2")
ts1=$(cat "$OUT/manifest-b1.update-timestamp" 2>/dev/null | tr -d '\n')
ts2=$(cat "$OUT/manifest-b2.update-timestamp" 2>/dev/null | tr -d '\n')
# every entry boot 1 left, except what a 2nd start rewrites on Linux too
# (tools/cp/pfx/host_pfx_ref.sh) and what the boot-2 steps create, must be
# byte-identical in type, mtime, size and link target
# plus pfx/drive_c's own mtime: boot 2's graded steps write into C:\ (the
# copy of winprobe.exe, and W7's C:\winprobe.tmp, created and deleted)
allow='^(pfx|pfx/dosdevices|pfx/dosdevices/com[0-9]+|pfx/(system|user|userdef)\.reg|pfx/drive_c)$'
cmpm() { # cmpm changed|gone|kept|regs : compare the two manifests by path
  awk -F'\t' -v w="$1" 'NR==FNR {a[$1]=$0; next}
    { seen[$1]=1; if ($1 in a) { k++; if (a[$1]!=$0) { if (w=="changed") print $1; if (w=="regs" && $1 ~ /\.reg$/) print $1 } } }
    END { if (w=="kept") print k+0; if (w=="gone") for (p in a) if (!(p in seen)) print p }' "$M1" "$M2"
  [ "$1" = new ] && awk -F'\t' 'NR==FNR {a[$1]=1; next} !($1 in a) {print $1}' "$M1" "$M2"
}
changed=$(cmpm changed | grep -Ev "$allow")
gone=$(cmpm gone | grep -Ev "$allow")
kept=$(cmpm kept)
regre=$(cmpm regs | tr '\n' ' ')
newb2=$(cmpm new | tr '\n' ' ')
echo "== pfx: boot 2 changed entries outside the allowed set: $(printf '%s\n' "$changed" | grep -c .); gone: $(printf '%s\n' "$gone" | grep -c .)"
printf '%s\n' "$changed" "$gone" | grep . | head -20 | sed 's/^/   /'
echo "== pfx: entries new in boot 2: ${newb2:-none}"
[ "$upd2" = 0 ] && [ "$cre2" = 0 ] && [ -n "$ts1" ] && [ "$ts1" = "$ts2" ] && [ -z "$changed" ] && [ -z "$gone" ] &&
  mf "$M2" pfx/system.reg | awk -F'\t' '$2=="f"' | grep -q . && P3=PASS
echo "P3 boot 2 did not re-initialise         : $P3   (has-been-updated $upd2, created-config $cre2; .update-timestamp '$ts1' -> '$ts2'; $kept entries kept; hives re-saved: ${regre:-none})"
wpc=FAIL
span winprobe-c | grep -q '^\[WP\] done rc=0$' && endrow winprobe-c | grep -q 'status=0 ' && wpc=PASS
[ "$G1$G2$G3$wpc" = PASSPASSPASSPASS ] && P4=PASS
echo "P4 boot 2: G1 G2 G3 + winprobe from C:  : $P4   ($G1 $G2 $G3, C:\\winprobe.exe on ext2 $wpc)"
[ "$(cat "$OUT/fsck-b1.rc" 2>/dev/null)" = 0 ] && [ "$(cat "$OUT/fsck-b2.rc" 2>/dev/null)" = 0 ] && P5=PASS
echo "P5 e2fsck -fn clean after both boots    : $P5   (b1 rc=$(cat "$OUT/fsck-b1.rc" 2>/dev/null), b2 rc=$(cat "$OUT/fsck-b2.rc" 2>/dev/null))"
[ "${P2_PFX_ONLY:-}" = 2 ] && echo "(P2_PFX_ONLY=2: boot 1 was not run here; P1/P2 graded from the earlier boot-1 files)"
[ "$P1$P2$P3$P4$P5" = PASSPASSPASSPASSPASS ]
