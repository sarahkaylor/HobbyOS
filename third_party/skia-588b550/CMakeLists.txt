cmake_minimum_required(VERSION 3.20)
project(skia-host-cpu LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Standalone HOST build of the Skia source tree WebKit 2.54.0 vendors, in
# CPU-raster mode.  This file reproduces Source/ThirdParty/skia/CMakeLists.txt
# (vendored verbatim in webkit/CMakeLists.txt, sha256 571da2e0...) with these
# documented deltas (line numbers below refer to that file):
#
#   D1  CPU raster only: src/gpu/** sources are dropped by gen_sources.py and
#       SK_GL / SK_GANESH are NOT defined.  WebKit hardcodes them for WPE's
#       GPU path ("# Enable the Ganesh GPU renderer." webkit/CMakeLists.txt
#       :1064-1071).  HobbyOS has no GL/EGL.
#   D2  SK_ASSUME_GL*/SK_ASSUME_WEBGL (:1079-1083) only affect GPU/WebGL code
#       (dropped with D1).  SK_TRIVIAL_ABI (:1060-1062) stays gated on Clang;
#       this build uses GCC, exactly as WebKit's GCC Linux builds skip it.
#   D3  The framework header symlinks (:1006-1018) are recreated under
#       ${CMAKE_BINARY_DIR}/framework instead of WebKit's
#       Skia_FRAMEWORK_HEADERS_DIR.  Consumers still include <skia/core/...>.
#   D4  WebKit's WEBKIT_ADD_TARGET_CXX_FLAGS / WEBKIT_CHECK_COMPILER_FLAGS
#       macros are inlined (:976-996, :1127-1179); -fno-exceptions -fno-rtti
#       are added to match WebKit's global compiler flags
#       (Source/cmake/WebKitCompilerFlags.cmake:203-204).
#   D5  find_package(EXPAT REQUIRED) replaces WebKit's fatal-if-missing check
#       (:887-891) -- same requirement, expressed as a CMake find.
#   D6  The font manager is switchable: SKIA_FONTMGR=fontconfig (WPE's shape,
#       requires the fontconfig headers) or =empty (SkFontMgr_custom_empty
#       from Skia's gn/ports.gni; the shape HobbyOS's target leg will use).
#       WebKit itself is fontconfig-only.
#
# Everything else -- source lists, compile definitions, warning suppressions,
# per-source SIMD flags, link libraries -- comes from webkit/CMakeLists.txt;
# gen_sources.py regenerates the lists from that file and hard-fails if its
# shape changes.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SKIA_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH
    "extracted Skia tree (created by fetch.sh)")
if (NOT EXISTS "${SKIA_SOURCE_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "Skia tree not found at ${SKIA_SOURCE_ROOT} -- run ./fetch.sh first")
endif ()

set(SKIA_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH
    "directory holding sources.cmake from gen_sources.py")
if (NOT EXISTS "${SKIA_GENERATED_DIR}/sources.cmake")
  message(FATAL_ERROR "missing ${SKIA_GENERATED_DIR}/sources.cmake -- run build.sh (gen_sources.py)")
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(SKIA_FONTMGR "fontconfig" CACHE STRING
    "fontconfig (WPE shape) | empty (SkFontMgr_custom_empty, HobbyOS shape)")
set_property(CACHE SKIA_FONTMGR PROPERTY STRINGS fontconfig empty)
option(SKIA_ENCODERS "Build Skia image encoders (WebKit USE_SKIA_ENCODERS=ON)" ON)
option(SKIA_OPENTYPE_SVG "Build SVG/OpenType-SVG modules (WebKit USE_SKIA_OPENTYPE_SVG=ON)" ON)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_SKIA_DEBUG_DEFAULT ON)
else ()
  set(_SKIA_DEBUG_DEFAULT OFF)
endif ()
option(SKIA_DEBUG "Enable internal Skia consistency checks" ${_SKIA_DEBUG_DEFAULT})

# --- dependencies (webkit/CMakeLists.txt:3-15, :887-891) --------------------
# Skia dependencies not used directly in WebKit.
find_package(Freetype 2.9.0 REQUIRED)
if (SKIA_FONTMGR STREQUAL "fontconfig")
  find_package(Fontconfig 2.13.0 REQUIRED)
else ()
  message(STATUS "SKIA_FONTMGR=empty: fontconfig not required (D6)")
endif ()
find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
if (SKIA_ENCODERS)
  find_package(WebP REQUIRED COMPONENTS mux)
endif ()
if (SKIA_OPENTYPE_SVG)
  find_package(EXPAT REQUIRED)
  find_package(HarfBuzz REQUIRED)
endif ()
find_package(Threads REQUIRED)

include("${SKIA_GENERATED_DIR}/sources.cmake")

# --- target (webkit:21-812 MAIN + selected conditional blocks) --------------
add_library(Skia STATIC
  ${SKIA_MAIN_SOURCES_CPU}
  ${SKIA_POSIX_SOURCES}
)
if (SKIA_FONTMGR STREQUAL "fontconfig")
  target_sources(Skia PRIVATE ${SKIA_FONTCONFIG_SOURCES})
else ()
  target_sources(Skia PRIVATE ${SKIA_CUSTOM_FONTMGR_SOURCES})
endif ()
if (SKIA_ENCODERS)
  target_sources(Skia PRIVATE ${SKIA_ENC_SOURCES})
else ()
  target_sources(Skia PRIVATE ${SKIA_ENC_STUB_SOURCES})
endif ()
if (SKIA_OPENTYPE_SVG)
  target_sources(Skia PRIVATE ${SKIA_SVG_SOURCES})
  # webkit/CMakeLists.txt:897-904 (the SVG block's compile definitions -- the
  # skshaper module headers gate declarations on SK_SHAPER_* / the legacy guard).
  target_compile_definitions(Skia PRIVATE
    SK_ENABLE_SVG
    SK_DISABLE_LEGACY_SKSHAPER_FUNCTIONS
    SK_SHAPER_HARFBUZZ_AVAILABLE
    SK_SHAPER_PRIMITIVE_AVAILABLE
  )
endif ()

# --- link libraries (webkit:834, :851, :878, :893-896, :971-974) -------------
target_link_libraries(Skia PRIVATE
  Freetype::Freetype
  JPEG::JPEG
  PNG::PNG
  ZLIB::ZLIB
  Threads::Threads
)
if (SKIA_FONTMGR STREQUAL "fontconfig")
  target_link_libraries(Skia PRIVATE Fontconfig::Fontconfig)
endif ()
if (SKIA_ENCODERS)
  target_link_libraries(Skia PRIVATE WebP::mux)
endif ()
if (SKIA_OPENTYPE_SVG)
  target_link_libraries(Skia PRIVATE EXPAT::EXPAT HarfBuzz::HarfBuzz)
endif ()

# --- compile flags (webkit:976-996 + WebKit global -fno-exceptions/-fno-rtti)
target_compile_options(Skia PRIVATE
  -ffp-contract=off
  -fstrict-aliasing
  -fno-exceptions
  -fno-rtti
  -Wno-attributes
  -Wno-cast-align
  -Wno-dangling-reference
  -Wno-deprecated
  -Wno-maybe-musttail-local-addr
  -Wno-psabi
  -Wno-redundant-move
  -Wno-suggest-attribute=format
  -Wno-nontrivial-memcall
  -Wno-undef
  -Wno-uninitialized
  -Wno-unused-parameter
)

# --- include dirs + header symlinks (webkit:998-1018, D3) --------------------
target_include_directories(Skia
  PUBLIC
    "${SKIA_SOURCE_ROOT}"
  INTERFACE
    "${CMAKE_BINARY_DIR}/framework/top"
    "${CMAKE_BINARY_DIR}/framework/mod"
)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/framework/top")
if (NOT IS_SYMLINK "${CMAKE_BINARY_DIR}/framework/top/skia")
  file(CREATE_LINK "${SKIA_SOURCE_ROOT}/include"
       "${CMAKE_BINARY_DIR}/framework/top/skia" SYMBOLIC)
endif ()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/framework/mod/skia/modules")
if (NOT IS_SYMLINK "${CMAKE_BINARY_DIR}/framework/mod/skia/modules/svg")
  file(CREATE_LINK "${SKIA_SOURCE_ROOT}/modules/svg/include"
       "${CMAKE_BINARY_DIR}/framework/mod/skia/modules/svg" SYMBOLIC)
endif ()

# --- compile definitions (webkit:1020-1052) ----------------------------------
target_compile_definitions(Skia PRIVATE
  SKIA_IMPLEMENTATION=1
  SK_CODEC_DECODES_PNG
  SK_GAMMA_APPLY_TO_A8
  "SK_USER_CONFIG_HEADER=\"./WebKitSkiaConfig.h\""
)
target_compile_definitions(Skia PUBLIC
  SK_DISABLE_LEGACY_GL_MAKE_NATIVE_INTERFACE
  SK_DISABLE_LEGACY_IMAGE_READBUFFER
  SK_DISABLE_LEGACY_INIT_DECODERS
  SK_DISABLE_LEGACY_PNG_WRITEBUFFER
  SK_DISABLE_TRACING
  SK_DISABLE_WEBGL_INTERFACE
  SK_ENABLE_PRECOMPILE
  $<IF:$<BOOL:${SKIA_DEBUG}>,SK_DEBUG,SK_RELEASE>
)

# D2: SK_TRIVIAL_ABI only for Clang (webkit:1060-1062)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_definitions(Skia PUBLIC "SK_TRIVIAL_ABI=[[clang::trivial_abi]]")
endif ()

# D1: SK_GL / SK_GANESH / SK_ASSUME_* intentionally NOT defined (webkit:1064-1083).

target_compile_definitions(Skia PRIVATE SK_TYPEFACE_FACTORY_FREETYPE)  # webkit:1085-1089

# byte order (webkit:1091-1104)
include(TestBigEndian)
test_big_endian(SKIA_BIG_ENDIAN)
if (SKIA_BIG_ENDIAN)
  target_compile_definitions(Skia PUBLIC
    I_ACKNOWLEDGE_SKIA_DOES_NOT_SUPPORT_BIG_ENDIAN
    SK_R32_SHIFT=24 SK_G32_SHIFT=16 SK_B32_SHIFT=8 SK_A32_SHIFT=0
  )
else ()
  target_compile_definitions(Skia PUBLIC SK_R32_SHIFT=16)
endif ()

# --- SkCMS / SkOpts SIMD flags (webkit:1107-1158) ----------------------------
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mf16c" SKIA_CC_mf16c)
check_cxx_compiler_flag("-mavx2" SKIA_CC_mavx2)
check_cxx_compiler_flag("-mavx512f" SKIA_CC_mavx512f)
check_cxx_compiler_flag("-mavx512dq" SKIA_CC_mavx512dq)
check_cxx_compiler_flag("-mavx512cd" SKIA_CC_mavx512cd)
check_cxx_compiler_flag("-mavx512bw" SKIA_CC_mavx512bw)
check_cxx_compiler_flag("-mavx512vl" SKIA_CC_mavx512vl)
set(Skia_SkCMS_HSW_OPTS FALSE)
set(Skia_SkCMS_SKX_OPTS FALSE)
set(Skia_SkCMS_HSW_FLAGS -ffp-contract=off -mf16c -mavx2)
set(Skia_SkCMS_SKX_FLAGS -ffp-contract=off -mavx512f -mavx512dq -mavx512cd -mavx512bw -mavx512vl)
if (SKIA_CC_mf16c AND SKIA_CC_mavx2)
  set(Skia_SkCMS_HSW_OPTS TRUE)
endif ()
if (SKIA_CC_mavx512f AND SKIA_CC_mavx512dq AND SKIA_CC_mavx512cd AND SKIA_CC_mavx512bw AND SKIA_CC_mavx512vl)
  set(Skia_SkCMS_SKX_OPTS TRUE)
endif ()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  target_compile_definitions(Skia PRIVATE SK_ENABLE_AVX512_OPTS)
  check_cxx_compiler_flag("-march=x86-64-v3" SKIA_CC_x86_64_v3)
  if (SKIA_CC_x86_64_v3)
    set_source_files_properties("${SKIA_SOURCE_ROOT}/src/opts/SkOpts_hsw.cpp"
      PROPERTIES COMPILE_OPTIONS "-march=x86-64-v3")
  else ()
    set_source_files_properties("${SKIA_SOURCE_ROOT}/src/opts/SkOpts_hsw.cpp"
      PROPERTIES COMPILE_OPTIONS "-march=haswell")
  endif ()
  check_cxx_compiler_flag("-march=x86-64-v4" SKIA_CC_x86_64_v4)
  check_cxx_compiler_flag("-mprefer-vector-width=512" SKIA_CC_pvw512)
  if (SKIA_CC_x86_64_v4 AND SKIA_CC_pvw512)
    set_source_files_properties("${SKIA_SOURCE_ROOT}/src/opts/SkOpts_skx.cpp"
      PROPERTIES COMPILE_OPTIONS "-march=x86-64-v4;-mprefer-vector-width=512")
  else ()
    set_source_files_properties("${SKIA_SOURCE_ROOT}/src/opts/SkOpts_skx.cpp"
      PROPERTIES COMPILE_OPTIONS "-march=skylake-avx512")
  endif ()
endif ()

if (Skia_SkCMS_HSW_OPTS)
  set_source_files_properties("${SKIA_SOURCE_ROOT}/modules/skcms/src/skcms_TransformHsw.cc"
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-mf16c;-mavx2")
else ()
  target_compile_definitions(Skia PRIVATE SKCMS_DISABLE_HSW)
endif ()
if (Skia_SkCMS_SKX_OPTS)
  set_source_files_properties("${SKIA_SOURCE_ROOT}/modules/skcms/src/skcms_TransformSkx.cc"
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-mavx512f;-mavx512dq;-mavx512cd;-mavx512bw;-mavx512vl")
else ()
  target_compile_definitions(Skia PRIVATE SKCMS_DISABLE_SKX)
endif ()

add_library(Skia::Skia ALIAS Skia)  # webkit:1181

# --- smoke (draws a fixed scene, hashes the pixels) --------------------------
add_executable(skia_smoke smoke/skia_smoke.cpp)
target_link_libraries(skia_smoke PRIVATE Skia)
