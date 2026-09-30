# L6 Skia host spike — pin, vendored recipe, CPU-raster host build

Lane `browser/l6-skia` (browser.md §2 Skia pin, §7.5 L6, AD-4, AD-11).
Status: **host spike complete** — pin determined + verified, recipe vendored
per AD-11, CPU-raster Skia built green on the host with a deterministic
draw/paint smoke.  Memo boundary: the GN/depot_tools alternative was probed
(bounded) and documented, not pursued — it is not the recipe WPE uses.

Vendored recipe + raw evidence: `third_party/skia-588b550/` (README.md there
is the operator-facing record; this note is the research summary + reuse plan).

---

## 1. Which Skia revision/recipe WPE(WebKit) 2.54.0 expects

**Upstream Skia @`588b550a4dd8af90dbe71c0554852806bd8f0b21` (2026-08-30,
autoroll), built from an in-tree snapshot by WebKit's own
`Source/ThirdParty/skia/CMakeLists.txt` — not GN, not depot_tools.**

Evidence, quoted from the WebKit 2.54.0 tree (clone `~/webkit-hobbyos`
@`5220e80b97`, i.e. the 2.54.0 release commit; all trees read-only):

* `Source/ThirdParty/skia/README.WebKit` (verbatim; also vendored at
  `third_party/skia-588b550/webkit/README.WebKit`):

  > This directory contains a copy of Skia from the official repository.
  >
  >   - URL: https://skia.googlesource.com/skia
  >   - Commit: 588b550a4dd8af90dbe71c0554852806bd8f0b21
  >
  > …Then check whether the updated sources require any changes to the CMake
  > build system, and update the Skia commit identifier in this file.

  (The commit was verified live at `skia.googlesource.com/skia/+/588b550a…`
  — exists, dated 2026-08-30.)

* `Source/CMakeLists.txt:45-47` — Skia is a plain subdirectory of the build:

  > `if (USE_SKIA)`
  > `    add_subdirectory(ThirdParty/skia)`
  > `endif ()`

* `Source/cmake/OptionsWPE.cmake` — on by default for WPE:
  `:155  WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SKIA PRIVATE ON)`,
  `:118  WEBKIT_OPTION_DEFINE(USE_SKIA_OPENTYPE_SVG … PUBLIC ON)`.

* `Source/ThirdParty/skia/CMakeLists.txt:1-22` — the build model (quotes):

  > `find_package(Freetype 2.9.0 REQUIRED)`
  > `find_package(Fontconfig 2.13.0 REQUIRED)`
  > `… if (USE_SKIA_ENCODERS) find_package(WebP REQUIRED COMPONENTS mux)`
  > `add_library(Skia STATIC` … (774 source files, lines 21-813)

  plus GPU defines (`SK_GL`, `SK_GANESH`) and the SVG/shaper block
  (`SK_ENABLE_SVG`, `SK_SHAPER_*`) further down; it links `Freetype`,
  `Fontconfig`, `WebP::mux`, `EXPAT`, `HarfBuzz`, `JPEG`, `PNG`.

**Provenance verification (this lane, 2026-09-30, re-runnable via
`third_party/skia-588b550/tools/compare-with-webkit.py`):**

| comparison | result |
|---|---|
| fetched upstream tree ↔ fork clone @2.54.0 | 12389 common files, **0 content diffs**; clone adds only the 3 WebKit files; upstream has 9 files WebKit's ignore rules dropped (package-lock ×7, gradle.properties, externs.js) |
| fetched upstream tree ↔ WPE 2.54.0 release tarball Skia subset | 3059 common files, **0 content diffs**; the tarball side is a strict prune (adds the 2 build files, drops 9339 dev/meta files) |

so building upstream @588b550a with WebKit's CMakeLists is faithful to what
WPE 2.54.0 ships.  Raw log: `third_party/skia-588b550/build-host/compare-with-webkit.log`.

### Alternative paths compared

| path | evidence (probed 2026-09-30) | verdict |
|---|---|---|
| **Standalone CMake (WebKit's own file)** — chosen | full CPU build green; no depot_tools; deps from vendored prefixes + system | the leanest faithful path; reproduced below |
| **depot_tools + GN (Skia's official flow)** | `bin/fetch-gn` works rootlessly (GN v2175, no depot_tools); `gn gen` with CPU/system args succeeds (104 targets/65 files) but `ninja` stops: `third_party/externals/libwebp/sharpyuv/sharpyuv.c … missing` — the graph needs DEPS externals (47 `third_party/externals` entries), fetched only by `tools/git-sync-deps` (multi-repo, multi-GB) | **not what WPE uses**; kept as documented fallback; boundary logged (`build-host/gn-probe.log`) |
| **Bazel** (upstream `MODULE.bazel` exists) | not investigated beyond the manifest listing; WPE does not use it | out of scope; noted |

GN arg names recorded for future reference (from the successful `gn gen`):
`skia_use_gl=false`, `skia_use_egl=false`, `skia_use_vulkan=false`,
`skia_enable_ganesh=false`, `skia_enable_graphite=false`,
`skia_enable_tools=false`, `skia_enable_spirv_validation=false` (this one
defaults ON and pulls `spirv-tools`), `skia_use_system_{zlib,libpng,
libjpeg_turbo,freetype2,expat,harfbuzz,icu}=true`.

---

## 2. Vendored recipe (AD-11)

`third_party/skia-588b550/` — small committed files only (the 80 MB archive,
216 MB tree and build outputs are gitignored and rebuilt by `fetch.sh`):

* `fetch.sh` — downloads `+archive/<pin>.tar.gz`, extracts, **content-gates**
  (hard-fail), overlays `webkit/` (each file checked against
  `webkit/SHA256SUMS`); re-verifies on every run (incl. marker path).
* `manifest.py` — canonical content manifest; **the** integrity gate, because
  googlesource tarball *containers are not byte-reproducible* (three
  downloads of the same commit → three different container sha256s;
  extracted content identical).  Pin: `c27786cade6336a16e284fc9ccf09a137eaba3684ddaf94d9414aab4d0d47213`
  over 12395 entries.
* `webkit/` — WebKit's build files verbatim: `CMakeLists.txt`,
  `WebKitSkiaConfig.h`, `README.WebKit` (+`cmake/FindWebP.cmake`,
  `cmake/FindHarfBuzz.cmake`; WebKit BSD headers retained).
* `gen_sources.py` — derives the source lists **from WebKit's CMakeLists**
  (774-file list → 524 CPU sources; 250 `src/gpu/**` excluded), plus POSIX /
  fontconfig / encoders / SVG-shaper blocks; records every exclusion.
* `CMakeLists.txt` — host overlay target `skia-host-cpu` (static `libSkia.a`
  + smoke); includes WebKit's SVG-block compile definitions verbatim.
* `build.sh` — one-command pipeline: fetch → deps → generate → configure →
  `ninja -j$JOBS` → smoke ×2 → checksum gate; `SKIA_FONTMGR=fontconfig|empty`,
  `SKIA_ENCODERS`, `SKIA_OPENTYPE_SVG`, `SKIA_DEBUG`, `*_PREFIX` knobs.
* `setup-deps.sh` — rootless fontconfig dev headers
  (`apt-get download` + `dpkg-deb -x` into `~/.local/share/l6-tools/`).
* `smoke/skia_smoke.cpp` + `smoke/smoke.sha256` — deterministic scene + pin.
* `tools/compare-with-webkit.py` — re-runs the provenance cross-checks.
* `README.md` — operator record incl. the delta table D1-D7.

**Deltas vs WebKit's file** (full table in the README): D1 GPU sources off
(CPU raster, AD-4); D2 `SK_ASSUME_*` (GPU-only) off; D3 framework-header
symlinks recreated standalone; D4 WebKit's flag helpers inlined
(`-fno-exceptions -fno-rtti`); D5 expat via find_package; D6 font-manager
switch (`fontconfig` host / `empty` target); **D7 `+src/core/SkStrikeRef.cpp`**
— WebKit's CMakeLists omits this TU although `SkFont.cpp:240` references its
constructor (upstream GN includes it, `gn/core.gni:572`); a plain static link
fails `undefined reference to SkStrikeRef::SkStrikeRef(...)` — **see WK-3
notes in §4**.

---

## 3. Host build + smoke result (2026-09-30, this machine)

* Toolchain: gcc 15.2.0 (GNU, C++23 — same class as WebKit's Linux builds),
  CMake 3.31.8 (user-local), Ninja 1.13.2, `-j4`.
* Build: 597 ninja edges (524 CPU sources + POSIX 4 + fontconfig 5 +
  encoders 7 + SVG/shaper 56 + D7 1); **`libSkia.a` = 15,079,504 bytes,
  595 objects, 7209 `T` symbols**; ~80 s wall at `-j4` (plus ~70 s first
  configure).  Deps resolved from vendored prefixes: freetype 2.14.3,
  harfbuzz 14.5.0, zlib 1.3.1, libpng 1.6.44, libjpeg-turbo 3.1.0,
  libwebp 1.6.0, fontconfig 2.17.1 (rootless), expat 2.7.4 (system).
* **Smoke** (`smoke/skia_smoke.cpp`, fixed 320×240 scene: exact fills, AA
  circle, `SkShaders::LinearGradient`, stroked `SkRRect`, dashed
  `SkPathBuilder` cubic, rotate + multiply blend; draws into a raster canvas,
  dumps raw BGRA + PPM):
  * two runs **byte-identical**;
  * `sha256(pixels)=eee5d809bcfb13e9e25dc6faaddb99da958580afd9ae7954c4f9feb0cac46eae`,
    `fnv1a64=0xb2e29142c06f8b86`; pinned in `smoke/smoke.sha256`; `build.sh`
    hard-fails if a later run drifts;
  * PPM spot-check: the 96×64 `#1e78dc` fill = exactly 6144 px of that exact
    value; 606 distinct colors; multiply-blend pixel = exact expected value.
* Raw evidence (absolute):
  `~/hobbyos-lanes/l6-skia/third_party/skia-588b550/build-host/build.log`,
  `…/smoke-run1.log`, `…/smoke-run2.log`, `…/smoke1.rgba`, `…/smoke2.rgba`,
  `…/generated/sources-report.json`, `…/compare-with-webkit.log`,
  `…/gn-probe.log`.

---

## 4. Reuse recipe — two-build-rule host/target legs (what WK-3 needs)

**Host leg (done, reproducible):**

```sh
cd third_party/skia-588b550
./fetch.sh && ./setup-deps.sh && JOBS=4 ./build.sh     # fontconfig mgr, host gcc
# prefixes: FREETYPE/HARFBUZZ=~/hobbyos-lanes/l5-fonts/obj/third_party/{freetype,harfbuzz}/prefix
#           ZLIB/PNG/JPEG/WEBP=~/hobbyos-lanes/l6-libs1/l5-fonts prefixes
#           FONTCONFIG=~/.local/share/l6-tools/fontconfig-deb/rooted
```

**Target leg (WK-3 raster bring-up) — plan grounded in this spike:**

1. Same pin, same `fetch.sh`, same `gen_sources.py` (lists derive from
   WebKit's file → no silent drift).  Swap `SKIA_FONTMGR=empty` to build
   `SkFontMgr_custom.cpp` + `SkFontMgr_custom_empty.cpp` (upstream
   `gn/ports.gni:52-54,96`) instead of the fontconfig manager; freetype stays
   (l5-fonts port) via `SK_TYPEFACE_FACTORY_FREETYPE` + `SK_USER_CONFIG_HEADER`.
2. Cross-compile with a HobbyOS toolchain file; carry the same compile
   definitions (SVG/shaper block, `-fno-exceptions -fno-rtti`,
   `SK_TRIVIAL_ABI` only under Clang); the x86 SIMD option block in the
   overlay auto-skips on non-x86 (fallback to generic SkOpts), per-arch
   handling can mirror WebKit's.
3. Platform layer to port for `libSkia.a` to *link* into WebCore: stdio file
   layer (`SkOSFile`), `SK_TYPEFACE_FACTORY_FREETYPE` usage paths, thread
   primitives, `SkFontMgr_custom` usage by WK-3 (does it need one custom
   manager per app, or `SkFontMgr::RefEmpty`?).  The smoke program should
   then be rebuilt against the target leg's `libSkia.a` as a bring-up
   harness — it is deterministic and dependency-light.
4. **D7 (`SkStrikeRef.cpp`) must be re-checked at WK-3**: if WebKit's own
   link hides the omission via dead-stripping, the upstream CMakeLists gap
   is still worth a one-line patch upstream (README.WebKit explicitly asks
   updaters to check for exactly this class of change); if not, WebKit's
   build shares our symptom.  Either way our recipe already compiles it.
5. Booking: target leg = same three scripts with `SKIA_FONTMGR=empty` +
   toolchain file; nothing in this lane's recipe assumes host-specific
   layout except `setup-deps.sh` (host-only step).

## 5. Risks / open questions

* Pin is a fast-moving autoroll target — if Skia rewrites history the
  manifest gate hard-fails by design (re-audit, don't bypass).
* gcc vs clang flag parity is untested here (WebKit supports both; our
  build used gcc 15.2.0 at WebKit's default C++ standard).
* The smoke exercises CPU raster + the WebCore-used public API paths
  (gradients, dash, path builder, blending) but not WebKit's
  `WebKitSkiaConfig.h` consumer wiring itself.
* The WPE release tarball's Skia subset (3059 files, ~30 MB extracted) is a
  **buildable prune** — if the full 216 MB tree ever becomes a problem for
  the target leg, the subset is a proven-equivalent alternative source.
