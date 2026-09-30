# ICU 78.3 → HobbyOS target cross build — feasibility memo (time-boxed probe)

Lane `browser/l6-icu`, 2026-09-30. Scope: find what an `aarch64-none-elf` /
`x86_64-none-elf` cross build of this ICU pin needs. Time-boxed: **the
two-stage wiring is proven and `configure` was made to pass on both arches;
`make` is blocked at two well-understood layers with exact next steps (no
full cross build was forced — per the brief).**

## What was probed (all reproducible; dirs live under `build-host/`)

- **Host-tools stage** (the "first half" of the two-stage model): the normal
  host build tree `build-host/obj` (built by `build-host.sh`, tools enabled —
  that choice was deliberate for this). ICU accepted it:
  `Using cross buildroot: .../build-host/obj`.
- **Target runtime**: the in-tree sysroot-in-the-making
  `make ARCH=arm obj/arm/libc.a` / `make ARCH=intel obj/intel/libc.a`
  (crt0 + libc + pthread + the small cxxrt shim in one archive) plus
  `src/user/linker.ld`.
- **Toolchain**: clang 21.1.8 + ld.lld 21.1.8 (the repo's `LD=ld.lld` choice
  is not optional — see below).
- Probe dirs / logs: `build-host/obj-cross-arm-probe{,2,3}/probe*-configure.log`,
  `build-host/obj-cross-x64-probe/probe-x64-configure.log`.

## Working `configure` recipe — VERIFIED (exit 0, both arches)

```sh
# aarch64 (x86_64: swap the triple/flags, see below, and obj/intel paths)
REPO=~/hobbyos-lanes/l6-icu   # the worktree: src/, obj/arm/, obj/intel/, linker.ld live here
VENDOR=$REPO/third_party/icu-78.3
CC="clang --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 \
    -mgeneral-regs-only -fuse-ld=lld -nostdlib"
CXX="clang -x c++ --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 \
    -mgeneral-regs-only -fuse-ld=lld -nostdlib"
CFLAGS="-O2 -I$REPO/src/libc/include -I$REPO/src/user_include -I$REPO/src/include"
CXXFLAGS="$CFLAGS"
LDFLAGS="-T $REPO/src/user/linker.ld"
LIBS="$REPO/obj/arm/crt0.o $REPO/obj/arm/libc.a"

$VENDOR/src/source/configure \
  --host=aarch64-none-elf --build=x86_64-pc-linux-gnu \
  --with-cross-build=$VENDOR/build-host/obj \
  --prefix=$VENDOR/build-host/prefix-arm \
  --disable-shared --enable-static --disable-samples --disable-tests \
  --disable-extras --disable-icuio --with-data-packaging=static
```

x86_64 variant: `--target=x86_64-none-elf -ffreestanding -mno-red-zone
-mno-sse -mno-sse2 -mno-mmx -mno-avx` + **`-Wl,-no-pie`** (critical, see #3)
+ `obj/intel` paths + `--host=x86_64-none-elf`.

### Why each flag is there (probed failure chain)

1. **`-fuse-ld=lld`** — Ubuntu's default link driver is x86-64 binutils
   `ld.bfd`: `unrecognised emulation mode: aarch64elf`. The repo Makefile
   uses `ld.lld` for exactly this reason.
2. **`-nostdlib`** + crt0/libc.a in `LIBS` + `-T linker.ld` in `LDFLAGS` —
   without it clang passes its own startfiles and `-lgcc`/`-lc`:
   `unable to find library -lgcc/-lgcc_s/-lc`. With it, configure's link
   tests are literally the in-tree user-program link recipe.
3. **`-Wl,-no-pie` (x86_64 only)** — for `x86_64-none-elf`, clang delegates
   the link to `/usr/bin/gcc` (`-###` shows `"/usr/bin/gcc" ... -m64`); Ubuntu
   gcc defaults to PIE → lld emits ET_DYN → every absolute relocation from
   crt0.o/libc.a is rejected (`R_X86_64_32S cannot be used against local
   symbol; recompile with -fPIC`). The driver-form `-no-pie` is *swallowed*
   by clang and never reaches gcc; `-Wl,-no-pie` fixes it. aarch64 does not
   hit this (clang links aarch64-elf directly with lld). If the gcc-delegation
   causes more trouble later, the alternative is a link wrapper that calls
   `ld.lld` directly, mirroring the repo's `$(LD)` usage.
4. **`-mgeneral-regs-only` (aarch64)** — configure's double-varargs test fails
   (`test_varargs requires 'double' type support, but ABI 'aapcs' does not
   support it`) but configure continues. Open item: audit ICU's internal
   `u_vsnprintf`-style paths (and future JSC/JIT ABI) if user-space FP stays
   banned outside SIMD lanes.

### What configure detected (arm & x64, recorded in the logs)

- `inttypes.h`/`stdint.h`/`strings.h` yes; `mmap` yes; `gettimeofday` yes;
  `pthread_mutex_lock` yes (from libc.a); `pthread_attr_init in -lpthread` no
  (expected — no separate libpthread); `dlopen` **no**; `tzset` **no**.
- `CPPFLAGS` auto-gained `-DHAVE_DLOPEN=0 -DU_HAVE_POPEN=0 -DU_HAVE_TZSET=0
  -DU_HAVE_TZNAME=0 -DU_HAVE_TIMEZONE=0 -DU_HAVE_WCSCPY=0`.
- `ICUDATA_CHAR = l` on both targets → little-endian data, no `icupkg -t`
  swap needed for the LE host cross; `cross_compiling=yes` (run tests are
  skipped, as they must be).

## make blocker #1 — no platform fragment (`mh-unknown`), both arches

```
*** ERROR - configure could not detect your platform
*** see the readme.html
*** or, try copying icu/source/config/mh-linux to mh-unknown and editing it.
make: *** [Makefile:153: all-recursive] Error 2
```

Mechanism: the generated `config/Makefile.inc` ends with
`include $(pkgdatadir)/config/@platform_make_fragment_name@`; `configure`'s
platform detection has no hit for `*-none-elf`, so the fragment is
`mh-unknown`, which is a deliberate error stub
(`src/source/config/mh-unknown`: "this is not a real mh- file …").

**Exact next step**: add a vendor patch (house `patches/` pattern) that
installs `config/mh-unknown` = copy of `config/mh-linux` adapted for
bare-metal: keep `THREADSCPPFLAGS=-D_REENTRANT` and
`LDFLAGSICUDT=-nodefaultlibs -nostdlib`; drop `-ldl`/`-lpthread` from link
libs (HobbyOS pthread lives inside libc.a); soname/rpath rules are moot with
`--disable-shared --enable-static`. Then re-run `make`.

## make blocker #2 — no target C++ standard library

Immediately after the mh layer, compilation dies on missing C++ std headers
(demonstrated by compiling single TUs with the target flags):

```
common/unistr.cpp:23:10: fatal error: 'string_view' file not found
i18n/unicode/coll.h:61:10: fatal error: 'functional' file not found      # a PUBLIC ICU header
```

ICU 78 uses a real subset of C++17 std: `utility`, `string_view`,
`type_traits`, `algorithm`, `string`, `atomic`, `mutex`, `memory`,
`condition_variable`, `vector`, `limits`, `functional`. No libc++/libc++abi
for the target exists on this box (host libstdc++ headers are target-gated;
ROCm ships host-x86_64 libc++ libs only).

**Exact next step**: land the l3-cxxrt lane's libc++/libc++abi port
(browser.md §3.4: "static; exceptions off initially") — or rootless-install
libc++ headers + build libc++abi for the target — then add
`-nostdinc++ -isystem <sysroot>/include/c++/v1` to CXXFLAGS and the libc++/
libc++abi archives to `LIBS` (before libc.a). Verify ICU's exception usage
against `-fno-exceptions` before committing to it.

## Other cross considerations (recon, to verify when the build reaches them)

- **Two-stage/data**: with `--with-cross-build` wired, the data step runs the
  HOST `genrb`/`pkgdata`/`icupkg` — no target execution, no target
  filesystem. The trimmed-static strategy composes with the cross build
  unchanged (filter is configure-time; data is LE-native).
- **pthread**: HobbyOS P1 pthreads (syscalls 72–75) + libc.a provide
  `pthread_mutex_lock` etc.; verify the full set ICU's `umutex.cpp` needs
  (`pthread_mutex_init/destroy/lock/unlock`, `pthread_cond_*`, `pthread_once`,
  `pthread_key_*`) is declared in the sysroot pthread.h.
- **Endianness**: host LE == both targets → nothing to do.
- **Filesystem**: none needed for the target libs (static data; `ICU_DATA`
  paths dead code).
- **C++17**: keep `-std=c++17` in the target CXXFLAGS explicitly.

## Verdict

The two-stage model is real and wired (host toolchain accepted; both target
`configure`s pass with the documented wrapper). The remaining work is two
layers deep — an `mh-unknown` patch, then the C++ stdlib (l3-cxxrt) — plus
verification items above. No full target cross build was forced inside the
time box; this memo + the probe dirs contain everything a follow-up lane
needs to finish it.
