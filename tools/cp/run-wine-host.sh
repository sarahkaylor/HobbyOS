#!/usr/bin/env bash
# tools/cp/run-wine-host.sh — run the staged Wine (tools/cp/build-wine.sh) on the
# Linux host against the PINNED glibc 2.31 runtime, and nothing else.
#
# HOW THE PIN IS SCOPED TO THE PROCESS (docs/rimworld-plan.md Appendix B
# pitfall: never export the pinned libc into a shell).  RimWorld patched
# PT_INTERP on a copy and scoped LD_LIBRARY_PATH to one process; that does not
# work for Wine, which re-executes itself (wine -> wine-preloader -> ld.so ->
# wine) and spawns wineserver, services.exe, winedevice.exe, ... by path, each
# with PT_INTERP=/lib64/ld-linux-x86-64.so.2 — the host's 2.4x loader, which
# would then be handed 2.31's libc.so.6 by LD_LIBRARY_PATH.  So the whole Wine
# process tree runs in a CHROOT whose root holds exactly:
#
#   /lib64/ld-linux-x86-64.so.2, /lib/x86_64-linux-gnu/*   libc6 2.31 + libgcc-s1 10.5
#   /CP/WINE/...                                           the staged Wine (F4 layout)
#   /tmp (fresh per run unless --warm), /dev, /proc        bind/proc mounts
#   /etc/passwd, /etc/group                                one user (uid of the caller)
#
# i.e. the same file set HobbyOS will hold, with no host library reachable.
# The mounts live in a private mount namespace (sudo unshare -m), so nothing
# leaks into the host; the Wine processes run as the calling user (chroot
# --userspec), never as root.
#
# Usage:
#   bash tools/cp/run-wine-host.sh [--warm] [--strace OUT] [--strace-c OUT] -- <wine args...>
#     e.g.  -- wineboot -i
#           -- wine cmd /c echo hello
#   --warm         keep $ROOTFS/tmp (the prefix) from the previous run
#   --strace OUT   strace -f -tt -T -yy -s 256 (raw, every process) -> OUT
#   --strace-c OUT strace -f -c -w (histogram over every process) -> OUT
#   --maps DIR     while running, snapshot /proc/<pid>/maps of every Wine process
#                  into DIR once, after --maps-delay seconds (default 4)
# Env: CP_WINE_WORK (default $HOME/cp/wine-build), WINEDEBUG (default -all)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RT="${CP_RUNTIME:-$ROOT/third_party/cp-runtime}"
WORK="${CP_WINE_WORK:-$HOME/cp/wine-build}"
STAGE="$WORK/stage"
ROOTFS="$WORK/hostroot"
WARM=0 STRACE="" STRACEC="" MAPS="" MAPS_DELAY=4
while [ $# -gt 0 ]; do
  case "$1" in
    --warm) WARM=1; shift ;;
    --strace) STRACE="$2"; shift 2 ;;
    --strace-c) STRACEC="$2"; shift 2 ;;
    --maps) MAPS="$2"; shift 2 ;;
    --maps-delay) MAPS_DELAY="$2"; shift 2 ;;
    --) shift; break ;;
    *) break ;;
  esac
done
[ $# -gt 0 ] || { echo "usage: $0 [opts] -- wine|wineboot|wineserver args..." >&2; exit 2; }
[ -x "$STAGE/CP/WINE/bin/wine" ] || { echo "run-wine-host: no staged Wine; run tools/cp/build-wine.sh" >&2; exit 2; }

say() { printf 'run-wine-host: %s\n' "$*" >&2; }
uid=$(id -u) gid=$(id -g)

# --- root filesystem (rebuilt when the stage manifest changes) ----------------
stamp="$(sha256sum "$WORK/stage-manifest.sha256" | cut -c1-16)"
if [ "$(cat "$ROOTFS/.stamp" 2>/dev/null)" != "$stamp" ]; then
  rm -rf "$ROOTFS"; mkdir -p "$ROOTFS"/{tmp,dev,proc,etc,CP}
  dpkg-deb -x "$RT/dl/libc6_2.31-0ubuntu9.18_amd64.deb" "$ROOTFS"
  dpkg-deb -x "$RT/dl/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb" "$ROOTFS"
  rm -rf "$ROOTFS/usr/share" "$ROOTFS/etc/ld.so.conf.d" "$ROOTFS/usr/lib/x86_64-linux-gnu/gconv"
  # absolute symlinks (lib64/ld-linux-x86-64.so.2 -> /lib/...) are correct
  # INSIDE the chroot; leave them.
  cp -a "$STAGE/CP/WINE" "$ROOTFS/CP/WINE"
  printf 'cp:x:%s:%s:cp:/tmp/home:/bin/false\n' "$uid" "$gid" > "$ROOTFS/etc/passwd"
  printf 'cp:x:%s:\n' "$gid" > "$ROOTFS/etc/group"
  echo "$stamp" > "$ROOTFS/.stamp"
  say "rootfs rebuilt: $ROOTFS ($(du -sh "$ROOTFS" | cut -f1))"
fi
if [ "$WARM" = 0 ]; then
  rm -rf "$ROOTFS/tmp"; mkdir -p "$ROOTFS/tmp"
fi
chmod 1777 "$ROOTFS/tmp"; mkdir -p "$ROOTFS/tmp/home"; chown "$uid:$gid" "$ROOTFS/tmp/home"

prog="$1"; shift
case "$prog" in
  wine|wineboot|wineserver|winepath|wineconsole) exe="/CP/WINE/bin/$prog" ;;
  /*) exe="$prog" ;;
  *) echo "run-wine-host: unknown program $prog" >&2; exit 2 ;;
esac

tracer=()
[ -n "$STRACE" ]  && tracer=(strace -f -tt -T -yy -s 256 -o "$STRACE")
[ -n "$STRACEC" ] && tracer=(strace -f -c -w -U name,calls,errors,time-total,time-avg -o "$STRACEC")

envv=(env -i HOME=/tmp/home USER=cp LOGNAME=cp WINEPREFIX=/tmp/pfx
      WINEDEBUG="${WINEDEBUG:--all}" PATH=/CP/WINE/bin LANG=C TERM=dumb)
# argv is passed through `sudo sh -c` as positional parameters (no re-quoting).
inner='R=$1; shift
mount --rbind /dev "$R/dev"
mount -t proc proc "$R/proc"
exec "$@"'

maps_pid=""
if [ -n "$MAPS" ]; then
  mkdir -p "$MAPS"
  ( sleep "$MAPS_DELAY"
    for p in $(pgrep -u "$uid" -f '^(/CP/WINE|[A-Za-z]:\\|.*\.exe)' || true); do
      n=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | cut -c1-80 || true)
      [ -n "$n" ] || continue
      safe=$(printf '%s' "$n" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-60)
      { echo "# pid $p cmdline: $n"; cat "/proc/$p/maps"; } > "$MAPS/$p-$safe.maps" 2>/dev/null || true
    done ) &
  maps_pid=$!
fi

# A wineserver lingers ~3 s after its last client; a measurement must start
# (and trace) its own, so wait for any previous one from this rootfs to go.
pkill -u "$uid" -f '^/CP/WINE/bin/wineserver' 2>/dev/null || true
for _ in $(seq 50); do
  pgrep -u "$uid" -f '^/CP/WINE/bin/wineserver' >/dev/null || break; sleep 0.2
done

set +e
sudo unshare -m --propagation private sh -c "$inner" sh "$ROOTFS" \
  "${tracer[@]}" "${envv[@]}" "$(command -v chroot)" --userspec="$uid:$gid" "$ROOTFS" "$exe" "$@"
rc=$?
set -e
[ -n "$maps_pid" ] && wait "$maps_pid" 2>/dev/null || true
[ -n "$STRACE" ]  && sudo chown "$uid:$gid" "$STRACE" 2>/dev/null || true
[ -n "$STRACEC" ] && sudo chown "$uid:$gid" "$STRACEC" 2>/dev/null || true
say "exit status $rc"
exit "$rc"
