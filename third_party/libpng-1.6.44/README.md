# libpng 1.6.44 — vendored host build (L6, browser/l6-libs1)

Carry-over pin of browser.md §2 (`libpng-1.6.44.tar.xz`). WPE 2.54.0 calls
`find_package(PNG REQUIRED)` with no version floor
(`docs/browser/f0-deps-audit.md` §1.1/§1.3; the Win port's 1.6.34 pin is the
only in-tree version constraint, so 1.6.44 satisfies it). Host-first build
only — **not** wired into the HobbyOS Makefile; `build.sh` is the recipe the
WPE/port cross builds reuse.

| Field | Value |
|---|---|
| Tarball | `libpng-1.6.44.tar.xz` (1,045,640 bytes) |
| Upstream | https://download.sourceforge.net/libpng/libpng-1.6.44.tar.xz |
| sha256 | `60c4da1d5b7f0aa8d158da48e8f8afa9773c1c8baa5d21974df61f1886b8ce8e` |
| Checksum file | `libpng-1.6.44.tar.xz.sha256` (committed) |
| License | libpng license — `LICENSE` copied from the tarball |
| Vendored | 2026-09-30, downloaded from the URL above |

**sha256 provenance:** recomputed locally on download; matches the **official**
checksum published by the libpng maintainers in the png-mng-announce release
message (sourceforge.net/p/png-mng/mailman/message/58815959/ — lists
`libpng-1.6.44.tar.xz 60c4da1d…`), and matches the browser.md §2 pin.

## Layout

    libpng-1.6.44.tar.xz         pinned tarball (committed)
    libpng-1.6.44.tar.xz.sha256  checksum (committed)
    README.md                    this file
    build.sh                     host build + smoke recipe (committed)
    smoke/png_smoke.c            generated-PNG decode/re-encode smoke (committed)
    LICENSE                      upstream license text (committed)
    src/                         extracted upstream tree   (gitignored, rebuilt)
    build-host/                  host build + smoke output (gitignored, rebuilt)

## Build recipe (verified)

`./build.sh` (idempotent; `ZLIB_PREFIX`, `JOBS`, `CC` overridable). It links
against the **third_party zlib build**
(`../zlib-1.3.1/build-host/prefix`, override with `ZLIB_PREFIX`) and fails
loudly if libpng resolves a different zlib:

    cmake -S src -B build-host/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=build-host/prefix \
      -DZLIB_ROOT=<zlib prefix> -DCMAKE_PREFIX_PATH=<zlib prefix> \
      -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
    cmake --build build-host/build -j4
    cmake --install build-host/build

Toolchain used: CMake 3.31.8 (official Kitware linux-x86_64 binary, installed
rootless at `~/.local/share/l6-tools/cmake-3.31.8-linux-x86_64`; on PATH as
`~/.local/bin/cmake`), gcc 15.2.0, GNU make 4.4.1. Installed into
`build-host/prefix/`: `include/png.h`, `include/pngconf.h`,
`include/pnglibconf.h`, `lib/libpng.a` + `lib/libpng16.a` (same bytes; the
1.6.x install also provides the versioned name), `lib/pkgconfig/libpng.pc`.

## Smoke test

`smoke/png_smoke.c` — generates a 64x64 RGBA gradient, writes it as PNG,
decodes it back, re-encodes the decoded pixels, decodes the second file, and
requires byte-exact pixel equality at every step (PNG is lossless). Prints
FNV-1a fingerprints of both decoded stages — the repeatable values diffed by
the lane's checksum-stability gate (`third_party/l6-libs1-gate.sh`). Run via
`build.sh`; the result line lands in `build-host/SMOKE.RESULT`.

## WPE / cross-build reuse

Same cmake line with a cross toolchain (`-DCMAKE_C_COMPILER=…`, a toolchain
file, `-DZLIB_ROOT` pointing at the cross zlib). WPE consumes the result via
`find_package(PNG)` with `CMAKE_PREFIX_PATH` pointed at `build-host/prefix`.
