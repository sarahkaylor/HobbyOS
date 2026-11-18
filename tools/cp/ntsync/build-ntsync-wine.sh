#!/usr/bin/env bash
# tools/cp/ntsync/build-ntsync-wine.sh -- C2-ntsync: a copy of the X11 Wine
# 11.0 build (~/cp/wine-build-x) with ntsync compiled in (docs/cp-ntsync.md §4).
#
# Wine 11.0 already carries the ntsync client and server (server/inproc_sync.c,
# dlls/ntdll/unix/sync.c, both behind HAVE_LINUX_NTSYNC_H); the glibc 2.31
# sysroot simply has no <linux/ntsync.h>, so configure said no.  This script:
#   1. copies build-x86_64/ and stage/ into $OUT (the original is never written);
#   2. vendors the Linux UAPI header as $OUT/build-x86_64/include/linux/ntsync.h
#      (the build dir's own -Iinclude, searched for <...> includes too);
#   3. proves what configure would change: re-runs the SAME configure line in a
#      scratch dir with the header on the include path and diffs config.h
#      (expected: only HAVE_LINUX_NTSYNC_H);
#   4. flips that one define in the copy's config.h, keeps its mtime (so make
#      does not rebuild the 5000 objects that include config.h but never test
#      the macro), deletes the only two objects that do, and runs make;
#   5. strips wineserver and ntdll.so into the copied stage exactly as
#      `make install` does (tools/install --strip-program=strip, i.e. strip -g; proven on the
#      original pair first).
# Env: SRC (default ~/cp/wine-build-x), OUT (default ~/cp/lanes/ntsync-wine),
#      HDR (default /usr/include/linux/ntsync.h, the strix host's UAPI copy).
set -euo pipefail
SRC="${SRC:-$HOME/cp/wine-build-x}"
OUT="${OUT:-$HOME/cp/lanes/ntsync-wine}"
HDR="${HDR:-/usr/include/linux/ntsync.h}"
MAKEFLAGS="${MAKEFLAGS:--j16}"; export MAKEFLAGS
log() { echo "[ntsync-wine] $*"; }

[ -f "$HDR" ] || { echo "no UAPI header at $HDR"; exit 2; }
grep -q NTSYNC_IOC_EVENT_READ "$HDR" || { echo "$HDR lacks NTSYNC_IOC_EVENT_READ"; exit 2; }
mkdir -p "$OUT"
if [ ! -d "$OUT/build-x86_64" ]; then
  log "copying $SRC/build-x86_64 and stage (cp -a, preserves mtimes)"
  cp -a "$SRC/build-x86_64" "$OUT/build-x86_64"
  cp -a "$SRC/stage" "$OUT/stage"
fi
B="$OUT/build-x86_64"
mkdir -p "$B/include/linux"
cp "$HDR" "$B/include/linux/ntsync.h"
sha256sum "$HDR" "$B/include/linux/ntsync.h" | tee "$OUT/ntsync-header.sha256"

# 3. what would configure say?
CFGLINE="$(sed -n 's/^  \$ //p' "$SRC/build-x86_64/config.log" | head -1)"
log "configure line: $CFGLINE"
SCR="$OUT/cfgcheck"
rm -rf "$SCR"; mkdir -p "$SCR/inc/linux"; cp "$HDR" "$SCR/inc/linux/ntsync.h"
# The environment tools/cp/build-wine.sh gave the original configure (its
# do_configure, variant x): CC/CFLAGS/LDFLAGS as config.log's ac_cv_env_*
# record them, pkg-config pointed at the sysroot and the X11 work dir's pc/.
CCV="$(sed -n "s/^ac_cv_env_CC_value='\(.*\)'$/\1/p" "$SRC/build-x86_64/config.log" | head -1)"
LDV="$(sed -n "s/^ac_cv_env_LDFLAGS_value='\(.*\)'$/\1/p" "$SRC/build-x86_64/config.log" | head -1)"
SYSR="${CCV#*--sysroot=}"; SYSR="${SYSR%% *}"
PCD="$SYSR/usr/lib/x86_64-linux-gnu/pkgconfig:${X11PC:-$HOME/cp/x11-build-x2/pc}"
log "scratch configure: CC='$CCV' LDFLAGS='$LDV' PKG_CONFIG_LIBDIR=$PCD"
( cd "$SCR" && eval "PKG_CONFIG_LIBDIR=\"$PCD\" PKG_CONFIG_PATH= CC=\"$CCV\" CFLAGS=\"-O2 -g\" x86_64_CC=clang CPPFLAGS=\"-I$SCR/inc\" LDFLAGS=\"$LDV\" $CFGLINE" > configure.out 2>&1 ) || {
  log "scratch configure failed: see $SCR/configure.out"; exit 1; }
diff "$SRC/build-x86_64/include/config.h" "$SCR/include/config.h" > "$OUT/config-h.diff" || true
log "config.h diff (original vs configure with the header):"
cat "$OUT/config-h.diff"
grep -q '^> #define HAVE_LINUX_NTSYNC_H 1' "$OUT/config-h.diff" || { log "configure did not find the header"; exit 1; }
rm -rf "$SCR"

# 4. flip the define, keep config.h's mtime, rebuild the two users.
CH="$B/include/config.h"
touch -r "$CH" "$OUT/.config-h-mtime"
sed -i 's|^/\* #undef HAVE_LINUX_NTSYNC_H \*/$|#define HAVE_LINUX_NTSYNC_H 1|' "$CH"
touch -r "$OUT/.config-h-mtime" "$CH"
grep -n 'HAVE_LINUX_NTSYNC_H' "$CH"
rm -f "$B/server/inproc_sync.o" "$B/dlls/ntdll/unix/sync.o"
log "make (MAKEFLAGS=$MAKEFLAGS)"
make -C "$B" > "$OUT/make.log" 2>&1 || { tail -30 "$OUT/make.log"; exit 1; }
grep -E 'inproc_sync|unix/sync\.c|-o server/wineserver|-o dlls/ntdll/ntdll.so' "$OUT/make.log" | cut -c1-160

# 5. strip into the stage, the way install does.
strip -g -o "$OUT/.ws-orig" "$SRC/build-x86_64/server/wineserver"
strip -g -o "$OUT/.nt-orig" "$SRC/build-x86_64/dlls/ntdll/ntdll.so"
if cmp -s "$OUT/.ws-orig" "$SRC/stage/CP/WINE/bin/wineserver" &&
   cmp -s "$OUT/.nt-orig" "$SRC/stage/CP/WINE/lib/wine/x86_64-unix/ntdll.so"; then
  log "strip method reproduces the original stage byte-for-byte"
else
  log "WARNING: strip of the original build != original stage (install differs)"
fi
rm -f "$OUT/.ws-orig" "$OUT/.nt-orig"
strip -g -o "$OUT/stage/CP/WINE/bin/wineserver" "$B/server/wineserver"
strip -g -o "$OUT/stage/CP/WINE/lib/wine/x86_64-unix/ntdll.so" "$B/dlls/ntdll/ntdll.so"
for f in bin/wineserver lib/wine/x86_64-unix/ntdll.so; do
  printf '%-40s orig %s  ntsync %s\n' "$f" \
    "$(sha256sum < "$SRC/stage/CP/WINE/$f" | cut -c1-16)" \
    "$(sha256sum < "$OUT/stage/CP/WINE/$f" | cut -c1-16)"
done
strings "$OUT/stage/CP/WINE/bin/wineserver" | grep -c '/dev/ntsync' | sed 's/^/wineserver mentions \/dev\/ntsync: /'
(cd "$OUT/stage" && find CP -type f | sort | xargs sha256sum) > "$OUT/stage-manifest.sha256"
log "done: stage $OUT/stage/CP/WINE"
