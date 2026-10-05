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
  prefix`: GREEN (CURL found), and `ninja NetworkProcess` links.
