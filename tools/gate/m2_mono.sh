#!/usr/bin/env bash
# tools/gate/m2_mono.sh — M2 gate (b): the official Mono 6.12 x86_64 Linux
# runtime under the Linux-ABI compat track.
#
# Two programs run, from one boot, both named on the wave manifest:
#
#   MONO.EXE --version     the runtime banner, which itself reports the two
#                          features M2 adds: "TLS: __thread" and
#                          "SIGSEGV: altstack"
#   MONO.EXE /HELLO.EXE    a C# hello that starts a managed Thread, joins it
#                          and prints HELLO-OK
#
# Verdicts are rows in the REAL serial log.  If Mono cannot get past a syscall,
# the [LCOMPAT] ENOSYS record names it and this script prints it — that is the
# finding, not a failure to hide.
#
# The runtime is fetched and sha256-verified by tools/rw/fetch-mono.sh (the
# official mono-project APT snapshot; there is no 6.12 Linux tarball).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${RW_MONO_DIR:-$ROOT/third_party/rw-mono}"
SRC="${M2M_SRC:-$ROOT/obj/m2-mono}"
LOG="${M2M_LOG:-$ROOT/obj/m2_mono.log}"
TIMEOUT_S="${M2M_TIMEOUT:-300}"
mkdir -p "$SRC"

echo "=== m2m: fetch the pinned Mono 6.12 tree ==="
bash "$ROOT/tools/rw/fetch-mono.sh" || { echo "m2m: FETCH FAILED"; exit 2; }
R="$OUT/root"
[ -x "$R/usr/bin/mono-sgen" ] || { echo "m2m: no mono-sgen in $R"; exit 2; }

echo "=== m2m: build kernel + disk ==="
TOOLS_SKIP="${TOOLS_SKIP:-1}" make -C "$ROOT" ARCH=intel MODE=test hobbyos.elf disk.img \
  > "$ROOT/obj/m2m_build.log" 2>&1 || { echo "m2m: BUILD FAILED"; tail -30 "$ROOT/obj/m2m_build.log"; exit 2; }

echo "=== m2m: stage glibc 2.31 + the Mono tree + hello.exe ==="
IMG="$ROOT/disk.img"
G="${GLIBC231:-/tmp/glibc231}"
LDIR="$G/lib/x86_64-linux-gnu"
if [ ! -f "$LDIR/ld-2.31.so" ]; then
  echo "m2m: glibc 2.31 pin missing at $G (run tools/gate/m1_glibc_hello.sh once)"; exit 2
fi
mkd() { mmd -i "$IMG" "$1" >/dev/null 2>&1 || true; }
for d in ::/lib64 ::/lib ::/lib/x86_64-linux-gnu ::/LIB64 \
         ::/usr ::/usr/bin ::/usr/lib ::/usr/lib/mono ::/usr/lib/mono/4.5 \
         ::/etc ::/etc/mono ::/etc/mono/4.5; do mkd "$d"; done
stage() { mcopy -o -i "$IMG" "$1" "$2" || { echo "m2m: mcopy failed $2"; exit 2; }; }
stage "$LDIR/ld-2.31.so"         ::/lib64/ld-linux-x86-64.so.2
stage "$LDIR/libc-2.31.so"       ::/lib/x86_64-linux-gnu/libc.so.6
stage "$LDIR/libm-2.31.so"       ::/lib/x86_64-linux-gnu/libm.so.6
stage "$LDIR/libpthread-2.31.so" ::/lib/x86_64-linux-gnu/libpthread.so.0
stage "$LDIR/libdl-2.31.so"      ::/lib/x86_64-linux-gnu/libdl.so.2
stage "$LDIR/librt-2.31.so"      ::/lib/x86_64-linux-gnu/librt.so.1
stage "$LDIR/libgcc_s.so.1"      ::/lib/x86_64-linux-gnu/libgcc_s.so.1
stage "$LDIR/ld-2.31.so"         ::/LIB64/ld-linux-x86-64.so.2
stage "$LDIR/libc-2.31.so"       ::/LIB64/libc.so.6
stage "$LDIR/libm-2.31.so"       ::/LIB64/libm.so.6
stage "$LDIR/libpthread-2.31.so" ::/LIB64/libpthread.so.0
stage "$LDIR/libdl-2.31.so"      ::/LIB64/libdl.so.2
stage "$LDIR/librt-2.31.so"      ::/LIB64/librt.so.1
stage "$LDIR/libgcc_s.so.1"      ::/LIB64/libgcc_s.so.1
# The launcher MUST live at the FAT16 root: the wave manifest names a
# root-relative file (the m1 gate stages PROBE.ELF at ::/PROBE.ELF for the
# same reason).  mono derives its prefix from its own path, and "/MONO.EXE"
# gives prefix "/" -> /usr/lib/mono/4.5, which is where the DLLs are.
stage "$R/usr/bin/mono-sgen"     ::/MONO.EXE
for f in "$R/usr/lib/mono/4.5"/*.dll; do stage "$f" "::/usr/lib/mono/4.5/${f##*/}"; done
# mono-sgen NEEDED: libz.so.1 + libstdc++.so.6 (focal builds; the host's need
# GLIBC_2.38 and cannot load under the 2.31 pin).
LS="$(ls "$R"/lib/libstdc++.so.6.* 2>/dev/null | head -1)"
LZ="$(ls "$R"/lib/libz.so.1.* 2>/dev/null | head -1)"
[ -n "$LS" ] && [ -n "$LZ" ] || { echo "m2m: libstdc++/libz missing from $R/lib (re-run fetch-mono.sh)"; exit 2; }
stage "$LS" ::/lib/x86_64-linux-gnu/libstdc++.so.6
stage "$LZ" ::/lib/x86_64-linux-gnu/libz.so.1
stage "$LS" ::/LIB64/libstdc++.so.6
stage "$LZ" ::/LIB64/libz.so.1
[ -f "$R/etc/mono/config" ] && stage "$R/etc/mono/config" ::/etc/mono/config
for f in "$R"/etc/mono/4.5/*.config; do [ -f "$f" ] && stage "$f" "::/etc/mono/4.5/${f##*/}"; done
stage "$OUT/hello.exe"           ::/hello.exe
# argv forwarding check (same boot-1 target the M6 launcher will need):
# a glibc probe that prints exactly the argv it was given.
DLIB2="${GLIBC231_DEV:-/tmp/glibc231-dev}/usr/lib/x86_64-linux-gnu"
gcc -O1 -fPIE -pie -nostdlib -nostartfiles -o "$SRC/argv_probe" \
    "$ROOT/src/user/rw_argv_probe.c" \
    "$DLIB2/Scrt1.o" "$DLIB2/crti.o" "$DLIB2/libc_nonshared.a" \
    "$LDIR/libc.so.6" "$DLIB2/crtn.o" \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none || { echo "m2m: argv probe link failed"; exit 2; }
stage "$SRC/argv_probe"          ::/ARGV.EXE
# One program per boot unless M2M_BOTH=1: two mono processes writing to the
# serial console at once interleave mid-line (measured), which turns a clear
# loader error into unreadable soup.
if [ "${M2M_BOTH:-0}" = "1" ]; then
  printf 'MONO.EXE --version\nMONO.EXE /HELLO.EXE\n' > "$SRC/WAVE.CFG"
elif [ "${M2M_ONLY:-hello}" = "argv2" ]; then
  printf 'ARGV.EXE /HELLO.EXE\n' > "$SRC/WAVE.CFG"
elif [ "${M2M_ONLY:-hello}" = "abs" ]; then
  printf '/MONO.EXE /HELLO.EXE\n' > "$SRC/WAVE.CFG"
elif [ "${M2M_ONLY:-hello}" = "wrap" ]; then
  # ld.so's "invoked directly" convention: argv[0]=loader, argv[1]=program,
  # argv[2..]=its arguments.
  printf 'MONO.EXE MONO.EXE /HELLO.EXE\n' > "$SRC/WAVE.CFG"
elif [ "${M2M_ONLY:-hello}" = "argv" ]; then
  printf 'ARGV.EXE one two\n' > "$SRC/WAVE.CFG"
elif [ "${M2M_ONLY:-hello}" = "none" ]; then
  printf 'MONO.EXE\n' > "$SRC/WAVE.CFG"
elif [ "${M2M_ONLY:-hello}" = "version" ]; then
  printf 'MONO.EXE --version\n' > "$SRC/WAVE.CFG"
else
  printf 'MONO.EXE /HELLO.EXE\n' > "$SRC/WAVE.CFG"
fi
stage "$SRC/WAVE.CFG"            ::/WAVE.CFG

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || { echo "m2m: no OVMF_CODE_4M.fd found"; exit 2; }
OVMF="$SRC/OVMF_CODE_4M.fd"
[ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

echo "=== m2m: boot (deadline ${TIMEOUT_S}s, log $LOG) ==="
: > "$LOG"
( make -C "$ROOT" -o disk.img ARCH=intel MODE=test EDK2_X86_64="$OVMF" run \
       QEMU_ARGS="-display none -enable-kvm" >> "$LOG" 2>&1 & )
for _ in $(seq 1 "$TIMEOUT_S"); do
  grep -aq "System halt" "$LOG" && break
  sleep 1
done
if ! grep -aq "System halt" "$LOG"; then
  echo "m2m: deadline hit without 'System halt' — stopping this run's QEMU"
  fuser -k "$IMG" >/dev/null 2>&1 || true
fi
sleep 1

echo "--- Mono rows ---"
grep -aE "Mono JIT compiler|TLS:|SIGSEGV:|Hello from C#|runtime: 4|thread counter|HELLO-OK" "$LOG" || \
  echo "(none: Mono produced no recognisable row)"
echo "--- unhandled Linux syscalls (first 24) ---"
grep -a '\[LCOMPAT\]' "$LOG" | head -24 || true
echo "--- loader / faults ---"
grep -aE "PT_INTERP mapped|memory fault|User process fault|System halt" "$LOG" | tail -8 || true
if grep -aq 'HELLO-OK' "$LOG"; then
  echo "m2m: PASS — the official Mono 6.12 runtime ran a threaded C# program under compat"
  exit 0
fi
echo "m2m: FAIL — no HELLO-OK in the serial log (the first [LCOMPAT] row above names the wall)"
exit 1
