# ICU4C 78.3 — vendored host build (L6, browser/l6-icu)

ICU4C is the Unicode/i18n library both browser tracks need (JSC + WebKit's
Intl, text-break, normalization, collation and encoding machinery; WPE 2.54.0
requires it via `find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)`).
This directory holds the **host-first** pin: pinned release + digests + build
recipe + smoke test + the data-packaging strategy for the bare-metal target.
Nothing here is wired into the HobbyOS Makefile — WPE/JSC consume
`build-host/prefix` (host), and the future target build reuses the same
recipe with a cross toolchain (see `cross-notes.md` for the feasibility memo).

## Pin & provenance

| Field | Value |
|---|---|
| Release | ICU 78.3 (`release-78.3`), published 2026-03-17 |
| Sources tarball | `icu4c-78.3-sources.tgz` (27,977,255 B) |
| Sources URL | https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz |
| sha256 | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |
| sha512 | `04a49455e1489030c520a4bfd2664fa2171e7938d08f2acdbbcb1fda976639fd8b1f0704f2eec89ba59a7b6d118ceaab6ec5a096e40d9085a0895d91ce225245` |
| md5 | `a7b736b570ef0e180c96a31715a00c78` |
| Data sources (optional) | `icu4c-78.3-data.zip` (20,187,983 B), sha256 `9d8b3899096aeb83e4e21ef8a40fec9e03b28db18c48452efac882ce25a91e27` — **not** in the sources tarball; needed for building/trimming data from source (see "Data packaging strategy") |
| License | Unicode License (ICU) — `src/LICENSE`, also installed to `prefix/share/icu/78.3/LICENSE` |
| Vendored | 2026-09-30 (`fetch.sh` re-verifies every run) |

**sha256 provenance:** the GitHub release-asset digest for `release-78.3`
(queried from the release API at vendoring time) — cross-checked against the
two upstream-published checksum records committed here (`SHASUM512.txt`
sha512 and `icu4c-78.3-sources.md5` md5). All three independently published
values match; `fetch.sh` verifies all three on every fetch.

## Version choice (newest stable >= 70.1)

- **WPE 2.54.0 floor = 70.1, no upper bound.** `Source/cmake/OptionsWPE.cmake:14`
  and `OptionsJSCOnly.cmake:117` both do `find_package(ICU 70.1 REQUIRED
  COMPONENTS data i18n uc)`; `Source/cmake/FindICU.cmake:37-46` defers to
  CMake's built-in finder on non-Apple. Nothing in the 2.54.0 tree caps the
  version (checked 2026-09-30 against the extracted tarball).
- **78.3 is the newest *stable* ICU4C release** (2026-03-17). 79.1rc
  (2026-09-25) is a prerelease and was rejected by pin policy. The l5-fonts
  lane already smoke-built harfbuzz `+hb-icu` against an ICU 78.2 host, so
  the 78.x series is proven in-tree; 78.3 is the same series with fixes.
- **Symbol renaming: kept at ICU's default (enabled).** On Linux/WPE, WebKit
  does *not* define `U_DISABLE_RENAMING` — the only definitions are Apple-only
  (`Source/WTF/wtf/Platform.h:126-128` inside `#if PLATFORM(COCOA)`;
  `OptionsJSCOnly.cmake:118-120` inside `if (APPLE)`), and WTF's vendored
  ICU-74 headers (`Source/WTF/icu/unicode/`) are copied only
  `if (APPLE AND NOT USE_APPLE_INTERNAL_SDK)` (`Source/WTF/CMakeLists.txt:5-6`).
  So the WPE build compiles against the *found* ICU's headers, with default
  versioned symbols/namespace (`u_strToUpper_78`, `icu_78`). Our build must
  not pass `--disable-renaming` — it doesn't. (`-DU_DISABLE_RENAMING=1` is
  the Apple shape, not ours.)
- WebKit's version guards are all forward-compatible (`#if
  U_ICU_VERSION_MAJOR_NUM >= 67/68/74`; `... < 64`), and WebKit compiles with
  `U_HIDE_DEPRECATED_API 1` (`Platform.h:120`), i.e. it uses no deprecated
  APIs — low risk on a newer major. Residual risk (API removal in 78.x that
  WebKit still calls) is only discoverable at the WPE configure/build; note
  anything found there in browser.md.

## Layout

    fetch.sh                       download + 3-digest gate + extract (committed)
    icu4c-78.3-sources.tgz.sha256  local sha256 record (committed)
    icu4c-78.3-sources.md5         upstream-published md5 record (committed)
    SHASUM512.txt                  upstream-published sha512 record (committed)
    README.md                      this file (committed)
    build-host.sh                  host build + smoke recipe (committed)
    build-host-trimmed.sh          trimmed-data rebuild + smoke (committed)
    data-filter-en.json            ICU_DATA_FILTER_FILE used by the above (committed)
    smoke/icu_smoke.c              smoke test source (committed)
    smoke/run-smoke.sh             compile + double-run smoke driver (committed)
    cross-notes.md                 target cross-build feasibility memo (committed)
    icu4c-78.3-sources.tgz         27 MB tarball         (gitignored, rebuilt)
    icu4c-78.3-data.zip            20 MB data sources    (gitignored, only with --data-src)
    src/                           extracted upstream tree (gitignored, rebuilt)
    build-host/                    build trees, prefixes, logs (gitignored, rebuilt)

## Build recipe (verified)

    ./build-host.sh                # JOBS=4; override JOBS / CC / CXX / ICU_PREFIX

which runs (out-of-tree build in `build-host/obj/`):

    ./fetch.sh                     # download + sha256/sha512/md5 gate + extract
    "$SRC/source/configure" --prefix=build-host/prefix \
      --disable-shared --enable-static \
      --disable-samples --disable-tests --disable-extras --disable-icuio \
      --with-data-packaging=static
    make -j$JOBS && make install
    smoke/run-smoke.sh

Deliberate choices, all documented in the script header:
- static libs + `--with-data-packaging=static` (bare-metal target; see below);
- **tools stay enabled** — `genrb/pkgdata/genbrk/…` are needed for the data
  build and the same build tree is the future `--with-cross-build` host-tools
  staging area (see `cross-notes.md`);
- renaming default (see version choice);
- `--disable-icuio` (C++-streams io lib — unused), `--disable-extras`,
  `--disable-samples`, `--disable-tests`.

Toolchain: gcc 15.2.0 / GNU make 4.4.1 on this host. **Fresh full pipeline
(fetch-verify → configure → make → install → smoke ×2): 17.5 s wall** with
`JOBS=32` (AMD Ryzen AI MAX+ 395, 32 threads; 3m32s user time). Build log:
`build-host/build.log`.

### Sizes (full data, static)

| artifact | bytes | note |
|---|---|---|
| `libicudata.a` | 33,108,268 | data baked in (`icudt78l_dat.o`); **no `.dat` file** |
| `libicuuc.a` ("uc", common) | 5,062,968 | code |
| `libicui18n.a` ("i18n") | 10,966,538 | code |
| `libicutu.a` | 472,196 | tool support (build-time only) |
| `libicutest.a` | 164,144 | test fw (unused; small) |
| `prefix/lib` total | 48 MB | |
| **WebKit's `data i18n uc` link set** | **49,137,774** (46.9 MiB) | data + uc + i18n |

Smoke runs with `ICU_DATA` unset **and** with `ICU_DATA=/nonexistent-icu-data`
— byte-identical output (the static data is filesystem-independent), and
`find build-host/prefix -name '*.dat'` finds **0** files.

## Data packaging strategy (bare-metal decision)

Options in ICU4C (`configure --with-data-packaging=…`):

| mode | what it gives | for HobbyOS |
|---|---|---|
| `static` **(chosen)** | data compiled into `libicudata.a`; linked into the final binary; no runtime file, no `ICU_DATA`, no filesystem | ✅ zero filesystem dependency; the ~33 MB (or trimmed) lands in the image/RAM once |
| `archive` | standalone `icudt78l.dat` (~33 MB) loaded at runtime from disk | ❌ would need FAT16 file staging + read-into-RAM; no mmap path |
| `library` | data in a shared `libicudata.so` | ❌ needs dynamic-ELF loading (target uses static links) |

**Trimming (`ICU_DATA_FILTER_FILE`)** — resolved here for the F0 open item
("data-trimming approach still open — resolve at the L6 ICU build"):

- The filter JSON is consumed **at configure time** (it generates
  `data/rules.mk`); `data-filter-en.json` keeps only `en`+`root` locale trees
  and drops the `brkitr_dictionaries` feature (Thai/Lao/Khmer/Myanmar/JA word
  dictionaries).
- **Caveat that cost real time (documented so nobody re-discovers it):** the
  release tarball ships a prebuilt `src/source/data/in/icudt78l.dat`, and
  ICU's makefile build *silently* prefers it whenever present — the filter is
  then ignored (measured: byte-identical `libicudata.a`; the tell is the
  `Unpacking …/in/icudt78l.dat` line in the build log). The data *sources*
  are **not** in the sources tarball — they come from `icu4c-78.3-data.zip`
  (`fetch.sh --data-src` overlays them). `build-host-trimmed.sh` parks the
  prebuilt archive, then data is really rebuilt from source (809 genrb/genbrk
  tool runs; conversions/unames/emoji/break rules kept).
- **Measured result** (same configure flags, only the filter differs):

  | | full data (prebuilt path) | trimmed `en` (source-built) |
  |---|---|---|
  | `libicudata.a` | 33,108,268 B | **10,196,012 B (−69 %)** |
  | `libicuuc.a` | 5,062,968 B | 5,062,968 B (code) |
  | `libicui18n.a` | 10,966,538 B | 10,966,538 B (code) |
  | data link set (data+uc+i18n) | 49.1 MB | **26,225,518 B (25.0 MiB)** |
  | .res files in data | 4,073 | 765 |

  `build-host-trimmed.sh` reproduces this and runs the smoke against the
  trimmed prefix (verdict + log: `build-host/SMOKE.RESULT.trim-src`,
  `build-host/smoke-trim-src/`, `build-host/trimmed.log`).
- **Product guidance for the target:** the mechanism is ready; whether the
  shipped browser keeps brkitr dictionaries (JSC `Intl.Segmenter` CJK
  quality) and non-`en` locales is a one-file filter edit + a rebuild —
  decide against the F0 flash budget, not against re-engineering. Conversion
  data (190 `.cnv`, kept here) can be subset later the same way if needed.

## Smoke test

`smoke/icu_smoke.c` (+ `smoke/run-smoke.sh`) — deterministic, C API only
(links `-licui18n -licuuc -licudata`):

1. version >= 70.1 floor; 2. `u_strToUpper("héllo wörld","en_US")` →
   `HÉLLO WÖRLD`; 3. `unorm2` NFC: `e`+U+0301 → U+00E9 + `isNormalized`
   both ways; 4. `ubrk` word pass over "The quick brown fox" = 4 words;
   5. `ucol` en_US: `"apple" < "banana"`, `=` equal, `"resume"` vs
   `"résumé"` primary-equal/tertiary-different; 6. CLDR data version readable
   (`48.0.0.0`). Verdict tail:

    PASS: ucol: tertiary strength differs for resume/résumé
    SMOKE PASS: ICU 78.3 u_strToUpper + NFC + BreakIterator + collation (static data, no filesystem)
    ALL TESTS PASSED SUCCESSFULLY!

The script runs it twice (`ICU_DATA` unset / nonexistent) and `diff`s the
runs — determinism + static-data proof in one gate. Canonical result:
`build-host/SMOKE.RESULT`.

## Cross build (bare-metal target)

Time-boxed feasibility memo with the exact two-stage recipe, probes and
blockers: **`cross-notes.md`**.

## WPE / JSC consumption notes

- Point CMake at `build-host/prefix` (`CMAKE_PREFIX_PATH`); CMake's built-in
  FindICU resolves `ICU::data/i18n/uc` from
  `prefix/lib/pkgconfig/icu-{uc,i18n}.pc` + `prefix/include/unicode/`.
- Static link order: `-licui18n -licuuc -licudata` (+ C++ runtime +
  `-lpthread`; ICU's `-lm` math too). `-ldl` only appears on hosts with
  dyload — not needed for the static/`static`-data config on the target.
- `prefix/bin/` carries the host tools (`genrb`, `genbrk`, `gencnval`,
  `makeconv`, `pkgdata`, …) for data regeneration; full tool binaries also
  live in the build tree (`build-host/obj/bin/`) which is what a future
  `--with-cross-build` consumes.
