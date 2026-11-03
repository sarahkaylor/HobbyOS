#!/usr/bin/env bash
# tools/gate/m1_glibc_hello.sh — M1 loader gate: a host-built glibc PIE plus
# the PINNED glibc 2.31 runtime on disk.img, executed by the Linux-ABI compat
# track (docs/rw-ldso-design.md).
#
# What it proves, in order:
#   1. the kernel resolves a 26-character VFAT long name out of PT_INTERP
#      (/lib64/ld-linux-x86-64.so.2) through fat16_resolve_path — no alias map;
#   2. it maps that ld.so in a USER_LIB slot and enters it with the SysV auxv
#      (AT_BASE / AT_PHDR / AT_ENTRY / AT_PLATFORM / AT_RANDOM ...);
#   3. ld.so opens libc.so.6 (openat), maps it, relocates both objects and
#      transfers to the executable's entry — i.e. userspace does the linking.
#
# The program prints "[M1] ..." rows and "[M1] done rc=0"; the verdict is that
# final row in the REAL serial log, never a build result.
#
# Pins (plan §2): libc6_2.31-0ubuntu9.18 and libgcc-s1_10.5.0 are downloaded and
# sha256-verified here; libc6-dev_2.31 (crt1/Scrt1 + libc_nonshared.a) is the
# build-time-only addition this gate needs, verified against its own sha256.
# Set GLIBC231 / GLIBC231_DEV to reuse an extraction.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
G="${GLIBC231:-/tmp/glibc231}"
D="${GLIBC231_DEV:-/tmp/glibc231-dev}"
SRC="${M1_SRC:-$ROOT/obj/m1-hello}"
OUT="${M1_LOG:-$ROOT/obj/m1_glibc_hello.log}"
TIMEOUT_S="${M1_TIMEOUT:-180}"

LIBC6_SHA=1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0
LIBGCC_SHA=4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c
LIBC6DEV_SHA=11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a

fetch_deb() { # $1 url, $2 sha256, $3 dest.deb
  mkdir -p "$(dirname "$3")"
  if [ ! -f "$3" ]; then
    echo "m1: fetching $(basename "$3")"
    curl -fsS -o "$3" "$1" || { echo "m1: FETCH FAILED $1"; exit 2; }
  fi
  echo "$2  $3" | sha256sum -c --quiet - || { echo "m1: SHA256 MISMATCH $3"; exit 2; }
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

mkdir -p "$SRC"
cat > "$SRC/hello.c" <<'EOF'
#include <gnu/libc-version.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  printf("[M1] hello from glibc %s (pid %d)\n", gnu_get_libc_version(),
         (int)getpid());
  printf("[M1] argc=%d\n", argc);
  for (int i = 0; i < argc; i++)
    printf("[M1] argv[%d]=%s\n", i, argv[i]);
  const char *h = getenv("HOME");
  printf("[M1] HOME=%s\n", h ? h : "(unset)");
  fflush(stdout);
  printf("[M1] done rc=0\n");
  return 0;
}
EOF

# Link against the PINNED 2.31 objects explicitly (-nostdlib): the host's
# libc6-dev linker script would otherwise pull in glibc 2.4x and the binary
# would require GLIBC_2.34 symbols the pin does not export.
LDIR="$G/lib/x86_64-linux-gnu"
DLIB="$D/usr/lib/x86_64-linux-gnu"
gcc -O2 -fPIE -pie -nostdlib -nostartfiles -o "$SRC/hello" "$SRC/hello.c" \
    "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
    "$LDIR/libc.so.6" "$DLIB/crtn.o" \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none || { echo "m1: hello link failed"; exit 2; }
echo "m1: hello built: $(readelf -h "$SRC/hello" | awk '/Type:/{print $2}') interp=$(readelf -lW "$SRC/hello" | awk '/INTERP/{print $NF}')"
readelf -dW "$SRC/hello" | grep NEEDED

echo "=== m1: build kernel + disk ==="
TOOLS_SKIP="${TOOLS_SKIP:-1}" make -C "$ROOT" ARCH=intel MODE=test hobbyos.elf disk.img \
  > "$ROOT/obj/m1_build.log" 2>&1 || { echo "m1: BUILD FAILED"; tail -30 "$ROOT/obj/m1_build.log"; exit 2; }

echo "=== m1: stage the glibc runtime (real sonames, VFAT long names) ==="
IMG="$ROOT/disk.img"
# Two locations, deliberately:
#   /lib/x86_64-linux-gnu  ld-2.31's OWN default search dir (the wave boot has
#                          no environment, so LD_LIBRARY_PATH is absent);
#   /LIB64                 the F3 launcher fallback, every DSO staged
#                          EXPLICITLY (libdl/librt/libpthread/libm), never
#                          left to libc.so.6 to imply (lane note, F3).
mmd -i "$IMG" ::/lib64 ::/lib >/dev/null 2>&1 || true
mmd -i "$IMG" ::/lib/x86_64-linux-gnu >/dev/null 2>&1 || true
mmd -i "$IMG" ::/LIB64 >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || { echo "m1: mcopy failed $2"; exit 2; }; }
stage "$LDIR/ld-2.31.so"        ::/lib64/ld-linux-x86-64.so.2
stage "$LDIR/libc-2.31.so"      ::/lib/x86_64-linux-gnu/libc.so.6
stage "$LDIR/libm-2.31.so"      ::/lib/x86_64-linux-gnu/libm.so.6
stage "$LDIR/libpthread-2.31.so" ::/lib/x86_64-linux-gnu/libpthread.so.0
stage "$LDIR/libdl-2.31.so"     ::/lib/x86_64-linux-gnu/libdl.so.2
stage "$LDIR/librt-2.31.so"     ::/lib/x86_64-linux-gnu/librt.so.1
stage "$LDIR/libgcc_s.so.1"     ::/lib/x86_64-linux-gnu/libgcc_s.so.1
stage "$LDIR/ld-2.31.so"        ::/LIB64/ld-linux-x86-64.so.2
stage "$LDIR/libc-2.31.so"      ::/LIB64/libc.so.6
stage "$LDIR/libm-2.31.so"      ::/LIB64/libm.so.6
stage "$LDIR/libpthread-2.31.so" ::/LIB64/libpthread.so.0
stage "$LDIR/libdl-2.31.so"     ::/LIB64/libdl.so.2
stage "$LDIR/librt-2.31.so"     ::/LIB64/librt.so.1
stage "$LDIR/libgcc_s.so.1"     ::/LIB64/libgcc_s.so.1
stage "$SRC/hello"             ::PROBE.ELF
printf 'PROBE.ELF\n' > "$SRC/WAVE.CFG"
stage "$SRC/WAVE.CFG"          ::WAVE.CFG

# Private OVMF pair: -pflash uses one file for code AND vars, so a shared copy
# accumulates boot entries across runs/lanes.  Reuse the host's pristine one.
OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || { echo "m1: no OVMF_CODE_4M.fd found"; exit 2; }
OVMF="$SRC/OVMF_CODE_4M.fd"
[ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

echo "=== m1: boot (deadline ${TIMEOUT_S}s, log $OUT) ==="
: > "$OUT"
( make -C "$ROOT" -o disk.img ARCH=intel MODE=test EDK2_X86_64="$OVMF" run \
       QEMU_ARGS="-display none -enable-kvm" >> "$OUT" 2>&1 & )
for _ in $(seq 1 "$TIMEOUT_S"); do
  grep -aq "System halt" "$OUT" && break
  sleep 1
done
sleep 1

echo "--- loader / track evidence ---"
grep -aE "PT_INTERP mapped|v2 loader: PID" "$OUT" | head -4 || true
echo "--- program rows ---"
grep -a '^\[M1\]' "$OUT" || echo "(none: the program produced no [M1] row)"
echo "--- unhandled Linux syscalls ---"
grep -a '\[LCOMPAT\]' "$OUT" | head -20 || true
echo "--- faults / exits ---"
grep -aE "memory fault|FATAL|System halt" "$OUT" | tail -6 || true
if grep -aq '^\[M1\] done rc=0' "$OUT"; then
  echo "m1: PASS — glibc 2.31 hello ran under the compat track"
  exit 0
fi
echo "m1: FAIL — no '[M1] done rc=0' in the serial log"
exit 1
