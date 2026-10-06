#!/usr/bin/env bash
# build.sh -- one-shot host build + smoke of the vendored Skia pin
#             (third_party/skia-588b550, CPU raster, WebKit 2.54.0 recipe).
#
# Stages: fetch.sh (pin gate) -> setup-deps.sh (rootless fontconfig, only
# when SKIA_FONTMGR=fontconfig) -> gen_sources.py (source lists from WebKit's
# CMakeLists, drift-checked) -> cmake configure -> ninja build -> smoke x2.
#
# Env knobs: JOBS (default 4), CMAKE, SKIA_FONTMGR (fontconfig|empty),
#   SKIA_ENCODERS (ON|OFF), SKIA_OPENTYPE_SVG (ON|OFF), SKIA_DEBUG,
#   FREETYPE_PREFIX HARFBUZZ_PREFIX ZLIB_PREFIX PNG_PREFIX JPEG_PREFIX
#   WEBP_PREFIX FONTCONFIG_PREFIX, SKIA_SMOKE_RECORD=1 (accept new checksum).
#
# Evidence lands in build-host/: build.log (everything), smoke-run{1,2}.log,
# smoke{1,2}.rgba(+.ppm), generated/sources-report.json, generated/excluded-gpu.txt.
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-4}"
CMAKE="${CMAKE:-cmake}"
BUILD_DIR="${VENDOR_DIR}/build-host"
GEN_DIR="${BUILD_DIR}/generated"
CMAKE_BUILD_DIR="${BUILD_DIR}/cmake-build"
LOG="${BUILD_DIR}/build.log"
SKIA_FONTMGR="${SKIA_FONTMGR:-fontconfig}"
SKIA_ENCODERS="${SKIA_ENCODERS:-ON}"
SKIA_OPENTYPE_SVG="${SKIA_OPENTYPE_SVG:-ON}"
SKIA_DEBUG="${SKIA_DEBUG:-OFF}"

FREETYPE_PREFIX="${FREETYPE_PREFIX:-$HOME/hobbyos-lanes/l5-fonts/obj/third_party/freetype/prefix}"
HARFBUZZ_PREFIX="${HARFBUZZ_PREFIX:-$HOME/hobbyos-lanes/l5-fonts/obj/third_party/harfbuzz/prefix}"
ZLIB_PREFIX="${ZLIB_PREFIX:-$HOME/hobbyos-lanes/l6-libs1/third_party/zlib-1.3.1/build-host/prefix}"
PNG_PREFIX="${PNG_PREFIX:-$HOME/hobbyos-lanes/l6-libs1/third_party/libpng-1.6.44/build-host/prefix}"
JPEG_PREFIX="${JPEG_PREFIX:-$HOME/hobbyos-lanes/l6-libs1/third_party/libjpeg-turbo-3.1.0/build-host/prefix}"
WEBP_PREFIX="${WEBP_PREFIX:-$HOME/hobbyos-lanes/l6-libs1/third_party/libwebp-1.6.0/build-host/prefix}"
FONTCONFIG_PREFIX="${FONTCONFIG_PREFIX:-$HOME/.local/share/l6-tools/fontconfig-2.17.1}"

mkdir -p "$BUILD_DIR"
exec > >(tee -a "$LOG") 2>&1

log() { printf '[build] %s\n' "$*"; }

log "=== skia-588b550 host build start $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
log "jobs=${JOBS}  build type=Release  Skia pin=588b550a4dd8af90dbe71c0554852806bd8f0b21"
log "cmake=$($CMAKE --version | head -1)"
log "compiler=$(c++ --version | head -1)"
log "host=$(uname -s -m)  nproc=$(nproc)"
log "options: SKIA_FONTMGR=${SKIA_FONTMGR} SKIA_ENCODERS=${SKIA_ENCODERS} SKIA_OPENTYPE_SVG=${SKIA_OPENTYPE_SVG} SKIA_DEBUG=${SKIA_DEBUG}"

# --- host dependency prefixes -------------------------------------------------
for p in FREETYPE_PREFIX HARFBUZZ_PREFIX ZLIB_PREFIX PNG_PREFIX JPEG_PREFIX WEBP_PREFIX; do
  v="${!p}"
  if [ ! -d "$v" ]; then
    log "ERROR: host dependency prefix missing: $p=$v"
    log "       build it via the owning lane (browser/l5-fonts for freetype/harfbuzz,"
    log "       browser/l6-libs1 for zlib/libpng/libjpeg-turbo/libwebp) or set $p to a"
    log "       compatible prefix."
    exit 1
  fi
done
if [ "$SKIA_FONTMGR" = "fontconfig" ] && [ ! -f "${FONTCONFIG_PREFIX}/include/fontconfig/fontconfig.h" ]; then
  "$VENDOR_DIR/setup-deps.sh"
fi

# --- stage 1: fetch + verify the pin -----------------------------------------
"$VENDOR_DIR/fetch.sh"

# --- stage 2: generate source lists from WebKit's CMakeLists ------------------
mkdir -p "$GEN_DIR"
python3 "$VENDOR_DIR/gen_sources.py" \
  --cmake "$VENDOR_DIR/webkit/CMakeLists.txt" \
  --source-root "$VENDOR_DIR/src" \
  --out-dir "$GEN_DIR"

# --- stage 3: configure -------------------------------------------------------
# NOTE: the ENV form of CMAKE_PREFIX_PATH uses ':' separators (platform path
# separator); the CMake VARIABLE (-D) form uses ';'. Getting this wrong makes
# find_package silently search one bogus prefixed path.
PREFIX_LIST="${FREETYPE_PREFIX};${HARFBUZZ_PREFIX};${ZLIB_PREFIX};${PNG_PREFIX};${JPEG_PREFIX};${WEBP_PREFIX};${FONTCONFIG_PREFIX}"
export CMAKE_PREFIX_PATH="${FREETYPE_PREFIX}:${HARFBUZZ_PREFIX}:${ZLIB_PREFIX}:${PNG_PREFIX}:${JPEG_PREFIX}:${WEBP_PREFIX}:${FONTCONFIG_PREFIX}"
export PKG_CONFIG_PATH="${FREETYPE_PREFIX}/lib/pkgconfig:${HARFBUZZ_PREFIX}/lib/pkgconfig:${ZLIB_PREFIX}/lib/pkgconfig:${PNG_PREFIX}/lib/pkgconfig:${JPEG_PREFIX}/lib/pkgconfig:${WEBP_PREFIX}/lib/pkgconfig:${FONTCONFIG_PREFIX}/lib/pkgconfig"
log "CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
log "PKG_CONFIG_PATH=${PKG_CONFIG_PATH}"

"$CMAKE" -S "$VENDOR_DIR" -B "$CMAKE_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_PREFIX_PATH="$PREFIX_LIST" \
  -DSKIA_SOURCE_ROOT="$VENDOR_DIR/src" \
  -DSKIA_GENERATED_DIR="$GEN_DIR" \
  -DSKIA_FONTMGR="$SKIA_FONTMGR" \
  -DSKIA_ENCODERS="$SKIA_ENCODERS" \
  -DSKIA_OPENTYPE_SVG="$SKIA_OPENTYPE_SVG" \
  -DSKIA_DEBUG="$SKIA_DEBUG" \
  -DFreetype_ROOT="$FREETYPE_PREFIX" \
  -DZLIB_ROOT="$ZLIB_PREFIX" \
  -DPNG_ROOT="$PNG_PREFIX" \
  -DJPEG_ROOT="$JPEG_PREFIX"

# --- stage 4: build -----------------------------------------------------------
time "$CMAKE" --build "$CMAKE_BUILD_DIR" -j "$JOBS"
ls -la "$CMAKE_BUILD_DIR/libSkia.a"
log "libSkia.a size: $(du -h "$CMAKE_BUILD_DIR/libSkia.a" | cut -f1)"

# --- stage 5: smoke (two runs, byte-compared) ---------------------------------
BIN="$CMAKE_BUILD_DIR/skia_smoke"
"$BIN" "$BUILD_DIR/smoke1.rgba" | tee "$BUILD_DIR/smoke-run1.log"
"$BIN" "$BUILD_DIR/smoke2.rgba" | tee "$BUILD_DIR/smoke-run2.log"
cmp "$BUILD_DIR/smoke1.rgba" "$BUILD_DIR/smoke2.rgba"
log "smoke reruns byte-identical"

SMOKE_SHA="$(sha256sum "$BUILD_DIR/smoke1.rgba" | cut -d' ' -f1)"
SMOKE_FNV="$(sed -n 's/.*fnv1a64=\(0x[0-9a-f]*\).*/\1/p' "$BUILD_DIR/smoke-run1.log")"
log "SMOKE: sha256(pixels)=${SMOKE_SHA}  fnv1a64=${SMOKE_FNV}  (${BUILD_DIR}/smoke1.rgba)"

EXPECT_FILE="${VENDOR_DIR}/smoke/smoke.sha256"
if [ -f "$EXPECT_FILE" ]; then
  WANT="$(cat "$EXPECT_FILE")"
  if [ "$SMOKE_SHA" != "$WANT" ]; then
    if [ "${SKIA_SMOKE_RECORD:-0}" = "1" ]; then
      log "WARNING: smoke checksum differs (want $WANT, got $SMOKE_SHA) -- SKIA_SMOKE_RECORD=1, re-recording"
      printf '%s\n' "$SMOKE_SHA" > "$EXPECT_FILE"
    else
      log "ERROR: smoke checksum mismatch (want $WANT, got $SMOKE_SHA)"
      log "       If the host toolchain legitimately changed, review the PPM and set SKIA_SMOKE_RECORD=1."
      exit 1
    fi
  else
    log "smoke checksum matches pinned value in smoke/smoke.sha256"
  fi
else
  printf '%s\n' "$SMOKE_SHA" > "$EXPECT_FILE"
  log "recorded first smoke checksum to smoke/smoke.sha256 (review + commit)"
fi

log "=== skia-588b550 host build done $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
