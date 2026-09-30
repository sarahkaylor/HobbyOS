# libwebp 1.6.0 — vendored host build (L6, browser/l6-libs1)

**Version chosen and recorded here:** browser.md §2 lists libwebp
"versionless" (pin at W0); **1.6.0** was selected as the latest stable release
in the official webmproject releases directory (released 2025-07-09; verified
as newest there on 2026-09-30). Proposed for the browser.md §2 pin table by
the lane report. WPE 2.54.0 calls
`find_package(WebP REQUIRED COMPONENTS demux)` with no version floor
(`docs/browser/f0-deps-audit.md` §1.1; §1.3 notes WebP has no in-tree version
check at all). Host-first build only — **not** wired into the HobbyOS
Makefile; `build.sh` is the recipe the WPE/port cross builds reuse.

| Field | Value |
|---|---|
| Tarball | `libwebp-1.6.0.tar.gz` (4,296,070 bytes) |
| Upstream | https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz |
| sha256 | `e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564` |
| Checksum file | `libwebp-1.6.0.tar.gz.sha256` (committed) |
| License | BSD-3-Clause — `COPYING` + `PATENTS` copied from the tarball |
| Vendored | 2026-09-30, downloaded from the URL above |

**sha256 provenance:** the webmproject releases publish GPG `.asc` signatures
only, no sha256 files — so this is the recorded computed value. It is
corroborated by independent packaging records for the same file:
FreeBSD ports `graphics/webp` distinfo and Buildroot's `webp.hash`
(both `e4ab7009…` for `libwebp-1.6.0.tar.gz`).

## Layout

    libwebp-1.6.0.tar.gz          pinned tarball (committed)
    libwebp-1.6.0.tar.gz.sha256   checksum (committed)
    README.md                     this file
    build.sh                      host build + smoke recipe (committed)
    smoke/webp_smoke.c            lossless roundtrip smoke (committed)
    COPYING, PATENTS              upstream license texts (committed)
    src/                          extracted upstream tree   (gitignored)
    build-host/                   host build + smoke output (gitignored)

## Build recipe (verified)

`./build.sh` (idempotent; `JOBS`, `CC` overridable). Library-only static
build — the CLI tools are off (they pull extra deps: giflib, GLUT, image I/O;
nothing the WPE port needs):

    cmake -S src -B build-host/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=build-host/prefix \
      -DBUILD_SHARED_LIBS=OFF \
      -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
      -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
      -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF \
      -DWEBP_BUILD_FUZZTEST=OFF
    cmake --build build-host/build -j4
    cmake --install build-host/build

Toolchain used: CMake 3.31.8 (Kitware official binary, rootless install),
gcc 15.2.0, GNU make 4.4.1. Installed into `build-host/prefix/`:
`include/webp/{encode,decode,demux,mux,types}.h`, `lib/libwebp.a`,
`lib/libwebpdemux.a`, `lib/libwebpmux.a`, `lib/libsharpyuv.a`
(+ pkgconfig files). `webpdemux` is exactly the component WPE's
`find_package(WebP COMPONENTS demux)` requires.

## Smoke test

`smoke/webp_smoke.c` — lossless RGBA roundtrip: `WebPEncodeLosslessRGBA` on a
generated 64x64 gradient (with alpha), `WebPDecodeRGBA` back, byte-exact
equality required (lossless must be exact). Prints the encoder version and an
FNV-1a fingerprint of the decoded pixels — the repeatable values diffed by the
lane's checksum-stability gate (`third_party/l6-libs1-gate.sh`). Run via
`build.sh`; the result line lands in `build-host/SMOKE.RESULT`.

## WPE / cross-build reuse

Same cmake line with a cross toolchain (`-DCMAKE_C_COMPILER=…` + toolchain
file). WPE consumes the result via `find_package(WebP COMPONENTS demux)` with
`CMAKE_PREFIX_PATH` pointed at `build-host/prefix`.
