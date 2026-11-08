#!/usr/bin/env bash
# tools/rw/run-rimworld-desktop.sh — M6: run RimWorld inside a REAL HobbyOS
# desktop session, with the X11/GL compat DSOs staged, and capture the next
# ladder (or the first Unity frame).
#
# Why a separate mode from tools/rw/run-rimworld.sh:
#   run-rimworld.sh boots MODE=test, which starts no desktop.  The game's
#   video init needs a window host — the compat libX11 has no X server; it
#   writes ESC ] X <w>;<h> ~ to stdout and reads ESC ] G ... from stdin, and
#   something must answer that for the child it spawned.  On the shipped
#   desktop that answer comes from HBHOST.BIN, which IceWM's Start menu runs
#   for every native app: `prog "RimWorld" app HBHOST.BIN RWLAUNCH.BIN`.  So
#   the game gets a display when the launcher is started from the Start menu
#   (MODE=desktop, DESKTOP=icewm — the default).
#
# What it does, in order:
#   1. pinned glibc 2.31 (sha256-checked, the M1 pins);
#   2. builds kernel + disk for MODE=desktop DESKTOP=icewm (RW_DESKTOP),
#      rebuilds the xcompat DSOs (rule: rebuild before any libGL work), builds
#      the AD-9 PlayerMain launcher as a glibc-2.31 PIE and the native rwlaunch;
#   3. stages /LIB64: glibc pin + libz + libX11.so.6/libasound.so.2/libudev.so.1
#      (src/user/xcompat/stage.sh) + libGL.so.1/libGLX.so.0
#      (tools/mesa/stage.sh); /RWDSO: the game's own DSOs (AD-10);
#      /RWMANAGED: the 102 managed assemblies (M5c); installs the AD-9
#      launcher as $RW_EXPORT/RimWorldLinux, stages /NFSEXPORT (this run's
#      export path) and RWLAUNCH.BIN in the image root;
#   4. boots with QMP and drives tools/rw/rw_desktop_drive.py: wait for the
#      desktop's [MENU] dump, F1/End/Up*N/Enter onto RWLAUNCH.BIN, screendump
#      the desktop while the game runs, print the raw ladder;
#   5. extracts Player.log and the screenshots (ppm + png) for evidence.
#
# Nothing from the game directory enters the tree.  The data tree is read over
# NFS from $RW_EXPORT (tools/rw/nfs-export.sh sync, one export per worktree);
# the DSOs and the managed assemblies are staged on disk.img.
#
# Usage:
#   tools/rw/run-rimworld-desktop.sh [--timeout S] [--run-timeout S]
# Env: RW_SKIP_EXPORT=1, RW_SKIP_BUILD=1 (reuse the existing kernel/disk AND
#      the already-built launcher binaries — staging still runs),
#      GLIBC231 / GLIBC231_DEV, RW_SHOT_EVERY (default 8s),
#      RW_WAIT_SECS (launcher's own wait, default 900; must exceed
#      --run-timeout for a soak), RW_APP (default RWLAUNCH.BIN).
#
#      RW_DESKTOP=icewm|hobby (default icewm).  icewm boots XSESSION.BIN +
#      ICEWM.BIN and starts the game from IceWM's Start menu: the launcher is
#      the Apps > RimWorld entry (gen-icewm-menu.sh, routed through
#      HBHOST.BIN), and rw_desktop_drive.py finds the taskbar in a screenshot,
#      clicks Start, opens Apps and clicks the entry.  RW_DESKTOP=hobby is a
#      compatibility shim for reproducing old evidence (the native
#      DESKTOP.BIN [MENU] walk); that desktop is being removed.
#      RW_LAUNCH_VIA=icewm-menu|hobby-menu|none (default per desktop);
#      RW_ICEWM_LAUNCH=<label> is the Start-menu entry to click (RimWorld).
#      RW_X11_BUILD_DIR=<dir> points the icewm build at a prebuilt X stack.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
# M6/L63: this worktree's own NFS export (/srv/nfs/rimworld-<lane>).  Two
# lanes used to share one directory and overwrite each other's launcher
# mid-boot; the per-lane export and the /NFSEXPORT marker end that.
# shellcheck source=tools/rw/lane.sh
. "$ROOT/tools/rw/lane.sh"

TIMEOUT_S=420        # boot deadline
RUN_TIMEOUT_S=300    # time the game is allowed to run after launch
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT_S="$2"; shift 2 ;;
    --run-timeout) RUN_TIMEOUT_S="$2"; shift 2 ;;
    *) echo "usage: $0 [--timeout S] [--run-timeout S]" >&2; exit 2 ;;
  esac
done

OUT="$ROOT/obj/rw-desktop"
IMG="$ROOT/disk.img"
LOG="$OUT/serial-desktop.log"
SHOT_DIR="$OUT/shots"
QMP="$ROOT/qmp-sock-rw-desktop"
mkdir -p "$OUT" "$SHOT_DIR"
rm -f "$QMP"

# RV/L83: the desktop this disk boots.  icewm is the default and the only
# supported target: XSESSION.BIN + ICEWM.BIN, and the launcher is the Start
# menu's Apps > RimWorld entry (gen-icewm-menu.sh, routed through HBHOST.BIN).
# RW_DESKTOP=hobby still selects the native DESKTOP.BIN session (the [MENU]/F1
# walk) but is a compatibility shim for reproducing old evidence only -- that
# desktop is being deleted.
RW_DESKTOP="${RW_DESKTOP:-icewm}"
case "$RW_DESKTOP" in
  hobby|icewm) ;;
  *) echo "RW_DESKTOP must be 'icewm' or 'hobby' (got '$RW_DESKTOP')" >&2; exit 2 ;;
esac
# How the app is started.  icewm-menu: click Start -> Apps -> <RW_ICEWM_LAUNCH>
# (default RimWorld).  hobby-menu: the old [MENU] index + F1/End/Up*N/Enter.
# none: boot and screenshot only (an operator drives the Start menu).
if [ "$RW_DESKTOP" = icewm ]; then
  RW_LAUNCH_VIA="${RW_LAUNCH_VIA:-icewm-menu}"
else
  RW_LAUNCH_VIA="${RW_LAUNCH_VIA:-hobby-menu}"
fi
case "$RW_LAUNCH_VIA" in
  icewm-menu) [ "$RW_DESKTOP" = icewm ] || { echo "RW_LAUNCH_VIA=icewm-menu needs RW_DESKTOP=icewm" >&2; exit 2; } ;;
  hobby-menu) [ "$RW_DESKTOP" = hobby ] || { echo "RW_LAUNCH_VIA=hobby-menu needs RW_DESKTOP=hobby" >&2; exit 2; } ;;
  none) ;;
  *) echo "RW_LAUNCH_VIA must be icewm-menu, hobby-menu or none (got '$RW_LAUNCH_VIA')" >&2; exit 2 ;;
esac
echo "== desktop: $RW_DESKTOP (launch via $RW_LAUNCH_VIA)"
X11VAR=""
if [ -n "${RW_X11_BUILD_DIR:-}" ]; then
  X11VAR="X11_BUILD_DIR=$RW_X11_BUILD_DIR"
  [ "$RW_DESKTOP" = icewm ] || echo "  (RW_X11_BUILD_DIR ignored: DESKTOP=$RW_DESKTOP)"
fi

if [ ! -f "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" ] && \
   [ -f "/home/$(id -un)/.local/share/OVMF/OVMF_CODE_4M.fd" ]; then
  HOME="/home/$(id -un)"; export HOME
fi
PATH="/usr/sbin:/sbin:$PATH"; export PATH
for t in clang ld.lld llvm-objcopy mcopy mmd mdir mkfs.fat gcc \
         qemu-system-x86_64 convert python3; do
  command -v "$t" >/dev/null 2>&1 || { echo "ENVIRONMENT: missing $t"; exit 2; }
done

G="${GLIBC231:-/tmp/glibc231}"
D="${GLIBC231_DEV:-/tmp/glibc231-dev}"
LDIR="$G/lib/x86_64-linux-gnu"
DLIB="$D/usr/lib/x86_64-linux-gnu"
LIBC6_SHA=1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0
LIBGCC_SHA=4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c
LIBC6DEV_SHA=11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a

fetch_deb() {
  mkdir -p "$(dirname "$3")"
  if [ ! -f "$3" ]; then
    echo "rw: fetching $(basename "$3")"
    curl -fsS -o "$3" "$1" || { echo "rw: FETCH FAILED $1"; exit 2; }
  fi
  echo "$2  $3" | sha256sum -c --quiet - || { echo "rw: SHA256 MISMATCH $3"; exit 2; }
}
if [ ! -f "$LDIR/ld-2.31.so" ]; then
  T="$ROOT/obj/m1-pins"
  fetch_deb http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb \
            "$LIBC6_SHA" "$T/libc6.deb"
  fetch_deb http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb \
            "$LIBGCC_SHA" "$T/libgcc-s1.deb"
  rm -rf "$G" && mkdir -p "$G"
  dpkg-deb -x "$T/libc6.deb" "$G" && dpkg-deb -x "$T/libgcc-s1.deb" "$G"
fi
if [ ! -f "$DLIB/Scrt1.o" ]; then
  T="$ROOT/obj/m1-pins"
  fetch_deb http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6-dev_2.31-0ubuntu9.18_amd64.deb \
            "$LIBC6DEV_SHA" "$T/libc6-dev.deb"
  rm -rf "$D" && mkdir -p "$D"
  dpkg-deb -x "$T/libc6-dev.deb" "$D"
fi

# L75: the kernel-side block-stress probe must be in the kernel the harness
# *builds* (the build block below produces the ELF that is staged into the
# image), so the flag is exported and the two objects that read it are touched
# before that build -- make cannot see a CFLAGS change by itself.
if [ "${RW_CHURN_SOAK:-0}" = 1 ]; then
  export RW_BLK_STRESS=1
  touch src/kernel/virtio_blk.c src/kernel/process.c
  echo "== block-stress probe: kernel-side (RW_BLK_STRESS=1)"
fi

if [ "${RW_SKIP_BUILD:-0}" != 1 ]; then
  echo "== build: kernel + disk (MODE=desktop DESKTOP=$RW_DESKTOP)"
  TOOLS_SKIP=1 make -j8 ARCH=intel MODE=desktop DESKTOP="$RW_DESKTOP" $X11VAR hobbyos.elf disk.img \
    >"$OUT/build.log" 2>&1 || { echo "BUILD FAILED"; tail -30 "$OUT/build.log"; exit 2; }

  echo "== build: xcompat DSOs (must predate any libGL audit/stage)"
  make -s ARCH=intel xcompat_dsos >"$OUT/build_dsos.log" 2>&1 || {
    echo "BUILD FAILED (xcompat_dsos)"; tail -30 "$OUT/build_dsos.log"; exit 2; }
  if [ "${RW_TRACE_X11:-0}" = 1 ]; then
    echo "== build: traced libX11 (-DXC_TRACE) for the X-level ladder"
    make -s ARCH=intel xcompat_dsos_trace >"$OUT/build_dsos_trace.log" 2>&1 || {
      echo "BUILD FAILED (xcompat_dsos_trace)"; tail -20 "$OUT/build_dsos_trace.log"; exit 2; }
  fi

  echo "== build: native rwlaunch + AD-9 PlayerMain launcher (glibc 2.31 PIE)"
  make -s ARCH=intel obj/intel/libc.a >"$OUT/build_libc.log" 2>&1 || {
    echo "BUILD FAILED (libc.a)"; tail -20 "$OUT/build_libc.log"; exit 2; }
  RESINC="$(clang -print-resource-dir)/include"
  UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
           -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
           -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large
           -DRW_WAIT_SECS="${RW_WAIT_SECS:-900}")
  clang "${UCFLAGS[@]}" -c src/user/rwlaunch.c -o "$OUT/rwlaunch.o" || exit 2
  ld.lld -T src/user/linker.ld -e _start -o "$OUT/rwlaunch.elf" "$OUT/rwlaunch.o" \
         obj/intel/libc.a || exit 2
  llvm-objcopy -O binary "$OUT/rwlaunch.elf" "$OUT/rwlaunch.bin"

  # RW_NO_SHADOW=1: A/B the pthread symbol-shadowing hypothesis.  The
  # launcher keeps -rdynamic (AD-10's dlopen redirect needs it) but omits its
  # own pthread_*/sem_* definitions, so DSOs bind libpthread's entry points.
  SHADOW_CFLAGS=()
  if [ "${RW_NO_SHADOW:-0}" = 1 ]; then SHADOW_CFLAGS=(-DRW_NO_SHADOW_PTHREAD); fi
  gcc -O2 -Wall -Wextra -fPIE -pie -nostdlib -nostartfiles -rdynamic \
      "${SHADOW_CFLAGS[@]}" \
      -nostdinc -isystem "$D/usr/include" \
      -isystem "$D/usr/include/x86_64-linux-gnu" \
      -isystem "$(gcc -print-file-name=include)" \
      -o "$OUT/RimWorldLinux.ad9" src/user/rwmain.c \
      "$ROOT/tools/rw/mono-probe/condtrace.c" \
      "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
      "$LDIR/libc.so.6" "$LDIR/libpthread-2.31.so" "$LDIR/libdl-2.31.so" "$DLIB/crtn.o" \
      -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
      -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none || {
    echo "AD-9 launcher link failed"; exit 2; }
  readelf -h "$OUT/RimWorldLinux.ad9" | awk '/Type:/{print "  ad9:",$2}'
  # Capture first: `readelf -sW | grep -q` under `set -o pipefail` fails when
  # grep exits early and readelf takes SIGPIPE (141) -- a correct launcher was
  # rejected once with "does not export dlopen".
  AD9_SYMS="$(readelf -sW "$OUT/RimWorldLinux.ad9")"
  grep -qw dlopen <<<"$AD9_SYMS" || {
    echo "AD-9 launcher does not export dlopen"; exit 2; }
fi

echo "== stage: glibc 2.31 runtime under /LIB64 (+ ld.so's default dir)"
mmd -i "$IMG" ::/LIB64 ::/lib ::/lib64 ::/lib/x86_64-linux-gnu ::/NFS \
    >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || { echo "mcopy failed $2"; exit 2; }; }
for f in ld-2.31.so:ld-linux-x86-64.so.2 libc-2.31.so:libc.so.6 libm-2.31.so:libm.so.6 \
         libpthread-2.31.so:libpthread.so.0 libdl-2.31.so:libdl.so.2 \
         librt-2.31.so:librt.so.1 libgcc_s.so.1:libgcc_s.so.1; do
  src="${f%%:*}"; dst="${f##*:}"
  stage "$LDIR/$src" "::/LIB64/$dst"
  stage "$LDIR/$src" "::/lib/x86_64-linux-gnu/$dst"
done
stage "$LDIR/ld-2.31.so" ::/lib64/ld-linux-x86-64.so.2

echo "== stage: compat window/audio DSOs (libX11.so.6, libasound.so.2)"
bash src/user/xcompat/stage.sh "$IMG" | sed 's/^/  /' || exit 2
if [ "${RW_TRACE_X11:-0}" = 1 ]; then
  stage "$ROOT/obj/xcompat/trace/libX11.so.6" ::/LIB64/libX11.so.6
  echo "  staged: traced libX11.so.6 over the shipped one (-DXC_TRACE)"
fi
echo "== stage: Mesa GL frontend (libGL.so.1, libGLX.so.0)"
bash tools/mesa/stage.sh "$IMG" | sed 's/^/  /' || {
  echo "ENVIRONMENT: Mesa DSOs missing — run bash tools/mesa/build-hobbyos.sh"; exit 2; }

if [ "${RW_SKIP_EXPORT:-0}" != 1 ]; then
  bash tools/rw/nfs-export.sh sync >"$OUT/export.log" 2>&1 || {
    echo "EXPORT FAILED"; tail -20 "$OUT/export.log"; exit 2; }
  tail -3 "$OUT/export.log"
fi
[ -x "$RW_EXPORT/RimWorldLinux" ] || {
  echo "ENVIRONMENT: $RW_EXPORT/RimWorldLinux missing"; exit 2; }

echo "== stage: AD-10 DSOs locally under /RWDSO"
mmd -i "$IMG" ::/RWDSO >/dev/null 2>&1 || true
for rel in UnityPlayer.so \
           RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so \
           RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmono-native.so \
           RimWorldLinux_Data/MonoBleedingEdge/x86_64/libMonoPosixHelper.so \
           RimWorldLinux_Data/Plugins/lib_burst_generated.so \
           RimWorldLinux_Data/Plugins/libsteam_api.so; do
  src="$RW_EXPORT/$rel"
  # L61: a probe build of the runtime (a byte-patched assert site) is staged
  # from RW_MONO_SO instead of the export, so the shared export is never
  # modified and the probe is named in the run's own output.
  case "$rel" in
    *libmonobdwgc-2.0.so) [ -n "${RW_MONO_SO:-}" ] && src="$RW_MONO_SO" ;;
  esac
  [ -f "$src" ] || { echo "ENVIRONMENT: missing $src"; exit 2; }
  stage "$src" "::/RWDSO/${rel##*/}"
done
mdir -i "$IMG" ::/RWDSO | sed 's/^/  /'

# Mono's etc/mono/config maps System.Native -> "$mono_libdir/libmono-native.so"
# and this runtime's mono_libdir is /usr/lib/../lib, i.e. /usr/lib/libmono-native.so.
# If that path does not exist, Mono's Interop.Sys type initializer throws
# DllNotFoundException on the first managed File.* call: every File.ReadAllBytes
# in the process fails, Unity cannot read ScriptingAssemblies.json, no engine
# assembly is loaded, every [UnityEngine.CoreModule.dll] type lookup returns
# NULL and Unity dies on mono_class_get_image(NULL).  Stage the player's own
# copies at the path the dllmap names (NOT /RWDSO: Mono asks by absolute path,
# and the AD-10 redirect only sees names that reach our dlopen).
mmd -i "$IMG" ::/usr ::/usr/lib >/dev/null 2>&1 || true
for so in libmono-native.so libMonoPosixHelper.so; do
  stage "$RW_EXPORT/RimWorldLinux_Data/MonoBleedingEdge/x86_64/$so" "::/usr/lib/$so"
done
echo "  staged: /usr/lib/libmono-native.so (System.Native dllmap target)"

# libmonobdwgc / libMonoPosixHelper NEED libz.so.1 (skill rule 17).
MZ="$ROOT/third_party/rw-mono/root/lib/libz.so.1"
if [ ! -e "$MZ" ]; then
  bash tools/rw/fetch-mono.sh >"$OUT/mono-fetch.log" 2>&1 || {
    echo "ENVIRONMENT: no libz.so.1 for the Mono DSOs"; exit 2; }
fi
MZ="$(readlink -f "$MZ")"
stage "$MZ" ::/LIB64/libz.so.1
stage "$MZ" ::/lib/x86_64-linux-gnu/libz.so.1

echo "== stage: M5c Managed/ locally under /RWMANAGED"
mmd -i "$IMG" ::/RWMANAGED >/dev/null 2>&1 || true
HOSTN=$(find "$RW_EXPORT"/RimWorldLinux_Data/Managed -maxdepth 1 -type f | wc -l)
HOSTB=$(find "$RW_EXPORT"/RimWorldLinux_Data/Managed -maxdepth 1 -type f -printf '%s\n' \
        | awk '{s+=$1} END{print s+0}')
[ "$HOSTN" -gt 0 ] || { echo "ENVIRONMENT: no Managed/ assemblies in the export"; exit 2; }
mcopy -o -i "$IMG" "$RW_EXPORT"/RimWorldLinux_Data/Managed/* ::/RWMANAGED/ || {
  echo "mcopy of Managed/ failed"; exit 2; }
GUESTN=$(mdir -b -i "$IMG" ::/RWMANAGED | wc -l)
GUESTB=$(mdir -i "$IMG" ::/RWMANAGED | awk '
  / files /{ on=0; s=""; for (i=1;i<=NF;i++) {
               if ($i=="files") { on=1; continue }
               if ($i=="bytes") { print s+0; exit }
               if (on) s = s $i } }')
echo "  staged: ${GUESTN:-0}/$HOSTN assemblies, ${GUESTB:-0}/$HOSTB bytes"
[ "${GUESTN:-0}" = "$HOSTN" ] && [ "${GUESTB:-0}" = "$HOSTB" ] || {
  echo "ENVIRONMENT: Managed/ staging incomplete (${GUESTN:-0}/$HOSTN files, ${GUESTB:-0}/$HOSTB bytes)"; exit 2; }

# AD-9: install the launcher AS the game's executable, in the game dir; the
# 14,720-byte ET_EXEC stub is preserved beside it as RimWorldLinux.orig.
if [ "$(stat -c %s "$RW_EXPORT/RimWorldLinux")" = 14720 ]; then
  cp -f "$RW_EXPORT/RimWorldLinux" "$RW_EXPORT/RimWorldLinux.orig"
fi
rm -f "$RW_EXPORT/RimWorldLinux" # break any stale hardlink, then install
cp -f "$OUT/RimWorldLinux.ad9" "$RW_EXPORT/RimWorldLinux"
# L63: no private alias is needed any more.  The export itself belongs to this
# worktree, so /NFS/RimWorldLinux is already this lane's binary (the .l61
# alias existed only because lanes shared one directory).
echo "  installed: $(stat -c '%s bytes' "$RW_EXPORT/RimWorldLinux") -> $RW_EXPORT/RimWorldLinux (lane $RW_LANE)"
# L63: tell the guest WHICH export this run owns.  rwlaunch prefixes this with
# the slirp host alias and mounts exactly that path
# (src/user_include/rwnfs_lane.h), so two lanes boot into two directories.
printf '%s\n' "$RW_EXPORT" >"$OUT/nfsexport"
stage "$OUT/nfsexport" ::/NFSEXPORT
echo "  staged: /NFSEXPORT -> $RW_EXPORT (lane $RW_LANE)"

# The desktop's Apps menu launches root entries by name; this is the launcher.
stage "$OUT/rwlaunch.bin" ::/RWLAUNCH.BIN

# L83: an icewm disk's /ICEWM/menu was generated by stage-disk.sh while the
# disk was BUILT -- before this harness staged RWLAUNCH.BIN -- so it has no
# RimWorld entry yet.  Regenerate it now, from this disk's own file listing:
# the launcher then sits at the top of Apps as `HBHOST.BIN RWLAUNCH.BIN`
# (hosted: it asks for a pixel window and only HBHOST answers) and is
# excluded from All programs.  Also kept in $OUT/icewm-menu for the driver.
if [ "$RW_DESKTOP" = icewm ]; then
  echo "== stage: icewm Start menu (/ICEWM/menu) with the RimWorld entry"
  MDIR_OUT="$OUT/icewm-menu"
  rm -rf "$MDIR_OUT" && mkdir -p "$MDIR_OUT"
  sh src/user/x11sess/gen-icewm-menu.sh "$IMG" src/user/desktop.c "$MDIR_OUT" \
    || { echo "icewm menu generation failed"; exit 2; }
  mdir -i "$IMG" ::/ICEWM >/dev/null 2>&1 || mmd -i "$IMG" ::/ICEWM
  stage "$MDIR_OUT/menu" ::/ICEWM/menu
  stage "$MDIR_OUT/toolbar" ::/ICEWM/toolbar
  if grep -q 'prog "RimWorld" app HBHOST.BIN RWLAUNCH.BIN' "$MDIR_OUT/menu"; then
    echo "  $(grep -n 'RimWorld' "$MDIR_OUT/menu" | head -1 | sed 's/^/menu:/')"
  else
    echo "STAGE FAILED: no RimWorld entry in the generated menu"; exit 2
  fi
fi
# Opt-in mutex lifetime tracer: the guest desktop starts the game, so no host
# environment reaches it.  rwlaunch reads this marker and emits
# RW_TRACE_MUTEX=1 in the child's env (0 otherwise).  Staged here, and REMOVED
# when not asked for, because disk.img is reused across runs.
if [ "${RW_TRACE_MUTEX:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/mtxon"
  stage "$OUT/mtxon" ::/MTXON
  echo "  staged: /MTXON -> RW_TRACE_MUTEX=1 in the game"
else
  mdel -i "$IMG" ::/MTXON >/dev/null 2>&1 || true
fi
# L42: /SIGQUITON makes rwlaunch send SIGQUIT to the game at 60 s, which is
# Mono's own full managed thread dump ("Full thread dump:" in Player.log).
# L61: /INTERP makes the launcher export MONO_ENV_OPTIONS=--interp, the
# interpreter control (same binary, same assemblies, no JIT codegen).
if [ "${RW_INTERP:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/interpon"
  stage "$OUT/interpon" ::/INTERP
  echo "  staged: /INTERP -> MONO_ENV_OPTIONS=--interp (interpreter control)"
else
  mdel -i "$IMG" ::/INTERP >/dev/null 2>&1 || true
fi
if [ "${RW_SIGQUIT:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/sigquiton"
  stage "$OUT/sigquiton" ::/SIGQUITON
  echo "  staged: /SIGQUITON -> SIGQUIT at 60 s (Mono managed thread dump)"
else
  mdel -i "$IMG" ::/SIGQUITON >/dev/null 2>&1 || true
fi
# L42: Mono's threadpool trace (worker park/unpark/create/timeout).
if [ "${RW_TPTRACE:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/tptrace"
  stage "$OUT/tptrace" ::/TPTRACE
  echo "  staged: /TPTRACE -> MONO_LOG_MASK=threadpool, level=debug"
else
  mdel -i "$IMG" ::/TPTRACE >/dev/null 2>&1 || true
fi
# L72: the launcher's own late-sampling probe (SIGUSR2 stack capture every
# 120 s from t=400 s) WITHOUT Mono's threadpool log: at ~112 futex ops/s that
# log would both drown the console and perturb the rate being measured.
if [ "${RW_TPLATE:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/tplate"
  stage "$OUT/tplate" ::/TPLATE
  echo "  staged: /TPLATE -> late stack sampler (no game-side logging)"
else
  mdel -i "$IMG" ::/TPLATE >/dev/null 2>&1 || true
fi
# L54: condvar flight recorder.  condtrace.c is linked into the launcher, so
# its pthread_cond_* wrappers win resolution for the game's DSOs; /CONDTRACE
# turns counting on.  Without the marker the wrappers are pass-through.
if [ "${RW_CONDTRACE:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/condtrace"
  stage "$OUT/condtrace" ::/CONDTRACE
  echo "  staged: /CONDTRACE -> condtrace counters + [CT2] park reports"
else
  mdel -i "$IMG" ::/CONDTRACE >/dev/null 2>&1 || true
fi
# L50: /TPPOKE makes the stall probe do ONE spurious Monitor.PulseAll on the
# ManualResetEventSlim the main thread is parked on (rule 24b).  Diagnostic
# only: a correct MRES consumer re-checks IsSet and returns.
if [ "${RW_TPPOKE:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/tppoke"
  stage "$OUT/tppoke" ::/TPPOKE
  echo "  staged: /TPPOKE -> one spurious PulseAll at each stall sample"
else
  mdel -i "$IMG" ::/TPPOKE >/dev/null 2>&1 || true
fi
# L42: ask Mono for cooperative thread suspension (thread dumps then avoid
# the async-suspend handshake that aborts on the Finalizer).
if [ "${RW_COOPSUSPEND:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/coopsuspend"
  stage "$OUT/coopsuspend" ::/COOPSUSPEND
  echo "  staged: /COOPSUSPEND -> MONO_THREADS_SUSPEND=coop"
else
  mdel -i "$IMG" ::/COOPSUSPEND >/dev/null 2>&1 || true
fi
# L42: GC_DONT_GC=1 A/B: does the park survive with Boehm not collecting?
if [ "${RW_NOGC:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/nogc"
  stage "$OUT/nogc" ::/NOGC
  echo "  staged: /NOGC -> GC_DONT_GC=1"
else
  mdel -i "$IMG" ::/NOGC >/dev/null 2>&1 || true
fi
# L42: A/B the Mono threadpool's worker ceiling (min/max = cpu*threads_per_cpu).
if [ "${RW_TPCPU:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/tpcpu"
  stage "$OUT/tpcpu" ::/TPCPU
  echo "  staged: /TPCPU -> MONO_THREADS_PER_CPU=50"
else
  mdel -i "$IMG" ::/TPCPU >/dev/null 2>&1 || true
fi
# Opt-in GLX-level trace (tools/mesa/patches/0005): /GLXON makes rwlaunch
# export HOBBYOS_GLX_TRACE=1, so the serial log carries every glX* request
# Unity made and what the Mesa xlib frontend answered.
if [ "${RW_TRACE_GLX:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/glxon"
  stage "$OUT/glxon" ::/GLXON
  echo "  staged: /GLXON -> HOBBYOS_GLX_TRACE=1 in the game"
else
  mdel -i "$IMG" ::/GLXON >/dev/null 2>&1 || true
fi
# L74: /VPON -> HOBBYOS_GL_VIEWPORT=1, the [GL] viewport rows (patches/0012).
# Independent of /GLXON because the GLX call trace prints a row per call and
# would perturb the fps it is measuring; the viewport row is bounded.
if [ "${RW_VIEWPORT:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/vpon"
  stage "$OUT/vpon" ::/VPON
  echo "  staged: /VPON -> HOBBYOS_GL_VIEWPORT=1 ([GL] viewport rows)"
else
  mdel -i "$IMG" ::/VPON >/dev/null 2>&1 || true
fi
# L62: /RWSCREEN makes the launcher ask the WM for a specific pixel size
# ("WxH"), and /WMSIZE makes the WM honour that request instead of handing the
# client the whole tile.  Both are measurement switches: absent/off, the
# launcher asks 1280x720 and the WM tiles as it always has.  The "off" content
# matters: this image has no mdel(1), so a marker is rewritten, never removed.
if [ -n "${RW_SCREEN:-}" ]; then
  printf '%s\n' "$RW_SCREEN" >"$OUT/rwscreen"
  echo "  staged: /RWSCREEN -> -screen-width/-screen-height $RW_SCREEN"
else
  printf 'off\n' >"$OUT/rwscreen"
fi
stage "$OUT/rwscreen" ::/RWSCREEN
if [ "${RW_WM_HONOUR:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/wmsize"
  echo "  staged: /WMSIZE -> the WM honours the pixel-mode content size"
else
  printf 'off\n' >"$OUT/wmsize"
fi
stage "$OUT/wmsize" ::/WMSIZE
# M3: which GL driver and which present path the game gets.  Both are files
# because a desktop-menu launch cannot pass an environment: /GLMODE is read by
# the DSO's xlib_create_screen() (the M2 switch) and /GLDMG by the virgl
# winsys (M3: 0 = full-surface readback, 1 = damage-rect).  Content-gated and
# rewritten every run -- this image has no mdel(1), so a marker is never
# removed, only rewritten.  Defaults reproduce the pre-M3 behaviour
# (softpipe, damage on for a virgl run).
printf '%s\n' "${RW_GLMODE:-softpipe}" >"$OUT/glmode"
stage "$OUT/glmode" ::/GLMODE
printf '%s\n' "${RW_GLDMG:-1}" >"$OUT/gldmg"
stage "$OUT/gldmg" ::/GLDMG
echo "  staged: /GLMODE -> ${RW_GLMODE:-softpipe}, /GLDMG -> ${RW_GLDMG:-1}"
# L80: the per-present budget.  /XPROF makes the compat X libX11 print one
# [XSWP] row per present (frame period, time inside glXSwapBuffers, and the
# gap between swaps); /GLPROF makes the virgl winsys print one [VGLP] row per
# present (submits/readbacks/uploads and their bytes, and the timed stages).
# One row per present is a measurement switch like /GLDMG, so it is staged the
# same way and off by default.
if [ "${RW_PRESENT_PROF:-0}" = 1 ]; then
  printf 'on\n' >"$OUT/xprof"
  stage "$OUT/xprof" ::/XPROF
  printf 'on\n' >"$OUT/glprof"
  stage "$OUT/glprof" ::/GLPROF
  echo "  staged: /XPROF + /GLPROF -> one [XSWP] and [VGLP] row per present"
else
  mdel -i "$IMG" ::/XPROF >/dev/null 2>&1 || true
  mdel -i "$IMG" ::/GLPROF >/dev/null 2>&1 || true
fi
# L75: the block-stress probe ("2 x 1800 s with the probe running alongside").
# RW_CHURN_SOAK=1 compiles the KERNEL-side stressor (RW_BLK_STRESS=1): random
# 512..4096-byte reads over a fixed file region plus small writes to a scratch
# LBA window, driven from the 5 s sampler, so it runs next to the game for the
# whole window through the same driver and cache.  Kernel-side on purpose: a
# second user process would have to share the desktop window-protocol pipes,
# and that starves the game's own first window request (measured).
if [ "${RW_CHURN_SOAK:-0}" = 1 ]; then
  echo "  block-stress probe: kernel-side (RW_BLK_STRESS=1)"
fi
# No WAVE.CFG: MODE=desktop has no boot-wave loader (KERNEL_MODE_TEST only),
# and the game must be started from the desktop so it inherits the window
# protocol pipes.

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || { echo "ENVIRONMENT: no OVMF_CODE_4M.fd"; exit 2; }
OVMF="$OUT/OVMF_CODE_4M.fd"
[ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

# M3: RW_QEMU_GL=1 boots the GL device (virtio-gpu-gl-pci + egl-headless,gl=on)
# instead of the 2D one, which is what a virgl client needs.  The Makefile
# emits the -display itself in that case (QEMU_GPU=gl), so QEMU_ARGS must not
# carry a second `-display none` -- that override is what pins every other
# mode to the 2D device.  QMP's screendump has no surface on egl-headless
# (P9), so screenshots come from guest physical memory instead
# (--fb-phys in rw_desktop_drive.py).
GLVARS=""
DISPARGS="-display none"
if [ "${RW_QEMU_GL:-0}" = 1 ]; then
  GLVARS="QEMU_GPU=gl QEMU_DISPLAY=egl-headless"
  DISPARGS=""
  echo "== boot: GL device (virtio-gpu-gl-pci, egl-headless,gl=on)"
fi
# The kernel lives INSIDE disk.img (::/boot/hobbyos.elf) and the boot below
# passes `-o disk.img`, so a rebuilt hobbyos.elf would never reach the guest --
# a kernel fix would look like a no-op in every run.  Re-stage it whenever it
# is newer than the image.
# Always: disk.img is rewritten by staging, so its mtime says nothing about
# which kernel is inside it (measured: a kernel fix silently did not boot).
llvm-objcopy --strip-debug "$ROOT/hobbyos.elf" "$OUT/hobbyos.boot.elf"
mcopy -o -i "$IMG" "$OUT/hobbyos.boot.elf" ::/boot/hobbyos.elf
echo "== kernel: staged $(stat -c %s "$OUT/hobbyos.boot.elf") bytes as ::/boot/hobbyos.elf"
echo "== boot: MODE=desktop DESKTOP=$RW_DESKTOP (deadline ${TIMEOUT_S}s, log $LOG)"
: >"$LOG"
MAKEEXTRA=""

( make -o disk.img ARCH=intel MODE=desktop DESKTOP="$RW_DESKTOP" $X11VAR EDK2_X86_64="$OVMF" \
       $GLVARS $MAKEEXTRA run QEMU_ARGS="$DISPARGS -enable-kvm -qmp unix:$QMP,server,nowait ${RW_QEMU_EXTRA:-}" \
       >>"$LOG" 2>&1 & )

RC=1
FBARGS=""
if [ "${RW_QEMU_GL:-0}" = 1 ]; then
  # The framebuffer's guest-physical address: the kernel's 2 MB-aligned BSS
  # array `framebuffer` (src/kernel/virtio_gpu.c), identity-mapped, so the
  # ELF symbol's address is its physical address.
  FBP=$(nm "$ROOT/hobbyos.elf" 2>/dev/null | awk '$3=="framebuffer"{print "0x"$1}')
  [ -n "$FBP" ] || FBP="${RW_FB_PHYS:-0x74c00000}"
  FBARGS="--fb-phys $FBP --fb-w 1920 --fb-h 1080"
  echo "== screenshots: /dev/fb0 from guest physical memory at $FBP"
fi
if [ "$RW_DESKTOP" = icewm ]; then
  # No DESKTOP.BIN menu and no [MENU] dump: the driver waits for XSESSION,
  # finds the taskbar in a screenshot, then clicks Start -> Apps -> <label>.
  DRIVE_ARGS=(--desktop icewm --launch-via "$RW_LAUNCH_VIA"
              --icewm-launch "${RW_ICEWM_LAUNCH:-RimWorld}"
              --menu-file "$OUT/icewm-menu/menu")
else
  DRIVE_ARGS=(--desktop hobby --launch-via "$RW_LAUNCH_VIA"
              --app "${RW_APP:-RWLAUNCH.BIN}")
fi
if python3 tools/rw/rw_desktop_drive.py --log "$LOG" --qmp "$QMP" \
        --shot-dir "$SHOT_DIR" "${DRIVE_ARGS[@]}" \
        --boot-timeout "$TIMEOUT_S" --run-timeout "$RUN_TIMEOUT_S" \
        --shot-every "${RW_SHOT_EVERY:-8}" $FBARGS; then
  RC=0
fi

echo
echo "== Player.log (extracted from disk.img)"
mdir -i "$IMG" ::/RW_HOME >/dev/null 2>&1 && \
  mcopy -o -i "$IMG" ::/RW_HOME/Player.log "$OUT/Player.log" 2>/dev/null && {
    echo "  $OUT/Player.log ($(stat -c %s "$OUT/Player.log") bytes)"
    echo "  ---"
    cat "$OUT/Player.log"
    echo "  ---"
  } || echo "  (no /RW_HOME/Player.log: the engine never opened it)"

# The launcher's own mutex-lifetime log (RW_TRACE_MUTEX): init/destroy of every
# lock plus every op inside the parked arena.  Only present when the marker was
# staged, so a normal run does not spend disk space on it.
mdir -i "$IMG" ::/RW_HOME >/dev/null 2>&1 && \
  mcopy -o -i "$IMG" ::/RW_HOME/mtx.log "$OUT/mtx.log" 2>/dev/null && {
    echo "  $OUT/mtx.log ($(stat -c %s "$OUT/mtx.log") bytes)"
  } || true

echo
echo "== the probe's own log (/TPLATE)"
mdir -i "$IMG" ::/RW_HOME >/dev/null 2>&1 && \
  mcopy -o -i "$IMG" ::/RW_HOME/l72.log "$OUT/l72.log" 2>/dev/null && \
  echo "  $OUT/l72.log ($(stat -c %s "$OUT/l72.log") bytes)" || true

echo
echo "== ordered ladder (raw serial lines, in order)"
grep -anE '\[RW\]|\[RWMAIN\]|\[HBHOST\]|\[XSESS\]|\[LCOMPAT\]|Loading program|PT_INTERP|Initialize engine|GfxDevice|GL_VERSION|GL_RENDERER|Mono path|Mono config|error while loading|failed to map|cannot open shared|undefined symbol|Video subsystem|Unable to load|Failed to load|mono library|\[LAUNCH\]|memory fault|FATAL|\[ALIVE-SET\]|\[IDLESTUCK\]|System halt' \
     "$LOG" || true

echo
echo "raw serial:  $LOG"
echo "screenshots: $SHOT_DIR (00_desktop, running shots, 99_final)"
echo "Player.log:  $OUT/Player.log"

fuser -k "$IMG" >/dev/null 2>&1
exit "$RC"
