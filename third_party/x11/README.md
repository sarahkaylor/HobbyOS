# third_party/x11 — the X11 / IceWM stack for `DESKTOP=icewm`

Cross-build tree for the secondary IceWM desktop (plan: `icewm.md`; frozen
layout §4.2). Everything here builds **static** archives and **flat** HobbyOS
programs for both arches (`arm` = aarch64-none-elf, `intel` = x86_64-none-elf)
with the repo's own sysroot (`src/libc`, `obj/<arch>/{crt0.o,libc.a,libcxx.a}`).
Plain `make run` (`DESKTOP=hobby`) never touches this directory.

## Quick start

```sh
bash third_party/x11/fetch.sh                      # download + sha256-verify into dl/
bash third_party/x11/build.sh --arch intel         # stage clientlibs (default)
bash third_party/x11/build.sh --arch arm --stage all -j8 --prefix ~/hobbyos-icewm-lanes/prefix/arm
```

`build.sh --arch arm|intel [--prefix DIR] [--stage clientlibs|extlibs|server|icewm|clients|all] [-j N] [--only PKG,..] [--force]`

| Path | What |
|---|---|
| `pins/<stage>.txt` | `<tarball> <url \| vendored:repo/path>` per stage (one file per owning lane) |
| `sums/<tarball>.sha256` | committed sha256 (`sha256sum` format); `fetch.sh` refuses mismatches |
| `dl/` | downloaded tarballs (gitignored) |
| `patches/<pkg>/*.patch` | every change to upstream (sources stay byte-identical), applied `-p1` in name order |
| `cross/env.sh` | the autotools environment helper (sourced): CC/CXX wrappers, pkg-config, flags |
| `cross/meson-{arm,intel}.ini` | meson cross files (pair with the generated `tools/meson-paths.ini`) |
| `cross/toolchain-hobbyos-c.cmake` | CMake toolchain for C/C++ deps |
| `cross/link-flat.sh` | link a static flat program: crt0 + objs + libc.a [+ libcxx.a], `-T src/user/linker.ld`, `objcopy -O binary` |
| `build.sh` | driver; sources `build-<stage>.sh` (each defines `stage_<stage>`) |
| `build-clientlibs.sh` | stage `clientlibs` (lane xlibs-core) |
| `smoke/` | `xsmoke.c`, the M2 link-model smoke client |
| `build/<arch>/` (gitignored) | `prefix/` (default), `bin/` flat .BINs (§4.3), `disk/` data tree (§4.4), `src/`, `obj/`, `logs/`, `tools/` |

`X11_BUILD_DIR=<dir>` relocates `build/<arch>/`; `X11_DL_DIR` relocates `dl/`.

## Toolchain model

* `cross/env.sh <arch> <prefix> [tools]` generates per-repo wrappers in
  `build/<arch>/tools/` (never in the shared prefix, so lanes sharing one
  prefix cannot clobber each other):
  * `hobbyos-cc` / `hobbyos-c++` — `clang --target=<triple> -ffreestanding`
    + the Makefile's per-arch USER_CFLAGS (`-mcpu=cortex-a53` /
    `-mno-red-zone -mcmodel=large`) + a hermetic `-nostdinc` search path:
    `src/libc/include`, `src/include`, `src/user_include`, clang's resource
    dir (all `-isystem`, so package `-I` dirs win). C++ adds libc++ headers,
    `-fno-exceptions -frtti`. **Link mode** (no `-c/-S/-E`) appends
    `-nostdlib -static -fuse-ld=lld -T src/user/linker.ld -e _start crt0.o …
    --start-group [libcxx.a] libc.a --end-group` (+ `-no-pie` on intel), so
    configure/meson/cmake link probes produce real HobbyOS images and
    `AC_CHECK_FUNCS` answers truthfully against the repo libc.
  * `hobbyos-pkg-config` — `pkg-config --static` confined to the prefix.
  * `meson-paths.ini` — `[constants]` consumed by `cross/meson-<arch>.ini`.
* Stage files get helpers from `build.sh`: `hb_autotools`, `hb_meson`,
  `hb_cmake` (fresh extract + patches + out-of-tree build + install, cached by
  a stamp in `<prefix>/.stamps/<pkg>` keyed on tarball sha + patch set +
  options, `flock`-serialised per package so lanes can share a prefix),
  `hb_link_flat [--cxx] OUT.BIN inputs…`, `hb_need_libcxx`, `hb_log/hb_die`.

## Pins (clientlibs stage)

Versions are the latest upstream releases as of 2026-10-30. sha256 values were
recorded at first download (`fetch.sh --record`); in addition every x.org
tarball's detached `.sig` was verified with gpg on 2026-10-30 ("Good
signature" — Alan Coopersmith `3AB28523…A6E7E2D2` / `2F7DBCAD…28C642A7`,
Matt Turner `16238A7D…5103DB07` for xcb). zlib/libpng/freetype reuse the
tarballs already vendored in `third_party/` (same sha256 as their READMEs).

| Package | Version | Source | sha256 |
|---|---|---|---|
| zlib | 1.3.1 | vendored `third_party/zlib-1.3.1/zlib-1.3.1.tar.gz` | `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` |
| libpng | 1.6.44 | vendored `third_party/libpng-1.6.44/libpng-1.6.44.tar.xz` | `60c4da1d5b7f0aa8d158da48e8f8afa9773c1c8baa5d21974df61f1886b8ce8e` |
| freetype | 2.14.3 | vendored `third_party/freetype-2.14.3.tar.xz` | `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f` |
| xorgproto | 2026.1 | https://www.x.org/archive/individual/proto/xorgproto-2026.1.tar.xz | `f9bfe4a9ed8c8ab9d2a3b0d49797f046052dadd06b7a8b45dbffaffb137e8290` |
| xtrans | 1.6.0 | https://www.x.org/archive/individual/lib/xtrans-1.6.0.tar.xz | `faafea166bf2451a173d9d593352940ec6404145c5d1da5c213423ce4d359e92` |
| libXau | 1.0.12 | https://www.x.org/archive/individual/lib/libXau-1.0.12.tar.xz | `74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb` |
| xcb-proto | 1.17.0 | https://www.x.org/archive/individual/proto/xcb-proto-1.17.0.tar.xz | `2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c` |
| libpthread-stubs | 0.5 | https://www.x.org/archive/individual/xcb/libpthread-stubs-0.5.tar.xz | `59da566decceba7c2a7970a4a03b48d9905f1262ff94410a649224e33d2442bc` |
| libxcb | 1.17.0 | https://www.x.org/archive/individual/lib/libxcb-1.17.0.tar.xz | `599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55` |
| libX11 | 1.8.14 | https://www.x.org/archive/individual/lib/libX11-1.8.14.tar.xz | `bbc78c46217174655c2b32b138ad119f3ac15f50808b4d0b3a2677a4b1afc950` |
| libXext | 1.3.7 | https://www.x.org/archive/individual/lib/libXext-1.3.7.tar.xz | `6c643c7035cdacf67afd68f25d01b90ef889d546c9fcd7c0adf7c2cf91e3a32d` |
| libXrender | 0.9.13 | https://www.x.org/archive/individual/lib/libXrender-0.9.13.tar.xz | `3fe88c609df6782e1e7af2e4de36b2563d70cd70ff7dcb1cda561ad64912bb64` |
| libXfixes | 6.0.2 | https://www.x.org/archive/individual/lib/libXfixes-6.0.2.tar.xz | `39f115d72d9c5f8111e4684164d3d68cc1fd21f9b27ff2401b08fddfc0f409ba` |

Other lanes append their own `pins/<stage>.txt` + `sums/` + a table here.

## Adding a stage / package

1. `pins/<stage>.txt`: add `<tarball> <url>`; run `fetch.sh --stage <stage> --record`,
   commit the new `sums/*.sha256`, add a row above.
2. `build-<stage>.sh`: define `stage_<stage>()` calling `hb_autotools` /
   `hb_meson` / `hb_cmake` / `hb_link_flat`.
3. Upstream changes go in `patches/<pkg>/NNNN-description.patch` (`-p1`
   relative to the extracted top dir).
