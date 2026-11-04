#!/usr/bin/env bash
# tools/gate/m2_pi.sh — M6/L25 gate: PRIO_INHERIT mutexes on the compat track.
#
# Why this gate exists
# --------------------
# glibc 2.31 probes the kernel for PI futexes on the FIRST pthread_mutex_init()
# of a PRIO_INHERIT mutex (futex(&zero, FUTEX_UNLOCK_PI|PRIVATE, 0)).  A kernel
# that answers -ENOSYS makes glibc set tpi_supported = -1, and every later
# PRIO_INHERIT init returns EOPNOTSUPP and writes NOTHING into the mutex.  Unity
# (UnityPlayer+0x1BA2E16) ignores that return; the object's __lock is the
# allocator's leftover free-list link, and the first pthread_mutex_lock() parks
# forever.  That was the M6 park at 0x10C00964A0.
#
# What this proves, in the real serial log:
#   1. init writes the word (rv=0, kind=33 RECURSIVE|PRIO_INHERIT) even when the
#      caller's memory was pre-filled with the measured 0x10C00964D0 link;
#   2. 8 threads x N critical sections lose no increments (FUTEX_LOCK_PI +
#      handoff on unlock), and a recursive re-lock succeeds;
#   3. trylock on a held PI mutex answers EBUSY;
#   4. a thread that exits holding a ROBUST|PI mutex leaves FUTEX_OWNER_DIED,
#      so the next lock returns EOWNERDEAD.
#
# Verdict is '[PTPI] PASS' in the REAL serial log, never a build log.
# Never run concurrently with tools/rw/run-rimworld-desktop.sh: both rewrite
# disk.img.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
G="${GLIBC231:-/tmp/glibc231}"
D="${GLIBC231_DEV:-/tmp/glibc231-dev}"
SRC="${M2PI_SRC:-$ROOT/obj/m2-pi}"
OUT="${M2PI_LOG:-$ROOT/obj/m2_pi.log}"
TIMEOUT_S="${M2PI_TIMEOUT:-180}"
IMG="$ROOT/disk.img"

for f in "$G/lib/x86_64-linux-gnu/ld-2.31.so" "$D/usr/lib/x86_64-linux-gnu/Scrt1.o"; do
  [ -e "$f" ] || { echo "m2pi: ENVIRONMENT: missing $f (run tools/gate/m2_threads.sh first)"; exit 2; }
done
LDIR="$G/lib/x86_64-linux-gnu"
DLIB="$D/usr/lib/x86_64-linux-gnu"
mkdir -p "$SRC"

echo "=== m2pi: build the PI probe against the 2.31 pin ==="
gcc -O1 -fPIE -pie -nostdlib -nostartfiles -o "$SRC/pthread_pi" \
    "$ROOT/src/user/rw_pthread_pi.c" \
    "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
    "$LDIR/libc.so.6" "$LDIR/libpthread-2.31.so" "$LDIR/libdl-2.31.so" \
    "$DLIB/crtn.o" \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none \
  || { echo "m2pi: probe link failed"; exit 2; }

echo "=== m2pi: build kernel + disk ==="
TOOLS_SKIP="${TOOLS_SKIP:-1}" make -C "$ROOT" ARCH=intel MODE=test hobbyos.elf disk.img \
  > "$ROOT/obj/m2_pi_build.log" 2>&1 || { echo "m2pi: BUILD FAILED"; tail -30 "$ROOT/obj/m2_pi_build.log"; exit 2; }

echo "=== m2pi: stage runtime + probe ==="
mmd -i "$IMG" ::/lib64 ::/lib ::/LIB64 ::/lib/x86_64-linux-gnu >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || { echo "m2pi: mcopy failed $2"; exit 2; }; }
for pair in ld-2.31.so:ld-linux-x86-64.so.2 libc-2.31.so:libc.so.6 \
            libm-2.31.so:libm.so.6 libpthread-2.31.so:libpthread.so.0 \
            libdl-2.31.so:libdl.so.2 librt-2.31.so:librt.so.1 \
            libgcc_s.so.1:libgcc_s.so.1; do
  s="${pair%%:*}"; d="${pair##*:}"
  stage "$LDIR/$s" "::/lib/x86_64-linux-gnu/$d"
  stage "$LDIR/$s" "::/LIB64/$d"
done
stage "$LDIR/ld-2.31.so" "::/lib64/ld-linux-x86-64.so.2"
stage "$SRC/pthread_pi" ::PROBE.ELF
printf 'PROBE.ELF\n' > "$SRC/WAVE.CFG"
stage "$SRC/WAVE.CFG" ::WAVE.CFG

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || { echo "m2pi: no OVMF_CODE_4M.fd found"; exit 2; }
OVMF="$SRC/OVMF_CODE_4M.fd"
[ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

echo "=== m2pi: boot (deadline ${TIMEOUT_S}s, log $OUT) ==="
: > "$OUT"
( make -C "$ROOT" -o disk.img ARCH=intel MODE=test EDK2_X86_64="$OVMF" run \
       QEMU_ARGS="-display none -enable-kvm" >> "$OUT" 2>&1 & )
for _ in $(seq 1 "$TIMEOUT_S"); do
  grep -aq "System halt" "$OUT" && break
  sleep 1
done
if ! grep -aq "System halt" "$OUT"; then
  echo "m2pi: deadline hit without 'System halt' — stopping this run's QEMU"
  fuser -k "$IMG" >/dev/null 2>&1 || true
fi
sleep 1

echo "--- [PTPI] rows ---"
grep -a '^\[PTPI\]' "$OUT" || echo "(none: the probe produced no [PTPI] row)"
echo "--- PI futex / ENOSYS diagnostics ---"
grep -aE 'PI-enosys|\[LCOMPAT\] ENOSYS' "$OUT" | head -12 || true
echo "--- faults / exits ---"
grep -aE "memory fault|FATAL|User process fault|System halt" "$OUT" | tail -6 || true
if grep -aq '^\[PTPI\] PASS' "$OUT"; then
  echo "m2pi: PASS — PI mutex init, 8-thread mutual exclusion, trylock, owner-died"
  exit 0
fi
echo "m2pi: FAIL — no '[PTPI] PASS' in the serial log"
exit 1
