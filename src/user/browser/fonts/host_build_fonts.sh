#!/bin/bash
#
# host_build_fonts.sh — L5 font stack: verified vendored sources, host builds
# (FreeType 2.14.3, HarfBuzz 14.5.0 + ICU) and the deterministic smoke gate.
#
# Reproduces the whole lane from a clean worktree:
#   1. verify every pinned tarball's sha256 (committed .sha256 files);
#   2. extract what is missing (extractions are gitignored);
#   3. host-build FreeType  — static, no optional deps, into
#      obj/third_party/freetype/prefix;
#   4. host-build HarfBuzz  — static, +FreeType +ICU, into
#      obj/third_party/harfbuzz/prefix;
#   5. compile the two smoke binaries from src/user/browser/fonts/ and run
#      each TWICE; diff the outputs byte-for-byte (the SMOKE GATE).
#
# Build trees + logs all live under obj/ (gitignored).  Raw logs:
#   obj/third_party/logs/*.log
#
# Prerequisites (rootless workstation; nothing here needs root):
#   cc/g++ (system gcc), cmake >= 3.20 + ninja, and ICU development files
#   extracted from Ubuntu debs (see third_party/harfbuzz-14.5.0.README.md):
#     mkdir -p ~/.local/share/pkg-tools/icu-dev && cd ~/.local/share/pkg-tools/icu-dev
#     apt-get download libicu-dev && dpkg-deb -x libicu-dev*.deb extracted/
#   Default location: ~/.local/share/pkg-tools/icu-dev/extracted/usr (the
#   matching runtime libicu78 is a system package here); override with
#   ICU_DEV_ROOT.
#
# Env overrides: ICU_DEV_ROOT, CC, CXX, JOBS (default 4), OBJ (build root,
# default <repo>/obj/third_party).
#
# Boundary: ICU here is HOST-ONLY.  The on-device ICU port + data trimming is
# the L6 track's item (browser.md); this script only closes the harfbuzz-ICU
# linkage loop on the host and records it.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
TP="$ROOT/third_party"
OBJ="${OBJ:-$ROOT/obj/third_party}"
LOG="$OBJ/logs"
FT_SRC="$TP/freetype-2.14.3"
HB_SRC="$TP/harfbuzz-14.5.0"
DJ_SRC="$TP/fonts/dejavu-2.37"
FT_PREFIX="$OBJ/freetype/prefix"
HB_PREFIX="$OBJ/harfbuzz/prefix"
FONT="$DJ_SRC/ttf/DejaVuSans.ttf"
BIN="$OBJ/fonts/bin"
SMOKE="$OBJ/fonts/smoke"
ICU_ROOT="${ICU_DEV_ROOT:-$HOME/.local/share/pkg-tools/icu-dev/extracted/usr}"
CC="${CC:-cc}"
CXX="${CXX:-g++}"
JOBS="${JOBS:-4}"

say() { printf '\n== %s\n' "$*"; }

say "prerequisites"
for tool in cmake ninja "$CC" "$CXX" unzip sha256sum; do
  command -v "$tool" >/dev/null || { echo "FATAL: missing tool: $tool" >&2; exit 1; }
done
[ -f "$ICU_ROOT/include/unicode/utypes.h" ] || {
  echo "FATAL: ICU dev files not found under $ICU_ROOT" >&2
  echo "       (rootless recipe: third_party/harfbuzz-14.5.0.README.md)" >&2
  exit 1
}
mkdir -p "$LOG" "$BIN" "$SMOKE"

say "verify pinned tarballs (sha256)"
( cd "$TP" && sha256sum -c freetype-2.14.3.tar.xz.sha256 harfbuzz-14.5.0.tar.xz.sha256 ) \
  | tee "$LOG/tarball-verify.log"
( cd "$TP/fonts" && sha256sum -c dejavu-fonts-ttf-2.37.zip.sha256 ) \
  | tee -a "$LOG/tarball-verify.log"

say "extract pinned sources (gitignored; skipped when present)"
( cd "$TP" && [ -d freetype-2.14.3 ] || tar -xf freetype-2.14.3.tar.xz )
( cd "$TP" && [ -d harfbuzz-14.5.0 ] || tar -xf harfbuzz-14.5.0.tar.xz )
if [ ! -d "$DJ_SRC" ]; then
  tmp=$(mktemp -d "$OBJ/.dejavu-unpack-XXXXXX")
  unzip -q "$TP/fonts/dejavu-fonts-ttf-2.37.zip" -d "$tmp"
  mv "$tmp/dejavu-fonts-ttf-2.37" "$DJ_SRC"
  rmdir "$tmp"
fi
( cd "$TP/fonts" && sha256sum -c dejavu-2.37.sha256 > "$LOG/dejavu-manifest-check.log" )
echo "extraction present; DejaVu files verified against dejavu-2.37.sha256"

say "build FreeType 2.14.3 (host, static, no optional deps)"
if [ -f "$FT_PREFIX/lib/libfreetype.a" ]; then
  echo "SKIP: $FT_PREFIX/lib/libfreetype.a exists (delete obj/third_party/freetype to rebuild)"
else
  mkdir -p "$OBJ/freetype/build"
  (
    cd "$OBJ/freetype/build"
    "$FT_SRC/configure" --prefix="$FT_PREFIX" --disable-shared --enable-static \
      --without-zlib --without-bzip2 --without-png --without-brotli --without-harfbuzz \
      > "$LOG/freetype-configure.log" 2>&1
    make -j"$JOBS" > "$LOG/freetype-make.log" 2>&1
    make install > "$LOG/freetype-install.log" 2>&1
  )
  echo "FreeType build OK: $FT_PREFIX/lib/libfreetype.a"
fi

say "build HarfBuzz 14.5.0 (host, static, +FreeType +ICU)"
if [ -f "$HB_PREFIX/lib/libharfbuzz.a" ]; then
  echo "SKIP: $HB_PREFIX/lib/libharfbuzz.a exists (delete obj/third_party/harfbuzz to rebuild)"
else
  # NB: -isystem for the ICU headers — harfbuzz's CMake links ICU only as a
  # raw library path (its ICU block never calls include_directories), i.e. it
  # assumes system-wide ICU headers.  For the rootless ICU prefix the compiler
  # needs an explicit include path; this is the only change our prefix needs.
  (
    export PKG_CONFIG_PATH="$FT_PREFIX/lib/pkgconfig:$ICU_ROOT/lib/x86_64-linux-gnu/pkgconfig"
    cmake -S "$HB_SRC" -B "$OBJ/harfbuzz/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$HB_PREFIX" \
      -DBUILD_SHARED_LIBS=OFF \
      -DHB_HAVE_FREETYPE=ON -DHB_HAVE_ICU=ON \
      -DICU_ROOT="$ICU_ROOT" \
      -DCMAKE_PREFIX_PATH="$FT_PREFIX;$ICU_ROOT" \
      -DCMAKE_CXX_FLAGS="-isystem $ICU_ROOT/include" \
      > "$LOG/harfbuzz-configure.log" 2>&1
    cmake --build "$OBJ/harfbuzz/build" --parallel "$JOBS" > "$LOG/harfbuzz-build.log" 2>&1
    cmake --install "$OBJ/harfbuzz/build" > "$LOG/harfbuzz-install.log" 2>&1
  )
  echo "HarfBuzz build OK: $HB_PREFIX/lib/libharfbuzz.a (+ -icu, -subset, -raster, -vector, -gpu)"
fi

say "compile smoke binaries"
"$CC" -O2 -Wall -Wextra -I"$FT_PREFIX/include/freetype2" \
  "$ROOT/src/user/browser/fonts/ft_memface_smoke.c" -o "$BIN/ft_memface_smoke" \
  "$FT_PREFIX/lib/libfreetype.a" -lm -pthread > "$LOG/smoke-build-ft.log" 2>&1
"$CC" -O2 -Wall -Wextra -c -I"$HB_PREFIX/include/harfbuzz" \
  -I"$FT_PREFIX/include/freetype2" -I"$ICU_ROOT/include" \
  "$ROOT/src/user/browser/fonts/hb_shape_smoke.c" -o "$OBJ/fonts/hb_shape_smoke.o" \
  > "$LOG/smoke-build-hb-c.log" 2>&1
"$CXX" "$OBJ/fonts/hb_shape_smoke.o" -o "$BIN/hb_shape_smoke" \
  "$HB_PREFIX/lib/libharfbuzz-icu.a" "$HB_PREFIX/lib/libharfbuzz.a" \
  "$FT_PREFIX/lib/libfreetype.a" \
  -L"$ICU_ROOT/lib/x86_64-linux-gnu" -licuuc -licudata -lm -pthread \
  > "$LOG/smoke-build-hb-link.log" 2>&1
echo "smokes compiled: $BIN/ft_memface_smoke $BIN/hb_shape_smoke"

say "smoke gate: run twice, expect byte-identical output"
for run in run1 run2; do
  rd="$SMOKE/$run"
  mkdir -p "$rd"
  ( cd "$rd" && env -u FREETYPE_PROPERTIES LC_ALL=C \
      "$BIN/ft_memface_smoke" "$FONT" ft_bitmap_dump.bin > ft_stdout.txt 2>&1 )
  ( cd "$rd" && env -u FREETYPE_PROPERTIES LC_ALL=C \
      "$BIN/hb_shape_smoke" "$FONT" > hb_stdout.txt 2>&1 )
done
if diff -u "$SMOKE/run1/ft_stdout.txt" "$SMOKE/run2/ft_stdout.txt" > "$LOG/smoke-diff-ft.log" \
   && diff -u "$SMOKE/run1/hb_stdout.txt" "$SMOKE/run2/hb_stdout.txt" > "$LOG/smoke-diff-hb.log" \
   && cmp -s "$SMOKE/run1/ft_bitmap_dump.bin" "$SMOKE/run2/ft_bitmap_dump.bin"; then
  echo "SMOKE GATE: two runs byte-identical (ft stdout, hb stdout, ft bitmap dump)"
else
  echo "SMOKE GATE FAILED: runs differ (see $LOG/smoke-diff-*.log)" >&2
  exit 1
fi

say "evidence summary (absolute paths)"
sha256sum "$FT_PREFIX/lib/libfreetype.a" "$HB_PREFIX/lib/libharfbuzz.a" \
  "$HB_PREFIX/lib/libharfbuzz-icu.a" "$BIN/ft_memface_smoke" "$BIN/hb_shape_smoke" \
  "$SMOKE/run1/ft_bitmap_dump.bin" "$FONT" | tee "$LOG/artifact-sha256.log"
grep -h "TOTAL" "$SMOKE/run1/ft_stdout.txt" "$SMOKE/run1/hb_stdout.txt"
echo "ft stdout : $SMOKE/run1/ft_stdout.txt"
echo "hb stdout : $SMOKE/run1/hb_stdout.txt"
echo "logs      : $LOG/"
echo "L5 FONT STACK HOST BUILD + SMOKE GATE: PASS"
