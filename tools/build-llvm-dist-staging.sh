#!/usr/bin/env bash
# build-llvm-dist-staging.sh — build the in-OS LLVM toolchain staging
# (third_party/llvm-21.1.8/dist-staging) from the vendored LLVM source.
#
# This is the "build the build tools" path behind `make run` completeness:
# tools-staged runs this only when NEITHER obj/selfhost/dist-staging nor
# third_party/llvm-21.1.8/dist-staging exists.  It is a faithful
# transcription of docs/selfhost/llvm-bootstrap.md §6 (configure recipes)
# and §6.1 (the wave-2 actual command sequence, including the sysroot
# re-stage order and the DESTDIR install-distribution staging).
#
# COST: cold (no llvm-build dir) = hours — host tablegen, then clang+lld
# under -j$(nproc).  Warm (build dir present) = reconfigure in place (~3 s)
# + incremental ninja.  The result is cached at third_party/llvm-21.1.8/
# dist-staging and reused by every later disk build.
#
# Status note (2026-10-15): the sequence below is transcribed from the
# wave-2 receipts; the persistent staging already exists on the machines
# that shipped it, so this script's full cold path has not been re-run
# end-to-end since.  Run with --check first; failures print the log path.
#
# Usage:
#   bash tools/build-llvm-dist-staging.sh [--check]
#     --check   report whether the staging exists / what a build would do
#
# Work dirs: obj/selfhost/{tblgen-native,llvm-build,sysroot}
# Logs:      obj/selfhost/llvm-build.log (appended; tail it on failure)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VENDOR="third_party/llvm-21.1.8"
SRC="$VENDOR/src/llvm-project-21.1.8.src"
DEST="$VENDOR/dist-staging"
WORK="obj/selfhost"
LOG="$WORK/llvm-build.log"

CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

say() { echo "[llvm-dist] $*"; }
die() { echo "[llvm-dist] ERROR: $*" >&2; exit 1; }

# ---- 0) already built? ------------------------------------------------
if [ -f "$DEST/usr/local/bin/clang-21" ]; then
  say "staging already present at $DEST — nothing to do"
  exit 0
fi
if [ -f "$WORK/dist-staging/usr/local/bin/clang-21" ]; then
  say "staging present at $WORK/dist-staging (tools/selfhost-stage.sh finds it there; no rebuild needed)"
  exit 0
fi

say "no LLVM dist staging found in $DEST or $WORK/dist-staging"

# ---- 1) host prerequisites -------------------------------------------
for t in cmake ninja clang ld.lld llvm-objcopy; do
  command -v "$t" >/dev/null 2>&1 || die "host tool missing: $t"
done

# ---- 2) vendored source present (else fetch) -------------------------
if [ ! -d "$SRC/llvm" ]; then
  say "LLVM source not extracted ($SRC/llvm missing) — running $VENDOR/fetch.sh"
  if [ "$CHECK" = "1" ]; then
    say "--check: would run: bash $VENDOR/fetch.sh (downloads/extracts the pinned monorepo tarball)"
  else
    bash "$VENDOR/fetch.sh"
  fi
fi

if [ "$CHECK" = "1" ]; then
  say "--check: a build would run, in order:"
  say "  1) cmake+ninja host tablegen        -> $WORK/tblgen-native (llvm-tblgen, clang-tblgen)"
  say "  2) sysroot re-stage                 -> $WORK/sysroot + make obj/arm/{crt0.o,libc.a,libcxx.a}"
  say "  3) cmake cross configure            -> $WORK/llvm-build (toolchain-llvm-hobbyos.cmake)"
  say "  4) ninja llvm-ar llvm-objcopy, ninja clang lld"
  say "  5) DESTDIR=$DEST ninja install-distribution"
  say "  log: $LOG; cold cost: hours (-j$(nproc))"
  exit 0
fi

mkdir -p "$WORK"
say "logging to $LOG"

# ---- 3) host tablegen (target binaries cannot run on the host) -------
# llvm-bootstrap.md §6: native tablegen first, from the same source tree.
if [ ! -f "$WORK/tblgen-native/bin/llvm-tblgen" ]; then
  say "configure+build host tablegen (llvm-tblgen, clang-tblgen)"
  cmake -G Ninja -S "$SRC/llvm" -B "$WORK/tblgen-native" \
    -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=AArch64 \
    -DLLVM_ENABLE_PROJECTS=clang \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_CURL=OFF 2>&1 | tee -a "$LOG"
  ninja -C "$WORK/tblgen-native" llvm-tblgen clang-tblgen 2>&1 | tee -a "$LOG"
else
  say "host tablegen already built ($WORK/tblgen-native)"
fi

# ---- 4) sysroot re-stage (§6.1 step 0 order matters) -----------------
# Stale sysroot headers after src/ changes reproduced the pid_t ABI assert
# before; always refresh includes + libc closure before configuring.
say "re-staging the sysroot closure ($WORK/sysroot)"
mkdir -p "$WORK/sysroot/include/libc-include" "$WORK/sysroot/include/kernel-include" \
         "$WORK/sysroot/include/user-include" "$WORK/sysroot/lib"
cp -a src/libc/include/. "$WORK/sysroot/include/libc-include/"
cp -a src/include/.      "$WORK/sysroot/include/kernel-include/"
cp -a src/user_include/. "$WORK/sysroot/include/user-include/"
make obj/arm/libc.a obj/arm/crt0.o obj/arm/libcxx.a -j"$(nproc)" 2>&1 | tee -a "$LOG"
cp -a obj/arm/crt0.o obj/arm/libc.a obj/arm/libcxx.a "$WORK/sysroot/lib/"

# ---- 5) cross configure (§6; reconfigure in place when warm) ---------
say "configuring the cross build ($WORK/llvm-build)"
cmake -G Ninja -S "$SRC/llvm" -B "$WORK/llvm-build" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/src/user/toolchain/cmake/toolchain-llvm-hobbyos.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=AArch64 \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_RTTI=OFF \
  -DBUILD_SHARED_LIBS=OFF -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_LIBPFM=OFF \
  -DLLVM_TABLEGEN="$ROOT/$WORK/tblgen-native/bin/llvm-tblgen" \
  -DCLANG_TABLEGEN="$ROOT/$WORK/tblgen-native/bin/clang-tblgen" \
  -DLLVM_NATIVE_TOOL_DIR="$ROOT/$WORK/tblgen-native/bin" \
  -DLLVM_DISTRIBUTION_COMPONENTS="clang;lld;llvm-ar;llvm-objcopy;clang-resource-headers" \
  2>&1 | tee -a "$LOG"

# ---- 6) build (§6.1 step 1 then the long step 2) ---------------------
say "building llvm-ar + llvm-objcopy"
ninja -C "$WORK/llvm-build" llvm-ar llvm-objcopy -j"$(nproc)" 2>&1 | tee -a "$LOG"
say "building clang + lld (long phase — hours cold; log: $LOG)"
ninja -C "$WORK/llvm-build" clang lld -j"$(nproc)" 2>&1 | tee -a "$LOG"

# ---- 7) install-distribution -> the persistent staging ---------------
say "staging install-distribution -> $DEST (DESTDIR)"
mkdir -p "$DEST"
DESTDIR="$ROOT/$DEST" ninja -C "$WORK/llvm-build" install-distribution -j8 2>&1 | tee -a "$LOG"

[ -f "$DEST/usr/local/bin/clang-21" ] || die "install-distribution produced no clang-21 at $DEST"
say "done. staged:"
ls -la "$DEST/usr/local/bin" | awk '{print "  "$5"  "$9}' | sed '/^  *$/d'
say "size: $(du -sh "$DEST" | cut -f1) at $DEST"
say "next: make disk.img (tools/selfhost-stage.sh --tools picks it up automatically)"
