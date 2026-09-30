# skia-588b550 — the Skia pin WPE(WebKit) 2.54.0 expects + CPU-raster host recipe

Lane `browser/l6-skia` (browser.md §2 Skia pin, §7.5 L6, AD-4, AD-11).
This directory is the **pinned, re-fetchable recipe**: fetch script + pinned
revision + checksums + the WebKit-authored build files + the host build/smoke
scripts.  The 80 MB archive, the extracted upstream tree and the host build
tree are rebuilt from this directory and stay gitignored (AD-11 pattern,
same as l5-fonts / l6-libs1 / l6-libs2).

Research note with the full evidence + two-build-rule reuse plan:
`docs/browser/l6-skia-host-spike.md`.

## The pin

| field | value |
|---|---|
| Upstream | `https://skia.googlesource.com/skia` |
| Commit | `588b550a4dd8af90dbe71c0554852806bd8f0b21` (2026-08-30, autoroll) |
| Where WebKit records it | `Source/ThirdParty/skia/README.WebKit` → `- Commit: 588b550a4dd8af90dbe71c0554852806bd8f0b21` (vendored verbatim in `webkit/`) |
| WebKit 2.54.0 identity | fork clone `~/webkit-hobbyos` @ `5220e80b97` ("…Update … for 2.54.0 release") |
| Fetch | `https://skia.googlesource.com/skia/+archive/588b550a4dd8af90dbe71c0554852806bd8f0b21.tar.gz` |
| Extraction | complete upstream tree @ pin, 12395 entries (12394 files + 1 symlink), 216 MB |
| Content gate | `manifest_sha256=c27786cade6336a16e284fc9ccf09a137eaba3684ddaf94d9414aab4d0d47213` (see below) |
| Host build result | `libSkia.a` 15,079,504 B (595 objects), smoke `sha256(pixels)=eee5d809bcfb13e9e25dc6faaddb99da958580afd9ae7954c4f9feb0cac46eae` |

### Provenance cross-checks (all re-verified 2026-09-30, read-only)

* **Upstream @588b550a ↔ WPE 2.54.0 release tarball** (`third_party/wpewebkit-2.54.0/Source/ThirdParty/skia`): 3057 common files, **0 content differences**; the tarball side adds exactly the two WebKit-authored files (`CMakeLists.txt`, `WebKitSkiaConfig.h` — `README.WebKit` is pruned from release tarballs), prunes 9338 dev files (tests/docs/tools/infra/bazel/gn/…) plus 554 build-meta files inside `src/ include/ modules/` (BUILD.bazel/BUILD.gn/OWNERS-style + unused modules), and leaves one dangling symlink (`src/ports/fontations/Cargo.toml → ../../../bazel/external/...`, `bazel/` pruned; unused by the CMake build).  WPE tarball subset manifest: `299fb4d93734181d100b304fc36a8cd8d6de8cfb46d691b2f8a7ec6f64b10577` (3059 entries).
* **Upstream ↔ fork clone** (`~/webkit-hobbyos/Source/ThirdParty/skia`): 12386 common files, **0 content differences**; the clone adds only the 3 WebKit files (`CMakeLists.txt`, `WebKitSkiaConfig.h`, `README.WebKit`); upstream has 9 files WebKit's ignore rules dropped (7× `package-lock.json`, `gradle.properties`, `externs.js`) — none referenced by the CMake build.  Clone manifest: `5cd5a19f61e6a562e3a4d0cf0040bd777c79f7ffff0436ec61225a7c5a777b65` (12389 entries).
* `tools/compare-with-webkit.py` reproduces these numbers against a local fork clone / tarball extraction.

### Integrity model (why a content manifest, not the tarball sha)

`skia.googlesource.com/+archive/…tar.gz` **containers are not byte-reproducible** —
three downloads of the same commit on 2026-09-30 produced different sha256s:
`307628b6…`, `3f2de0db…`, `52b4faa5…` (container bytes differ; extracted
content identical).  The gate is therefore the canonical content digest
(`manifest.py`: sha256 over the byte-sorted `<path>\t<sha256|LINK:target>`
list, excluding the 3 overlay names).  `fetch.sh` hard-fails on mismatch and
then overlays `webkit/` (each file checked against `webkit/SHA256SUMS`).

## The build files (`webkit/`)

Vendored verbatim from the fork clone @5220e80b97 so the recipe is
self-contained; hashes in `webkit/SHA256SUMS`:

* `CMakeLists.txt` — WebKit's Skia build (the recipe WPE 2.54.0 actually uses;
  `sha256=571da2e0…`; identical to the copy inside the WPE release tarball).
* `WebKitSkiaConfig.h` — `SK_USER_CONFIG_HEADER` (`GR_AA_TESSELLATOR_MAX_VERB_COUNT 100`).
* `README.WebKit` — names the URL + commit.
* `../cmake/FindWebP.cmake`, `../cmake/FindHarfBuzz.cmake` — WebKit's find
  modules (BSD headers retained), needed standalone because CMake ships
  neither.

## Usage

```sh
./fetch.sh          # download + content-gate + overlay WebKit files
./setup-deps.sh     # rootless fontconfig headers (apt-get download + dpkg-deb -x)
./build.sh          # gen_sources.py -> cmake -> ninja -j4 -> smoke x2 -> checksum
#   knobs: JOBS, SKIA_FONTMGR=fontconfig|empty, SKIA_ENCODERS, SKIA_OPENTYPE_SVG,
#          SKIA_DEBUG, *_PREFIX (host dep prefixes), SKIA_SMOKE_RECORD=1
```

Evidence lands under `build-host/`: `build.log` (full), `smoke-run{1,2}.log`,
`smoke{1,2}.rgba(+.ppm)`, `generated/sources-report.json`,
`generated/excluded-gpu.txt`, `gn-probe.log`.

## Host build record (2026-09-30; this machine)

* Toolchain: gcc 15.2.0 (GNU, C++23 like WebKit), CMake 3.31.8, Ninja 1.13.2, `-j4`.
* WebKit's 774-file `add_library(Skia …)` list → **524 CPU sources** (250
  `src/gpu/**` excluded; `generated/excluded-gpu.txt`), plus POSIX 4 +
  fontconfig 5 + encoders 7 + SVG/shaper 56 + D7 extra 1 = **597 TUs**;
  597 ninja edges, ~80 s at `-j4`; `libSkia.a` = 15,079,504 B / 595 objects / 7209 `T` symbols.
* Dependencies resolve to the vendored host prefixes:
  freetype 2.14.3 (l5-fonts), harfbuzz 14.5.0 (l5-fonts), zlib 1.3.1 /
  libpng 1.6.44 / libjpeg-turbo 3.1.0 / libwebp 1.6.0 (l6-libs1), fontconfig
  2.17.1 (rootless deb header extraction), expat 2.7.4 (system).
* Smoke (`smoke/skia_smoke.cpp`, fixed 320×240 scene: fills, AA circle,
  `SkShaders::LinearGradient`, stroked `SkRRect`, dashed `SkPathBuilder` cubic,
  rotate + multiply blend): two runs **byte-identical**;
  `sha256=eee5d809bcfb13e9e25dc6faaddb99da958580afd9ae7954c4f9feb0cac46eae`
  (pinned in `smoke/smoke.sha256`), `fnv1a64=0xb2e29142c06f8b86`; PPM shows the
  expected exact fills (e.g. the 96×64 rect = 6144 px of `#1e78dc`).

## Recipe deltas vs `webkit/CMakeLists.txt` (all deliberate, all documented)

| # | delta | why |
|---|---|---|
| D1 | `src/gpu/**` sources dropped; `SK_GL`/`SK_GANESH` not defined (WebKit :1064-1071 defines them unconditionally) | CPU raster only (AD-4; HobbyOS has no GL/EGL) |
| D2 | `SK_ASSUME_*` dropped (GPU-only); `SK_TRIVIAL_ABI` stays Clang-gated (:1060-1062) | follows D1; GCC build like WebKit's Linux GCC builds |
| D3 | framework header symlinks recreated under `${CMAKE_BINARY_DIR}/framework` | standalone instead of WebKit's `Skia_FRAMEWORK_HEADERS_DIR` |
| D4 | WebKit's `WEBKIT_ADD_TARGET_CXX_FLAGS` / `WEBKIT_CHECK_COMPILER_FLAGS` inlined; `-fno-exceptions -fno-rtti` added | match WebKit's global flags (WebKitCompilerFlags.cmake:203-204); warning-suppression list copied verbatim |
| D5 | `find_package(EXPAT REQUIRED)` replaces WebKit's fatal-if-missing | same requirement |
| D6 | `SKIA_FONTMGR=fontconfig\|empty` switch; `empty` swaps in `SkFontMgr_custom.cpp` + `SkFontMgr_custom_empty.cpp` (upstream `gn/ports.gni:52-54,96`) | target leg (HobbyOS) has no fontconfig; host leg stays WPE-faithful |
| D7 | `+src/core/SkStrikeRef.cpp` (extra, upstream `gn/core.gni:572`) | **WebKit 2.54.0's CMakeLists omits this TU**; `SkFont.cpp:240` references `SkStrikeRef`'s ctor and a plain static link fails without it (`undefined reference`). WebKit's own final link may hide the gap via dead-stripping — WK-3: check before relying on it; candidate upstream CMake fix |

## GN / depot_tools alternative (probed 2026-09-30, bounded)

* `python3 bin/fetch-gn` works **rootlessly** (no depot_tools): `bin/gn` v2175.
* `gn gen out/host-cpuonly --args='skia_use_gl=false skia_use_egl=false
  skia_use_vulkan=false skia_enable_ganesh=false skia_enable_graphite=false
  is_official_build=false skia_enable_tools=false
  skia_enable_spirv_validation=false skia_use_system_{zlib,libpng,
  libjpeg_turbo,freetype2,expat,harfbuzz,icu}=true'` → OK (104 targets / 65 files).
* `ninja -C out/host-cpuonly skia` → fails immediately:
  `third_party/externals/libwebp/sharpyuv/sharpyuv.c … missing` — the GN graph
  pulls DEPS externals (47 `third_party/externals` entries in `DEPS`), which
  only `tools/git-sync-deps` (multi-repo, multi-GB) provides.
* Conclusion: GN is **not** the recipe WPE 2.54.0 uses (WebKit vendors Skia
  in-tree + its own CMake); kept as a documented fallback only.  Log: `build-host/gn-probe.log`.

## License

Skia is BSD-3 (`LICENSE` in the fetched tree); `webkit/*` and `cmake/*` are
WebKit files (BSD, headers retained; hashes in the `SHA256SUMS` files).
The fontconfig dev package is downloaded at setup time and only extracted
locally — nothing from it is redistributed here.
