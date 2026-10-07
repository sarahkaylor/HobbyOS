# WE-2 integration note — intel (x86_64) WebKit curl/mbedTLS staging

Status: DONE (WE-2 lane). Makes the fork's canonical `WebKitBuild/HobbyOS-intel`
build dir **re-configurable and reproducible from the merged tree**, without
touching the (read-only) fork repo.

## Problem (WF1 integration flag)

The canonical `WebKitBuild/HobbyOS-intel` build dir predates the merged
`USE_CURL` wiring in `Source/cmake/OptionsHobbyOS.cmake` (WK-4b path B: curl
NetworkProcess backend). Its CMakeCache has no CURL entries, so a **fresh
re-generate fails at `find_package(CURL 7.87.0 REQUIRED)`** for intel: the fork's
`wk4b-libs-cross.sh` is ARM-only, so no intel curl/mbedTLS staging exists.
The existing intel binaries are green — this closes the *reproducibility* gap.

## The fix

1. `tools/cross-intel-wk2-libs.sh` (OS repo) — the missing intel half of
   `wk4b-libs-cross.sh`, mirroring it one-for-one (same prefix layout, same
   curl source patches, same probe/link closure) with the intel spec delta:
   `--target=x86_64-none-elf -mno-red-zone -mcmodel=large -nostdinc
   -isystem <clang-resource>/include`, `-Wl,-no-pie` (x86_64 none-elf defaults
   PIE; the non-PIC sysroot closure is not PIC), and setjmp/longjmp from
   `obj/intel/setjmp.o` (the OS keeps them out of libc.a on x86_64; the ARM
   naked defs in the fork `gaps.c` are compiled out there).
2. `tools/mbedtls-user-config.h` (OS repo) — same user config the fork uses
   (no `MBEDTLS_NET_C`/`TIMING_C`, entropy + ms_time ALT via `mbedtls_alt.c`).
3. Staged artifacts (same layout as the ARM prefix):
   `webkit-hobbyos-wk2/intel/prefix/{include,lib}`:
   - mbedTLS 3.6.7   → `include/mbedtls` (+ `include/psa`),
     `lib/libmbedtls.a lib/libmbedx509.a lib/libmbedcrypto.a`
   - libcurl 8.22.0  → `include/curl/curl.h`, `lib/libcurl.a` (mbedTLS backend)
   - plus the fork net-compat closure objects in
     `webkit-hobbyos-wk2/intel/src/wk4b-obj/{netcompat,resolv,mbedtls_alt,gaps,gaps_varc}.o`
     (used by curl's autoconf probes and, via the fork toolchain, the WebKit
     link closure).

Tarballs come from the OS repo's committed `third_party/`
(`mbedtls-3.6.7.tar.bz2`, `curl-8.22.0.tar.xz` — the fork's dl dir only ships
libxml2). The fork is READ-ONLY: sources are read from `FORK=/home/sarah/
webkit-hobbyos`, nothing is written there.

## How to re-configure the canonical intel build dir (exact commands)

Requires the staging (once):

```sh
export PATH="$HOME/.local/bin:/usr/lib/llvm-21/bin:$PATH"
bash /home/sarah/Documents/GitHub/HobbyOS/tools/cross-intel-wk2-libs.sh
# -> mbedTLS + curl into /home/sarah/webkit-hobbyos-wk2/intel/prefix
#    + link smoke (curl_easy_init + mbedtls_ssl_init) — prints "smoke ok"
```

Then the canonical configure (the fork CI recipe in `HobbyOS/scripts/
wk2-link-ci.sh`, with `HOBBYOS_WK2_PREFIX` pointing at the intel prefix —
this is the ONLY flag the intel curl wiring needs beyond the ARM recipe):

```sh
F=/home/sarah/webkit-hobbyos
CMAKE=/home/sarah/.local/share/l6-tools/cmake-3.31.8-linux-x86_64/bin/cmake
"$CMAKE" -S "$F" -B "$F/WebKitBuild/HobbyOS-intel" -G Ninja \
  -DPORT=HobbyOS -DHOBBYOS_ARCH=intel \
  -DCMAKE_TOOLCHAIN_FILE="$F/HobbyOS/toolchain-hobbyos.cmake" \
  -DHOBBYOS_WK2_PREFIX=/home/sarah/webkit-hobbyos-wk2/intel/prefix \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=ninja \
  -DUSE_LCMS=OFF -DUSE_WOFF2=OFF -DUSE_JPEGXL=OFF -DUSE_AVIF=OFF
```

`OptionsHobbyOS.cmake` resolves the CURL find from `HOBBYOS_WK2_PREFIX`:
`CURL_INCLUDE_DIR=${_P}/include`, `CURL_LIBRARY=${_P}/lib/libcurl.a`, and the
`MbedTLS::MbedTLS` imported target from `${_P}/lib/libmbed*.a` — so a fresh
configure goes GREEN at CURL, and `ninja -C "$F/WebKitBuild/HobbyOS-intel"
NetworkProcess` links.

## Arm/intel staging parity

| | ARM (wk4b-libs-cross.sh) | intel (cross-intel-wk2-libs.sh) |
|---|---|---|
| triple | aarch64-none-elf | x86_64-none-elf |
| arch CFLAGS | `-mcpu=cortex-a53` | `-mno-red-zone -mcmodel=large` |
| freestanding | `-nostdinc -isystem $(clang -print-resource-dir)/include` | same |
| PIE | default non-PIE | `-Wl,-no-pie` required (non-PIC closure) |
| setjmp/longjmp | gaps.c naked defs (ARM-only) | `obj/intel/setjmp.o` |
| prefix | `webkit-hobbyos-wk2/arm/prefix` | `webkit-hobbyos-wk2/intel/prefix` (same `include/ lib/` layout) |
| mbedTLS | 3.6.7 static, same user-config | same |
| libcurl | 8.22.0 static, mbedTLS backend, `select.h`/`wait.c` patches | same |
| closure | netcompat/resolv/mbedtls_alt/gaps/gaps_varc + setjmp | same + setjmp.o |

## Evidence (WE-2, fresh /tmp fork copy, fork @ 232f694b74)

- `tools/cross-intel-wk2-libs.sh`: mbedTLS 3.6.7 + libcurl 8.22.0 built into
  the intel prefix; link smoke `intel-link-smoke` (6.5 MB ELF) resolves 2/2
  backend symbols, 0 undefined refs.
- Fresh configure in `/tmp/we2-fork` (rsync of the fork without .git/
  WebKitBuild) with `-DHOBBYOS_WK2_PREFIX=/home/sarah/webkit-hobbyos-wk2/intel/
  prefix`: GREEN — `-- Found CURL:
  .../intel/prefix/lib/libcurl.a (found suitable version "8.22.0", minimum
  required is "7.87.0")` (the WF1 integration flag), and the generated
  closure objects all compile from the /tmp copy (WDK_FORK_DIR).
- `ninja NetworkProcess -j 64` (all 7913 steps) LINKS GREEN:
  `bin/NetworkProcess` = 180,723,360 B, ELF64 x86-64, entry `0x1000000000`,
  `curl_easy_init` + `mbedtls_ssl_init` concretely defined inside (static
  backend closure), no undefined symbols.  The canonical predates the USE_CURL
  wiring (171,644,168 B, no CURL in its cache); the fresh build is ~9 MB
  larger — that delta is the curl/mbedTLS path-B networking closure.
- `ninja WebProcess HobbyOS-UIProcess` also LINKS GREEN in the same fresh
  build dir (182,197,792 B / 174,769,472 B), so the WHOLE intel WebKit2
  process stack is re-configurable + reproducible from the merged tree.

---

_Update (2026-10-07, vendoring):_ the ARM half now also lives in the OS repo —
`tools/cross-arm-wk2-libs.sh` (direct port of `continuation/wk4b/
wk4b-libs-cross.sh`, which is excluded from the vendored snapshot in
`third_party/webkit-hobbyos/`), so both arches stage curl/mbedTLS from
committed sources alone.  The vendored `build.sh --build-deps` runs
`wk2-libs-cross.sh --only zlib,png,jpeg,webp,freetype,hbcore,sqlite,xml2`
(D-15: the full harfbuzz build is blocked; hbcore fills `lib/libharfbuzz.a`)
plus the per-arch curl/mbedTLS script, with `WK4B_SRCROOT` keeping the
workspaces inside the repo.  Verified: ARM staging + link smoke green on a
fresh prefix; `rebuild_browser.sh arm` completes end-to-end from it.
