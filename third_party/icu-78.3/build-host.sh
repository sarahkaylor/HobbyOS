#!/usr/bin/env bash
# build-host.sh -- one-shot host build + smoke of the vendored ICU4C 78.3
#                 (third_party/icu-78.3; L6 lane browser/l6-icu).
#
#   ./build-host.sh                  # JOBS=4; override JOBS / CC / CXX /
#                                    # ICU_PREFIX / CONFIGURE_EXTRA
#
# Stages: fetch.sh (download + 3-digest gate + extract) -> out-of-tree
# configure (static libs, --with-data-packaging=static) -> make -> make
# install -> smoke/run-smoke.sh.  All build output is teed to build.log;
# smoke runs land in build-host/smoke/ + build-host/SMOKE.RESULT.
#
# Why these flags (full rationale in README.md):
#   --disable-shared --enable-static   static libs for a bare-metal target
#   --with-data-packaging=static       data compiled into libicudata.a; no
#                                      .dat file / filesystem at run time
#   --disable-samples/tests/extras     nothing WebKit needs
#   --disable-icuio                    libicuio (C++ streams) unused
#   tools stay ENABLED                 genrb/pkgdata/icupkg are needed for the
#                                      data build, and the same build tree is
#                                      the --with-cross-build host-tools
#                                      staging area for the future target
#                                      cross build (see cross-notes.md)
#   renaming stays ENABLED (default)   WPE 2.54 defines U_DISABLE_RENAMING=1
#                                      only on Apple; Linux/WPE builds use the
#                                      default versioned symbols, so the
#                                      library must be built the same way
set -euo pipefail

# macOS-only guard for a host SDK that the installed clang cannot read.  A
# newer CLT/Xcode SDK can ship libSystem.tbd with arch syntax the local clang
# does not know yet (macOS 27: `arm64e.x1-macos` -> "ld: tapi error: malformed
# file ... unknown architecture"), which breaks EVERY native link, including
# this host stage.  When the default toolchain cannot link a trivial program,
# pin SDKROOT to the newest installed SDK that does link (27.0 -> 26.5 -> ...).
# No-op on the common path (the probe links fine and nothing changes).
if [ "$(uname -s)" = "Darwin" ] && [ -z "${SDKROOT:-}" ]; then
  cc_probe="$(mktemp "${TMPDIR:-/tmp}/icu-hostcc-XXXXXX.c")"
  printf 'int main(void){return 0;}\n' > "$cc_probe"
  if ! cc "$cc_probe" -o "$cc_probe.out" 2>/dev/null; then
    for sdk_root in /Library/Developer/CommandLineTools/SDKs \
                    /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs; do
      [ -d "$sdk_root" ] || continue
      # newest first; skip symlinks (MacOSX.sdk -> 27.0.sdk etc.) -- the real
      # versioned dir is always listed, so symlinks would only re-try the same.
      for sdk in $(ls -1d "$sdk_root"/MacOSX*.sdk 2>/dev/null | sort -r); do
        [ -L "$sdk" ] && continue
        if SDKROOT="$sdk" cc "$cc_probe" -o "$cc_probe.out" 2>/dev/null; then
          SDKROOT="$sdk"; export SDKROOT
          echo "== icu build-host: default macOS SDK unparseable by installed clang; using SDKROOT=$sdk ==" >&2
          break 2
        fi
      done
    done
    [ -n "${SDKROOT:-}" ] || echo "== icu build-host: WARNING no linkable macOS SDK found; host stage will fail ==" >&2
  fi
  rm -f "$cc_probe" "$cc_probe.out"
fi

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-4}"
CC="${CC:-cc}"
CXX="${CXX:-c++}"
SRC="$VENDOR_DIR/src"
BUILD_ROOT="$VENDOR_DIR/build-host"
BUILD_DIR="$BUILD_ROOT/obj"
PREFIX="${ICU_PREFIX:-$BUILD_ROOT/prefix}"
CONFIGURE_EXTRA="${CONFIGURE_EXTRA:-}"

mkdir -p "$BUILD_ROOT"
exec > >(tee -a "$BUILD_ROOT/build.log") 2>&1

echo "== icu-78.3 build-host.sh: JOBS=$JOBS CC=$CC CXX=$CXX =="
echo "== host build start: $(date -u '+%Y-%m-%dT%H:%M:%SZ') =="

# Committed vendored scripts are mode 644 (the repo convention is to invoke
# them via `bash script.sh` — see the Makefile's libcxx rules), so call
# fetch.sh through bash rather than relying on the executable bit.
bash "$VENDOR_DIR/fetch.sh"

if [ ! -f "$SRC/source/data/in/icudt78l.dat" ] && [ -f "$SRC/source/data/locales/root.txt" ]; then
  echo "== NOTE: prebuilt data/in/icudt78l.dat is absent while data sources are"
  echo "==       present -> this build will rebuild the data from source"
  echo "==       (no ICU_DATA_FILTER_FILE -> unfiltered; slower)."
  echo "==       build-host-trimmed.sh parked the prebuilt archive."
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ ! -f "$BUILD_DIR/config.status" ]; then
  "$SRC/source/configure" \
    --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --disable-samples --disable-tests --disable-extras --disable-icuio \
    --with-data-packaging=static \
    CC="$CC" CXX="$CXX" $CONFIGURE_EXTRA
else
  echo "== configure already done in $BUILD_DIR (rm -rf it for a fresh build) =="
fi

make -j"$JOBS"
make install

echo "-- installed static libs --"
ls -la "$PREFIX"/lib/libicu*.a
echo "-- sizes (prefix/lib) --"
du -sh "$PREFIX/lib"
size_of() { # GNU stat -c%s vs BSD/macOS stat -f%z
  if [ "$(uname -s)" = "Darwin" ]; then stat -f%z "$1"; else stat -c%s "$1"; fi
}
for f in "$PREFIX"/lib/libicu*.a; do
  printf '%-56s %10s bytes\n' "${f#"$PREFIX"/}" "$(size_of "$f")"
done

bash "$VENDOR_DIR/smoke/run-smoke.sh"

echo "== host build done: $(date -u '+%Y-%m-%dT%H:%M:%SZ') =="
