# ICU 78.3 → HobbyOS target cross build (aarch64-none-elf / x86_64-none-elf)

Lane `browser/l6-icu-target`, 2026-09-30. **Status: DONE — both targets build
green from a fresh checkout with one script, no Makefile changes needed in
this lane.** The recipe is `build-target.sh` (this directory); artifacts land
in `obj/<arch>/icu/{libicuuc.a,libicui18n.a,libicudata.a}` + `MANIFEST.txt`.

The lower half of this file ("probed history") is the prior feasibility memo
(`browser/l6-icu`, same pin): the two-stage model, the `mh-unknown` overlay,
and the flag-failure chain it documented are all now *wired* — kept because
it records why each flag exists.

## Re-run — exact commands

```sh
cd <worktree>                                   # any worktree of this repo
bash third_party/icu-78.3/fetch.sh --data-src   # digest-check + extract (src/ is gitignored)
bash third_party/icu-78.3/build-host.sh         # host tools = the cross buildroot (one-time, ~18s @ -j32)
bash third_party/icu-78.3/build-target.sh --arch arm     # → obj/arm/icu/*
bash third_party/icu-78.3/build-target.sh --arch intel   # → obj/intel/icu/*
```

`build-target.sh` is self-healing: it builds `obj/<arch>/libc.a` + `crt0.o`
via the repo Makefile when missing, runs `fetch.sh --data-src` when the ICU
data sources are absent, runs `build-host.sh` when
`build-host/obj/bin/genrb` is absent, and builds `obj/<arch>/libcxx.a` via
`third_party/libcxx-21.1.8/build-target.sh` for the link probe when absent.
Options: `--outdir DIR --objdir DIR -j N --no-trim --no-probe
--probe-strict`. Vendor scripts are committed mode 644: invoke with `bash`.

Vendored tree layout: `src/` (extracted, gitignored), `build-host/` (host
tools + `obj-trim-src/`), `obj/third_party/icu-78.3/<arch>/` (default
out-of-tree configure/build dir), `obj/<arch>/icu/` (deliverables).

## Library set — what is built and why

| archive | members | JSC/Intl surface it covers |
|---|---|---|
| `libicuuc.a` (common+i18n-agnostic) | 202 | **the hard floor**: `u_init`/`u_cleanup`, `u_charType`/`u_getIntPropertyValue` (Unicode properties), `u_strToUpper`/`u_strToLower` (case mapping), `u_strToUTF8`/`u_strFromUTF8` (conversions), `ubrk_*` (ICU 78 moved `ubrk.cpp` into *common*), normalizer2, RTTI-using service code |
| `libicudata.a` | 1 (`icudt78l_dat.o`) | the trimmed static data blob (`icudt78_dat`); `--with-data-packaging=static` links data *into* the archive — no target filesystem anywhere |
| `libicui18n.a` | 254 | Intl-class: collation `ucol_*`, calendar `ucal_*`, date `udat_*`, numbers `unum_*`, transliteration `utrans_*`, charset detect `ucsdet_*` (the browser.md L6 list) |

*Minimality decision*: `uc + data` is the irreducible pair (the APIs named in
the brief all live in common). `i18n` is a **separate leaf archive** — verified
mechanically: `libicuuc.a` has **0** undefined symbols that `libicui18n.a`
defines (cross-check via `nm --undefined-only` ∩ `nm --defined-only`), so an
integrator that defers Intl can simply not link it. It is built anyway because
the marginal cost is ~11s of compile and zero risk (static archives), and the
alternative (a second configure/build mode) buys nothing. `--disable-tools
--disable-samples --disable-tests --disable-extras --disable-icuio` keep
everything else out; the host half (genrb/icupkg/pkgdata) lives in
`build-host/obj`.

## Invocation (verbatim as recorded in `obj/<arch>/icu/MANIFEST.txt`)

```
configure --host=aarch64-none-elf --build=x86_64-pc-linux-gnu
  --with-cross-build=third_party/icu-78.3/build-host/obj
  --prefix=<objdir>/prefix --disable-shared --enable-static
  --disable-samples --disable-tests --disable-extras --disable-icuio
  --disable-tools --with-data-packaging=static
  CC=<clang --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53
      -nostdinc -isystem <clang resource include> -fuse-ld=lld -nostdlib>
  CXX=<...same... -x c++ ... -nostdinc++ -I<libcxx include>>
  CFLAGS=-O2 -g -std=c11 -I<src/libc/include> -I<src/include> -I<src/user_include>
  CXXFLAGS=-O2 -g -std=c++17 -fno-exceptions -frtti <same -I's> <libcxx include>
  LDFLAGS=-T src/user/linker.ld [-Wl,-no-pie]      LIBS=obj/<arch>/crt0.o obj/<arch>/libc.a
make -j32
```
intel differs only in the triple/flags: `--target=x86_64-none-elf
-ffreestanding -mno-red-zone`, `--host=x86_64-none-elf`, LDFLAGS gains
`-Wl,-no-pie`, and `obj/intel/*` paths.

### Flag rationale (probed failure chain → why it is in the script)

1. **`-fuse-ld=lld`** — Ubuntu's default link driver is x86-64 binutils `ld.bfd`:
   `unrecognised emulation mode: aarch64elf`. (Repo Makefile uses `ld.lld` for
   the same reason.)
2. **`-nostdlib`** + crt0/libc.a in `LIBS` + `-T linker.ld` in `LDFLAGS` —
   without it the driver adds its own startfiles and `-lgcc`/`-lgcc_s`/`-lc`:
   `unable to find library -lgcc/-lgcc_s/-lc`. With it, configure's link tests
   *are* the in-tree user-program recipe.
3. **`-nostdinc` + explicit `-isystem $(clang -print-resource-dir)/include`** —
   hermetic search path; for `x86_64-none-elf` the driver would otherwise
   append the host glibc dirs (arch match!). For `aarch64-none-elf` glibc
   dirs would be silently skipped, so *both* arches pin it. Without this,
   ICU's configure probes find host headers that cannot link for the target.
4. **`-Wl,-no-pie` (intel only)** — for `x86_64-none-elf` clang delegates the
   link to `/usr/bin/gcc` (`-###` shows `"/usr/bin/gcc" ... -m64`), which
   defaults to PIE → lld emits `ET_DYN` → absolute relocations from
   crt0.o/libc.a are rejected (`R_X86_64_32S cannot be used against local
   symbol; recompile with -fPIC`). The driver form `-no-pie` is swallowed;
   only `-Wl,-no-pie` reaches gcc. aarch64 links with lld directly.
5. **`-ffreestanding -mcpu=cortex-a53`** (arm) / **`-mno-red-zone`** (intel) —
   match the Makefile's `USER_CFLAGS`. **Deliberate change vs the old memo:
   aarch64 does NOT get `-mgeneral-regs-only`** — ICU's on-tag code does
   double math (`astro.cpp`, `putil.cpp`) and HobbyOS userland has per-process
   FPU/SSE save-restore since F1.5 (the Makefile's own user flags drop
   `-mgeneral-regs-only` the same way).
6. **`-x c++`** — this box has no `clang++` binary; house pattern.
7. **`-fno-exceptions -frtti`** — ICU never throws (every `throw` hit in
   `common/`+`i18n/` is inside a comment/disabled block); RTTI stays **ON**
   because `normalizer2.cpp`, `serv.cpp`, `rbbi.cpp`, `alphaindex.cpp` use
   `dynamic_cast`/`typeid` (see gaps).
8. **`-std=c++17`** + `-nostdinc++ -I<libcxx include>` — ICU 78 uses a real
   C++17 std subset (`utility`, `string_view`, `mutex`, `atomic`, ...);
   served by the vendored libc++ 21.1.8 headers (l3-libcxx lane).
9. **`-O2 -g`** — matches house builds; note `-g` embeds the build dir path
   into DWARF (see reproducibility below).

## Two-stage model + data strategy (wired)

- `--with-cross-build=build-host/obj` — configure accepts it (`Using cross
  buildroot: ...`); `genrb`/`gencmn`/`pkgdata`/`icupkg` run from the **host**
  stage while data is *built target-side from source* (rules.mk invokes the
  host binaries), so `.res` bytes are produced by the pinned ICU 78.3 tools,
  not copied from a prebuilt blob.
- Trimming: `ICU_DATA_FILTER_FILE=<vendor>/data-filter-en.json` (committed
  next to this file; root+en only, no brkitr) is passed to configure.
- **Trap (handled in the script)**: a prebuilt `src/source/data/in/icudt78l.dat`
  present in the tarball makes ICU silently ignore the filter. The script
  parks it (`mv` to `icudt78l.dat.parked-prebuilt`) before a trimmed build and
  restores it under `--no-trim`. Measured consequence: trimmed result = 765
  `.res` files (identical count + names to the host-trimmed reference build;
  `diff -r` of the two res trees = 0).
- Endianness: host LE == both targets → no `icupkg -t` swap; `ICUDATA_CHAR=l`.

## Artifact inventory (the delivered bytes; from each MANIFEST)

| arch | archive | bytes | members | sha256 |
|---|---|---|---|---|
| arm | libicuuc.a | 18,930,074 | 202 | `dc90aaa5c6681a727896232629a1d1dd2f821b4a4278011189453e11495498e8` |
| arm | libicui18n.a | 32,115,574 | 254 | `9aa0a46db0bbb374bd51bbae6bfed8d0be4a21cb53e55fc38f833f88c06f59a9` |
| arm | libicudata.a | 10,195,700 | 1 | `d1eab185757d4e392e90fc5809f7a16e60f9c62845bffebbb18bab6ddfc84667` |
| intel | libicuuc.a | 18,897,126 | 202 | `907c7426381f74075140811abb3ef7436e963727fc359c8485a40484775870d3` |
| intel | libicui18n.a | 32,207,642 | 254 | `5f4401bc3393936678db26d5f894612c5ab59b288272f5c932df41855d477b38` |
| intel | libicudata.a | 10,195,700 | 1 | `60f887b3f1d68994a508d34973f2d9aee92ebc7565c3b564aad249486862b0c8` |

- These were produced by the final cold runs (`--objdir
  ~/.hermes/cache/scratch/icu-final-<arch> --outdir obj/<arch>/icu`); each
  MANIFEST records the exact configure line, times, member counts, sample
  member ELF type and probe status of *those* runs.
- Spot checks (`llvm-nm-21 --defined-only`, **13 symbols PASS on both
  arches**, versioned `_78` names because ICU renaming is on by default):
  `libicuuc.a` → `u_init_78 u_charType_78 u_strToUpper_78 u_strToLower_78
  u_strFromUTF8_78 u_strToUTF8_78 ubrk_open_78`; `libicui18n.a` →
  `ucol_open_78 ucal_open_78 udat_open_78 unum_formatDouble_78
  ucsdet_open_78`; `libicudata.a` → `icudt78_dat`.
- Member ELF check: `uinit.ao` = `ELF 64-bit LSB relocatable, ARM aarch64` /
  `... x86-64` respectively.
- **Reproducibility caveat**: `libicudata.a` is byte-identical across builds
  and build dirs (same sha256 from three separate runs). `libicuuc.a` /
  `libicui18n.a` are *content*-identical but not byte-identical: `-g` embeds
  the build-dir path in DWARF and GNU `ar` records member mtimes. Do not gate
  CI on archive sha256 — compare `nm` symbol sets / sizes.

## Build times (this box: 32-thread dev host)

| stage | time |
|---|---|
| `build-host.sh` (host tools, one-time, -j32) | **18s** |
| target cold, script end-to-end (fresh objdir; configure+make+nm+probe+manifest) | **arm 14.3s / intel 14.1s** (stage logs: configure 3s, make 11s, both arches) |
| `libcxx-21.1.8/build-target.sh` cold (probe prerequisite, per arch) | **3.9s** (80 objects) |
| target re-run, warm objdir | 0.6s (make no-op + probe + manifest) |

## Gaps — findings, not hacks (identical on both arches)

The committed link probe (`probe/icu_target_probe.cpp`, compiled as C++ and
linked against `libicui18n.a libicuuc.a libicudata.a libcxx.a libc.a` with
`src/user/linker.ld`) currently **fails to close**: 16 undefined symbols.
Exact names + their referencers (from `ld.lld`):

| symbol(s) | referenced by |
|---|---|
| `modf`, `log` | `common/putil.cpp:465,586` (`uprv_modf_78`, `uprv_log_78`) |
| `pow` | `common/putil.cpp:478,484` + libc++ `__math/exponential_functions.h` via `i18n/units_converter.cpp` (`std::pow`) |
| `expf`, `tanhf` | `common/lstmbe.cpp:278` (LSTM break engine; `std::tanh` from libc++ `__math/hyperbolic_functions.h`) |
| `sin cos tan asin atan atan2 sqrt` | `i18n/astro.cpp` (CalendarAstronomer) |
| `__dynamic_cast`, `vtable for __cxxabiv1::__{class,si_class,vmi_class}_type_info` | any `dynamic_cast`/`typeid` TU: `common/serv.cpp`, `common/normalizer2.cpp`, `common/rbbi.cpp`, `i18n/alphaindex.cpp` |

A uc-only closure experiment (probe reduced to `u_init`/`u_charType`/
`u_strToUpper`/`u_strToUTF8`/`ubrk_*`, linked against only `libicuuc.a
libicudata.a libcxx.a libc.a`) closes down to **9** symbols: the four RTTI
ones + `pow log modf expf tanhf` — i.e. all the trig is `i18n/astro.cpp`
only. Log: `obj/third_party/icu-78.3/<arch>/probe/icu_probe.link.log`.

Two sysroot/policy gaps to close upstream (not in this lane's scope):

1. **libm transcendentals** — the HobbyOS `<math.h>` slice (P3.2) declares but
   deliberately does not implement them (`src/libc/src/math.c` header comment:
   "The transcendentals (sin/cos/exp/pow/...) are declared in <math.h> but
   [not implemented]"). ICU needs them for real (analysis above).
2. **libc++abi RTTI cast machinery** — `libcxx.a` (l3-libcxx lane) excludes
   `private_typeinfo.cpp` (P3.1 policy: `dynamic_cast`/`typeid` = link-time
   gap by design). ICU's JSC-class surface *requires* it (RTTI on). Options:
   add `private_typeinfo.cpp` (+ `__cxa_bad_cast`/`__cxa_bad_typeid` handlers)
   to `libcxx.a`, or accept that Intl/ICU C++ paths cannot link until then.

**C-mode gap (minor)**: an *C* consumer of ICU headers cannot compile against
the HobbyOS sysroot — ICU's `common/unicode/ptypes.h:60` includes the C11
header `<uchar.h>` when `U_HAVE_CHAR16_T`:
`fatal error: 'uchar.h' file not found with <angled> include; use "quotes"
instead`. C++ consumers (all of ICU itself, JSC, the probe) are unaffected
(`!defined(__cplusplus)` guard). Workaround for a C TU: `-DU_HAVE_CHAR16_T=0`
(ICU then typedefs `char16_t` itself). Fix properly by adding `<uchar.h>`
to the sysroot.
**C++ `main` gotcha**: with `-ffreestanding`, clang mangles a C++ `main`
(`_Z4mainiPPc`) → `ld.lld: error: undefined symbol: main` against crt0.o.
House pattern: `extern "C" int main(...)` (see `src/user/cxx_t.cpp`).

Risks / open items for the integrator:

- ICU `configure` link tests currently *pass* with `-Wl,-no-pie` on intel; if
  the gcc delegation ever breaks, replace the driver form with an explicit
  `ld.lld` call (repo `$(LD)` pattern) — nothing else depends on it.
- Data is 10.2MB trimmed (English-only). Any locale the browser ships beyond
  root/en needs the filter file extended + a rebuild (11s each) — the full
  untrimmed blob is ~33MB.
- `-frtti`/exceptions: keep RTTI on until gap #2 is decided; `-fno-exceptions`
  is safe (verified by inspection; ICU's error model is `UErrorCode`).
- ICU's `umutex.cpp` pthread usage: configure detected
  `pthread_mutex_lock` yes / `pthread_attr_init in -lpthread` no — HobbyOS
  pthreads live in libc.a, so link order `... libicuuc.a libicui18n.a
  libcxx.a libc.a` (data anywhere before libc.a) is the constraint.

## What the parent must wire into the Makefile (suggested shape)

1. A rule `obj/<arch>/icu/libicuuc.a : <recipe deps>` that invokes
   `bash third_party/icu-78.3/build-target.sh --arch $(ARCH)` (phonies for
   `icu-host` → `build-host.sh`). Evidence file to depend on:
   `obj/<arch>/icu/MANIFEST.txt`.
2. Nothing else: the archives are self-contained static libs; user programs
   add `-L obj/<arch>/icu -licui18n -licuuc -licudata` (before `-lcxx -lc`).
3. Do NOT wire `--probe-strict` until gaps #1/#2 are fixed; the probe is
   informational (manifest field `probe:`).

## Probed history (prior lane `browser/l6-icu` — context, all now resolved)

The first memo proved the two-stage model and got `configure` green on both
arches with the flag set above, then stopped at two layers with exact next
steps; both are now done: (a) the `mh-unknown` overlay is committed at
`config/mh-unknown` (mh-linux copy + bare-metal header, re-copied over the
vendored tree by the script — upstream ships `mh-unknown` as an error stub,
and `configure` maps every `*-none-elf` host to it); (b) the C++ stdlib is
served by the l3-libcxx libc++ headers checked into `third_party/`.
Prior artifacts kept for reference: `build-host/obj-cross-arm-probe{,2,3}/`
(rpath/`-zorigin` link failures from `mh-linux`'s `LDFLAGSICUDT`) and
`build-host/obj-cross-x64-probe/`.
