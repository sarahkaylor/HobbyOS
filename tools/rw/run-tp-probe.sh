#!/usr/bin/env bash
# tools/rw/run-tp-probe.sh — run a managed probe under the GAME's own Mono
# runtime (MonoBleedingEdge/libmonobdwgc-2.0.so) in a MODE=test boot wave.
#
# Why not the official mono-sgen: that build is monolithic and hides four
# mono_* symbols that libmono-native.so imports, so System.Native fails to load
# and EVERY managed ThreadPool worker dies in
# ThreadPoolWorkQueueThreadLocals..ctor -> new Random() -> Interop.Sys on the
# HOST as well.  A probe under mono-sgen therefore reports "the ThreadPool is
# dead" on any kernel; it cannot separate a guest defect from a probe artefact.
#
# This script drives libmonobdwgc-2.0.so through tools/rw/mono-probe/
# mono_main_launcher.c, i.e. the exact runtime the game runs, so a host run and
# a guest run of the same bytes are a real differential.
#
# Usage: tools/rw/run-tp-probe.sh [--timeout S] [--exe FILE] [--log NAME]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/rw/lane.sh
. "$ROOT/tools/rw/lane.sh"
cd "$ROOT" || exit 2
TIMEOUT_S=180
EXE="$ROOT/obj/tp-probe/tp.exe"
LOGNAME=serial-tp.log
TPTRACE=0
CONDTRACE=0
TPMIN8=0
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT_S="$2"; shift 2;;
    --exe) EXE="$2"; shift 2;;
    --log) LOGNAME="$2"; shift 2;;
    --tptrace) TPTRACE=1; shift 1;;
    --condtrace) CONDTRACE=1; shift 1;;
    --tpmin8) TPMIN8=1; shift 1;;
    *) echo "unknown arg $1"; exit 2;;
  esac
done
OUT="$ROOT/obj/tp-probe"
IMG="$ROOT/disk.img"
LOG="$OUT/$LOGNAME"
mkdir -p "$OUT"

MB="$RW_EXPORT"/RimWorldLinux_Data/MonoBleedingEdge
MAN="$RW_EXPORT"/RimWorldLinux_Data/Managed
G="${GLIBC231:-/tmp/glibc231}"
LDIR="$G/lib/x86_64-linux-gnu"
DLIB2="${GLIBC231_DEV:-/tmp/glibc231-dev}/usr/lib/x86_64-linux-gnu"
[ -f "$LDIR/ld-2.31.so" ] || { echo "no glibc 2.31 pin at $LDIR"; exit 2; }
[ -f "$MB/x86_64/libmonobdwgc-2.0.so" ] || { echo "no $MB/x86_64/libmonobdwgc-2.0.so"; exit 2; }
[ -f "$EXE" ] || { echo "no managed exe at $EXE"; exit 2; }

echo "== build: launcher (pinned 2.31) + kernel/disk (MODE=test)"
LAUNCH_SRC="$ROOT/tools/rw/mono-probe/mono_main_launcher.c"
LAUNCH_EXTRA=""
if [ "$CONDTRACE" = 1 ]; then
  # condtrace.c interposes pthread_cond_* for the DSOs the launcher dlopens
  # (rule 28: a -rdynamic launcher wins resolution).  A version script is
  # mandatory: a full -rdynamic export makes Boehm's
  # GC_init_linux_data_start die with "Wrong __data_start/_end pair".
  printf '{ global: pthread_cond_wait; pthread_cond_timedwait; pthread_cond_signal; pthread_cond_broadcast; ct_init; local: *; };\n' >"$OUT/ct.ver"
  LAUNCH_SRC="$LAUNCH_SRC $ROOT/tools/rw/mono-probe/condtrace.c"
  LAUNCH_EXTRA="-rdynamic -Wl,--version-script=$OUT/ct.ver $LDIR/libpthread-2.31.so"
fi
gcc -O1 -fPIE -pie -nostdlib -nostartfiles -o "$OUT/mono-launch" \
    $LAUNCH_SRC \
    "$DLIB2/Scrt1.o" "$DLIB2/crti.o" "$DLIB2/libc_nonshared.a" \
    "$LDIR/libc-2.31.so" "$LDIR/libdl-2.31.so" "$DLIB2/crtn.o" \
    $LAUNCH_EXTRA \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none 2>"$OUT/launcher-build.log" || {
      echo "LAUNCHER BUILD FAILED"; tail -20 "$OUT/launcher-build.log"; exit 2; }
TOOLS_SKIP=1 make -j8 ARCH=intel MODE=test hobbyos.elf disk.img \
  >"$OUT/build.log" 2>&1 || { echo "BUILD FAILED"; tail -20 "$OUT/build.log"; exit 2; }

echo "== stage: glibc + game Mono runtime + mscorlib + probe"
mkd() { mmd -i "$IMG" "$1" >/dev/null 2>&1 || true; }
for d in ::/lib64 ::/lib ::/lib/x86_64-linux-gnu ::/LIB64 ::/usr ::/usr/lib \
         ::/usr/lib/mono ::/usr/lib/mono/4.5 ::/etc ::/etc/mono ::/etc/mono/4.5; do mkd "$d"; done
stage() { mcopy -o -i "$IMG" "$1" "$2" || { echo "mcopy failed $2"; exit 2; }; }
for f in ld-2.31.so:ld-linux-x86-64.so.2 libc-2.31.so:libc.so.6 libm-2.31.so:libm.so.6 \
         libpthread-2.31.so:libpthread.so.0 libdl-2.31.so:libdl.so.2 \
         librt-2.31.so:librt.so.1 libgcc_s.so.1:libgcc_s.so.1; do
  src="${f%%:*}"; dst="${f##*:}"
  stage "$LDIR/$src" "::/lib/x86_64-linux-gnu/$dst"
  stage "$LDIR/$src" "::/LIB64/$dst"
done
stage "$LDIR/ld-2.31.so" ::/lib64/ld-linux-x86-64.so.2
# MonoBleedingEdge: the runtime DSO, libmono-native (System.Native -> $mono_libdir,
# which this runtime expands to /usr/lib/../lib = /usr/lib), posix helper, zlib.
for so in libmonobdwgc-2.0.so libmono-native.so libMonoPosixHelper.so; do
  stage "$MB/x86_64/$so" "::/usr/lib/$so"
  stage "$MB/x86_64/$so" "::/LIB64/$so"
done
LZ="$(ls "$ROOT"/third_party/rw-mono/root/lib/libz.so.1.* 2>/dev/null | head -1)"
[ -n "$LZ" ] || { echo "no libz.so.1 in third_party/rw-mono/root/lib"; exit 2; }
stage "$LZ" ::/usr/lib/libz.so.1
stage "$LZ" ::/LIB64/libz.so.1
# The game's own config (dllmap) and its own mscorlib: $mono_libdir -> /usr/lib.
# $mono_libdir in this runtime is the Unity build prefix
# (/home/bokken/build/output/Unity-Technologies/mono/tmp/lib/../lib), which is
# not a path a FAT16 image can carry (long-name dirs) and does not exist
# outside the player.  The player resolves it through the AD-9 dlopen redirect;
# this bare launcher gets the same file via an explicit dllmap target, so the
# staged config is the game's own with only that target rewritten to the
# absolute /usr/lib path where both SOs are staged.
sed -e 's|\$mono_libdir/libmono-native.so|/usr/lib/libmono-native.so|g' \
    -e 's|\$mono_libdir/libMonoPosixHelper.so|/usr/lib/libMonoPosixHelper.so|g' \
    "$MB/etc/mono/config" >"$OUT/MONO.CONFIG"
stage "$OUT/MONO.CONFIG" ::/MONO.CONFIG
stage "$MAN/mscorlib.dll" ::/usr/lib/mono/4.5/mscorlib.dll
# LINQ/PLINQ probes need System.Core staged as an assembly, not just as a
# reference inside the probe.
stage "$MAN/System.Core.dll" ::/usr/lib/mono/4.5/System.Core.dll
# System.Core's PLINQ sort helpers reference System.dll types, so a probe that
# touches ParallelEnumerable needs it as a real staged assembly (missing ->
# TypeLoadException on SortHelper`2._sharedBarriers, before any query runs).
stage "$MAN/System.dll" ::/usr/lib/mono/4.5/System.dll
stage "$EXE" ::/TP.EXE
stage "$OUT/mono-launch" ::/MONO.EXE
printf 'MONO.EXE --config /MONO.CONFIG /TP.EXE\n' >"$OUT/WAVE.CFG"
stage "$OUT/WAVE.CFG" ::/WAVE.CFG
# /TPTRACE: the probe launcher turns on MONO_LOG_MASK=threadpool for it, so the
# log carries worker starting/parking/unparking/timed-out rows.
if [ "$TPTRACE" = 1 ]; then : >"$OUT/tptrace"; stage "$OUT/tptrace" ::/TPTRACE; fi
if [ "$CONDTRACE" = 1 ]; then : >"$OUT/condtrace"; stage "$OUT/condtrace" ::/CONDTRACE; fi
if [ "$TPMIN8" = 1 ]; then : >"$OUT/tpmin8"; stage "$OUT/tpmin8" ::/TPMIN8; fi

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || { echo "no OVMF_CODE_4M.fd"; exit 2; }
OVMF="$OUT/OVMF_CODE_4M.fd"
[ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

echo "== boot (deadline ${TIMEOUT_S}s, log $LOG)"
: >"$LOG"
( make -o disk.img ARCH=intel MODE=test EDK2_X86_64="$OVMF" run \
       QEMU_ARGS="-display none -enable-kvm" >>"$LOG" 2>&1 & )
for _ in $(seq 1 "$TIMEOUT_S"); do
  # Stop as soon as the probe has said everything it is going to say: the
  # managed process may not exit (Mono's shutdown joins the finalizer), and a
  # gate that waits for "System halt" would add minutes per run for nothing.
  grep -aq "TP-OK\|TP TIMEOUT\|STARVE-OK\|STARVE outer TIMEOUT\|System halt" "$LOG" && break
  sleep 1
done
grep -aq "System halt" "$LOG" || {
  echo "probe done or deadline hit — stopping this run's QEMU"
  fuser -k "$IMG" >/dev/null 2>&1 || true
}
sleep 1
echo "--- probe rows ---"
grep -aE "TP |TP-OK|STARVE|Exception|DllNotFound|memory fault|System halt|\[LCOMPAT\]|\[GCT\]|\[GCST\]" "$LOG" | head -80 || echo "(none)"
fuser -k "$IMG" >/dev/null 2>&1
exit 0
