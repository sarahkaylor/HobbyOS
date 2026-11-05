#!/usr/bin/env bash
# tools/rw/run-mono-probe.sh — run a managed probe under the guest's own
# Mono 6.12 (MODE=test, boot wave) with a VFS-local /RWMANAGED copy of the
# assemblies the engine fails to resolve.
#
# Why MODE=test and not the desktop harness: this asks a question about the
# *managed runtime*, not about the window, and a wave process needs no X
# session.  It does mean this script owns disk.img; re-run the desktop harness
# afterwards (skill rule 5: never trust an image a gate left behind).
#
# Usage: tools/rw/run-mono-probe.sh [--timeout S]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
TIMEOUT_S=300
[ "${1:-}" = "--timeout" ] && TIMEOUT_S="$2"
OUT="$ROOT/obj/mono-probe"
IMG="$ROOT/disk.img"
LOG="$OUT/serial-probe.log"
mkdir -p "$OUT"

G="${GLIBC231:-/tmp/glibc231}"
LDIR="$G/lib/x86_64-linux-gnu"
M="$ROOT/third_party/rw-mono/root"
[ -x "$M/usr/bin/mono-sgen" ] || { echo "no Mono tree at $M (run tools/rw/fetch-mono.sh)"; exit 2; }
[ -f "$LDIR/libc-2.31.so" ] || { echo "no glibc 2.31 pin at $LDIR (run m1 gate once)"; exit 2; }
[ -f "$OUT/probe.exe" ] || { echo "no $OUT/probe.exe (compile tools/rw/mono-probe/probe.cs)"; exit 2; }

echo "== build: kernel + disk (MODE=test)"
TOOLS_SKIP=1 make -j8 ARCH=intel MODE=test hobbyos.elf disk.img \
  >"$OUT/build.log" 2>&1 || { echo "BUILD FAILED"; tail -20 "$OUT/build.log"; exit 2; }

echo "== stage: glibc + mono + probe + /RWMANAGED"
mkd() { mmd -i "$IMG" "$1" >/dev/null 2>&1 || true; }
for d in ::/lib64 ::/lib ::/lib/x86_64-linux-gnu ::/LIB64 ::/usr ::/usr/bin \
         ::/usr/lib ::/usr/lib/mono ::/usr/lib/mono/4.5 ::/etc ::/etc/mono \
         ::/etc/mono/4.5 ::/RWMANAGED; do mkd "$d"; done
stage() { mcopy -o -i "$IMG" "$1" "$2" || { echo "mcopy failed $2"; exit 2; }; }
for f in ld-2.31.so:ld-linux-x86-64.so.2 libc-2.31.so:libc.so.6 libm-2.31.so:libm.so.6 \
         libpthread-2.31.so:libpthread.so.0 libdl-2.31.so:libdl.so.2 \
         librt-2.31.so:librt.so.1 libgcc_s.so.1:libgcc_s.so.1; do
  src="${f%%:*}"; dst="${f##*:}"
  stage "$LDIR/$src" "::/lib/x86_64-linux-gnu/$dst"
  stage "$LDIR/$src" "::/LIB64/$dst"
done
stage "$LDIR/ld-2.31.so" ::/lib64/ld-linux-x86-64.so.2
LS="$(ls "$M"/lib/libstdc++.so.6.* 2>/dev/null | head -1)"
LZ="$(ls "$M"/lib/libz.so.1.* 2>/dev/null | head -1)"
stage "$LS" ::/lib/x86_64-linux-gnu/libstdc++.so.6
stage "$LZ" ::/lib/x86_64-linux-gnu/libz.so.1
stage "$LS" ::/LIB64/libstdc++.so.6
stage "$LZ" ::/LIB64/libz.so.1
stage "$M/usr/bin/mono-sgen" ::/MONO.EXE
# The Mono config's dllmap for System.Native expands to
# "$mono_libdir/libmono-native.so" and this runtime's mono_libdir is
# /usr/lib/../lib (= /usr/lib).  Without a real file there, every managed
# File.* call throws TypeInitializationException for 'Sys'.  Stage the
# player's own copies at the path the dllmap names.
for d in ::/usr ::/usr/lib; do mkd "$d"; done
for so in libmono-native.so libMonoPosixHelper.so libmonobdwgc-2.0.so; do
  stage "/srv/nfs/rimworld/RimWorldLinux_Data/MonoBleedingEdge/x86_64/$so" "::/usr/lib/$so"
done
for f in "$M/usr/lib/mono/4.5"/*.dll; do stage "$f" "::/usr/lib/mono/4.5/${f##*/}"; done
[ -f "$M/etc/mono/config" ] && stage "$M/etc/mono/config" ::/etc/mono/config
for f in "$M"/etc/mono/4.5/*.config; do [ -f "$f" ] && stage "$f" "::/etc/mono/4.5/${f##*/}"; done
stage "$OUT/probe.exe" ::/PROBE.EXE
for a in UnityEngine.CoreModule.dll Assembly-CSharp.dll; do
  stage "/srv/nfs/rimworld/RimWorldLinux_Data/Managed/$a" "::/RWMANAGED/$a"
done
printf 'MONO.EXE /PROBE.EXE\n' >"$OUT/WAVE.CFG"
stage "$OUT/WAVE.CFG" ::/WAVE.CFG

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
  grep -aq "System halt" "$LOG" && break
  sleep 1
done
grep -aq "System halt" "$LOG" || {
  echo "deadline hit without 'System halt' — stopping this run's QEMU"
  fuser -k "$IMG" >/dev/null 2>&1 || true
}
sleep 1
echo "--- probe rows ---"
grep -aE "PROBE |\[LCOMPAT\]|memory fault|System halt" "$LOG" || echo "(none)"
fuser -k "$IMG" >/dev/null 2>&1
exit 0
