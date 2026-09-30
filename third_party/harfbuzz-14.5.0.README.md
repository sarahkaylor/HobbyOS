# HarfBuzz 14.5.0 — vendored pin (L5 font stack)

Pinned for the HobbyOS browser program (`browser.md` §2: "harfbuzz (WebKit
min)" recorded at W0; lane L5 — fonts/images). Vendoring layout per AD-11 /
Appendix C: **committed** = this README + the tarball + its `.sha256`;
**gitignored** = the extraction (`third_party/harfbuzz-14.5.0/`) and the
build trees under `obj/`.

## Version choice

WebKit 2.54 hard-requires **HarfBuzz >= 2.7.4 with the ICU component**
(`Source/cmake/OptionsWPE.cmake:13` — `find_package(HarfBuzz 2.7.4 REQUIRED
COMPONENTS ICU)`, per `docs/browser/f0-deps-audit.md` §1.1). That is a floor,
not a pin: this lane pins the **latest stable release at vendoring time,
14.5.0** (GitHub release `14.5.0`), same "recent stable" policy as the
libcurl 8.x pick in §2, so the host stack gets upstream maintenance and
toolchain compatibility. Floor compliance: 14.5.0 >= 2.7.4. Recorded here
for the W0 pin table.

## Provenance

| Item | Value |
|---|---|
| Tarball | `harfbuzz-14.5.0.tar.xz` — 20,262,956 bytes |
| URL | https://github.com/harfbuzz/harfbuzz/releases/download/14.5.0/harfbuzz-14.5.0.tar.xz |
| sha256 | `b7132e148358a45185c9feafd049dbaf243649d3c44414b3534d9c95d18592b9` |
| Verified against | GitHub Releases API asset **digest** (`sha256:b7132e1…`, fetched 2026-09-30) — official published checksum; matched local `sha256sum` byte-for-byte |
| License | MIT ("Old MIT") — see the extraction's `COPYING` |

Note: harfbuzz >= 10 ships **no autotools `configure`** — meson or CMake
only. This lane uses the CMake build (also what Windows/PlayStation builds
use), with Ninja.

## Extraction (rebuildable from the committed tarball)

```sh
cd third_party
sha256sum -c harfbuzz-14.5.0.tar.xz.sha256
tar -xf harfbuzz-14.5.0.tar.xz          # -> third_party/harfbuzz-14.5.0/
```

## Host build — exact executed recipe

Run by `src/user/browser/fonts/host_build_fonts.sh` (idempotent; skipped when
`obj/third_party/harfbuzz/prefix/lib/libharfbuzz.a` exists):

```sh
HB_SRC=<repo>/third_party/harfbuzz-14.5.0
HB_PREFIX=<repo>/obj/third_party/harfbuzz/prefix
FT_PREFIX=<repo>/obj/third_party/freetype/prefix          # from the FreeType pin
ICU_ROOT=~/.local/share/pkg-tools/icu-dev/extracted/usr   # see "ICU" below

export PKG_CONFIG_PATH="$FT_PREFIX/lib/pkgconfig:$ICU_ROOT/lib/x86_64-linux-gnu/pkgconfig"
cmake -S "$HB_SRC" -B <repo>/obj/third_party/harfbuzz/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HB_PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    -DHB_HAVE_FREETYPE=ON -DHB_HAVE_ICU=ON \
    -DICU_ROOT="$ICU_ROOT" \
    -DCMAKE_PREFIX_PATH="$FT_PREFIX;$ICU_ROOT" \
    -DCMAKE_CXX_FLAGS="-isystem $ICU_ROOT/include"
cmake --build <repo>/obj/third_party/harfbuzz/build --parallel 4
cmake --install <repo>/obj/third_party/harfbuzz/build
```

Switch rationale / wrinkles found during bring-up:

- **static** (`BUILD_SHARED_LIBS=OFF`) — AD-6: HobbyOS links statically.
- **`HB_HAVE_FREETYPE=ON`** — compiles `hb-ft` interop into `libharfbuzz.a`
  against the FreeType pin prefix (CMake's `FindFreetype` resolves it via
  `CMAKE_PREFIX_PATH`; the configure log shows
  `Found Freetype: …/libfreetype.a (found version "2.14.3")`).
- **`HB_HAVE_ICU=ON`** — builds the separate `libharfbuzz-icu.a`, the
  component WebKit's `find_package(HarfBuzz … COMPONENTS ICU)` wants.
- **ICU include wrinkle (rootless):** HarfBuzz's CMake links ICU only as a
  raw library path — its ICU block never adds the ICU include directory (it
  assumes system-wide installation, where `/usr/include` covers it). With ICU
  dev files in a private prefix the compile fails
  (`unicode/uscript.h: No such file or directory`), hence the explicit
  `-DCMAKE_CXX_FLAGS="-isystem $ICU_ROOT/include"`. Upstream tree stays
  unpatched.
- **Glib/cairo/graphite2/gobject are OFF** (defaults) — nothing else is
  needed for shaping; `HB_BUILD_UTILS` stays OFF (needs cairo+glib).
- Subset/raster/vector/gpu libraries are built at their defaults
  (`HB_BUILD_SUBSET` etc. ON) — harmless static libs; subset is the one the
  WebKit font pipeline can reuse.

## ICU (host leg)

- Runtime: Ubuntu system package `libicu78` 78.2-2ubuntu1 (already installed).
- Dev files: rootless extraction of the matching `libicu-dev` 78.2-2ubuntu1
  deb (no root needed):

  ```sh
  mkdir -p ~/.local/share/pkg-tools/icu-dev && cd ~/.local/share/pkg-tools/icu-dev
  apt-get download libicu-dev
  mkdir -p extracted && dpkg-deb -x libicu-dev_*.deb extracted/
  # default ICU_DEV_ROOT: ~/.local/share/pkg-tools/icu-dev/extracted/usr
  ```

- The configure log shows `Found ICU: …/icu-dev/extracted/usr/include (found
  version "78.2") found components: uc`.

**Boundary (documented, deferred):** this is the *host* ICU only. The
**on-device ICU port + data trimming** is the L6 track's item (browser.md §6
L6; §10 Q3 still open) — HarfBuzz for HobbyOS will link against L6's
cross-built ICU when it lands. Until then the target-side harfbuzz-ICU
linkage is explicitly out of scope for L5.

## Toolchain at execution

Ubuntu 26.04, rootless workstation: cmake **4.2.3** and ninja **1.13.2**
(both installed rootlessly — Ubuntu debs extracted under
`~/.local/share/pkg-tools/cmake-ninja/`, with a small `~/.local/bin/cmake`
shim that sets `LD_LIBRARY_PATH` for the `librhash.so.1` extracted alongside;
`ninja` runs directly), gcc/g++ 15.2.0. Note for W0.5 follow-ups: `ruby` and
`gperf` remain absent on this workstation (still needed for the WPE WebCore
builds); cmake + ninja are now covered.

Logs (gitignored, under `obj/`):

- `obj/third_party/logs/harfbuzz-configure.log` (freetype + ICU found lines),
  `-build.log` (0 warnings), `-install.log` — rc=0 each
- cold re-run (fresh build root, same recipe, rc=0):
  `obj/third_party-cold/logs/harfbuzz-*.log`

Artifacts (installed): `libharfbuzz.a`, `libharfbuzz-icu.a`,
`libharfbuzz-subset.a`, `libharfbuzz-raster.a`, `libharfbuzz-vector.a`,
`libharfbuzz-gpu.a`, headers under `include/harfbuzz/`, `.pc` files and CMake
config under `obj/third_party/harfbuzz/prefix/`.

## Consumers in this lane

- `src/user/browser/fonts/hb_shape_smoke.c` — shapes fixed strings through
  both `hb-ot` (design units) and `hb-ft` (16 px em, FreeType memory face)
  and checksums glyph ids + positions; calls `hb_icu_get_unicode_funcs()` to
  close the ICU linkage loop.
