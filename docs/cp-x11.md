# Cyberpunk track — real X11 client libraries for compat processes (lane C4-x, P4 groundwork)

Plan: `docs/cyberpunk.md` §2 (X client row), AD-2, §6 P4, §7.2b. Precedents:
`docs/rimworld-plan.md` AD-4/AD-11, `src/user/xcompat/README.md` (the RimWorld
libX11 shim), `third_party/x11/README.md` (the native X stack), `tools/cp/build-wine.sh`
(the glibc 2.31 sysroot). Everything below was measured on `linuxci` on 2026-11-15.

| What | Where |
|---|---|
| fetch + verify | `bash tools/cp/x11/fetch.sh` → `third_party/cp-runtime/x11/dl/` (git-ignored) |
| signer keyring | `tools/cp/x11/x11-signers.asc` (§1.2) |
| build, stage, audit | `bash tools/cp/x11/build.sh [build\|stage\|audit\|smoke\|all]` → `$CP_X11_WORK` (default `~/cp/x11-build`) |
| in-OS smoke client | `tools/cp/x11/xsmoke.c` |
| in-OS smoke harness | `flock /tmp/cp-qemu.lock python3 tools/cp/x11/smoke.py` |
| evidence | `evidence/cp-x/` |

## 1. Pins

All from `https://xorg.freedesktop.org/releases/individual/<dir>/` and nowhere else.
Each was the newest release in its directory listing on 2026-11-15. The `.tar.xz`
**and** its detached `.sig` are both sha256-pinned in `fetch.sh`. That table is the
executable copy of this one, so change both or neither.

| Package | Ver | Dir | sha256 (tarball) | Signer (VALIDSIG fpr) | Check |
|---|---|---|---|---|---|
| xorgproto | 2026.1 | proto | `f9bfe4a9ed8c8ab9d2a3b0d49797f046052dadd06b7a8b45dbffaffb137e8290` | Alan Coopersmith `3AB28523…A6E7E2D2` | sha256 + gpg |
| xcb-proto | 1.17.0 | xcb | `2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c` | Matt Turner `16238A7D…5103DB07` (ed25519) | sha256 only (§1.2) |
| libpthread-stubs | 0.5 | xcb | `59da566decceba7c2a7970a4a03b48d9905f1262ff94410a649224e33d2442bc` | Alan Coopersmith `2F7DBCAD…28C642A7` | sha256 + gpg |
| libXau | 1.0.12 | lib | `74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb` | `2F7DBCAD…28C642A7` | sha256 + gpg |
| libXdmcp | 1.1.5 | lib | `d8a5222828c3adab70adf69a5583f1d32eb5ece04304f7f8392b6a353aa2228c` | `2F7DBCAD…28C642A7` | sha256 + gpg |
| libxcb | 1.17.0 | xcb | `599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55` | `16238A7D…5103DB07` | sha256 only (§1.2) |
| xtrans | 1.6.0 | lib | `faafea166bf2451a173d9d593352940ec6404145c5d1da5c213423ce4d359e92` | `2F7DBCAD…28C642A7` | sha256 + gpg |
| libX11 | 1.8.14 | lib | `bbc78c46217174655c2b32b138ad119f3ac15f50808b4d0b3a2677a4b1afc950` | `3AB28523…A6E7E2D2` | sha256 + gpg |
| libXext | 1.3.7 | lib | `6c643c7035cdacf67afd68f25d01b90ef889d546c9fcd7c0adf7c2cf91e3a32d` | `3AB28523…A6E7E2D2` | sha256 + gpg |
| libXrender | 0.9.13 | lib | `3fe88c609df6782e1e7af2e4de36b2563d70cd70ff7dcb1cda561ad64912bb64` | `3AB28523…A6E7E2D2` | sha256 + gpg |
| libXfixes | 6.0.2 | lib | `39f115d72d9c5f8111e4684164d3d68cc1fd21f9b27ff2401b08fddfc0f409ba` | `3AB28523…A6E7E2D2` | sha256 + gpg |
| libXi | 1.8.4 | lib | `ddd6057b89a91903bc2f884d55c8467440906945e482c36547260dff28661afc` | `3AB28523…A6E7E2D2` | sha256 + gpg |
| libXrandr | 1.5.5 | lib | `72b922c2e765434e9e9f0960148070bd4504b288263e2868a4ccce1b7cf2767a` | `3AB28523…A6E7E2D2` | sha256 + gpg |
| libXcursor | 1.2.3 | lib | `fde9402dd4cfe79da71e2d96bb980afc5e6ff4f8a7d74c159e1966afb2b2c2c0` | Thomas E. Dickey `19882D92…2167BE03` | sha256 + gpg |
| libXinerama | 1.1.6 | lib | `d00fc1599c303dc5cbc122b8068bdc7405d6fcb19060f4597fc51bd3a8be51d7` | `3AB28523…A6E7E2D2` | sha256 + gpg |
| libxshmfence | 1.3.3 | lib | `d4a4df096aba96fea02c029ee3a44e11a47eb7f7213c1a729be83e85ec3fde10` | `2F7DBCAD…28C642A7` | sha256 + gpg |

Hard requirements are covered by this set:
- libxcb needs xcb-proto, pthread-stubs and Xau. libXdmcp is optional, but it is
  included and linked.
- libX11 needs xtrans, xorgproto and libxcb.
- Xcursor needs Render and Fixes.
- Xi needs Xext and Fixes.

Every row is MIT/X11-family licensed (from each tarball's `COPYING`).

### 1.1 Cross-checks

- **Overlap with the native stack.** 13 of the 16 tarballs are byte-identical, by
  sha256, to the ones the native X stack already pins in `third_party/x11/sums/`.
  That lane gpg-verified them on 2026-10-30, as recorded in
  `third_party/x11/README.md`, and its record includes Matt Turner's signature on
  xcb-proto and libxcb.
- **New in this lane:** libXi 1.8.4, libXdmcp 1.1.5 and libxshmfence 1.3.3.
- **Independent check for libxcb.** The `.tar.gz` twin from xorg.freedesktop.org
  (`2c692874…cf8f0b85`) equals Ubuntu's `libxcb_1.17.0.orig.tar.gz` and the
  Checksums-Sha256 line of `libxcb_1.17.0-2ubuntu1.dsc`. We pin the `.tar.xz`;
  this only corroborates the release.

### 1.2 Signer keyring

`tools/cp/x11/x11-signers.asc` holds three public keys, exported from the
`debian/upstream/signing-key.asc` files of Ubuntu source packages fetched from
`archive.ubuntu.com` (apt's archive):

| Source file | sha256 |
|---|---|
| `libx11_1.8.14-1.diff.gz` | `d8a820cabaceedcdf0d74318133eab8d3f4bc69d1f7b08f8a8af1eab94cb77c2` |
| `libxi_1.8.4-1.debian.tar.xz` | `9720c170c270c313be1c4422cc2ac8a1eb5872dbf943603a7f012a64f2bf1bff` |
| `libxdmcp_1.1.5-2.diff.gz` | `c64245c976c5e54214c43936aa73a7186c417f549fb0d10ee396fe34d6115196` |
| `libxshmfence_1.3.3-1.diff.gz` | `e84dfe14637642e84a5a6523f771d7b55b271cac5cb0532aaf36acf776c8e4a5` |
| `ncurses_6.6+20260608-2.debian.tar.xz` (Dickey's key) | `52a49c453121bd90d21edef53903beac9d9d8229e0ef4f49d3b0252e49932f4c` |

The three keys:

| Key | Fingerprint | Notes |
|---|---|---|
| Alan Coopersmith | `3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2` | |
| Alan Coopersmith (older) | primary `4A193C06…1F2D130E`, signing subkey `2F7DBCAD5B30E668BBD516C1CFDF148828C642A7` | Expiry in this copy is 2018-04-24. gpg reports `EXPKEYSIG` + `VALIDSIG` for signatures made in 2024–2025, which means upstream extended the key and Ubuntu's copy lacks the newer self-signature. `fetch.sh` accepts GOODSIG/EXPKEYSIG only together with the pinned VALIDSIG fingerprint. |
| Thomas E. Dickey | `19882D92DDA4C400C22C0D56CC2AF4472167BE03` | |

Matt Turner's ed25519 key `16238A7D…` is not in any Ubuntu `signing-key.asc` we
found (libxcb's and xcb-proto's Ubuntu packages carry none). Those two rows
therefore rest on sha256 plus the §1.1 cross-checks. `CP_REQUIRE_GPG=1` turns them
into an error.

### 1.3 Gate: `fetch.sh` (raw, linuxci)

```
cp-x11-fetch: ok  xorgproto-2026.1.tar.xz  [sha256+gpg(3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2)]
cp-x11-fetch: ok  xcb-proto-1.17.0.tar.xz  [sha256-only (signer 16238A7D92448B02B7B10344B6B1CEAE5103DB07 not in keyring)]
cp-x11-fetch: ok  libpthread-stubs-0.5.tar.xz  [sha256+gpg(2F7DBCAD5B30E668BBD516C1CFDF148828C642A7)]
cp-x11-fetch: ok  libXau-1.0.12.tar.xz  [sha256+gpg(2F7DBCAD5B30E668BBD516C1CFDF148828C642A7)]
cp-x11-fetch: ok  libXdmcp-1.1.5.tar.xz  [sha256+gpg(2F7DBCAD5B30E668BBD516C1CFDF148828C642A7)]
cp-x11-fetch: ok  libxcb-1.17.0.tar.xz  [sha256-only (signer 16238A7D92448B02B7B10344B6B1CEAE5103DB07 not in keyring)]
cp-x11-fetch: ok  xtrans-1.6.0.tar.xz  [sha256+gpg(2F7DBCAD5B30E668BBD516C1CFDF148828C642A7)]
cp-x11-fetch: ok  libX11-1.8.14.tar.xz  [sha256+gpg(3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2)]
cp-x11-fetch: ok  libXext-1.3.7.tar.xz  [sha256+gpg(3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2)]
cp-x11-fetch: ok  libXrender-0.9.13.tar.xz  [sha256+gpg(3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2)]
cp-x11-fetch: ok  libXfixes-6.0.2.tar.xz  [sha256+gpg(3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2)]
cp-x11-fetch: ok  libXi-1.8.4.tar.xz  [sha256+gpg(3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2)]
cp-x11-fetch: ok  libXrandr-1.5.5.tar.xz  [sha256+gpg(3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2)]
cp-x11-fetch: ok  libXcursor-1.2.3.tar.xz  [sha256+gpg(19882D92DDA4C400C22C0D56CC2AF4472167BE03)]
cp-x11-fetch: ok  libXinerama-1.1.6.tar.xz  [sha256+gpg(3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2)]
cp-x11-fetch: ok  libxshmfence-1.3.3.tar.xz  [sha256+gpg(2F7DBCAD5B30E668BBD516C1CFDF148828C642A7)]
cp-x11-fetch: all 16 tarballs sha256-verified into /home/sarah/cp/lanes/x/third_party/cp-runtime/x11/dl (14 also gpg-verified, 2 sha256-only)
exit=0
```

(The 64-hex sums are elided from this copy of the lines. The script prints them.)

A second run is idempotent: nothing is re-fetched and the same "all 16" line is
printed. Negative test: flipping one byte of a cached `libXi-1.8.4.tar.xz` gives
`MISMATCH (existing) libXi-1.8.4.tar.xz: have a72a6a47… want ddd6057b… — not
replacing; inspect it` / `VERIFICATION FAILED` / `exit=1`.

## 2. Build, stage, audit

### 2.1 Toolchain and layout

The toolchain is `clang --sysroot=~/cp/wine-build/sysroot-glibc231`. That is lane
C2-wine's sysroot, used read-only: libc6/libc6-dev 2.31-0ubuntu9.18,
linux-libc-dev 5.4.0-218.238, and focal libgcc-s1/libgcc-10-dev 10.5. See
`tools/cp/build-wine.sh` for why it is clang and why it is gcc-10's libgcc. With
no such sysroot present, `build.sh` assembles an identical one from
`tools/cp/fetch-runtime.sh`'s debs.

Each package is built with autotools:
- `--prefix=/CP/X11 --host=x86_64-linux-gnu --disable-static`
- `CFLAGS=-O2 -g -fno-plt`
- `PKG_CONFIG_LIBDIR` pointing only at the tree's own rewritten `.pc` files, so
  host X11 `.pc` files are never visible.
- Libtool `.la` files deleted.

The only host tool added is **patchelf** (apt).

Trees produced:

| Tree | Path on linuxci | Use |
|---|---|---|
| dev (headers, `.so` links, `.pc`) | `~/cp/x11-build/destdir/CP/X11/{include,lib}` | build Wine / Mesa against it |
| pkg-config | `~/cp/x11-build/pc/` (`/CP/X11` rewritten to the destdir) | `PKG_CONFIG_LIBDIR=` for those builds |
| runtime stage | `~/cp/x11-build/stage/CP/X11/` — `lib/<SONAME>` ×37 (3.2 MiB) + `share/X11/{locale,XErrorDB,Xcms.txt}`; 5.1 MiB, 228 files; manifest `~/cp/x11-build/stage-manifest.sha256` | copied to the disk at `/CP/X11` |

Libraries are staged under their SONAME as real files, because FAT has no
symlinks. Debug info is stripped, and `patchelf --force-rpath --set-rpath
'$ORIGIN'` is applied. libxcb's configure enabled these extensions: `composite
damage dbe dpms dri2 dri3 glx present randr record render res screensaver shape
shm sync xf86dri xfixes xinerama xinput xkb xtest xv xvmc`.

Built-in behaviour to know about:
- **libX11:** XCB transport, XTHREADS (so `libpthread.so.0` is in DT_NEEDED), XKB,
  locale data compiled in at `/CP/X11/share/X11/locale`.
- **libXdmcp:** no `arc4random_buf` in 2.31, so it uses `getentropy`.
- **libxshmfence:** the futex backend plus `memfd_create`. Both must work in
  HobbyOS for DRI3-style fences; see §4.

### 2.2 Gate: build + audit (raw lines, `build.sh all`)

The first five lines come from the first, uncached run. Everything from `stage:`
on comes from the final `build.sh all` re-run on the same tree (packages cached,
stage/audit/smoke redone; log `~/cp/x11-build/build-final.log`). Both runs ended
`EXIT=0`.

```
cp-x11-build: libXinerama: ok (9 s)
cp-x11-build: libxshmfence: configure + make -j6 (log /home/sarah/cp/x11-build/logs/libxshmfence.log)
cp-x11-build: libxshmfence: ok (9 s)
cp-x11-build: libxcb extensions built: composite damage dbe dpms dri2 dri3 glx present randr record render res screensaver shape shm sync xf86dri xfixes xinerama xinput xkb xtest xv xvmc
cp-x11-build: stage: 37 shared objects -> /home/sarah/cp/x11-build/stage/CP/X11/lib (3.2M); tree 5.1M, 228 files
libX11-xcb.so.1            GLIBC_max=2.2.5 RPATH=$ORIGIN  needed=libdl.so.2 libc.so.6
libX11.so.6                GLIBC_max=2.26  RPATH=$ORIGIN  needed=libpthread.so.0 libxcb.so.1 libdl.so.2 libc.so.6
libXau.so.6                GLIBC_max=2.25  RPATH=$ORIGIN  needed=libc.so.6
libXcursor.so.1            GLIBC_max=2.14  RPATH=$ORIGIN  needed=libXrender.so.1 libXfixes.so.3 libX11.so.6 libc.so.6
libXdmcp.so.6              GLIBC_max=2.25  RPATH=$ORIGIN  needed=libc.so.6
libXext.so.6               GLIBC_max=2.26  RPATH=$ORIGIN  needed=libX11.so.6 libc.so.6
libXfixes.so.3             GLIBC_max=2.14  RPATH=$ORIGIN  needed=libX11.so.6 libc.so.6
libXi.so.6                 GLIBC_max=2.14  RPATH=$ORIGIN  needed=libXext.so.6 libX11.so.6 libc.so.6
libXinerama.so.1           GLIBC_max=2.2.5 RPATH=$ORIGIN  needed=libXext.so.6 libX11.so.6 libc.so.6
libXrandr.so.2             GLIBC_max=2.14  RPATH=$ORIGIN  needed=libXext.so.6 libXrender.so.1 libX11.so.6 libc.so.6
libXrender.so.1            GLIBC_max=2.14  RPATH=$ORIGIN  needed=libX11.so.6 libc.so.6
libxcb-composite.so.0      GLIBC_max=2.2.5 RPATH=$ORIGIN  needed=libxcb.so.1 libXau.so.6 libXdmcp.so.6 libc.so.6
  … (24 libxcb-*.so rows, all GLIBC_max 2.2.5 or 2.14, needed=libxcb.so.1 libXau.so.6 libXdmcp.so.6 libc.so.6)
libxcb.so.1                GLIBC_max=2.14  RPATH=$ORIGIN  needed=libXau.so.6 libXdmcp.so.6 libc.so.6
libxshmfence.so.1          GLIBC_max=2.27  RPATH=$ORIGIN  needed=libc.so.6
cp-x11-build: audit: 37 shared objects; max GLIBC_ required = GLIBC_2.27 (allowed <= 2.31)
cp-x11-build: audit: DT_NEEDED union: libX11.so.6 libXau.so.6 libXdmcp.so.6 libXext.so.6 libXfixes.so.3 libXrender.so.1 libc.so.6 libdl.so.2 libpthread.so.0 libxcb.so.1
cp-x11-build: audit: every NEEDED resolves inside {stage} + {glibc 2.31}; every strong undefined symbol is defined in that closure: yes
cp-x11-build: smoke: /home/sarah/cp/x11-build/xsmoke/xsmoke.elf (77936 B) GLIBC_max=2.17
cp-x11-build: smoke: DT_RPATH=/CP/X11/lib  interp=/lib64/ld-linux-x86-64.so.2
cp-x11-build: smoke: DT_NEEDED: libX11.so.6 libX11-xcb.so.1 libxcb.so.1 libxcb-present.so.0 libxcb-dri3.so.0 libxcb-composite.so.0 libxcb-damage.so.0 libxcb-shm.so.0 libxcb-sync.so.1 libXext.so.6 libXi.so.6 libXrandr.so.2 libXrender.so.1 libXfixes.so.3 libXcursor.so.1 libXinerama.so.1 libxshmfence.so.1 libdl.so.2 libc.so.6
EXIT=0
```

The whole build of 16 packages takes about 3 minutes at `-j6`. The glibc 2.31 pin
holds with room to spare: the highest version required is GLIBC_2.27, from
libxshmfence (`memfd_create`).

The undefined-symbol check runs `nm -D --undefined-only` on every staged library
and compares it against the defined dynamic symbols of the stage plus glibc 2.31
plus libgcc_s. That is the check that catches a host-header leak such as
`__isoc23_strtol`, the RimWorld AD-11 failure.

### 2.3 Layout proposal for F4, and how it coexists with the RimWorld shim

**F4 row to add:** `/CP/X11` holds the real X client libraries.
- `lib/` contains the 37 libraries under their SONAMEs, each with DT_RPATH
  `$ORIGIN`.
- `share/X11/` contains locale, XErrorDB and Xcms.txt.
- It sits beside `/CP/WINE` and `/CP/VK`.

| Process | Finds | Why |
|---|---|---|
| RimWorld (and anything else with no X path of its own) | `/LIB64/libX11.so.6` = the shim | The kernel's compat startup block appends `LD_LIBRARY_PATH=/LIB64` (`src/user/xcompat/README.md`). Nothing about the shim changes, and it is still the only libX11 in `/LIB64`. |
| An executable built for the real libs (xsmoke) | `/CP/X11/lib/*` | Its own DT_RPATH `/CP/X11/lib`. Old-style DT_RPATH is searched **before** LD_LIBRARY_PATH and applies to the whole dependency tree. |
| Any library loaded *from* `/CP/X11/lib` | its siblings in `/CP/X11/lib` | DT_RPATH `$ORIGIN` on each staged library, again searched before LD_LIBRARY_PATH. Once one real library is in, the shim cannot be mixed in under it. |
| Wine (`winex11.so` `dlopen`s `libX11.so.6` etc. by soname) and Mesa's X11 WSI | must put `/CP/X11/lib` **first** | The launcher sets `LD_LIBRARY_PATH=/CP/X11/lib:/CP/WINE/lib:/CP/VK/lib:/LIB64`. A bare-soname `dlopen` from an object without RPATH uses LD_LIBRARY_PATH order, so `/LIB64` first would hand Wine the shim. Alternative: link `winex11.so`/`winevulkan.so` with DT_RPATH `/CP/X11/lib`. |

Do not put the real libX11 into `/LIB64`, and do not put the shim into `/CP/X11`.
Each tree must be self-consistent.

## 3. What `XHOBBY.BIN` offers

XHOBBY.BIN is X.Org xserver 21.1.25 with the `hw/hobbyos` DDX, built by
`third_party/x11/build-server.sh`. Source evidence is `third_party/x11/…` (in git)
plus the extracted server tree on linuxci, `third_party/x11/build/intel/src/xserver/`
(file:line below) and its generated `obj/xserver/include/dix-config.h`. The
"runtime" column is what the smoke client got back inside HobbyOS (§4).

| Extension | Built / advertised | Version (source) | Runtime (in-OS smoke) | Stubbed / caveats | Evidence |
|---|---|---|---|---|---|
| XInputExtension (XI2) | yes, always | 2.4 (`include/protocol-versions.h:146-147`) | **2.4**; device 6 "HobbyOS pointer": valuators relative=0 absolute=2; `XISelectEvents(root, RawMotion\|RawButtonPress)` accepted | One slave pointer whose two valuators are **Absolute** in screen pixels (`ddx/hobbyos/hbinput.c:156-160`). REL_X/REL_Y from evdev are folded into a clamped absolute position (`hbinput.c:258-262`, clamp `:221-224`) and posted as `POINTER_ABSOLUTE \| POINTER_SCREEN` (`:186-187`). XI_RawMotion is generated by dix (`dix/getevents.c:180`), but it carries absolute screen coordinates that stop changing at the screen edge, so there are **no relative deltas for mouselook**. | `mi/miinitext.c:116`, `dix-config.h:275` |
| RANDR | yes | 1.6 (`protocol-versions.h:75-76`) | **1.6**; crtcs=1 outputs=1 (HOBBYFB, mm=0x0) modes=1 (1920x1080), monitors=1 | One CRTC, one output "HOBBYFB", **one mode** = the framebuffer size. `rrScreenSetSize` accepts only the current size (`ddx/hobbyos/hbscreen.c:730-735`), and `RRScreenSetSizeRange` is min = max (`:753-754`). `rrCrtcSet` only notifies (`:723-727`). Gamma size 256, not applied. Initialised per screen (`:896`). | `miinitext.c:136-137`, `hbscreen.c:738-775` |
| RENDER | yes (`+/-render`, default on) | 0.11 (`protocol-versions.h:83-84`) | **0.11**, 47 picture formats; `XRenderFillRectangle` drew the red bar | Software fb (`fbPictureInit`, `hbscreen.c:888`). No acceleration. | `miinitext.c:135` |
| XFIXES (and Cursor via XFIXES + RENDER) | yes | 6.0 (`protocol-versions.h:142-143`) | **6.0**; `XcursorSupportsARGB=1`, `XcursorSupportsAnim=1` | Cursor is the mi software sprite (`miDCInitialize`, `hbscreen.c:904`). ARGB/animated cursors go through RENDER. Pointer barriers and `XFixesHideCursor` are dix-generic. | `miinitext.c:131`, `xfixes/xfixes.c:228-237` |
| MIT-SHM | **no** | (would be 1.2) | absent | `-Dmitshm=false` (`third_party/x11/build-server.sh:106`) gives `#undef MITSHM` (`dix-config.h:203`). The server is also built without `XTRANS_SEND_FDS` (`patches/xserver/0001…patch`: xtrans's 528-byte SCM_RIGHTS buffer exceeds the kernel's `K_IPC_CTRL_MAX 512`, `src/include/ipc_proto.h:40`, `src/kernel/fs.c:3985`), so even SHM 1.2's fd attach would have no transport. | `miinitext.c:113-115` |
| Present | yes | 1.2 (`protocol-versions.h:71-72`) | **1.2** (xcb) | The DDX never calls `present_screen_init`. The extension init does it with `info=NULL` (`present/present_screen.c:245-247`), which selects the **fake** (timer-driven msc, copy-only) mode (`present_screen.c:211`, `present/present_fake.c:119`). There are no flips. Present is disabled whenever Xinerama/PanoramiX is active (`present_screen.c:226-229`); that is off by default (`os/utils.c:168`). | `miinitext.c:157-158`, `dix-config.h:215` |
| DRI3 | **no** | (would be 1.2) | absent | `-Ddri3=false` (`build-server.sh:107`) gives `#undef DRI3` (`dix-config.h:46`). It needs fd passing (above) and a dma-buf/render-node story HobbyOS does not have. | `miinitext.c:160-162` |
| SYNC | yes | 3.1 (`protocol-versions.h:111-112`) | **3.1**, 8 system counters | dix-generic: counters and fences (`miSyncInitFence`). No DDX fence hooks. | `miinitext.c:121` |
| XINERAMA | **not advertised in the default session** | PanoramiX 1.1 | **absent** (`ScreenCount=2`) | PanoramiX is off (`noPanoramiXExtension = TRUE`, `os/utils.c:168`). RandR's Xinerama shim is skipped when `screenInfo.numScreens > 1` (`randr/rrxinerama.c:430`), and the default depth-24 session **always has two screens**: `:0.1` is the depth-8 PseudoColor Doom screen, added when screen 0 is not depth 8 (`ddx/hobbyos/hbscreen.c:957-966`). | `rrxinerama.c:415-434`, `randr/randr.c:450` |
| Composite | built, but **not advertised in the default session** | 0.4 (`protocol-versions.h:42-43`) | **absent** | `CompositeExtensionInit` returns without registering if *any* screen's root visual is PseudoColor (`composite/compext.c:540-545`), and the default session's `:0.1` is depth-8 PseudoColor (`hbscreen.c:957-966`). Same root cause as XINERAMA. Wine uses XComposite for its offscreen child windows (GL/Vulkan child surfaces); without it they fall back. | `miinitext.c:139-141`, `dix-config.h:18` |
| DAMAGE | yes | 1.1 (`protocol-versions.h:46-47`) | **1.1** (xcb) | Generic. | `miinitext.c:142-144`, `dix-config.h:30` |
| XKEYBOARD | yes | 1.0 | **1.0** | Keymap is a prebuilt `/X11/US.XKM`; xkbcomp is never run (`patches/xserver/0003`). | `miinitext.c:122` |
| GLX | no | — | absent | `-Dglx=false` (`build-server.sh:97`) | |

### 3.1 What P4 and P7 must add server-side (input for the next lane)

**P4**

1. **Relative pointer motion for XI2 raw events.** Add REL_X/REL_Y valuators,
   either as relative axes on the existing device or as a second relative slave
   pointer, and post REL events with `POINTER_RELATIVE` and accumulated, unclamped
   deltas. This is what Wine's `XI_RawMotion` mouselook consumes. Today the raw
   valuators are absolute, clamped screen coordinates (`hbinput.c:156-160`,
   `:221-224`, `:258-262`). It also needs the kernel/QEMU mouse path to deliver
   relative events: with `-device usb-tablet`/vmmouse only ABS arrives
   (`hbinput.c:18-19`). Pointer grab and confine are dix-generic and need no DDX
   work, but must be verified with IceWM.
2. **RandR modes.** One mode per supported resolution, plus a real
   `rrCrtcSet`/`rrScreenSetSize` that changes the scanout (or accepts the mode
   and scales), so Wine's fullscreen mode switch works or fails cleanly. Today
   only the current size is accepted (`hbscreen.c:730-735`, `:753-754`).
3. **XINERAMA + Composite: single-screen sessions.** Both are missing at runtime only
   because of the depth-8 `:0.1` screen (`rrxinerama.c:430`, `compext.c:540-545`).
   Either add a session switch that drops
   the depth-8 `:0.1` screen for Wine sessions (so RandR's Xinerama shim
   and Composite are advertised again and `ScreenCount` is 1). Measured with the
   smoke: `screens=2`, XINERAMA absent, Composite absent. Wine falls back to RandR
   monitors without Xinerama. Composite matters more, because Wine's child-window
   GL/Vulkan surfaces use it. Recommend: a Wine session sets
   `HB_SECOND_SCREEN=0`-style one-screen mode (a new DDX switch; today
   `HB_SECOND_SCREEN` can only force the second screen *on*,
   `hbscreen.c:818-824`).
4. **MIT-SHM.** Enable `-Dmitshm=true` with the SysV path (`shmget`/`shmat`/
   `shmctl` in the compat and native syscall tables; none exist today, as
   `grep -rn shmget src/kernel` shows nothing) **or** the fd path (SHM 1.2
   `ShmAttachFd`), which needs `XTRANS_SEND_FDS` back. That in turn needs the
   kernel's `K_IPC_CTRL_MAX` raised above xtrans's 528-byte control buffer (or
   recvmsg truncation semantics like Linux's) and a shareable memfd. winex11
   uses XShm for its DIB surfaces when present and falls back to XPutImage.
   Mesa's software WSI uses shm + Present when both exist.

**P7**

5. **Present with real vblank and flips,** or a HobbyOS-specific scanout
   handoff. Today Present runs in fake mode, timer msc, copy only
   (`present_screen.c:211`). P7's `SET_SCANOUT_BLOB` path (AD-6, F5) needs either
   a `present_screen_info` with `flip`/`queue_vblank` hooks in the DDX, or WSI
   bypassing X entirely while it owns the screen.
6. **DRI3** (only if the Venus/WSI path wants X-mediated buffer sharing). It needs
   fd passing (item 4's transport) plus a dma-buf-like fd for blobs. Not needed
   for lavapipe, whose X11 WSI uses PutImage or SHM.

## 4. In-OS smoke

`tools/cp/x11/xsmoke.c` is an ordinary glibc 2.31 PIE:
- GLIBC_max 2.17, interpreter `/lib64/ld-linux-x86-64.so.2`, DT_RPATH `/CP/X11/lib`.
- 19 DT_NEEDED entries: libX11, X11-xcb, xcb, xcb-{present,dri3,composite,damage,shm,sync},
  Xext, Xi, Xrandr, Xrender, Xfixes, Xcursor, Xinerama, xshmfence, libdl, libc.

`tools/cp/x11/smoke.py` reuses existing harness pieces:
- From `tools/doom_elf/run_doom_elf_test.py`: `qemu_cmd`, `wait_for`, and the
  XSTART/XSESSION.ENV recipe.
- From the RimWorld M3 gate `tools/gate/m3_x11_client.py`: `ppm_colors` and the
  `/LIB64` glibc set.
- From `run_icewm_test.py`: `Qmp`.

What the harness does:
1. Copies the default `DESKTOP=icewm` `disk.img`.
2. Stages `/CP/X11` (the §2 stage, `mcopy -s`), glibc 2.31 in `/LIB64` plus
   `/lib64/ld-linux-x86-64.so.2`, `/XSMOKE.ELF`, and `/XSTART.TXT`.
3. Boots under KVM. XSESSION starts XHOBBY.BIN and IceWM, then spawns the client.
4. Screendumps once the client reports its window drawn.
5. Grades the result by the exact pixel colours of the window.

Run it with `flock /tmp/cp-qemu.lock python3 tools/cp/x11/smoke.py`.

### 4.1 Gate: smoke output (verbatim lines, excerpted; the full log is `evidence/cp-x/smoke-harness.log`)

```
[CPX11] staged: 37 files in ::/CP/X11/lib, glibc 2.31 in ::/LIB64, ::XSMOKE.ELF, ::XSTART.TXT
[XS] xsmoke up pid=9 DISPLAY=:0 LD_LIBRARY_PATH=/LIB64
[XS] prefault: touched 1124 pages in 24 loaded objects in 5142 ms (kernel writev workaround)
[XS] libX11=/CP/X11/lib/libX11.so.6 libxcb=/CP/X11/lib/libxcb.so.1 libXi=/CP/X11/lib/libXi.so.6 libXrandr=/CP/X11/lib/libXrandr.so.2
[XS] XOpenDisplay ok: ":0" vendor="The X.Org Foundation" release=12101025 proto=11.0 screens=2 default=0 1920x1080 depth=24
[XS] XListExtensions: 17
[XS]   Generic Event Extension
[XS]   SHAPE
[XS]   XInputExtension
[XS]   XTEST
[XS]   BIG-REQUESTS
[XS]   SYNC
[XS]   XKEYBOARD
[XS]   XC-MISC
[XS]   XFIXES
[XS]   RENDER
[XS]   RANDR
[XS]   DAMAGE
[XS]   MIT-SCREEN-SAVER
[XS]   DOUBLE-BUFFER
[XS]   RECORD
[XS]   Present
[XS]   X-Resource
[XS] ext MIT-SHM                absent  opcode=0 event=0 error=0
[XS] ext DRI3                   absent  opcode=0 event=0 error=0
[XS] ext XINERAMA               absent  opcode=0 event=0 error=0
[XS] ext Composite              absent  opcode=0 event=0 error=0
[XS] XI2: XIQueryVersion(2.4) -> Success, server 2.4
[XS] XI2: device 6 "HobbyOS pointer" slave-pointer classes=3 valuators=2 (relative=0 absolute=2)
[XS] XI2: XISelectEvents(root, RawMotion|RawButtonPress) -> accepted
[XS] RandR: version 1.6
[XS] RandR: crtcs=1 outputs=1 modes=1
[XS] RandR: mode 0x39 1920x1080 1920x1080
[XS] RandR: output HOBBYFB connection=connected crtc=0x3a nmode=1 mm=0x0
[XS] RENDER: version 0.11, 47 picture formats
[XS] XFIXES: version 6.0
[XS] Xcursor: XcursorSupportsARGB=1 XcursorSupportsAnim=1
[XS] MIT-SHM: absent (XShmQueryVersion=False)
[XS] SYNC: version 3.1, 8 system counters, first=DEVICEIDLETIME 7
[XS] XINERAMA: absent
[XS] XKB: present 1.0
[XS] xcb: XGetXCBConnection=0x1040001750 (libX11-xcb /CP/X11/lib/libX11-xcb.so.1)
[XS] xcb Present: version 1.2
[XS] xcb DRI3: absent
[XS] xcb Damage: version 1.1
[XS] xcb SYNC: version 3.1
[XS] xshmfence: alloc_shm fd=4 map=ok
[XS] xshmfence: trigger -> query=1
[XS] ConfigureNotify 640x400 at 332,24
[XS] MapNotify
[XS] Expose -> window drawn (green field, blue block, RENDER red bar, text) font=fixed render_pic=yes
[XS] exposes=1 xerrors=0
[XS] done rc=0
  [XSESS] child pid=9 exited status=0
[CPX11] screendump /home/sarah/cp/x11-build/smoke-run/xsmoke-window.ppm: sampled pixels green=49775 blue=9600 red=4295
[CPX11] PASS: real libX11/libxcb client on XHOBBY.BIN :0 -- extensions listed, window drawn, rc=0
EXIT=0
```

The full row list, including the per-extension opcode rows and all six XI2
devices, is in `evidence/cp-x/smoke-harness.log`. The screenshot is
`evidence/cp-x/xsmoke-window.png`: the full 1920×1080 framebuffer at 50 %. It
shows the IceWM-decorated "xsmoke: real libX11 1.8.14 on HobbyOS" window with
the green field, blue block, RENDER red bar and core-font text, next to xeyes,
with the taskbar entry.

Other measured facts:
- libxshmfence's memfd + `MAP_SHARED` + futex path works in-OS (`trigger ->
  query=1`).
- The loader honoured DT_RPATH: every library came from `/CP/X11/lib`, while
  `LD_LIBRARY_PATH` was `/LIB64`.
- The prefault cost is 5.1 s for 1124 file-backed pages, which shows how slow
  the kernel's per-page `fault_obj` path is. With the kernel fix below, this
  cost goes away.

As a host control, the same binary against a host Xvfb on linuxci lists 23
extensions: MIT-SHM, XINERAMA (active), Composite, SECURITY, XVideo and GLX
appear, and the HobbyOS server lacks them.

### 4.2 Kernel defect found: compat `writev` from a not-yet-faulted file-backed page hangs a CPU

Reported, not fixed: this lane does not touch kernel sources. Evidence is in
`evidence/cp-x/noprefault-hang.txt`.

**What happens without the prefault.** The first in-OS run did not have it.
xsmoke loads all its libraries from `/CP/X11/lib` and prints its first row. Then
`XOpenDisplay` never returns. Its first `XQueryExtension` ("XKEYBOARD") sends a
request whose iovec includes libX11's static `pad[]` in `.rodata`, and no
user-mode access has touched that page yet.

**What the serial log shows.** CPU1 stays for 285 s at `0x700c166a`, which is
`memcpy` (addr2line, `obj/intel/hobbyos.boot.elf`), with:
- `vec=14` (#PF) and `rflags=0x10046` (IF=0)
- `cr2=0x10800b936c`, which is libX11 + 0xb936c, inside `.rodata`
- `syscall=20` (writev)

The rest of the system keeps running.

**Cause (from the source).** `compat_writev` (`src/kernel/arch/x64/trap.c:1149-1185`)
validates only the iovec array (`sys_user_range_ok(iov, cnt*16)`). It then passes
each segment's raw user `base` to `file_write`, which copies from it in kernel
mode. A kernel-mode #PF on a lazily mapped file page (`[PUB] who=fault_obj`) is
never serviced. `compat_readv` (`:1192-1223`) and `l_misc_readv`
(`src/kernel/linux_compat_misc.c:70-104`) have the same shape. They only
range-check `base` and call `file_read` on it.

**Fix (owner: C2-sys or C1-mmu).**
1. Bounce each segment through `vm_user_read`/`l_copy_from_user`, which handle
   lazy pages, or pre-fault `[base, base+len)` before calling `file_write`/`file_read`.
2. Make kernel-mode #PF on a user address either resolve the fault or return
   `-EFAULT`. It must never spin.

Wine (its `writev`s go to wineserver and X) and Mesa will hit this on every
untouched read-only page, so it blocks P2 and P4, not just this smoke.

The workaround in `xsmoke.c` walks `dl_iterate_phdr` and reads one byte per page
of every PT_LOAD before `XOpenDisplay`. `XSMOKE_PREFAULT=0` in the environment
when running `smoke.py` reproduces the hang.

## 5. Facts for lane C5-vk (Mesa X11 WSI)

**Paths to build against (linuxci).**
- Headers and link libraries: `~/cp/x11-build/destdir/CP/X11/{include,lib}`.
- pkg-config: `PKG_CONFIG_LIBDIR=~/cp/x11-build/pc`. It already contains
  `xcb.pc`, `xcb-dri3.pc`, `xcb-present.pc`, `xcb-shm.pc`, `xcb-sync.pc`,
  `xcb-xfixes.pc`, `xcb-randr.pc`, `x11-xcb.pc`, `xshmfence.pc`, `x11.pc`,
  `xrandr.pc`, and the rest.
- Compile with `clang --sysroot=~/cp/wine-build/sysroot-glibc231`, as here. Add
  `-Wl,-rpath-link,~/cp/x11-build/destdir/CP/X11/lib` so ld resolves libxcb's
  libXau/libXdmcp.

**Runtime.**
- The libraries live in `/CP/X11/lib`, under their SONAMEs: `libxcb.so.1`,
  `libX11-xcb.so.1`, `libxcb-dri3.so.0`, `libxcb-present.so.0`,
  `libxcb-shm.so.0`, `libxcb-sync.so.1`, `libxcb-xfixes.so.0`,
  `libxcb-randr.so.0`, `libxshmfence.so.1`.
- Either link the WSI DSO or `vulkaninfo` with `-Wl,--disable-new-dtags,-rpath,/CP/X11/lib`,
  or run with `LD_LIBRARY_PATH=/CP/X11/lib:…:/LIB64`, with `/CP/X11/lib` before `/LIB64`.
- Stage the tree with `mcopy -s -o -i disk.img ~/cp/x11-build/stage/CP/X11 ::/CP/`
  (see `tools/cp/x11/smoke.py:stage`).

**What the server offers a WSI.**
- Present 1.2 in fake-msc copy mode.
- No DRI3 and no MIT-SHM, so expect Mesa's X11 WSI to take its software
  `xcb_put_image` path (this lane did not run Mesa; verify in C5-vk).
- SYNC 3.1.
- RandR 1.6 with one output/mode (HOBBYFB, mm 0×0 — beware DPI maths).
- XFIXES 6.0.

libxshmfence works in-OS. Expect the §4.2 writev hang unless the kernel is fixed
or the process prefaults.

## 6. Not done / follow-ups

- Wine's optional X libraries are not built: libXxf86vm, libXcomposite and the
  XSS screensaver library. Wine configures without them. Add them to `fetch.sh`
  and `build.sh` if P4 wants them; the pattern is one row each.
- `xcb-proto` and `libxcb` are not gpg-verified in this lane (§1.2).
- The staged tree is not part of the default disk recipe. Only the smoke harness
  stages it. F4/integrator: add a `make` rule mirroring `smoke.py:stage` when a
  P4 gate needs it.
- XI2 raw *motion delivery* was not exercised with injected input; only selection
  succeeded. P4's raw-input gate covers it after §3.1 item 1.
