# webkit-hobbyos — the HobbyOS WebKit fork, vendored into this repo (source-first)

The HobbyOS browser is a [WebKit](https://github.com/WebKit/WebKit) fork whose
`WebProcess` runs inside HobbyOS under the WK-5 windowed shell (browser.md
Track B, AD-11).  Since 2026-10-07 the fork source lives **in this
repository**: building the browser from that source is the supported path.
The binaries under `cache/` are an OPTIONAL shortcut for machines that have
not built anything yet — never a requirement.

## Contents

| Path | Role |
|---|---|
| `webkit-hobbyos-rp-f-45349cb2.tar.xz.part-00/01` | The fork snapshot (split tarball; parts are sha-verified) |
| `extract.sh` | Reassemble the parts → verify sha256 → extract to `src/` |
| `build.sh` | Build `WebProcess` from the vendored source → `build/<arch>/browser.bin` |
| `rebuild_browser.sh` | Regenerate the optional caches (`cache/browser-{arm,x64}.bin.xz`) from the vendored source |
| `cache/browser-arm.bin.xz`, `cache/browser-x64.bin.xz` | Optional binary cache (see below) |
| `SHA256SUMS` | All hashes (parts, reassembled snapshot, raw tar, cache) |
| `src/`, `src-wk2/`, `build/` | Rebuilt workspaces (gitignored; produced by `extract.sh` / `build.sh`) |

Quick start (from the repo root):

```sh
bash third_party/webkit-hobbyos/extract.sh           # verify + extract -> src/
bash third_party/webkit-hobbyos/build.sh --arch arm  # from-source build (add --build-deps on a fresh machine)
make ARCH=arm MODE=desktop disk.img                  # auto-picks build/arm/browser.bin when no fork build exists
```

`make browser` is the same thing wired into make:
`make browser ARCH=arm BUILD_ARGS='--build-deps'`;
`build.sh --configure-only` stops after the configure step (fast dependency
sanity check), and `--deps-only` just builds the cross-deps prefix.

## Snapshot provenance

- Fork origin: `https://github.com/WebKit/WebKit.git` (clone; base = tag
  `webkitgtk-2.54.0` @ `5220e80b97a253c60ed899361654142ab5021998`).
- Snapshot ref: **`browser/rp-f` @ `45349cb2929b07035f90b3b4fc7db49c3c6ecfab`**
  (2026-10-07) — the integration tip containing every merged port line
  (`browser/l8-wk5` @ `cdabe011af` included).  291 commits / 3,953 files over
  the base; port content is grouped under `HobbyOS/` in the tree (AD-11).
- Exclusions (and why):
  - Test suites: `LayoutTests/`, `JSTests/`, `PerformanceTests/`, `Websites/`,
    `WebDriverTests/`, `ManualTests/` — not build inputs.
  - Apple-only trees: `WebKitLibraries/`, `WebKit.xcworkspace/`,
    `Configurations/`.
  - `Source/ThirdParty/libwebrtc` — not referenced by the port's build graph
    (`ENABLE_WEB_RTC=OFF` in `Source/cmake/OptionsHobbyOS.cmake`; zero refs in
    the arm/intel `build.ninja`s; the upstream release tarball omits it too).
  - `HobbyOS/continuation/` — ~33 GB of lane receipts/evidence (a working
    archive, not port source).
  - Root-level lane scratch (`*-BRIEF.md`, `fs-*-REPORT.json`, `logs/`, agent
    dirs).  `Source/ThirdParty/{skia,ANGLE,capstone,...}` are KEPT (skia is on
    the port's link graph; ANGLE/capstone mirror the upstream release-tarball
    composition).
- Contents: 58,592 files; raw tar 865,198,080 B; xz 173,702,644 B over two parts.

SHA256 (canonical list in `SHA256SUMS`):

| Artifact | SHA256 |
|---|---|
| reassembled snapshot `.tar.xz` | `3baa2237da6f70e81055ba2b92bbca124a4d82049186920277f440778f0cea17` |
| `...part-00` | `9ab77fd43d17dc1ed710015d5b907fcae5612542242299cc170b2123e7cf329d` |
| `...part-01` | `aefdfc20be732fc61b10699ed68e57fe64beb8ec67d76b9fc5c44c88fa88c68a` |
| raw tar (reference) | `5d86449c5e506bcf757b634529af7e39660bf978510e2601facf78a82697b2d5` |

Refreshing the snapshot when the fork moves (run in the fork clone, from the
desired ref; then re-split with `split -b 90000000 -d`, update `SHA256SUMS`
and this README — the full 2026-10-07 exclusion list is in the browser.md §11
entry for that date):

```sh
git archive --format=tar --prefix=webkit-hobbyos-<ref>/ <ref> \
  ':(exclude)HobbyOS/continuation' ':(exclude)LayoutTests' ':(exclude)JSTests' \
  ':(exclude)PerformanceTests' ':(exclude)Websites' ':(exclude)ManualTests' \
  ':(exclude)WebDriverTests' ':(exclude)WebKitLibraries' \
  ':(exclude)WebKit.xcworkspace' ':(exclude)Configurations' \
  ':(exclude)Source/ThirdParty/libwebrtc' \
  | xz -T0 -6 > webkit-hobbyos-<ref>.tar.xz
```

## Build from source (supported path)

`build.sh` runs: extract → deps check → configure → ninja → flat image +
verification (including a `WK5WindowDriver` marker-symbol check).

The canonical configure recipe it runs (mirrors
`src/HobbyOS/scripts/wk2-link-ci.sh`; relative paths shown — `build.sh` uses
absolute ones):

```sh
CMAKE=~/.local/share/l6-tools/cmake-3.31.8-linux-x86_64/bin/cmake   # pinned 3.31.8; fallback: cmake
R="$(pwd)"   # HobbyOS repo root
"$CMAKE" -S third_party/webkit-hobbyos/src \
         -B third_party/webkit-hobbyos/build/arm/WebKitBuild -G Ninja \
  -DPORT=HobbyOS -DHOBBYOS_ARCH=arm \
  -DCMAKE_TOOLCHAIN_FILE="$R"/third_party/webkit-hobbyos/src/HobbyOS/toolchain-hobbyos.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=ninja \
  -DUSE_LCMS=OFF -DUSE_WOFF2=OFF -DUSE_JPEGXL=OFF -DUSE_AVIF=OFF \
  -DWDK_FORK_DIR="$R"/third_party/webkit-hobbyos/src \
  -DHDYOS_SYSROOT="$R" \
  -DHOBBYOS_WK2_PREFIX="$R"/third_party/webkit-hobbyos/src-wk2/arm/prefix
ninja -C third_party/webkit-hobbyos/build/arm/WebKitBuild WebProcess
```

The last three `-D` flags are the vendoring delta: they re-point the
toolchain file's defaults (`~/webkit-hobbyos`, `~/Documents/GitHub/HobbyOS`,
`<fork>-wk2/<arch>/prefix`) at this repo's extracted copy, so nothing
resolves through a developer's home directory.

### Dependencies on a fresh machine

1. Tools on `PATH`: `clang`, `ld.lld`, `llvm-ar`/`llvm-objcopy`, `ninja`,
   `xz`, `tar`; cmake 3.31.8 (the pinned l6-tools copy, or pass `CMAKE=...`).
2. OS-side sysroot closure in this repo — `obj/<arch>/crt0.o`,
   `obj/<arch>/libc.a`, `obj/<arch>/libcxx.a` (plus `obj/<arch>/setjmp.o` on
   intel) and `obj/<arch>/icu/libicu*.a` — produced by the OS repo's own
   build targets / cross recipes (`third_party/icu-78.3/build-target.sh`,
   `third_party/libcxx-21.1.8/build-target.sh`, the Makefile's `OBJ_DIR`
   targets).  `rebuild_browser.sh` builds any missing pieces automatically
   via these same make targets.
3. Cross-deps staging prefix (`src-wk2/<arch>/prefix`) — `build.sh
   --arch <arch> --build-deps` runs the port's own recipes
   (`rebuild_browser.sh` adds `--build-deps` automatically when missing):
   - `src/HobbyOS/scripts/wk2-libs-cross.sh --only zlib,png,jpeg,webp,
     freetype,hbcore,sqlite,xml2`: zlib, libpng, libjpeg-turbo, libwebp,
     freetype, HarfBuzz, sqlite3, libxml2 — source tarballs come from this
     repo's committed `third_party/` (libxml2 is downloaded from gnome.org).
     HarfBuzz is the minimal-core build ("hbcore", D-15 — the full
     CMake/ICU harfbuzz is blocked in the port and its default order entry
     would abort the deps stage), so the canonical `--only` set is used.
   - `tools/cross-arm-wk2-libs.sh` / `tools/cross-intel-wk2-libs.sh`:
     mbedTLS + libcurl (mbedTLS backend) + the net-compat closure objects,
     from this repo's committed tarballs (the arm half is the in-repo port
     of the fork's `continuation/wk4b/wk4b-libs-cross.sh`).
4. `WebProcess` is the browser; the other process binaries build with the
   same recipe via `--targets "WebProcess NetworkProcess HobbyOS-UIProcess"`.

## Optional binary cache

For a machine that wants the browser without a from-source build, the
committed cache carries the two known-good flat images:

| Cache file | Flat sha256 (decompressed) | Size | Built from |
|---|---|---|---|
| `cache/browser-arm.bin.xz` | `1ebac9a4c2dc04b693bdb3c550ba9e018d003d536942e1e75ef64edee4cb3b03` | 87,507,168 B | `WebKitBuild/HobbyOS-arm-wk5` `WebProcess` (ELF sha256 `0a815545…`), 2026-10-07 |
| `cache/browser-x64.bin.xz` | `8f256a3641c2ec70e7723d5d04dabd1036a2e438339bda59429aa04fa6a208ba` | 101,206,816 B | wf1b `WebKitBuild/HobbyOS-intel` `WebProcess` (ELF sha256 `a90337ea…`), 2026-10-06 |

`make disk.img` uses a cache file only when neither a `BROWSER_BIN` file
(explicit override or a local fork/worktree build) nor a vendored-source
build (`build/<arch>/browser.bin`) exists; the line it prints says
`(cache)` when the cache is what shipped.

### Rebuilding the cache

`rebuild_browser.sh` regenerates the cache files from the vendored source
(either architecture, or both), and on a fresh clone it sets the machine up
on its own: any missing OS-side sysroot closure pieces
(`obj/<arch>/{crt0.o,libc.a,libcxx.a[,setjmp.o]}` + `obj/<arch>/icu/libicuuc.a`)
are built from the OS tree's own make targets, and a missing cross-deps
prefix is built via `build.sh --build-deps` from the committed `third_party/`
tarballs — `--no-build-closure` / `--no-build-deps` turn those off.

```sh
bash third_party/webkit-hobbyos/rebuild_browser.sh both        # or: arm | x64
# on this workstation, reuse the existing prefixes instead of --build-deps:
bash third_party/webkit-hobbyos/rebuild_browser.sh both \
  --prefix-arm ~/webkit-hobbyos-wk2/arm/prefix \
  --prefix-x64 ~/webkit-hobbyos-wk2/intel/prefix
```

It then drives `build.sh`, re-compresses the resulting flat with the
committed settings (`xz -T0 -6`), verifies the round-trip (decompressed
sha256 == flat sha256), rewrites the `cache/…` lines in `SHA256SUMS`, and
prints the new hashes — then commit `cache/` + `SHA256SUMS` to publish.
`--from-existing` refreshes the cache from `build/<arch>/browser.bin`
without rebuilding.  A first run on a fresh machine is long (closure +
deps + a full WebKit build per arch); `-j N` is forwarded throughout.

License note: the WebKit source tree carries its own license files; the
binary license/relink package for the shipped `WebProcess` is recorded in
the fork's `HobbyOS/continuation/wk7-license/` (decision D-16).
