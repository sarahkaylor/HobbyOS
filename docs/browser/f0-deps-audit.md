# F0 dependency & build audit — WPE WebKit 2.54.0

Audit of the pinned fork checkout `/home/sarah/webkit-hobbyos` (`git describe --tags` → `webkitgtk-2.54.0`,
commit `5220e80b97a253c60ed899361654142ab5021998`). All paths below are relative to that tree root.
Every claim cites `path:line` plus a verbatim excerpt. `NOT FOUND` = searched for and absent (search shown).
No build was run; this is a static audit. Items explicitly marked `SPECULATION` are judgement, not quotes.

Port under audit: `WPE` (and `JSCOnly` where noted). WPE is selectable via `-DPORT=WPE`:
`Source/cmake/WebKitCommon.cmake:75-84` — `set(ALL_PORTS GTK IOS JSCOnly Mac PlayStation WPE Win)` /
`set(PORT "NOPORT" CACHE STRING "choose which WebKit port to build (one of ${ALL_PORTS})")`.
Project version inside the tree: `Source/cmake/OptionsWPE.cmake:4` — `SET_PROJECT_VERSION(2 53 92)`.

---

## 1. MIN VERSIONS

### 1.1 WPE port — hard-required packages (all `REQUIRED`, checked in `Source/cmake/OptionsWPE.cmake:12-26`)

| Dependency | Min version | Controlling line (verbatim) |
|---|---|---|
| GLib | 2.70.0 (GioUnix, Thread, Module) | `:12` `find_package(GLib 2.70.0 REQUIRED COMPONENTS GioUnix Thread Module)` |
| HarfBuzz | 2.7.4 (+ ICU component) | `:13` `find_package(HarfBuzz 2.7.4 REQUIRED COMPONENTS ICU)` |
| ICU | 70.1 (data, i18n, uc) | `:14` `find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)` |
| libjpeg (JPEG) | none stated → NOT FOUND (see 1.3) | `:15` `find_package(JPEG REQUIRED)` |
| Epoxy | 1.5.4 | `:16` `find_package(Epoxy 1.5.4 REQUIRED)` |
| libgcrypt | 1.7.0 | `:17` `find_package(LibGcrypt 1.7.0 REQUIRED)` |
| libsoup3 | 3.0.0 | `:18` `find_package(Soup3 3.0.0 REQUIRED)` |
| libtasn1 (Tasn1) | none stated → NOT FOUND | `:19` `find_package(Tasn1 REQUIRED)` |
| xkbcommon | 0.4.0 | `:20` `find_package(XkbCommon 0.4.0 REQUIRED)` |
| libxml2 | 2.9.13 | `:21` `find_package(LibXml2 2.9.13 REQUIRED)` |
| libpng (PNG) | none stated → NOT FOUND (see 1.3) | `:22` `find_package(PNG REQUIRED)` |
| SQLite3 | none stated → NOT FOUND (see 1.3) | `:23` `find_package(SQLite3 REQUIRED)` |
| Threads | — | `:24` `find_package(Threads REQUIRED)` |
| libwebp (+ demux) | none stated → NOT FOUND (see 1.3) | `:25` `find_package(WebP REQUIRED COMPONENTS demux)` |
| zlib | none stated → NOT FOUND (see 1.3) | `:26` `find_package(ZLIB REQUIRED)` |
| FreeType | 2.9.0 | `:267` `find_package(Freetype 2.9.0 REQUIRED)` |

Notes:
- The GLib floor is linked to WTF headers: `OptionsWPE.cmake:11` — `# Update Source/WTF/wtf/Platform.h to match required GLib versions.`
- HarfBuzz version is cross-checked with a fatal comparison in the finder, `Source/cmake/FindHarfBuzz.cmake:93-95`:
  `if ("${HarfBuzz_FIND_VERSION}" VERSION_GREATER "${HarfBuzz_VERSION}")` /
  `message(FATAL_ERROR "Required version (" ${HarfBuzz_FIND_VERSION} ") is higher than found version (" ${HarfBuzz_VERSION} ")")`.
- ICU defers to CMake's built-in finder for non-Apple: `Source/cmake/FindICU.cmake:37-45` —
  `# Defer to CMake's built-in FindICU by removing ourselves` / `find_package(ICU ${ICU_FIND_VERSION} ... COMPONENTS ${ICU_FIND_COMPONENTS})`.
- Fontconfig is **not** looked up by the WPE port itself; it is pulled in only via Skia (see §2), `Source/ThirdParty/skia/CMakeLists.txt:6` — `find_package(Fontconfig 2.13.0 REQUIRED)`.

### 1.2 JSCOnly port — hard-required

| Dependency | Min version | Verbatim |
|---|---|---|
| ICU | 70.1 (data, i18n, uc) | `Source/cmake/OptionsJSCOnly.cmake:117` `find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)` |
| GLib | 2.70.0 (GioUnix, Object) — only when `EVENT_LOOP_TYPE=GLib` (default is `Generic`) | `:107-108` `if (LOWERCASE_EVENT_LOOP_TYPE STREQUAL "glib")` / `find_package(GLib 2.70.0 REQUIRED COMPONENTS GioUnix Object)` |
| Threads | — | `:1` `find_package(Threads REQUIRED)` |

### 1.3 libjpeg / libpng / libwebp / sqlite / zlib — no minimum for the WPE/JSCOnly ports

The WPE port calls the finders **without a version** (see table 1.1), so no minimum is enforced for those five.
The only pins in-tree are on other ports, shown here for reference:

| Dependency | Pin | Verbatim (other ports only) |
|---|---|---|
| libjpeg | 1.5.2 | `Source/cmake/OptionsWin.cmake:47` `find_package(JPEG 1.5.2 REQUIRED)`; `Source/cmake/OptionsPlayStation.cmake:120` same |
| libpng | 1.6.34 | `Source/cmake/OptionsWin.cmake:50` `find_package(PNG 1.6.34 REQUIRED)` |
| sqlite | 3.23.1 | `Source/cmake/OptionsWin.cmake:51` `find_package(SQLite3 3.23.1 REQUIRED)`; `Source/cmake/OptionsPlayStation.cmake:136` same |
| zlib | 1.2.11 | `Source/cmake/OptionsWin.cmake:52` `find_package(ZLIB 1.2.11 REQUIRED)` |
| libwebp | (none anywhere) | `Source/cmake/FindWebP.cmake:78-80` — `# There's nothing in the WebP headers that could be used to detect the exact` / `# WebP version being used so don't attempt to do so. A version can only be found` / `# through pkg-config`; mismatch handling at `:81-87` warns instead of failing when pkg-config is absent. |

SQLite code paths only *feature-gate* on version, not require it — `Source/WebCore/platform/sql/SQLiteDatabase.cpp:89`
`#if SQLITE_VERSION_NUMBER >= 3007015` (used to decide whether `sqlite3_errstr` exists).

### 1.4 Optional / feature-gated dependencies (only checked when their feature is ON)

| Dependency | Min version | Controlling site (verbatim) |
|---|---|---|
| ATK | 2.16.0 | `Source/cmake/OptionsWPE.cmake:214` `find_package(ATK 2.16.0)` (fatal if `USE_ATK` and missing, `:215-217`) |
| ATK-bridge | none stated → NOT FOUND | `:218` `find_package(ATKBridge)` |
| Flite | 2.2 | `:233` `find_package(Flite 2.2)` (when `ENABLE_SPEECH_SYNTHESIS` + `USE_FLITE`) |
| libSpiel | none stated → NOT FOUND | `:226` `find_package(LibSpiel)` |
| libjxl (JPEG XL) | 0.7.0 | `:245` `find_package(JPEGXL 0.7.0)`; fatal `:247` `message(FATAL_ERROR "libjxl is required for USE_JPEGXL")` |
| libhyphen | none stated → NOT FOUND | `:252` `find_package(Hyphen)` |
| libwoff2 (dec) | 1.0.2 | `:281` `find_package(WOFF2 1.0.2 COMPONENTS dec)` |
| libxslt | 1.1.13 | `:299` `find_package(LibXslt 1.1.13 REQUIRED)` (when `ENABLE_XSLT`) |
| libwpe | none stated → NOT FOUND | `:303` `find_package(WPE REQUIRED)` (when `ENABLE_WPE_LEGACY_API`) |
| libinput | 1.19.0 | `:315` `find_package(LibInput 1.19.0 REQUIRED)` |
| libudev | none stated → NOT FOUND | `:316` `find_package(Udev REQUIRED)` |
| Wayland | 1.20 | `:327` `find_package(Wayland 1.20 REQUIRED)` |
| wayland-protocols | 1.24 | `:328` `find_package(WaylandProtocols 1.24 REQUIRED)` |
| libmanette | 0.2.4 (optional; no fatal) | `:334` `find_package(Manette 0.2.4)` |
| Qt6 (+QuickPrivate/Test) | none stated → NOT FOUND | `:353` `find_package(Qt6 REQUIRED COMPONENTS Core Quick Gui)` (when `ENABLE_WPE_QT_API`) |
| OpenXR | none stated → NOT FOUND | `:365` `find_package(OpenXR REQUIRED CONFIG)` |
| libavif | 0.9.0 | `:378` `find_package(AVIF 0.9.0)`; fatal `:380` `message(FATAL_ERROR "libavif 0.9.0 is required for USE_AVIF.")` |
| journald | none stated → NOT FOUND | `:385` `find_package(Journald)` |
| libcms2 (LCMS2) | none stated → NOT FOUND | `:396` `find_package(LCMS2)` |
| Enchant | none stated → NOT FOUND | `:403` `find_package(Enchant)` (when `ENABLE_SPELLCHECK`) |
| Breakpad | none stated → NOT FOUND | `:410` `find_package(Breakpad REQUIRED)` |
| libdrm | none stated → NOT FOUND | `:450` `find_package(LibDRM)` (when `USE_LIBDRM`) |
| GBM | none stated → NOT FOUND | `:464` `find_package(GBM)` (when `USE_GBM`) |
| libbacktrace | none stated → NOT FOUND | `:475` `find_package(LibBacktrace)` (when `USE_LIBBACKTRACE`) |
| sysprof-capture | none stated → NOT FOUND | `Source/CMakeLists.txt:49-51` `if (USE_SYSTEM_SYSPROF_CAPTURE)` / `find_package(SysProfCapture)` |
| GStreamer | 1.18.4 | `Source/cmake/GStreamerChecks.cmake:3` `find_package(GStreamer 1.18.4 REQUIRED COMPONENTS full)` and `:23` `find_package(GStreamer 1.18.4 REQUIRED COMPONENTS ${GSTREAMER_COMPONENTS})` |
| GStreamer (recorder) | 1.20 | `GStreamerChecks.cmake:45-46` `if (ENABLE_MEDIA_RECORDER AND PC_GSTREAMER_VERSION VERSION_LESS "1.20")` / `message(FATAL_ERROR "GStreamer >= 1.20 is needed for ENABLE_MEDIA_RECORDER.")` |
| librice | 0.4.2 | `GStreamerChecks.cmake:65` `find_package(Rice 0.4.2 COMPONENTS Io Proto)` |
| OpenSSL | 3.0.0 | `GStreamerChecks.cmake:60-61` `if (NOT OPENSSL_FOUND OR OPENSSL_VERSION VERSION_LESS "3.0.0")` / `message(FATAL_ERROR "OpenSSL 3 is needed for ENABLE_WEB_RTC.")` |
| libseccomp / bwrap | bwrap ≥ 0.3.1 (executable check) | `Source/cmake/BubblewrapSandboxChecks.cmake:51-52` `if (NOT "${BWRAP_VERSION}" VERSION_GREATER_EQUAL "0.3.1")` / `message(FATAL_ERROR "bwrap must be >= 0.3.1 but ${BWRAP_VERSION} found")` |
| unifdef | none stated → NOT FOUND | `Source/cmake/FindUnifdef.cmake:8` `find_program(UNIFDEF_EXECUTABLE unifdef)` |
| gperf | 3.0.1 | `Source/cmake/WebKitCommon.cmake:335-338` `if (ENABLE_WEBCORE)` / `find_package(Gperf 3.0.1 REQUIRED)` |

### 1.5 "Recommended" versions — what WebKit CI's jhbuild set pins (`Tools/jhbuild/jhbuild-minimal.modules`)

| Library | Pinned version | Line |
|---|---|---|
| ICU | `version="70.1"` | `:90` |
| HarfBuzz | `version="2.7.4"` (`tag="2.7.4"`) | `:109` |
| FreeType | `version="2.11.1"` (`tag="VER-2-11-1"`) | `:120` |
| GLib | `version="2.72.4"` | `:231` |
| glib-networking | `version="2.70.0"` | `:242` |
| libsoup | `tag="3.6.5"` / `version="3.6.5"` | `:199-200` |
| libxml2 | `version="2.9.13"` (`tag="v2.9.13"`) | `:144-145` |
| libdrm | `version="2.4.114"` | `:184` |
| libavif | `version="0.9.3"` | `:158` |
| libaom | `version="3.3.0"` | `:173` |
| libjxl | `version="0.8.2"` | `:269` |
| libwpe | `version="1.16.0"` | `:72` |
| wpebackend-fdo | `version="1.14.3"` | `:82` |
| libpsl | `version="0.21.0"` | `:211` |
| libvpx | `version="1.15.2"` | `:222` |
| gobject-introspection | `version="1.72.0"` | `:132` |
| libbacktrace | (unpinned) | `:280-284` |

No jhbuild pin exists for zlib/libpng/libjpeg/libwebp/sqlite — those come from system/dev packages listed in
`Tools/glib/dependencies/apt:29-88` (section header `:30` — `# These are dependencies necessary for building WebKitGTK/WPE.`),
e.g. `:57` `libjpeg-dev`, `:65` `libpng-dev`, `:67` `libsqlite3-dev`, `:73` `libwebp-dev`, `:74` `libwoff-dev`,
`:53-55` GStreamer dev packages, `:49` `libepoxy-dev`, `:52` `libgcrypt20-dev`.

---

## 2. SKIA

**Consumption model: bundled source copy, built in-tree; NOT a system dependency, NOT a separate checkout/`gclient`.**

| Question | Finding | Evidence (verbatim) |
|---|---|---|
| Where does Skia live? | Full source vendored under `Source/ThirdParty/skia/` (src/, modules/, third_party/ subdirs; no `.git`) | `ls Source/ThirdParty/skia/` → `CMakeLists.txt  WebKitSkiaConfig.h  include  modules  src  third_party ...` |
| Pinned revision? | Yes — commit `588b550a4dd8af90dbe71c0554852806bd8f0b21`, recorded in a plain text README | `Source/ThirdParty/skia/README.WebKit`: `This directory contains a copy of Skia from the official repository.` / `  - URL: https://skia.googlesource.com/skia` / `  - Commit: 588b550a4dd8af90dbe71c0554852806bd8f0b21` |
| How updated? | Manual re-copy, then bump this file | `README.WebKit`: `The simplest way of updating its contents is to re-create the directory` … `Then check whether the updated sources require any changes to the CMake` / `build system, and update the Skia commit identifier in this file.` |
| Other revision pins? | NOT FOUND — `grep -rn "588b550" .` matches only `Source/ThirdParty/skia/README.WebKit`; `grep -rn "skia.googlesource"` outside the skia dir: none; no `skia_revision`/`SKIA_REVISION` file anywhere; no `.gclient*` files; no `Source/cmake/FindSkia.cmake` and no `find_package(Skia` in the tree | (searches shown above) |
| How is it built? | Static library target, added only when `USE_SKIA` is ON | `Source/ThirdParty/skia/CMakeLists.txt:1` `set_property(DIRECTORY . PROPERTY FOLDER "skia")`; `:21` `add_library(Skia STATIC`; `:1181` `add_library(Skia::Skia ALIAS Skia)`; `Source/CMakeLists.txt:45-47` `if (USE_SKIA)` / `add_subdirectory(ThirdParty/skia)` / `endif ()` |
| Is it ON for WPE? | Yes, by default | `Source/cmake/OptionsWPE.cmake:155` `WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SKIA PRIVATE ON)`; option defined `Source/cmake/WebKitFeatures.cmake:327` `WEBKIT_OPTION_DEFINE(USE_SKIA "Whether to use Skia instead of Cairo." PRIVATE OFF)` |
| What system libs does the Skia build itself demand? | FreeType ≥ 2.9.0 and Fontconfig ≥ 2.13.0 (non-Windows/non-PlayStation); libwebp **mux** if `USE_SKIA_ENCODERS`; Expat + HarfBuzz if `USE_SKIA_OPENTYPE_SVG` (default ON); Epoxy or EGL; JPEG + PNG at link time | `skia/CMakeLists.txt:3-7` `# Skia dependencies not used directly in WebKit.` / `if (NOT WIN32 AND NOT PLAYSTATION)` / `find_package(Freetype 2.9.0 REQUIRED)` / `find_package(Fontconfig 2.13.0 REQUIRED)`; `:9-11` `if (USE_SKIA_ENCODERS)` / `find_package(WebP REQUIRED COMPONENTS mux)`; `:887-895` `find_package(EXPAT)` … `message(FATAL_ERROR "Expat is required by USE_SKIA_OPENTYPE_SVG")` … `target_link_libraries(Skia PRIVATE EXPAT::EXPAT HarfBuzz::HarfBuzz)`; `:855-857` `if (USE_LIBEPOXY)` / `target_link_libraries(Skia PRIVATE Epoxy::Epoxy)`; `:971-974` `target_link_libraries(Skia PRIVATE JPEG::JPEG PNG::PNG)` |
| Are Skia's own `third_party/*` copies (freetype2, libpng, libjpeg-turbo, libwebp, zlib, harfbuzz, icu, …) used? | No — `grep -n "third_party" Source/ThirdParty/skia/CMakeLists.txt` returns nothing; the CMake build links system libraries instead | dirs exist under `Source/ThirdParty/skia/third_party/` (e.g. `freetype2/`, `libpng/`, `libjpeg-turbo/`, `libwebp/`, `zlib/`, `harfbuzz/`, `icu/`), unused by CMake |
| Encoders | `USE_SKIA_ENCODERS` default ON; WebCore actually calls Skia encoders, so they must be built | `WebKitFeatures.cmake:328` `WEBKIT_OPTION_DEFINE(USE_SKIA_ENCODERS "Whether to use Skia image encoders" PRIVATE ON)`; `skia/CMakeLists.txt:868-878` builds `src/encode/SkJpegEncoderImpl.cpp` etc. and links `WebP::mux`, else `:880-884` builds `*_none` stubs; consumer: `Source/WebCore/platform/graphics/skia/ImageUtilitiesSkia.cpp:39-41` `#include <skia/encode/SkJpegEncoder.h>` / `#include <skia/encode/SkPngEncoder.h>` / `#include <skia/encode/SkWebpEncoder.h>` |

Compile-configuration facts worth knowing for a HobbyOS port (all `Source/ThirdParty/skia/CMakeLists.txt`):

- `:1020-1027` `target_compile_definitions(Skia PRIVATE` `SKIA_IMPLEMENTATION=1` … `SK_CODEC_DECODES_PNG` … `SK_USER_CONFIG_HEADER="./WebKitSkiaConfig.h"`.
- `:1060-1062` `if (COMPILER_IS_CLANG)` / `target_compile_definitions(Skia PUBLIC SK_TRIVIAL_ABI=[[clang::trivial_abi]])` — Clang gets an ABI attribute; GCC does not.
- `:1064-1071` `# Enable the Ganesh GPU renderer.` / `# FIXME: Support other combinations.` / `target_compile_definitions(Skia PRIVATE SK_GL SK_GANESH)` — the GL/Ganesh backend is **always** compiled into the Skia target in this tree; there is no option switching it off.
- `:1079-1083` `target_compile_definitions(Skia PUBLIC SK_ASSUME_GL=0 SK_ASSUME_GL_ES=1 SK_ASSUME_WEBGL=0)`.
- `:1085-1089` `else ()` … `target_compile_definitions(Skia PRIVATE SK_TYPEFACE_FACTORY_FREETYPE)` (non-Windows uses the FreeType typeface factory).
- `:1091-1104` big-endian gets `I_ACKNOWLEDGE_SKIA_DOES_NOT_SUPPORT_BIG_ENDIAN` + explicit `SK_R32_SHIFT=24 …`; little-endian gets `SK_R32_SHIFT=16` only.
- Header exposure: `Source/cmake/WebKitFS.cmake:55` `set(Skia_FRAMEWORK_HEADERS_DIR "${CMAKE_BINARY_DIR}/Skia/Headers")`, populated by symlinks at `skia/CMakeLists.txt:1006-1018` (`file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/include" "${Skia_FRAMEWORK_HEADERS_DIR}/top/skia" SYMBOLIC)`, plus `modules/svg`).
- WPE-side switch for SVG-in-OT fonts: `OptionsWPE.cmake:118` `WEBKIT_OPTION_DEFINE(USE_SKIA_OPENTYPE_SVG "Whether to use the Skia built-in support for OpenType SVG fonts." PUBLIC ON)`.
- `WebKitSkiaConfig.h` (the `SK_USER_CONFIG_HEADER`) contains one tuning define:
  `#define GR_AA_TESSELLATOR_MAX_VERB_COUNT 100` (comment: `// Reduce the CPU usage by improving the batching of path tessellator.`).
- `Source/WebCore/platform/Skia.cmake:39-42` shows the consumer targets: `list(APPEND WebCore_LIBRARIES` `HarfBuzz::HarfBuzz` `HarfBuzz::ICU` `Skia::Skia)`; source dirs added at `Source/WebCore/platform/Skia.cmake:1-10` (`platform/graphics/skia`, unified sources `platform/SourcesSkia.txt`).

**Where a pin would live / what must move together when bumping:** `Source/ThirdParty/skia/README.WebKit`
(only revision record), `Source/ThirdParty/skia/CMakeLists.txt` (source list + compile defs; hand-maintained —
the README explicitly warns to "check whether the updated sources require any changes to the CMake build system"),
and `Source/ThirdParty/skia/WebKitSkiaConfig.h` (overrides handed to Skia via `SK_USER_CONFIG_HEADER`).

---

## 3. JSC INTERPRETER-ONLY FLAGS

Sources: `Source/cmake/OptionsJSCOnly.cmake`, `Source/cmake/WebKitFeatures.cmake`, defaults computed per-arch in
`WebKitFeatures.cmake:76-146`.

### 3.1 The flags and their defaults

| Option | Default | Verbatim |
|---|---|---|
| `ENABLE_JIT` | `ON` on `WTF_CPU_ARM64`/`X86_64`, `OFF` otherwise; `OFF` under `USE_64KB_PAGE_BLOCK` | `WebKitFeatures.cmake:92-98` `elseif (WTF_CPU_ARM64 OR WTF_CPU_X86_64)` / `set(ENABLE_JIT_DEFAULT ON)`; `:236` `WEBKIT_OPTION_DEFINE(ENABLE_JIT "Toggle JustInTime JavaScript support" PRIVATE ${ENABLE_JIT_DEFAULT})` |
| `ENABLE_C_LOOP` | `OFF` on ARM64/x86_64/RISCV64; `ON` for "unknown" CPUs; `OFF` for 64KB-page aarch64 | `:92-98` `set(ENABLE_C_LOOP_DEFAULT OFF)`; `:120-126` `else ()` / `set(ENABLE_C_LOOP_DEFAULT ON)`; `:79-91` `if (USE_64KB_PAGE_BLOCK)` … `set(ENABLE_C_LOOP_DEFAULT OFF)`; `:215` `WEBKIT_OPTION_DEFINE(ENABLE_C_LOOP "Enable CLoop interpreter" PRIVATE ${ENABLE_C_LOOP_DEFAULT})` |
| `ENABLE_DFG_JIT` | `${ENABLE_JIT_DEFAULT}` | `:219` `WEBKIT_OPTION_DEFINE(ENABLE_DFG_JIT "Toggle data flow graph JIT tier" PRIVATE ${ENABLE_JIT_DEFAULT})` |
| `ENABLE_FTL_JIT` | `${ENABLE_FTL_DEFAULT}` | `:223` `WEBKIT_OPTION_DEFINE(ENABLE_FTL_JIT "Toggle FTL JIT support" PRIVATE ${ENABLE_FTL_DEFAULT})` |
| `ENABLE_WEBASSEMBLY` | `ON` unless `ENABLE_C_LOOP_DEFAULT`; `OFF` when `ENABLE_C_LOOP_DEFAULT` | `:129-133` `if (ENABLE_C_LOOP_DEFAULT)` / `set(ENABLE_WEBASSEMBLY_DEFAULT OFF)` / `else ()` / `set(ENABLE_WEBASSEMBLY_DEFAULT ON)`; `:297` `WEBKIT_OPTION_DEFINE(ENABLE_WEBASSEMBLY "Toggle WebAssembly support" PRIVATE ${ENABLE_WEBASSEMBLY_DEFAULT})` |
| `ENABLE_WEBASSEMBLY_BBQJIT` / `_OMGJIT` | `${ENABLE_FTL_DEFAULT}` | `:298-299` `WEBKIT_OPTION_DEFINE(ENABLE_WEBASSEMBLY_BBQJIT "Toggle WebAssembly BBQ JIT support" PRIVATE ${ENABLE_FTL_DEFAULT})` / `... OMGJIT ...` |
| `ENABLE_SAMPLING_PROFILER` | `ON` ARM64/x86_64; `OFF` in the fallback and 64KB-page-non-aarch64 cases | `:98` `set(ENABLE_SAMPLING_PROFILER_DEFAULT ON)`; `:126` `set(ENABLE_SAMPLING_PROFILER_DEFAULT OFF)`; `:277` `WEBKIT_OPTION_DEFINE(ENABLE_SAMPLING_PROFILER "Toggle sampling profiler support" PRIVATE ${ENABLE_SAMPLING_PROFILER_DEFAULT})` |
| `USE_SYSTEM_MALLOC` / `USE_MIMALLOC` | ARM64: both `OFF` (bmalloc); unknown CPU: system malloc `ON`; 64KB-page aarch64: mimalloc `ON` | `:95-96` `set(USE_SYSTEM_MALLOC_DEFAULT OFF)` / `set(USE_MIMALLOC_DEFAULT OFF)`; `:123-124`; `:82-83` `set(USE_SYSTEM_MALLOC_DEFAULT OFF)` / `set(USE_MIMALLOC_DEFAULT ON)` |
| `ENABLE_UNIFIED_BUILDS` | `ON` (off only when analyzing) | `:141-146` `if (DEFINED ClangTidy_EXE OR DEFINED IWYU_EXE)` … `set(ENABLE_UNIFIED_BUILDS_DEFAULT ON)`; `:291` |

Enforced relationships (verbatim, `WebKitFeatures.cmake`):

- `:334-337`
  `WEBKIT_OPTION_CONFLICT(ENABLE_JIT ENABLE_C_LOOP)` /
  `WEBKIT_OPTION_CONFLICT(ENABLE_LLVM_PROFILE_GENERATION USE_PGO_PROFILE)` /
  `WEBKIT_OPTION_CONFLICT(ENABLE_SAMPLING_PROFILER ENABLE_C_LOOP)` /
  `WEBKIT_OPTION_CONFLICT(ENABLE_WEBASSEMBLY ENABLE_C_LOOP)`
  enforced by `:384-390` `message(FATAL_ERROR "${_name} conflicts with ${_conflict}. You must disable one or the other.")`
- `:342-345`
  `WEBKIT_OPTION_DEPEND(ENABLE_DFG_JIT ENABLE_JIT)` /
  `WEBKIT_OPTION_DEPEND(ENABLE_FTL_JIT ENABLE_DFG_JIT)` /
  `WEBKIT_OPTION_DEPEND(ENABLE_WEBASSEMBLY_BBQJIT ENABLE_FTL_JIT)` /
  `WEBKIT_OPTION_DEPEND(ENABLE_WEBASSEMBLY_OMGJIT ENABLE_FTL_JIT)`
  auto-disabled with the log `message(STATUS "Disabling ${_name} since ${_dependency} is disabled.")` (`:361-370`).

What `ENABLE_C_LOOP` actually selects (grounded):

- The offline assembler backend switches to the CLoop backend only when JIT is off **and** C_LOOP is on:
  `Source/JavaScriptCore/CMakeLists.txt:320-324` `if (NOT ENABLE_JIT)` / `if (ENABLE_C_LOOP)` / `set(OFFLINE_ASM_BACKEND "C_LOOP")`.
- The runtime path is described as non-JIT: `Source/JavaScriptCore/llint/LLIntThunks.cpp:947-948`
  `#if ENABLE(C_LOOP)` / `// Non-JIT (i.e. C Loop LLINT) case:`.
- Code guarded by `ENABLE(C_LOOP)`, e.g. `Source/JavaScriptCore/runtime/VM.h:863-865`
  `#if ENABLE(C_LOOP)` / `ALWAYS_INLINE CLoopStack& cloopStack() { return traps().cloopStack(); }`; and
  `Source/JavaScriptCore/interpreter/Interpreter.cpp:1623` `#if CPU(ARM64) && CPU(ADDRESS64) && !ENABLE(C_LOOP)`.
- `cloop.rb` is part of the offlineasm Ruby inputs regardless: `Source/JavaScriptCore/CMakeLists.txt:242-262`
  `set(OFFLINE_ASM` … `offlineasm/cloop.rb` (`:249`).

### 3.2 "FFI / asm disable" knobs

- FFI: **NOT FOUND**. Searches: `grep -rn "libffi|USE_LIBFFI|ENABLE_FFI" Source/cmake Source/JavaScriptCore Source/WTF` → no matches;
  `grep -rli "\blibffi\b" …` → no files. There is no FFI option in 2.54.
- The closest "asm off" knobs are the WebAssembly JIT tiers (`ENABLE_WEBASSEMBLY`,
  `ENABLE_WEBASSEMBLY_BBQJIT`, `ENABLE_WEBASSEMBLY_OMGJIT`; quotes in 3.1) — they are the only options that
  add JIT-generated code paths and they *conflict* with `ENABLE_C_LOOP`.
- Debug-only offlineasm knob: `Source/cmake/OptionsCommon.cmake:199-206`
  `option(GCC_OFFLINEASM_SOURCE_MAP "Produce debug line information for offlineasm-generated code" ${GCC_OFFLINEASM_SOURCE_MAP_DEFAULT})`.

### 3.3 Smallest JS-only build recipe (JSCOnly port)

JSCOnly turns the upper layers off itself — `Source/cmake/OptionsJSCOnly.cmake:39-44`:
`set(ENABLE_WEBCORE OFF)` / `set(ENABLE_WEBKIT_LEGACY OFF)` / `set(ENABLE_WEBKIT OFF)` /
`set(ENABLE_WEBINSPECTORUI OFF)` / `set(ENABLE_WEBGL OFF)` / `set(ENABLE_WEBGPU OFF)`; and adds
`:11` `add_definitions(-DBUILDING_JSCONLY__)`.

JSCOnly-only knobs (`OptionsJSCOnly.cmake:18-28`):

| Option | Default | Verbatim |
|---|---|---|
| `ENABLE_STATIC_JSC` | `OFF` (JS engine built `SHARED`; `bmalloc`/`WTF` as `OBJECT`) | `:19` `WEBKIT_OPTION_DEFINE(ENABLE_STATIC_JSC "Whether to build JavaScriptCore as a static library." PUBLIC OFF)`; `:56-60` `if (NOT ENABLE_STATIC_JSC)` / `set(JavaScriptCore_LIBRARY_TYPE SHARED)` |
| `USE_LIBBACKTRACE` | `OFF` | `:20` |
| `ENABLE_REMOTE_INSPECTOR` | `OFF` | `:21` `WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_REMOTE_INSPECTOR PRIVATE OFF)` |
| `ENABLE_FUZZILLI` | `OFF` (non-Windows) | `:23` |
| `ENABLE_JSC_GLIB_API` | `OFF` (no GLib needed; forces the GLib event loop when ON) | `:25`, `:98` |
| `USE_SYSTEM_UNIFDEF` | `OFF` → builds bundled `ThirdParty/unifdef` | `:26`; `Source/CMakeLists.txt:15-18` `if (USE_SYSTEM_UNIFDEF)` / `find_package(Unifdef REQUIRED)` / `else ()` / `add_subdirectory(ThirdParty/unifdef)` |
| `EVENT_LOOP_TYPE` | `Generic` (`GLib` also allowed) | `:30-37` `set(DEFAULT_EVENT_LOOP_TYPE "Generic")` / `set(EVENT_LOOP_TYPE ${DEFAULT_EVENT_LOOP_TYPE} CACHE STRING "Implementation of event loop to be used in JavaScriptCore (one of ${ALL_EVENT_LOOP_TYPES})")` |
| `ENABLE_API_TESTS` | `ON` on non-Windows | `:46-50` |

Interpreter-only invocation (all four `-D` flags below are *required together* on aarch64/x86_64 because of the
defaults in 3.1 + the conflict checks in `WebKitFeatures.cmake:334-337`):

```
-DPORT=JSCOnly \
-DENABLE_JIT=OFF \
-DENABLE_C_LOOP=ON \
-DENABLE_WEBASSEMBLY=OFF \
-DENABLE_SAMPLING_PROFILER=OFF
```

`ENABLE_DFG_JIT`/`ENABLE_FTL_JIT` need not be passed: they default ON (from `ENABLE_JIT_DEFAULT`) but are
auto-disabled by the dependency machinery (`WebKitFeatures.cmake:342-345`). If `ENABLE_C_LOOP` is *not* requested,
JIT-off alone still yields an interpreter via the arch offlineasm LLInt backend (`JavaScriptCore/CMakeLists.txt:306-318`
selects `ARM64`/`X86_64`/… before the C_LOOP override at `:320-324`).

---

## 4. FEATURE TRIM LIST (for HobbyOS v1 minimal browser)

Defaults are `Source/cmake/WebKitFeatures.cmake` unless a WPE override is cited (`Source/cmake/OptionsWPE.cmake:52-97`,
`GStreamerDefinitions.cmake:1-13`, `:137-145`). Flags not listed keep their defaults. Auto-disable behavior applies
(`WEBKIT_OPTION_DEPEND`, `WebKitFeatures.cmake:339-358`) — the OFF list notes where the build turns a flag off for you.

### 4.1 Keep ON

| Flag | Why keep (one line) | Default / citation |
|---|---|---|
| `ENABLE_CONTEXT_MENUS` | Basic right-click UI path; no extra dependency. | `ON` `WebKitFeatures.cmake:212` |
| `ENABLE_FULLSCREEN_API` | DOM/CSS fullscreen; renderer-only. | `ON` `:224` |
| `ENABLE_MATHML` | Pure C++ layout feature. | `ON` `:242` |
| `ENABLE_USER_MESSAGE_HANDLERS` | Script↔embedder messaging (`window.webkit.messageHandlers`). | `ON` `:292` |
| `ENABLE_SMOOTH_SCROLLING` | Scroll behavior for the Skia compositor path. | `ON` `:281` |
| `ENABLE_REMOTE_INSPECTOR` | Debug the port during bring-up; uses built-in networking only. | `ON` `:275` |
| `ENABLE_JAVASCRIPT_SHELL` | Builds `jsc` test shell — useful for W0/W1 bring-up. | `ON` `:235` |
| `ENABLE_UNIFIED_BUILDS` | Build-time win (fewer translation units). | `ON` `:291` |
| `ENABLE_DARK_MODE_CSS` | `prefers-color-scheme` plumbing; no deps. | WPE `ON` `OptionsWPE.cmake:72` |
| `ENABLE_CURSOR_VISIBILITY` | Embedder cursor updates; no deps. | WPE `ON` `OptionsWPE.cmake:71` |
| `ENABLE_DRAG_SUPPORT` | Description includes "selection of text with mouse". | WPE `ON` `OptionsWPE.cmake:73` |
| `ENABLE_ASYNC_SCROLLING` | Scrolling mode used by the WPE compositor. | WPE `ON` `OptionsWPE.cmake:67` |
| `ENABLE_AUTOCAPITALIZE` | Input/IME plumbing; no deps. | WPE `ON` `OptionsWPE.cmake:68` |
| `ENABLE_VARIATION_FONTS` | FreeType variable fonts; no extra dependency. | WPE `ON` `OptionsWPE.cmake:90` |
| `ENABLE_CSS_TAP_HIGHLIGHT_COLOR` | CSS property only. | WPE `ON` `OptionsWPE.cmake:70` |
| `ENABLE_MOUSE_CURSOR_SCALE` | Cursor image scaling; no extra deps. | WPE `ON` `OptionsWPE.cmake:81` |
| `ENABLE_WEBINSPECTORUI` | Inspector front-end resources for debugging (no external dep). | `WebKitCommon.cmake:68-70` `if (NOT DEFINED ENABLE_WEBINSPECTORUI)` / `set(ENABLE_WEBINSPECTORUI ON)` |
| `ENABLE_WPE_PLATFORM` | The WPE 2.0 embedder platform API — the integration surface for HobbyOS. | WPE `ON` `OptionsWPE.cmake:105` |
| `ENABLE_WPE_PLATFORM_HEADLESS` | Headless platform is the natural target for bare-metal bring-up. | WPE `ON` `OptionsWPE.cmake:107` |
| `USE_SKIA` | New WPE compositor is Skia-based (§2). | WPE `ON` `OptionsWPE.cmake:155` |
| `USE_SKIA_ENCODERS` | WebCore calls `SkJpegEncoder`/`SkPngEncoder`/`SkWebpEncoder`; `_none` stubs would break encoding. | `ON` `WebKitFeatures.cmake:328`; consumer `WebCore/platform/graphics/skia/ImageUtilitiesSkia.cpp:39-41`; needs system `WebP::mux` |
| `USE_SKIA_OPENTYPE_SVG` | SVG-in-OpenType font rendering via Skia; requires Expat+HarfBuzz (small add). | WPE `ON` `OptionsWPE.cmake:118`; `skia/CMakeLists.txt:887-896` |

### 4.2 Turn OFF

| Flag | Why OFF (one line) | Default / citation |
|---|---|---|
| `ENABLE_VIDEO` | Video requires the whole GStreamer stack. | `ON` `WebKitFeatures.cmake:294`; WPE `ON` `GStreamerDefinitions.cmake:2` |
| `ENABLE_WEB_AUDIO` | Audio requires GStreamer audio/fft. | `ON` `:315`; WPE `ON` `GStreamerDefinitions.cmake:3` |
| `ENABLE_WEB_CODECS` | Codec API rides on GStreamer (`GStreamerDependencies.cmake:4`). | WPE `ON` `OptionsWPE.cmake:91` |
| `ENABLE_MEDIA_SOURCE` | MSE; depends on `ENABLE_VIDEO` → auto-off. | WPE `ON` `GStreamerDefinitions.cmake:4`; dep `WebKitFeatures.cmake:350` |
| `ENABLE_MEDIA_STREAM` | getUserMedia capture; depends `ENABLE_VIDEO` → auto-off. | WPE `ON` `OptionsWPE.cmake:80`; dep `:351` |
| `ENABLE_MEDIA_RECORDER` | Recording; depends `ENABLE_MEDIA_STREAM` → auto-off. | WPE `ON` `OptionsWPE.cmake:77`; dep `:348` |
| `ENABLE_MEDIA_SESSION`, `ENABLE_MEDIA_SESSION_PLAYLIST` | MPRIS/D-Bus media session; no D-Bus on target. | Linux default `ON` `OptionsWPE.cmake:78`; `:79` |
| `ENABLE_MEDIA_CONTROLS_CONTEXT_MENUS` | Video UI; depends `ENABLE_VIDEO` → auto-off. | WPE `ON` `OptionsWPE.cmake:76`; dep `:347` |
| `ENABLE_VIDEO_USES_ELEMENT_FULLSCREEN` | Video path; depends `ENABLE_VIDEO` → auto-off. | `ON` `:296`; dep `:354` |
| `ENABLE_ENCRYPTED_MEDIA` (+`ENABLE_LEGACY_ENCRYPTED_MEDIA`) | EME; depends `ENABLE_VIDEO`. | WPE `=${ENABLE_EXPERIMENTAL_FEATURES}` `OptionsWPE.cmake:52`; deps `:340-341` |
| `ENABLE_THUNDER` | EME Thunder; dev-mode default. | WPE `=${ENABLE_DEVELOPER_MODE}` `OptionsWPE.cmake:88` |
| `ENABLE_WEB_RTC` | WebRTC; also needs libwebrtc/OpenSSL. | WPE `OFF` `OptionsWPE.cmake:92`; dep `:339` |
| `ENABLE_PICTURE_IN_PICTURE_API`, `ENABLE_WIRELESS_PLAYBACK_TARGET`, `ENABLE_AVF_CAPTIONS`, `ENABLE_AV1` | Media-adjacent; AV1 pulls bundled `ThirdParty/dav1d`. | OFF defaults `:270`, `:319`, `:205`, `:204` |
| `ENABLE_WEBGL` | No GL in v1; also gates ANGLE subdirectory. | `ON` `:307`; `Source/CMakeLists.txt:23-24` `if (ENABLE_WEBGL OR USE_ANGLE_EGL)` / `add_subdirectory(ThirdParty/ANGLE)` |
| `ENABLE_GPU_PROCESS` | No GPU process; depends `USE_GBM` → auto-off once GBM off. | WPE `ON` `OptionsWPE.cmake:75`; dep `:157` |
| `ENABLE_WEBGPU` | No GPU API. | OFF `:308` (JSCOnly also `set(ENABLE_WEBGPU OFF)` `OptionsJSCOnly.cmake:44`) |
| `ENABLE_WEBXR` (+`_HIT_TEST`, `_LAYERS`) | XR; depends `ENABLE_WEBGL`. | WPE `=${ENABLE_EXPERIMENTAL_FEATURES}` `OptionsWPE.cmake:95-97`; deps `:355-357` |
| `ENABLE_OFFSCREEN_CANVAS`, `ENABLE_OFFSCREEN_CANVAS_IN_WORKERS` | Canvas worker plumbing not needed v1. | WPE `ON` `OptionsWPE.cmake:84-85` |
| `ENABLE_GEOLOCATION` | No location backend on target. | `ON` `:226` |
| `ENABLE_NOTIFICATIONS` | Needs a desktop notification service. | WPE `ON` `OptionsWPE.cmake:83` |
| `ENABLE_GAMEPAD` | No gamepads; also drops optional libmanette probe. | WPE `ON` `OptionsWPE.cmake:74`; `:333-341` |
| `ENABLE_TOUCH_EVENTS` | No touchscreen in v1. | WPE `ON` `OptionsWPE.cmake:89` |
| `ENABLE_DEVICE_ORIENTATION`, `ENABLE_ORIENTATION_EVENTS` | Sensor APIs; no sensors. | OFF `:218`, `:262` |
| `ENABLE_WEB_AUTHN`, `ENABLE_PAYMENT_REQUEST` | Need platform authenticator / payment backend. | OFF `:316`, `:263` |
| `ENABLE_BUBBLEWRAP_SANDBOX` | Requires `bwrap` ≥ 0.3.1 + `xdg-dbus-proxy` + libseccomp. | Linux default `ON` `OptionsWPE.cmake:138`; checks `BubblewrapSandboxChecks.cmake:1-31` |
| `ENABLE_JOURNALD_LOG` | No journald/systemd. | WPE `ON` `OptionsWPE.cmake:104` |
| `ENABLE_MEMORY_SAMPLER`, `ENABLE_RESOURCE_USAGE` | Developer diagnostics. | Linux defaults `ON` `OptionsWPE.cmake:139-140` |
| `ENABLE_PERIODIC_MEMORY_MONITOR` | Devtool-style monitor. | WPE `ON` `OptionsWPE.cmake:86` |
| `USE_ATK` | No AT-SPI/a11y stack; drops ATK + atk-bridge requirements. | WPE `ON` `OptionsWPE.cmake:112`; fatal checks `:213-222` |
| `ENABLE_SPEECH_SYNTHESIS` | Requires Flite (or Spiel) and GStreamer dep. | WPE `ON` `OptionsWPE.cmake:57`; `:224-242`; dep `GStreamerDependencies.cmake:1` |
| `USE_FLITE` (+`USE_SPIEL`) | Speech backends; unused once speech is off. | WPE `ON` `OptionsWPE.cmake:113`; `USE_SPIEL` OFF `:123` |
| `ENABLE_SPELLCHECK` | Drops Enchant requirement. | WPE `ON` `OptionsWPE.cmake:54`; fatal check `:402-407` |
| `ENABLE_XSLT` | Drops libxslt requirement. | WPE `ON` `OptionsWPE.cmake:56`; `:298-300` |
| `ENABLE_PDFJS` | No PDF feature in v1 (also drops PDF.js resource generation). | WPE `ON` `OptionsWPE.cmake:53` |
| `ENABLE_PDF_HUD`, `ENABLE_PDF_PLUGIN`, `ENABLE_PDFKIT_PLUGIN`, `ENABLE_UNIFIED_PDF` | PDF variants; stay off. | OFF `:265-268` |
| `ENABLE_MHTML` | Niche archive format. | WPE `ON` `OptionsWPE.cmake:82` |
| `ENABLE_WEBDRIVER` (+ its `_BIDI`/interaction flags) | No WebDriver service on-device. | WPE `ON` `OptionsWPE.cmake:55`; `:287-296` |
| `ENABLE_LAYOUT_TESTS`, `ENABLE_API_TESTS`, `ENABLE_IMAGE_DIFF` | Test tooling; layout tests are force-off without developer mode. | `WebKitFeatures.cmake:465-468`; `ENABLE_IMAGE_DIFF` ON `:228` |
| `ENABLE_CONTENT_EXTENSIONS`, `ENABLE_CONTENT_FILTERING` | Rule-list filtering; not v1. | WPE `ON` `OptionsWPE.cmake:69`; OFF `:211` |
| `ENABLE_SHAREABLE_RESOURCE` | Network cache feature; not needed. | WPE `ON` `OptionsWPE.cmake:87` |
| `ENABLE_SANDBOX_EXTENSIONS`, `ENABLE_SERVICE_CONTROLS`, `ENABLE_WK_WEB_EXTENSIONS` | Platform services / extensions; none on target. | OFF `:278`, `:279`; WPE `=${ENABLE_EXPERIMENTAL_FEATURES}` `OptionsWPE.cmake:94` |
| `ENABLE_INTROSPECTION` / `ENABLE_DOCUMENTATION` | Drop g-ir-scanner and gi-docgen host tools. | WPE `ON` `OptionsWPE.cmake:103`, `:102`; fatal checks `:171-179`; dep `:133` |
| `ENABLE_WPE_LEGACY_API` | Use the new WPEPlatform API instead (keep at least one of the two — see 5.1). | WPE `ON` `OptionsWPE.cmake:109` |
| `ENABLE_WPE_PLATFORM_DRM`, `ENABLE_WPE_PLATFORM_WAYLAND` | Drop libinput/udev and Wayland/wayland-protocols requirements. | WPE `ON` `OptionsWPE.cmake:106`, `:108`; checks `:314-331` |
| `ENABLE_WPE_QT_API`, `ENABLE_COG` | Qt plugin / Cog browser wrapper. | dev-mode / OFF `OptionsWPE.cmake:110`, `:127` |
| `ENABLE_WPE_1_1_API` | Old API; conflicts with `ENABLE_WPE_PLATFORM`. | OFF `OptionsWPE.cmake:111`; `WEBKIT_OPTION_CONFLICT(ENABLE_WPE_PLATFORM ENABLE_WPE_1_1_API)` `:130` |
| `ENABLE_SAMPLING_PROFILER` | Must be OFF together with `ENABLE_C_LOOP` (conflict, §3.1). | `:277`, conflict `:336` |
| `ENABLE_WEBASSEMBLY` | JIT-only; must be OFF with `ENABLE_C_LOOP` (conflict, §3.1). | `:297`, conflict `:337` |
| `USE_AVIF`, `USE_JPEGXL`, `USE_LCMS`, `USE_WOFF2`, `USE_LIBHYPHEN` | Drop libavif(+aom), libjxl, lcms2, libwoff2, libhyphen. | WPE `ON` `OptionsWPE.cmake:59-62`, `:117`; `USE_WOFF2` also auto-off if FreeType has built-in WOFF2 (`WOFF2Checks`, `:270-278`) |
| `USE_VULKAN` | No Vulkan; drops volk. | `=${ENABLE_EXPERIMENTAL_FEATURES}` `OptionsWPE.cmake:119`; `:258-263` |
| `USE_GBM`, `USE_LIBDRM` | Drop GBM/libdrm (and by dependency the DRM platform + GPU process). | WPE `ON` `OptionsWPE.cmake:114`, `:116`; deps `:157-159`; checks `:449-472` |
| `USE_LIBBACKTRACE` | Drop libbacktrace if not needed for crash reporting. | WPE `ON` `OptionsWPE.cmake:115`; `:474-479` |
| `USE_SYSPROF_CAPTURE`, `USE_SYSTEM_SYSPROF_CAPTURE` | Drop libsysprof-capture-4 dependency. | WPE `ON` `OptionsWPE.cmake:124-125` |

Notes: some OFF rows are also forced off *for you* through the `WEBKIT_OPTION_DEPEND` graph
(`GStreamerDependencies.cmake:1-11` + `WebKitFeatures.cmake:339-358`) — the table marks those as "auto-off".
A `ENABLE_WEB_CRYPTO` flag does not exist in 2.54 (`grep -rn "WEB_CRYPTO" Source/cmake` → NOT FOUND); the WPE port
compiles crypto unconditionally with `SET_AND_EXPOSE_TO_BUILD(USE_GCRYPT TRUE)` (`OptionsWPE.cmake:434`), i.e.
libgcrypt is required regardless of feature flags.

---

## 5. DEPENDENCY-OFF LIST (optional vs required for the WPE / JSCOnly ports)

### 5.1 Cannot be disabled for the WPE port (unconditional `REQUIRED` in `Source/cmake/OptionsWPE.cmake:12-26`)

GLib 2.70, HarfBuzz 2.7.4+ICU, ICU 70.1, JPEG, Epoxy, libgcrypt, **libsoup3 3.0.0** (networking), libtasn1,
xkbcommon, libxml2, PNG, SQLite3, Threads, WebP(demux), zlib, FreeType 2.9.0 — see table 1.1.
Additionally forced by the port: `OptionsWPE.cmake:434-436` `SET_AND_EXPOSE_TO_BUILD(USE_GCRYPT TRUE)` /
`SET_AND_EXPOSE_TO_BUILD(USE_LIBEPOXY TRUE)` / `SET_AND_EXPOSE_TO_BUILD(USE_XDGMIME TRUE)`, and
`:433` `SET_AND_EXPOSE_TO_BUILD(USE_ATSPI TRUE)` (AT-SPI a11y backend is always compiled for WPE even with `USE_ATK=OFF`).
The legacy-vs-new API constraint: `OptionsWPE.cmake:167-169`
`if (NOT ENABLE_WPE_PLATFORM AND NOT ENABLE_WPE_LEGACY_API)` /
`message(FATAL_ERROR "At least one of ENABLE_WPE_PLATFORM or ENABLE_WPE_LEGACY_API needs to be enabled")`.
JSCOnly avoids libsoup/GLib entirely (no networking; `OptionsJSCOnly.cmake` finds only Threads + ICU, +GLib only for the GLib event loop).

### 5.2 Disable-able dependencies and their controlling options

| Dependency | Controlling option (default) | Evidence (verbatim) |
|---|---|---|
| GStreamer ≥ 1.18.4 (whole media stack) | `USE_GSTREAMER` (WPE `ON`, PUBLIC) | `GStreamerDefinitions.cmake:5` `WEBKIT_OPTION_DEFINE(USE_GSTREAMER "Whether to enable features that require GStreamer" PUBLIC ON)`; auto-disables video/audio/codecs/speech via `GStreamerDependencies.cmake:1-4` `WEBKIT_OPTION_DEPEND(ENABLE_SPEECH_SYNTHESIS USE_GSTREAMER)` … `WEBKIT_OPTION_DEPEND(ENABLE_WEB_CODECS USE_GSTREAMER)`; checks `GStreamerChecks.cmake:1-53` |
| libsoup3 | **Not optional for WPE** (see 5.1). Only the JSCOnly port avoids it. | `OptionsWPE.cmake:18` |
| Wayland / wayland-protocols | `ENABLE_WPE_PLATFORM_WAYLAND` (`ON`) | `OptionsWPE.cmake:108`; `:326-331` `find_package(Wayland 1.20 REQUIRED)` |
| libinput + libudev (DRM platform) | `ENABLE_WPE_PLATFORM_DRM` (`ON`) | `OptionsWPE.cmake:106`; `:314-319` |
| Headless platform | `ENABLE_WPE_PLATFORM_HEADLESS` (`ON`) — keep | `OptionsWPE.cmake:107`; `:321-324` |
| libwpe / WPE renderer | `ENABLE_WPE_LEGACY_API` (`ON`) | `OptionsWPE.cmake:109`; `:302-306` `find_package(WPE REQUIRED)` |
| ATK + atk-bridge | `USE_ATK` (`ON`) | `OptionsWPE.cmake:112`; fatal checks `:213-222` |
| Flite (+libSpiel) | `USE_FLITE` / `USE_SPIEL` under `ENABLE_SPEECH_SYNTHESIS` | `OptionsWPE.cmake:113`, `:123`; `:224-242` |
| Enchant | `ENABLE_SPELLCHECK` (WPE `ON`) | `OptionsWPE.cmake:402-407` `message(FATAL_ERROR "Enchant is needed for ENABLE_SPELLCHECK")` |
| libxslt | `ENABLE_XSLT` (WPE `ON`) | `OptionsWPE.cmake:298-300` `find_package(LibXslt 1.1.13 REQUIRED)` (inside `if (ENABLE_XSLT)`) |
| libjxl | `USE_JPEGXL` (`ON`) | `OptionsWPE.cmake:244-249` |
| libavif (+aom) | `USE_AVIF` (`ON`) | `OptionsWPE.cmake:377-382` |
| lcms2 | `USE_LCMS` (`ON`) | `OptionsWPE.cmake:395-400` |
| libwoff2 | `USE_WOFF2` (`ON`; auto-turns-off if FreeType provides WOFF2) | `OptionsWPE.cmake:269-285` `set(USE_WOFF2 OFF)` / `# Turn off use of libwoff2` |
| libhyphen | `USE_LIBHYPHEN` (`ON`) | `OptionsWPE.cmake:251-256` |
| libdrm | `USE_LIBDRM` (`ON`) | `OptionsWPE.cmake:449-453` |
| GBM | `USE_GBM` (`ON`; depends `USE_LIBDRM`) | `OptionsWPE.cmake:463-467`; dep `:159` |
| libbacktrace | `USE_LIBBACKTRACE` (`ON`) | `OptionsWPE.cmake:474-479` |
| libsysprof-capture-4 | `USE_SYSPROF_CAPTURE` (`ON`) + `USE_SYSTEM_SYSPROF_CAPTURE` (`ON`) | `OptionsWPE.cmake:124-125`; `Source/CMakeLists.txt:49-56` |
| systemd/journald | `ENABLE_JOURNALD_LOG` (WPE `ON`) | `OptionsWPE.cmake:384-389` `message(FATAL_ERROR "libsystemd or libelogind are needed for ENABLE_JOURNALD_LOG")` |
| bubblewrap + libseccomp + xdg-dbus-proxy (+ bwrap exe) | `ENABLE_BUBBLEWRAP_SANDBOX` (Linux default `ON`) | `OptionsWPE.cmake:137-138`; `BubblewrapSandboxChecks.cmake:1-31` |
| gobject-introspection | `ENABLE_INTROSPECTION` (WPE `ON`) | `OptionsWPE.cmake:103`, fatal `:171-174` |
| gi-docgen | `ENABLE_DOCUMENTATION` (WPE `ON`; depends on introspection) | `OptionsWPE.cmake:102`, fatal `:176-179`, dep `:133` |
| Qt6 | `ENABLE_WPE_QT_API` (dev-mode default) | `OptionsWPE.cmake:110`; `:348-362` |
| OpenXR | `ENABLE_WEBXR` (experimental default) | `OptionsWPE.cmake:95`; `:364-375` |
| libmanette | `ENABLE_GAMEPAD` (WPE `ON`; non-fatal probe) | `OptionsWPE.cmake:333-341` |
| OpenSSL ≥ 3 (only WebRTC/GStreamer-WebRTC path) | `ENABLE_WEB_RTC` + `USE_GSTREAMER_WEBRTC` | `GStreamerChecks.cmake:55-62` |
| bundled libwebrtc | `USE_LIBWEBRTC` — set FALSE unless `ENABLE_MEDIA_STREAM AND ENABLE_WEB_RTC` | `GStreamerChecks.cmake:55-57`, `:70-75` `SET_AND_EXPOSE_TO_BUILD(USE_LIBWEBRTC FALSE)` |
| librice | `USE_LIBRICE` (OFF) | `GStreamerDefinitions.cmake:11` |
| Breakpad | `ENABLE_BREAKPAD` (OFF) | `OptionsWPE.cmake:409-419` |
| Vulkan/volk | `USE_VULKAN` (experimental default) | `OptionsWPE.cmake:119`, `:258-263` |
| unifdef | `USE_SYSTEM_UNIFDEF` — WPE default `ON` (system required), JSCOnly default `OFF` (bundled) | `OptionsWPE.cmake:126`; `OptionsJSCOnly.cmake:26`; `Source/CMakeLists.txt:15-18` |
| X11 | **Not queried by the WPE port at all** (only GTK has `ENABLE_X11_TARGET`) | `Source/cmake/OptionsGTK.cmake:55` `WEBKIT_OPTION_DEFINE(ENABLE_X11_TARGET "Whether to enable support for the X11 windowing target." PUBLIC ON)` — no X11 reference in `OptionsWPE.cmake` |

---

## 6. TOOLCHAIN REQUIREMENTS (W0.5 carry-over)

### 6.1 Compiler / build system

| Requirement | Value | Verbatim |
|---|---|---|
| CMake | ≥ 3.20 | `CMakeLists.txt:9` `cmake_minimum_required(VERSION 3.20)` |
| C++ standard | C++23, no compiler extensions | `Source/cmake/OptionsCommon.cmake:1-3` `set(CMAKE_CXX_STANDARD 23)` / `set(CMAKE_CXX_STANDARD_REQUIRED ON)` / `set(CMAKE_CXX_EXTENSIONS OFF)` |
| Generator | Ninja **required** for GTK/WPE | `Source/cmake/WebKitCommon.cmake:102-107` `if (PORT STREQUAL "GTK" OR PORT STREQUAL "WPE")` / `if (NOT CMAKE_GENERATOR MATCHES "Ninja")` / `message(FATAL_ERROR "The ${PORT} port requires the Ninja generator, but this build ...")` |
| GCC | ≥ 12.2 | `WebKitCommon.cmake:127-131` `if (${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS "12.2.0")` / `message(FATAL_ERROR "GCC 12.2 or newer is required to build WebKit. Use a newer GCC version or Clang.")` |
| Clang | no explicit minimum; workarounds for `< 19`; Clang assumed for some paths (libc++, Swift) | `Source/cmake/WebKitCompilerFlags.cmake:244-246` `# FIXME: Remove once Clang 18 does no longer need to be supported for the GTK and WPE ports` / `if ((CMAKE_CXX_COMPILER_ID STREQUAL Clang) AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19))`; `OptionsCommon.cmake:223-224` `#if defined(__clang__)` `int main() { return _LIBCPP_VERSION; }` |
| C++ stdlib | libc++ 19+ (hardening macro) or libstdc++ (assertions macro) | `OptionsCommon.cmake:236-260`: `#if _LIBCPP_VERSION >= 190000` / `set(CXX_STDLIB_VARIANT "LIBCPP 19+")`; `#include <utility>` `int main() { return _GLIBCXX_RELEASE; }` / `set(CXX_STDLIB_ASSERTIONS_MACRO _GLIBCXX_ASSERTIONS=1)` |
| Linker/ar | probed for LLD/mold/gold/BFD; thin archives preferred | `OptionsCommon.cmake:33-34` `CMAKE_DEPENDENT_OPTION(USE_LD_LLD "Use LLD linker" ON "TRY_USE_LD_LLD;NOT WIN32" OFF)`; `:165` `option(USE_THIN_ARCHIVES "Produce all static libraries as thin archives" ${USE_THIN_ARCHIVES_DEFAULT})` |

### 6.2 Host tools (found during configure)

| Tool | Requirement | Verbatim |
|---|---|---|
| Perl | ≥ 5.10.0 + modules `English`, `FindBin`, `JSON::PP` (module list is `REQUIRED`); version check is a TODO | `WebKitCommon.cmake:252-254` `# TODO Enforce version requirement for perl` / `find_package(Perl 5.10.0 REQUIRED)` / `find_package(PerlModules COMPONENTS English FindBin JSON::PP REQUIRED)` |
| Python | REQUIRED, "preferably version 3" (no version pin) | `WebKitCommon.cmake:256-259` `# This module looks preferably for version 3 of Python. If not found, version 2 is searched.` / `find_package(Python COMPONENTS Interpreter REQUIRED)` / `set(PYTHON_EXECUTABLE ${Python_EXECUTABLE} ...)` |
| Ruby | ≥ 2.5 (REQUIRED) | `WebKitCommon.cmake:282-294` `if (Ruby_VERSION VERSION_LESS 2.5)` … `message(FATAL_ERROR "Ruby 2.5 or higher is required.")` |
| GPerf | ≥ 3.0.1, only when `ENABLE_WEBCORE` (so not for JSCOnly) | `WebKitCommon.cmake:333-338` `# Check gperf after including OptionsXXX.cmake since gperf is required only when ENABLE_WEBCORE is true,` / `find_package(Gperf 3.0.1 REQUIRED)` |
| pkg-config | required in practice — every `Find*.cmake` resolves via `pkg_check_modules` | e.g. `Source/cmake/FindHarfBuzz.cmake:68-69` `find_package(PkgConfig QUIET)` / `pkg_check_modules(PC_HARFBUZZ QUIET harfbuzz)`; WPE dep list `Tools/wpe/dependencies/apt:16` `pkg-config` |
| unifdef | WPE default `USE_SYSTEM_UNIFDEF=ON` → system `unifdef`; JSCOnly builds bundled | `Source/CMakeLists.txt:15-18`; `Tools/wpe/dependencies/apt:18` `unifdef` |
| Bison / Flex | NOT FOUND as WebKit build requirements — `grep -rn -i "bison\|flex" Source/cmake/*.cmake Source/WebCore/CMakeLists.txt Source/JavaScriptCore/CMakeLists.txt` finds no invocations; they appear only under the "dependencies necessary for building the jhbuild" block (`Tools/glib/dependencies/apt:109-110`) |
| gawk | listed among "dependencies necessary for building WebKitGTK/WPE" | `Tools/glib/dependencies/apt:39` `gawk` (in the array opened by the comment at `:30`) |
| gi-docgen / g-ir-scanner | only with `ENABLE_DOCUMENTATION` / `ENABLE_INTROSPECTION` (WPE defaults ON; turn OFF to drop) | `OptionsWPE.cmake:171-179`; `Source/cmake/FindGI.cmake:75-76` `find_program(GI_SCANNER_EXE NAMES ${_GI_SCANNER_EXE} g-ir-scanner)`; `FindGIDocgen.cmake`
| bwrap / xdg-dbus-proxy | runtime programs, only with `ENABLE_BUBBLEWRAP_SANDBOX` | `BubblewrapSandboxChecks.cmake:12-17`, `:25-31` |
| CMake / Ninja / Ruby packages as shipped for the GLib ports | `cmake`, `ninja-build`, `ruby`, `clang`, `gperf` | `Tools/glib/dependencies/apt`: `:37` `cmake`, `:79` `ninja-build`, `:81` `ruby`, `:36` `clang`, `:40` `gperf`; WPE extras (`g++`, `gcc`) `Tools/wpe/dependencies/apt:7-8` |
| Build-revision stamping | not required — degrades to `tarball` when Tools scripts are absent | `OptionsWPE.cmake:429-431` `if (NOT EXISTS "${TOOLS_DIR}/glib/apply-build-revision-to-files.py")` / `set(BUILD_REVISION "tarball")` |
| Build wrapper dirs | `Source/cmake/clang-wrapper`, `Source/cmake/ninja-wrapper` exist (optional wrappers) | `ls Source/cmake/` output |

### 6.3 Linux-only host signals

- Host machine probed with `uname` (Windows cannot): `OptionsCommon.cmake:14-17` `# NB: We can't use CMAKE_HOST_SYSTEM_PROCESSOR ... Also, `uname` is not available on Windows` / `execute_process(COMMAND uname -m OUTPUT_VARIABLE WTF_HOST_SYSTEM_MACHINE ...)`.
- glibc/`features.h` probing: `OptionsCommon.cmake:304-307` `# Check whether features.h header exists.` / `# Including glibc's one defines __GLIBC__, that is used in Platform.h` / `WEBKIT_CHECK_HAVE_INCLUDE(HAVE_FEATURES_H features.h)`.
- Dependency installers are per-Linux-distro only: `Tools/wpe/dependencies/{apt,dnf,pacman}` (`ls Tools/wpe/dependencies`); `Tools/Scripts/update-webkitwpe-libs` is a Perl/webkitdirs wrapper that drives jhbuild/container SDK (`Tools/glib/dependencies/apt:153-160` mentions `podman`, `crun`, `catatonit` for the "WebKit Container SDK").
- Sandboxing tooling (bwrap, xdg-dbus-proxy) is Linux-specific (`BubblewrapSandboxChecks.cmake`).
- `Documentation/BuildInstructions*`: **NOT FOUND** in either source tree. The fork checkout has no `Documentation/` directory; the pristine tarball's `third_party/wpewebkit-2.54.0/Documentation/` contains only gi-docgen HTML templates (`class.Context.html`, `callback.*.html`, …) for the API modules, not build instructions.

---

## 7. NOT FOUND index

| Item | Status |
|---|---|
| Min versions for libjpeg / libpng / libwebp / sqlite / zlib on the WPE port | NOT FOUND (versionless `find_package`; only other ports pin: JPEG 1.5.2, PNG 1.6.34, SQLite3 3.23.1, ZLIB 1.2.11) |
| Skia revision outside `Source/ThirdParty/skia/README.WebKit` | NOT FOUND (commit `588b550a4dd8af90dbe71c0554852806bd8f0b21` recorded only there; no `.gclient`, no `skia_revision` file, no `FindSkia.cmake`, no `find_package(Skia`) |
| FFI / libffi build option | NOT FOUND (no `libffi`, `USE_LIBFFI`, `ENABLE_FFI` matches anywhere under `Source/`) |
| `ENABLE_WEB_CRYPTO`-style flag | NOT FOUND (crypto always on via `SET_AND_EXPOSE_TO_BUILD(USE_GCRYPT TRUE)`, `OptionsWPE.cmake:434`) |
| `Documentation/BuildInstructions*` | NOT FOUND in both trees (see §6.3) |
| Bison/Flex as build requirements | NOT FOUND in the CMake build (only in the jhbuild-deps package list) |
| Minimum Clang version | NOT FOUND stated; explicit accommodations for Clang 18 exist (`WebKitCompilerFlags.cmake:244-253`) |
