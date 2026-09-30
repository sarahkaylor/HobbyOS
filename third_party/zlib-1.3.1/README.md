# zlib 1.3.1 — vendored host build (L6, browser/l6-libs1)

Carry-over pin of browser.md §2 (`zlib-1.3.1.tar.gz`). WPE 2.54.0 calls
`find_package(ZLIB REQUIRED)` with no version floor
(`docs/browser/f0-deps-audit.md` §1.1/§1.3). Host-first build only — **not**
wired into the HobbyOS Makefile; `build.sh` is the recipe the WPE/port cross
builds reuse.

| Field | Value |
|---|---|
| Tarball | `zlib-1.3.1.tar.gz` (1,512,791 bytes) |
| Upstream | https://zlib.net/fossils/zlib-1.3.1.tar.gz |
| sha256 | `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` |
| Checksum file | `zlib-1.3.1.tar.gz.sha256` (committed) |
| License | zlib license — `LICENSE` copied from the tarball |
| Vendored | 2026-09-30, downloaded from the URL above |

**sha256 provenance:** recomputed locally on download; matches the browser.md
§2 pin (recorded at planning from the zlib.net download). zlib.net serves no
per-file `.sha256`/`.asc` for the fossils archive; this is therefore the
recorded computed value (the same bytes are re-downloaded and re-checked on
every gate run).

## Layout

    zlib-1.3.1.tar.gz          pinned tarball (committed)
    zlib-1.3.1.tar.gz.sha256   checksum (committed)
    README.md                  this file
    build.sh                   host build + smoke recipe (committed)
    smoke/zlib_smoke.c         deflate/inflate roundtrip smoke (committed)
    LICENSE                    upstream license text (committed)
    src/                       extracted upstream tree   (gitignored, rebuilt)
    build-host/                host build + smoke output (gitignored, rebuilt)

## Build recipe (verified)

`./build.sh` (idempotent; `JOBS`, `CC`, `CFLAGS_EXTRA` overridable). It runs
zlib's canonical configure — zlib has no out-of-tree build mode, so it builds
in-tree under `src/`:

    ./configure --static --prefix=<dir>/build-host/prefix   # CFLAGS="-O2 -fPIC"
    make -j4
    make install

Toolchain used: gcc 15.2.0 (`cc`, Ubuntu 15.2.0-16ubuntu1), GNU make 4.4.1.
Installed into `build-host/prefix/`: `include/zlib.h`, `include/zconf.h`,
`lib/libz.a`, `share/pkgconfig/zlib.pc` (+ man pages).

CMake alternative (not used): zlib's `CMakeLists.txt` builds `zlib` +
`zlibstatic` targets; its `cmake_minimum_required(VERSION 2.4.4...3.15.0)`
requires `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` under CMake >= 4.0. The
`configure` route avoids that landmine entirely.

## Smoke test

`smoke/zlib_smoke.c` — compresses a deterministic 64 KiB buffer (structured +
LCG pseudo-random bytes) with `compress2(level 9)`, decompresses with
`uncompress`, requires a byte-exact roundtrip, and prints crc32 / adler32 /
FNV-1a of the payload plus the compressed size. Those fingerprints are the
repeatable values diffed by the lane's checksum-stability gate
(`third_party/l6-libs1-gate.sh`). Run via `build.sh`; the result line lands in
`build-host/SMOKE.RESULT`.

## WPE / cross-build reuse

For the aarch64-none-elf target, reuse the same script with a cross toolchain:

    CC="clang --target=aarch64-none-elf ..." CFLAGS_EXTRA="..." ./build.sh

(keep `--static` + `-fPIC`; `build.sh` passes `CC` straight to configure).
WPE consumes the result via `find_package(ZLIB)` with `CMAKE_PREFIX_PATH`
pointed at `build-host/prefix`.

## Note for the pin table (flagged to the integrator)

As of 2026-09-30 zlib **1.3.2** (2026-02-17) is out and carries 7ASecurity
audit fixes. The 1.3.1 pin was built as briefed; a pin bump is the parent's
call (see the lane report's risks).
