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

### Header overlay and transitional compat (cross/)

* `cross/include/` is searched **before** the repo sysroot, by the X11 wrappers
  only: `limits.h` (PATH_MAX, NAME_MAX, SSIZE_MAX, IOV_MAX, HOST_NAME_MAX…),
  `unistd.h` (`_POSIX_THREAD_SAFE_FUNCTIONS`, so Xos_r.h picks the POSIX
  5-argument `getpw*_r`), `sys/select.h` (POSIX `select()` bound by asm
  label to libc's `hb_select_posix` — the device API in `libc.h` owns the
  plain `select` symbol with a millisecond timeout). These are deliberately
  NOT in `src/libc/include`: several in-tree ports pick their own `PATH_MAX`
  when it is undefined, so adding it there would change default binaries.
* `cross/compat/` (transitional): while `src/libc/include/sys/socket.h` lacks
  `bind()`, the wrappers force-include `hb-socket-compat.h` (POSIX socket
  prototypes) and link `libhbsockcompat.a` (weak ENOSYS stubs). Both switch
  off automatically once lane `kern`'s wrappers are merged — which this
  branch now has (lane/kern cb0a2acd merged), so the compat path is inactive
  here and kept only for trees without kern. The compat state is part of the
  package cache key.
* Package cache keys include a hash of everything under `cross/`, so a flag
  or overlay change rebuilds the stage.
* intel links go through the host gcc driver (clang's x86_64-none-elf
  toolchain does that; `-fuse-ld=lld` keeps ld.lld doing the work), hence
  `-Wl,--build-id=none` in the link mode — a build-id note would otherwise sit
  at the image base in front of `_start`.

### Configuration choices (clientlibs)

* libxcb: unix transport only (no abstract sockets: `HAVE_ABSTRACT_SOCKETS`
  undefined), default socket path `/tmp/.X11-unix/X<n>`; extensions built:
  core + composite, damage, dbe, randr, render, res, shape, xf86dri, xfixes,
  xinerama (static archives; unused ones cost nothing at link).
* libX11: static, `--disable-loadable-i18n --disable-loadable-xcursor
  --disable-xf86bigfont --disable-tcp-transport --disable-ipv6
  --disable-xlocaledir`, XTHREADS on (pthread from libc.a; the thread-safety
  constructor runs from `.init_array`), XKB on, `malloc0returnsnull=no`.
* libpng: no NEON/SSE paths; freetype: zlib + png, no bzip2/brotli/harfbuzz.

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

## Pins (extlibs stage, lane xlibs-ext)

Latest upstream releases as of 2026-10-30, recorded with `fetch.sh --record`
and cross-checked on 2026-10-30: every x.org `.sig` gpg-verified ("Good
signature" — Alan Coopersmith `3AB28523…A6E7E2D2` for Xdamage/Xcomposite/
Xrandr/Xinerama/Xres/Xpm, Thomas E. Dickey `19882D92…2167BE03` for Xcursor/
Xft); expat's `.asc` gpg-verified (Sebastian Pipping `CB8DE70A…FBD3AEC6`);
fontconfig's sha256 equals upstream's published `.sha256sum`; libjpeg-turbo
is the vendored tarball (same sha256 as its `.sha256` file).

| Package | Version | Source | sha256 |
|---|---|---|---|
| libXdamage | 1.1.7 | https://www.x.org/archive/individual/lib/libXdamage-1.1.7.tar.xz | `127067f521d3ee467b97bcb145aeba1078e2454d448e8748eb984d5b397bde24` |
| libXcomposite | 0.4.7 | https://www.x.org/archive/individual/lib/libXcomposite-0.4.7.tar.xz | `8bdf310967f484503fa51714cf97bff0723d9b673e0eecbf92b3f97c060c8ccb` |
| libXcursor | 1.2.3 | https://www.x.org/archive/individual/lib/libXcursor-1.2.3.tar.xz | `fde9402dd4cfe79da71e2d96bb980afc5e6ff4f8a7d74c159e1966afb2b2c2c0` |
| libXrandr | 1.5.5 | https://www.x.org/archive/individual/lib/libXrandr-1.5.5.tar.xz | `72b922c2e765434e9e9f0960148070bd4504b288263e2868a4ccce1b7cf2767a` |
| libXinerama | 1.1.6 | https://www.x.org/archive/individual/lib/libXinerama-1.1.6.tar.xz | `d00fc1599c303dc5cbc122b8068bdc7405d6fcb19060f4597fc51bd3a8be51d7` |
| libXres | 1.2.3 | https://www.x.org/archive/individual/lib/libXres-1.2.3.tar.xz | `d2de8f5401d6c86a8992791654547eb8def585dfdc0c08cc16e24ef6aeeb69dc` |
| libXpm | 3.5.20 | https://www.x.org/archive/individual/lib/libXpm-3.5.20.tar.xz | `c064ead03e4dde78afdeb121fe0b644d25c550444cf903fbdc537f1a78f3ef74` |
| libjpeg-turbo | 3.1.0 | vendored `third_party/libjpeg-turbo-3.1.0/libjpeg-turbo-3.1.0.tar.gz` | `9564c72b1dfd1d6fe6274c5f95a8d989b59854575d4bbee44ade7bc17aa9bc93` |
| expat | 2.9.0 | https://github.com/libexpat/libexpat/releases/download/R_2_9_0/expat-2.9.0.tar.xz | `1e6371862cc31999b368c3b89b49994f0677e1bab5f1b2b85ae3741f5d803051` |
| fontconfig | 2.18.3 | https://gitlab.freedesktop.org/api/v4/projects/890/packages/generic/fontconfig/2.18.3/fontconfig-2.18.3.tar.xz | `4f7b554a38cdf78c033f666c8871f3749e14a094f65a07f630c91ed0b43d35e3` |
| libXft | 2.3.9 | https://www.x.org/archive/individual/lib/libXft-2.3.9.tar.xz | `60a25b78945ed6932635b3bb1899a517d31df7456e69867ffba27f89ff3976f5` |

### Configuration choices (extlibs)

* libjpeg-turbo: CMake, `WITH_SIMD=OFF WITH_TURBOJPEG=OFF`, static.
* expat: hash salt from libc `getentropy()`, `--without-dev-urandom`;
  `patches/expat/0001` guards `O_CLOEXEC` (missing from the repo libc) in
  the always-compiled but unused urandom TU.
* libXpm: `--disable-open-zfile --disable-stat-zfile` (no popen'd gzip).
* libXcursor: icon/cursor path `/X11/ICONS` (no themes shipped yet).
* fontconfig 2.18.3 (autotools; library + headers + `.pc` only, no fc-*
  tools): compiled-in config `/X11/FONTS.CNF` (`FONTCONFIG_PATH=/X11` +
  `-DFONTCONFIG_FILE="FONTS.CNF"`), so it works with or without the
  `FONTCONFIG_FILE` env from XSESSION; default font dir `/X11/FONTS`, cache
  `/TMP/FCCACHE` (an unwritable cache only costs a rescan).
* Data tree (`_ex_fontdata`): `build/<arch>/disk/X11/FONTS.CNF` and
  `build/<arch>/disk/X11/FONTS/*.TTF` — DejaVu 2.37 Sans/Sans Condensed/
  Sans Mono/Serif (14 files, 6.5 MB) renamed to 8.3 (`DJVSANS.TTF`,
  `DJVMONO.TTF`, …) + `LICENSE.TXT`. Aliases: sans-serif/sans → DejaVu Sans,
  serif → DejaVu Serif, monospace/mono/fixed → DejaVu Sans Mono, anything
  else falls back to DejaVu Sans.
* Smoke: `build/<arch>/smoke/XFTSMOKE.BIN` (`smoke/xftsmoke.c`).
* libc gaps filled for this stage (new files, prerequisite-only Makefile
  rules): `rmdir()` (`src/libc/src/fsext.c`), `frexp/ldexp/scalbn` families
  (`src/libc/src/math_ext.c`, host test `x11ext_libc_test`), compiler-rt
  binary128 soft-float (`__divtf3`, `__floatunsitf`, … — aarch64 long
  double), `getentropy`/`rmdir` in `<unistd.h>`, `utimes` + `timercmp`
  family in `<sys/time.h>`.

Other lanes append their own `pins/<stage>.txt` + `sums/` + a table here.

## Pins (icewm stage, lane icewm)

| Tarball | Source | sha256 |
|---|---|---|
| icewm-4.1.0.tar.gz | https://github.com/bbidulock/icewm/archive/refs/tags/4.1.0.tar.gz (tag 4.1.0 = commit 3a19fee0b5c9) | 224a8636af6b7749b2b9e324709323068db96ccb94e1682a170d4f66bd3baeb7 |

IceWM 4.1.0 (2026-08-06) is the newest release as of 2026-10-30.  The
project's release tarballs are `.tar.lz` files on github.com/ice-wm/icewm;
the host has no lzip, so the pin is GitHub's archive of the same tag in the
bbidulock/icewm repo (the CMake build needs no generated files).

### Build (`build-icewm.sh`)

`bash third_party/x11/build.sh --arch arm|intel --stage icewm --prefix <P> -j 8`
(needs clientlibs + extlibs in the prefix) produces, in `build/<arch>/`:

* `bin/ICEWM.BIN`, `bin/ICEWMBG.BIN`, `bin/ICESH.BIN`, `bin/ICEWMHNT.BIN`
  (IceWM's CMake targets `icewm icewmbg icesh icewmhint`, ELF kept in
  `obj/icewm/`, `llvm-objcopy -O binary` like every repo program).
* `bin/XEYESR.BIN`, `bin/XCALCR.BIN` -- the "clients" sub-step: the shim demo
  sources `src/user/x11/apps/{xeyes,xcalc}/main.c`, unchanged, compiled and
  linked against the real libX11 (xcalc gets `-include X11/Xutil.h`).
* `disk/ICEWM/` -- LIBDIR = CFGDIR = DATADIR = `/ICEWM`: HobbyOS
  `preferences menu toolbar programs keys theme` (from `icewm/data/`) +
  upstream `icons ledclock mailbox taskbar` + themes `icedesert` (default)
  and `win95`.  Upstream long mixed-case names are kept (FAT16 LFN is read
  and written by the kernel, case-insensitively).

`--only icewm|icewm-data|clients|icewm-hosttest` runs one sub-step.

Configuration: XPM + libpng + libjpeg loaders, Xft (+ core fonts), Xrandr,
Xinerama, shape; off: imlib2, gdk-pixbuf, SVG, NLS/I18N, session (SM/ICE),
sound, fribidi, XRes, fdo menus (`icewm-menu-fdo` needs C++ exceptions).
C++ is `-fno-exceptions -frtti` against the repo libc++ (+ libcxxabi's
`cxxabi.h` for `__cxa_demangle`).  The toolchain's static-library
`try_compile` makes CMake's link probes always succeed, so every
`HAVE_*` probe result is passed explicitly.

HobbyOS adaptations:

* `patches/icewm/0001` -- icesh's `throw <exit code>` / `catch` becomes
  `IceSh::bail()` (same cleanup as `~IceSh`, then `exit`).
* `patches/icewm/0002` -- the `sh -c` fallback is `ICE_BINSH` (= `/SH.BIN`).
* `patches/icewm/0003` -- `isExeFile()` accepts any regular file: FAT has no
  execute bits (files stat 0644), so menu `prog` lookups would all fail.
* `patches/icewm/0004` -- pixmap resource scan matches file names
  case-insensitively: the FAT driver returns 8.3 names that mtools stored
  lower-case (`taskbar/start.xpm`, `mailbox/mail.xpm`, `ledclock/*.xpm`) in
  upper case, so the start button image was never found.
* `icewm/include/hb_icewm_compat.h` (force-included): declarations missing
  from the repo headers, the `.BIN` program names (`ICEWMEXE`=ICEWM.BIN,
  `ICEWMBGEXE`=ICEWMBG.BIN, ...), C linkage for `main` (`-ffreestanding`
  C++), and `O_CLOEXEC/O_NOCTTY/O_DIRECTORY/O_NOFOLLOW` hints.
* `icewm/include/` + `icewm/compat/` -> `libhbicecompat.a`: fnmatch, glob,
  wordexp (no command substitution -> IceWM falls back to `/SH.BIN -c`),
  execl/execlp, `dirfd`/`rewinddir`/`openat`/`fstatat`/`faccessat` over a
  pseudo-fd -> path table (the repo DIR is path based), popen/pclose/
  system/mkdtemp via `/SH.BIN -c`, nftw, `<net/if.h>` with an empty
  `if_nameindex()`, getloadavg (ENOSYS), tzset, nice/getpriority, memmem,
  fpathconf, daemon, getprogname.  All lane-local: the default build never
  sees them.  `icewm/test/compat_host_test.c` checks fnmatch/wordexp/glob
  against glibc on every build (`icewm-hosttest`).
* Program launching: menu/toolbar/keys entries name flat images
  (`XEYESR.BIN`); IceWM `fork()`s and `execvp()`s them with `PATH=/`.
* `/proc`/sysctl applets (CPU, memory, network, battery, mailbox) are compiled
  but switched off in `/ICEWM/preferences`; Run/Help/address bar hidden.

## Adding a stage / package

1. `pins/<stage>.txt`: add `<tarball> <url>`; run `fetch.sh --stage <stage> --record`,
   commit the new `sums/*.sha256`, add a row above.
2. `build-<stage>.sh`: define `stage_<stage>()` calling `hb_autotools` /
   `hb_meson` / `hb_cmake` / `hb_link_flat`.
3. Upstream changes go in `patches/<pkg>/NNNN-description.patch` (`-p1`
   relative to the extracted top dir).
