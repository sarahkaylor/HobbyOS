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

# macOS ships no GNU `nproc`; resolve the job count portably (the Makefile
# passes -j externally but the script's internal ninja/make calls need it).
if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif [ "$(uname -s)" = "Darwin" ]; then
  JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
else
  JOBS=4
fi
export JOBS

CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

say() { echo "[llvm-dist] $*"; }
die() { echo "[llvm-dist] ERROR: $*" >&2; exit 1; }

# macOS-only: the default SDK may ship a libSystem.tbd the installed clang
# cannot parse (macOS 27: `arm64e.x1-macos` -> "ld: tapi error: malformed
# file ... unknown architecture"), which breaks EVERY native link — including
# the host tablegen build below.  When the default toolchain cannot link a
# trivial program, pin SDKROOT to the newest installed SDK that does link
# (27.0 -> 26.5 -> ...), exactly like third_party/icu-78.3/build-host.sh.
# No-op on the common path (the probe links fine and nothing changes).  The
# cross (aarch64-none-elf) stages are freestanding and unaffected; only the
# native host stages (tablegen, install tooling) need a linkable SDK.
if [ "$(uname -s)" = "Darwin" ] && [ -z "${SDKROOT:-}" ]; then
  cc_probe="$(mktemp "${TMPDIR:-/tmp}/llvm-dist-hostcc-XXXXXX.c")"
  printf 'int main(void){return 0;}\n' > "$cc_probe"
  if ! cc "$cc_probe" -o "$cc_probe.out" 2>/dev/null; then
    for sdk_root in /Library/Developer/CommandLineTools/SDKs \
                    /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs; do
      [ -d "$sdk_root" ] || continue
      # newest first; skip symlinks (MacOSX.sdk -> 27.0.sdk etc.) — the real
      # versioned dir is always listed, so symlinks would only re-try the same.
      for sdk in $(ls -1d "$sdk_root"/MacOSX*.sdk 2>/dev/null | sort -r); do
        [ -L "$sdk" ] && continue
        if SDKROOT="$sdk" cc "$cc_probe" -o "$cc_probe.out" 2>/dev/null; then
          SDKROOT="$sdk"; export SDKROOT
          say "default macOS SDK unparseable by installed cc; using SDKROOT=$sdk"
          break 2
        fi
      done
    done
    [ -n "${SDKROOT:-}" ] || say "WARNING: no linkable macOS SDK found — host tablegen build will fail"
  fi
  rm -f "$cc_probe" "$cc_probe.out"
fi

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
# The Makefile hardcodes the Homebrew LLVM bin dir on macOS (CC/OBJCOPY/AR
# at /opt/homebrew/opt/llvm/bin), but that dir is not necessarily on PATH.
# Prepend it (mirroring the Makefile) so an installed-but-not-exported LLVM
# toolchain still passes the check below.  On Linux the tools are expected
# on PATH, as in the Makefile's linux branch.
case "$(uname -s)" in
  Darwin)
    if [ -d /opt/homebrew/opt/llvm/bin ] && ! printf ':%s:' "$PATH" | grep -q ":/opt/homebrew/opt/llvm/bin:"; then
      PATH="/opt/homebrew/opt/llvm/bin:$PATH"
      export PATH
      say "prepended /opt/homebrew/opt/llvm/bin to PATH (Mac LLVM tools found there)"
    fi
    ;;
esac

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
  say "  log: $LOG; cold cost: hours (-j$JOBS)"
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
make obj/arm/libc.a obj/arm/crt0.o obj/arm/libcxx.a -j"$JOBS" 2>&1 | tee -a "$LOG"
cp -a obj/arm/crt0.o obj/arm/libc.a obj/arm/libcxx.a "$WORK/sysroot/lib/"

# Empty stub archives for the host-style -l flags LLVM's configure adds
# (HAVE_LIBDL/HAVE_LIBRT are compile-only probes, so -ldl/-lrt/-lpthreads/
# -lm land on the link line even though HobbyOS has no such libraries).
# The toolchain file already adds -L$SYSROOT/lib/stubs; these empty archives
# resolve the -l flags with no members, leaving the whole closure to resolve
# from libc.a/libcxx.a (llvm-bootstrap.md §6.1 blockers — the exact gap).
# Idempotent.
# Resolve an archiver for the stubs.  They are EMPTY archives whose only job
# is to satisfy -ldl/-lrt/-lpthreads/-lm, so any ar works.  macOS/Homebrew
# puts `llvm-ar` on PATH; Debian/Ubuntu ship it suffixed (llvm-ar-N, or
# unsuffixed under /usr/lib/llvm-<ver>/bin) and the Makefile's Linux branch
# uses GNU `ar` — fall through the whole ladder before giving up.
AR_TOOL=""
for c in llvm-ar ar; do
  if command -v "$c" >/dev/null 2>&1; then AR_TOOL="$c"; break; fi
done
if [ -z "$AR_TOOL" ]; then
  for d in /usr/lib/llvm-*/bin; do
    [ -x "$d/llvm-ar" ] && { AR_TOOL="$d/llvm-ar"; break; }
  done
fi
[ -n "$AR_TOOL" ] || die "no archiver (llvm-ar / ar) on PATH — cannot stub the host -l flags"
mkdir -p "$WORK/sysroot/lib/stubs"
for l in pthreads rt dl m; do
  [ -f "$WORK/sysroot/lib/stubs/lib$l.a" ] || "$AR_TOOL" rcs "$WORK/sysroot/lib/stubs/lib$l.a"
done

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
ninja -C "$WORK/llvm-build" llvm-ar llvm-objcopy -j"$JOBS" 2>&1 | tee -a "$LOG"
say "building clang + lld (long phase — hours cold; log: $LOG)"
ninja -C "$WORK/llvm-build" clang lld -j"$JOBS" 2>&1 | tee -a "$LOG"

# ---- 7) install-distribution -> the persistent staging ---------------
say "staging install-distribution -> $DEST (DESTDIR)"
mkdir -p "$DEST"
DESTDIR="$ROOT/$DEST" ninja -C "$WORK/llvm-build" install-distribution -j8 2>&1 | tee -a "$LOG"

[ -f "$DEST/usr/local/bin/clang-21" ] || die "install-distribution produced no clang-21 at $DEST"
say "done. staged:"
ls -la "$DEST/usr/local/bin" | awk '{print "  "$5"  "$9}' | sed '/^  *$/d'
say "size: $(du -sh "$DEST" | cut -f1) at $DEST"
say "next: make disk.img (tools/selfhost-stage.sh --tools picks it up automatically)"
