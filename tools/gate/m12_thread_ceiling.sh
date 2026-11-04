#!/usr/bin/env bash
# tools/gate/m12_thread_ceiling.sh — L12 gate: a 150-thread Linux-ABI process
# on the compat track.
#
# Why: the real game measures 106 threads at the menu and 145 peak
# (docs/rimworld-evidence.md §3.4). With MAX_PROCESSES == 64 the 65th clone
# returned EAGAIN, so no amount of GL/X11 work could reach the menu. This
# gate proves a single process can hold 150 threads + main (151 PCBs), each
# with its own __thread storage and stack, contending on shared futexes, and
# that they all join. It also cancels a parked worker: glibc implements
# pthread_cancel with SIGCANCEL(32), so this is the real-time-signal proof.
#
# Verdict is the '[T150] PASS' row in the REAL serial log, never a build.
# Pins reuse the M1/M2 gate downloads (glibc 2.31 + libc6-dev, same sha256).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
G="${GLIBC231:-/tmp/glibc231}"
D="${GLIBC231_DEV:-/tmp/glibc231-dev}"
SRC="${M12_SRC:-$ROOT/obj/m12-threads}"
OUT="${M12_LOG:-$ROOT/obj/m12_thread_ceiling.log}"
TIMEOUT_S="${M12_TIMEOUT:-300}"

LIBC6_SHA=1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0
LIBGCC_SHA=4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c
LIBC6DEV_SHA=11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a

fetch_deb() { # $1 url, $2 sha256, $3 dest.deb
  mkdir -p "$(dirname "$3")"
  if [ ! -f "$3" ]; then
    echo "m12: fetching $(basename "$3")"
    curl -fsS -o "$3" "$1" || { echo "m12: FETCH FAILED $1"; exit 2; }
  fi
  echo "$2  $3" | sha256sum -c --quiet - || { echo "m12: SHA256 MISMATCH $3"; exit 2; }
}

if [ ! -f "$G/lib/x86_64-linux-gnu/ld-2.31.so" ]; then
  T="$ROOT/obj/m1-pins"
  fetch_deb http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb \
            "$LIBC6_SHA" "$T/libc6.deb"
  fetch_deb http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb \
            "$LIBGCC_SHA" "$T/libgcc-s1.deb"
  rm -rf "$G" && mkdir -p "$G"
  dpkg-deb -x "$T/libc6.deb" "$G" && dpkg-deb -x "$T/libgcc-s1.deb" "$G"
fi
if [ ! -f "$D/usr/lib/x86_64-linux-gnu/Scrt1.o" ]; then
  T="$ROOT/obj/m1-pins"
  fetch_deb http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6-dev_2.31-0ubuntu9.18_amd64.deb \
            "$LIBC6DEV_SHA" "$T/libc6-dev.deb"
  rm -rf "$D" && mkdir -p "$D"
  dpkg-deb -x "$T/libc6-dev.deb" "$D"
fi

LDIR="$G/lib/x86_64-linux-gnu"
DLIB="$D/usr/lib/x86_64-linux-gnu"

mkdir -p "$SRC"
echo "=== m12: build the 150-thread stress against the pinned glibc 2.31 ==="
gcc -O1 -fPIE -pie -nostdlib -nostartfiles ${M12_CFLAGS:-} -o "$SRC/thread_stress" \
    "$ROOT/src/user/rw_thread_stress.c" \
    "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
    "$LDIR/libc.so.6" "$LDIR/libpthread-2.31.so" \
    "$DLIB/crtn.o" \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none \
  || { echo "m12: stress link failed"; exit 2; }
echo "m12: built $(readelf -h "$SRC/thread_stress" | awk '/Type:/{print $2}')"

echo "=== m12: build kernel + disk ==="
TOOLS_SKIP="${TOOLS_SKIP:-1}" make -C "$ROOT" ARCH=intel MODE=test hobbyos.elf disk.img \
  > "$ROOT/obj/m12_build.log" 2>&1 || { echo "m12: BUILD FAILED"; tail -40 "$ROOT/obj/m12_build.log"; exit 2; }

echo "=== m12: stage the runtime + the probe ==="
IMG="$ROOT/disk.img"
mmd -i "$IMG" ::/lib64 ::/lib >/dev/null 2>&1 || true
mmd -i "$IMG" ::/lib/x86_64-linux-gnu >/dev/null 2>&1 || true
mmd -i "$IMG" ::/LIB64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || { echo "m12: mcopy failed $2"; exit 2; }; }
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
stage "$SRC/thread_stress"       ::PROBE.ELF
printf 'PROBE.ELF\n' > "$SRC/WAVE.CFG"
stage "$SRC/WAVE.CFG"            ::WAVE.CFG

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || { echo "m12: no OVMF_CODE_4M.fd found"; exit 2; }
OVMF="$SRC/OVMF_CODE_4M.fd"
[ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

echo "=== m12: boot (deadline ${TIMEOUT_S}s, log $OUT) ==="
: > "$OUT"
( make -C "$ROOT" -o disk.img ARCH=intel MODE=test EDK2_X86_64="$OVMF" run \
       QEMU_ARGS="-display none -enable-kvm" >> "$OUT" 2>&1 & )
for _ in $(seq 1 "$TIMEOUT_S"); do
  grep -aq "System halt" "$OUT" && break
  grep -aq '^\[T150\] PASS' "$OUT" && break
  sleep 1
done
if ! grep -aq "System halt" "$OUT"; then
  echo "m12: deadline hit without 'System halt' — stopping this run's QEMU"
  fuser -k "$IMG" >/dev/null 2>&1 || true
fi
sleep 1

echo "--- program rows ---"
grep -a '^\[T150\]' "$OUT" || echo "(none: the probe produced no [T150] row)"
echo "--- slot / clone / futex diagnostics ---"
grep -aE '\[LCOMPAT\]|no free slot|no free process slots' "$OUT" | head -20 || true
echo "--- faults / exits ---"
grep -aE "memory fault|FATAL|User process fault|System halt" "$OUT" | tail -8 || true

ok=1
grep -aq '^\[T150\] PASS' "$OUT" || ok=0
grep -aq '^\[T150\] threads=150 created=150' "$OUT" || ok=0
grep -aq '^\[T150\] joined=150 tls_sum=11175 shared=1113775 expect=1113775' "$OUT" || ok=0
grep -aq '^\[T150\] cancel=ok' "$OUT" || ok=0
if [ "$ok" = 1 ]; then
  echo "m12: PASS — 150 threads created + joined, per-thread TLS, shared futex,"
  echo "     and pthread_cancel (SIGCANCEL 32) delivered"
  exit 0
fi
echo "m12: FAIL — the serial log does not contain the full [T150] PASS contract"
exit 1
