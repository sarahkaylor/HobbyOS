#!/usr/bin/env bash
# tools/gate/m2_threads.sh — M2 gate (a): a host-built pthread stress program
# running on the Linux-ABI compat track with the PINNED glibc 2.31.
#
# What it proves, in the real serial log:
#   1. clone(56) with CLONE_VM|THREAD|SETTLS gives 8 real threads;
#   2. a contended pthread_mutex and a condvar work -> futex WAIT/WAKE with
#      real wait queues (a tick-poll or an EAGAIN shim deadlocks or miscounts);
#   3. each thread has its own FS base: its __thread counter comes back
#      through pthread_join and the main thread's copy is still 0;
#   4. a dlopen'ed DSO with __thread gets per-thread storage (dynamic TLS via
#      the guest's own ld.so, no kernel DTV);
#   5. the checksum derived from all of that matches.
#
# Verdict is the '[PTHR] PASS' row in the REAL serial log, never a build.
# Pins reuse the M1 gate's downloads (same glibc 2.31 + libgcc 10.5 sha256).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
G="${GLIBC231:-/tmp/glibc231}"
D="${GLIBC231_DEV:-/tmp/glibc231-dev}"
SRC="${M2_SRC:-$ROOT/obj/m2-threads}"
OUT="${M2_LOG:-$ROOT/obj/m2_threads.log}"
TIMEOUT_S="${M2_TIMEOUT:-240}"

LIBC6_SHA=1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0
LIBGCC_SHA=4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c
LIBC6DEV_SHA=11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a

fetch_deb() { # $1 url, $2 sha256, $3 dest.deb
  mkdir -p "$(dirname "$3")"
  if [ ! -f "$3" ]; then
    echo "m2: fetching $(basename "$3")"
    curl -fsS -o "$3" "$1" || { echo "m2: FETCH FAILED $1"; exit 2; }
  fi
  echo "$2  $3" | sha256sum -c --quiet - || { echo "m2: SHA256 MISMATCH $3"; exit 2; }
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
echo "=== m2: build the pthread stress + TLS DSO against the 2.31 pin ==="
# The DSO needs no libc: it only defines a __thread variable and accessors, so
# its TLS block is allocated by the guest's ld.so at dlopen time.
gcc -O1 -fPIC -shared -nostdlib -o "$SRC/libtls.so" \
    "$ROOT/src/user/rw_tls_dso.c" -Wl,--build-id=none \
  || { echo "m2: DSO link failed"; exit 2; }

gcc -O1 -fPIE -pie -nostdlib -nostartfiles ${M2_PTHR_FLAGS:-} -o "$SRC/pthread_stress" \
    "$ROOT/src/user/rw_pthread_stress.c" \
    "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
    "$LDIR/libc.so.6" "$LDIR/libpthread-2.31.so" "$LDIR/libdl-2.31.so" \
    "$DLIB/crtn.o" \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none \
  || { echo "m2: stress link failed"; exit 2; }
echo "m2: built $(readelf -h "$SRC/pthread_stress" | awk '/Type:/{print $2}')"
readelf -dW "$SRC/pthread_stress" | grep NEEDED
echo "m2: DSO TLS relocs:"
readelf -rW "$SRC/libtls.so" | grep -c TPOFF64 || true

echo "=== m2: build kernel + disk ==="
TOOLS_SKIP="${TOOLS_SKIP:-1}" make -C "$ROOT" ARCH=intel MODE=test hobbyos.elf disk.img \
  > "$ROOT/obj/m2_build.log" 2>&1 || { echo "m2: BUILD FAILED"; tail -30 "$ROOT/obj/m2_build.log"; exit 2; }

echo "=== m2: stage the runtime + the probes ==="
IMG="$ROOT/disk.img"
mmd -i "$IMG" ::/lib64 ::/lib >/dev/null 2>&1 || true
mmd -i "$IMG" ::/lib/x86_64-linux-gnu >/dev/null 2>&1 || true
mmd -i "$IMG" ::/LIB64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || { echo "m2: mcopy failed $2"; exit 2; }; }
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
stage "$SRC/pthread_stress"      ::PROBE.ELF
stage "$SRC/libtls.so"           ::LIBTLS.SO
printf 'PROBE.ELF\n' > "$SRC/WAVE.CFG"
stage "$SRC/WAVE.CFG"            ::WAVE.CFG

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || { echo "m2: no OVMF_CODE_4M.fd found"; exit 2; }
OVMF="$SRC/OVMF_CODE_4M.fd"
[ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

echo "=== m2: boot (deadline ${TIMEOUT_S}s, log $OUT) ==="
: > "$OUT"
( make -C "$ROOT" -o disk.img ARCH=intel MODE=test EDK2_X86_64="$OVMF" run \
       QEMU_ARGS="-display none -enable-kvm" >> "$OUT" 2>&1 & )
for _ in $(seq 1 "$TIMEOUT_S"); do
  grep -aq "System halt" "$OUT" && break
  sleep 1
done
if ! grep -aq "System halt" "$OUT"; then
  echo "m2: deadline hit without 'System halt' — stopping this run's QEMU"
  fuser -k "$IMG" >/dev/null 2>&1 || true
fi
sleep 1

echo "--- program rows ---"
grep -a '^\[PTHR\]' "$OUT" || echo "(none: the probe produced no [PTHR] row)"
echo "--- clone / futex diagnostics ---"
grep -aE '\[LCOMPAT\]|\[GB021' "$OUT" | head -20 || true
echo "--- faults / exits ---"
grep -aE "memory fault|FATAL|User process fault|System halt" "$OUT" | tail -8 || true
if grep -aq '^\[PTHR\] PASS' "$OUT"; then
  echo "m2: PASS — 8 pthreads, mutex+condvar+TLS+join+dlopen TLS under compat"
  exit 0
fi
echo "m2: FAIL — no '[PTHR] PASS' in the serial log"
exit 1
