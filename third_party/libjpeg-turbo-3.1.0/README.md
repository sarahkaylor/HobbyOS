# libjpeg-turbo 3.1.0 — vendored host build (L6, browser/l6-libs1)

Carry-over pin of browser.md §2 (`libjpeg-turbo-3.1.0.tar.gz`). WPE 2.54.0
calls `find_package(JPEG REQUIRED)` with no version floor
(`docs/browser/f0-deps-audit.md` §1.1/§1.3). Host-first build only — **not**
wired into the HobbyOS Makefile; `build.sh` is the recipe the WPE/port cross
builds reuse.

| Field | Value |
|---|---|
| Tarball | `libjpeg-turbo-3.1.0.tar.gz` (2,507,094 bytes) |
| Upstream | https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz |
| sha256 | `9564c72b1dfd1d6fe6274c5f95a8d989b59854575d4bbee44ade7bc17aa9bc93` |
| Checksum file | `libjpeg-turbo-3.1.0.tar.gz.sha256` (committed) |
| GPG signature | `libjpeg-turbo-3.1.0.tar.gz.sig` (committed) — **verified GOOD** |
| License | BSD-3-Clause + IJG — `LICENSE.md` copied from the tarball |
| Vendored | 2026-09-30, downloaded from the URL above |

**Verification provenance:** the release publishes no sha256 file; its official
integrity mechanism is the GPG-signed tarball. The signature was verified
against the official key (`https://raw.githubusercontent.com/libjpeg-turbo/repo/main/LJT-GPG-KEY`,
fingerprint `0338 C8D8 D9FD A62C F9C4  21BD 7EC2 DBB6 F4DB F434`):

    gpg --verify libjpeg-turbo-3.1.0.tar.gz.sig libjpeg-turbo-3.1.0.tar.gz
    → Good signature from "The libjpeg-turbo Project (Signing key for official binaries)"

The computed sha256 above (matches the browser.md §2 pin, and independent
distro/build-system records such as Buildroot's `jpeg-turbo.hash`) is recorded
alongside for tooling that wants a hash.

## Layout

    libjpeg-turbo-3.1.0.tar.gz          pinned tarball (committed)
    libjpeg-turbo-3.1.0.tar.gz.sha256   checksum (committed)
    libjpeg-turbo-3.1.0.tar.gz.sig      upstream GPG signature (committed)
    README.md                           this file
    build.sh                            host build + smoke recipe (committed)
    smoke/jpeg_smoke.c                  gradient encode/decode smoke (committed)
    LICENSE.md                          upstream license text (committed)
    src/                                extracted upstream tree  (gitignored)
    build-host/                         host build + smoke output (gitignored)

## Build recipe (verified)

`./build.sh` (idempotent; `JOBS`, `CC` overridable):

    cmake -S src -B build-host/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=build-host/prefix \
      -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
      -DWITH_SIMD=ON -DWITH_TURBOJPEG=ON \
      -DWITH_12BIT=OFF -DWITH_FUZZ=OFF -DWITH_JAVA=OFF
    cmake --build build-host/build -j4
    cmake --install build-host/build

Toolchain used: CMake 3.31.8 (Kitware official binary, rootless install),
gcc 15.2.0, GNU make 4.4.1, NASM 3.01 (rootless-extracted Ubuntu `nasm` .deb —
required for `WITH_SIMD=ON`; the smoke prints `simd=1` to prove it was
compiled in). Installed into `build-host/prefix/`: `include/jpeglib.h`,
`jconfig.h`, `jmorecfg.h`, `jerror.h`, `turbojpeg.h`, `lib/libjpeg.a`,
`lib/libturbojpeg.a` (+ `lib/pkgconfig/libjpeg.pc`, `libturbojpeg.pc`).

Note on warnings: a clean build emits 7 GCC 15 warnings from **upstream**
sources (`turbojpeg-mp.c` const-qualifier, `jchuff.c`/`rdtarga.c`
stringop-overflow) — upstream code builds as-is; the lane's own smoke code
compiles `-Wall -Wextra` warning-free.

## Smoke test

`smoke/jpeg_smoke.c` — encodes a generated 128x128 grayscale gradient through
the libjpeg API (`jpeg_mem_dest`, quality 95), decodes it back
(`jpeg_mem_src`), and compares against the source: max/mean absolute error
must stay small (JPEG is lossy — the 16-count max-error threshold guards
against codec breakage on a smooth ramp, not codec quality). Prints the
version/ABI/SIMD flags, error stats, and an FNV-1a fingerprint of the decoded
pixels — the repeatable values diffed by the lane's checksum-stability gate
(`third_party/l6-libs1-gate.sh`). Run via `build.sh`; the result line lands in
`build-host/SMOKE.RESULT`.

## WPE / cross-build reuse

Same cmake line with a cross toolchain (`-DCMAKE_C_COMPILER=…` + toolchain
file; set `-DWITH_SIMD=OFF` if the target toolchain lacks NASM, or point
`-DCMAKE_ASM_NASM_COMPILER=` at a target NASM). WPE consumes the result via
`find_package(JPEG)` with `CMAKE_PREFIX_PATH` pointed at `build-host/prefix`.
