# libc++ 21.1.8 vendor (browser.md §6, P3.1)

Static libc++ + libc++abi for the HobbyOS bare-metal targets, vendored per
the same recipe as `third_party/skia-588b550/` (fetch.sh + committed
manifest/overlay + gitignored extracted tree).

## Pin and integrity

| | |
|---|---|
| upstream | `https://github.com/llvm/llvm-project/releases` |
| tag | `llvmorg-21.1.8` (release tag; matches the workstation clang 21) |
| container | `llvm-project-21.1.8.src.tar.xz` |
| container sha256 | `4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142` (`container.sha256`) |
| content gate | `anchors.sha256` -- sha256 of four untouched upstream files, re-verified on every `fetch.sh` run |
| overlay | `config/__config_site`, `config/__assertion_handler` (hand-written CMake configure_file substitutes; copied into the tree by `fetch.sh`) |

The container tarball is **gitignored** (158 MB).  `fetch.sh` downloads it
(GitHub release assets are content-stable; verified twice on 2026-09-30), or
copy it from a lane worktree.  `src/llvm-project-21.1.8.src/` (99 MB) is
gitignored too; `fetch.sh` re-extracts and re-syncs the overlay, then
re-verifies the anchors.

Extracted subtrees (only what the port needs):

- `libcxx/` -- the C++ standard library (sources + headers)
- `libcxxabi/` -- the Itanium C++ ABI runtime
- `libc/shared/` -- LLVM-libc shared headers that libc++'s `<charconv>`
  floating-point code includes (`shared/fp_bits.h`,
  `shared/str_to_float.h`; build #3's failure -- "fp_bits.h not found" --
  is what made this subtree part of the pin)
- `compiler-rt/lib/builtins/` -- aarch64 binary128 helpers
  (`__extenddftf2` et al; x86_64 lowers long double to x87 and needs none)
- `cmake/` + `LICENSE.TXT` -- upstream glue kept for reference, Apache-2.0
  WITH LLVM-exception

## Why vendored

There is no system libc++ for the bare-metal target (`--target=aarch64-none-elf`
/ `x86_64-none-elf`), no libc++ dev package on the workstation
(`dpkg -l | grep libc++` is empty; `/usr/lib/llvm-21/include/c++/v1` does not
exist), and no CMake configure step in the HobbyOS build.  The port therefore:
vendors the pinned sources, hand-writes the two files CMake would generate,
and builds with plain `clang++` flags against the HobbyOS sysroot.

## Build configuration (what the Makefile/README call "the option set")

| option | value | why |
|---|---|---|
| `LIBCXX_ENABLE_EXCEPTIONS` | **OFF** | browser.md §6 P3.1; HobbyOS has no unwind tables/`__cxa_throw` landing pads |
| `LIBCXX_ENABLE_RTTI` | **OFF, one per-TU exception** | same block; everything stays `-fno-rtti` except `sources.txt` class `B` (`-frtti`): the libc++abi RTTI closure — `private_typeinfo.cpp` plus the `stdlib_typeinfo.cpp` / `stdlib_exception.cpp` typeinfo owners that reference it (see "RTTI closure" below). The rest of the archive, and every first-party user program but `RTTI_T.BIN`, stay RTTI-off |
| `LIBCXX_ENABLE_THREADS` | ON | P1 pthreads are implemented (syscalls 72-75, `docs/browser/p1-threads-design.md`); libc++ needs zero pthread symbols beyond HobbyOS's surface (checked: `comm` of the two symbol lists is empty) |
| `LIBCXX_ENABLE_MONOTONIC_CLOCK` | ON | `clock_gettime(CLOCK_MONOTONIC)` exists; `<unistd.h>` advertises `_POSIX_TIMERS` so libc++ enables its `clock_gettime` steady_clock path |
| `LIBCXX_ENABLE_LOCALIZATION` | ON, C-only | LLVM 21 compiles `<sstream>`/`<iostream>`/`basic_ostream` out entirely when 0; the C-only `*_l`/xlocale shims (`src/libc/src/xlocale.c`) make every `std::locale` observe the C locale |
| `LIBCXX_ENABLE_FILESYSTEM` | OFF | FAT16-only VFS; no directory-fd/stat surface `<filesystem>` needs |
| `LIBCXX_ENABLE_RANDOM_DEVICE` | ON | backed by libc `getentropy()` -> `SYS_GETRANDOM`; the `/dev/urandom` token is never opened (getentropy backend) |
| `LIBCXX_ENABLE_NEW_DELETE_DEFINITIONS` | ON | `libcxx/src/new.cpp` supplies C++17 aligned `operator new/delete` (backed by `aligned_alloc` added to `src/user/malloc.c`) |
| `LIBCXXABI_ENABLE_NEW_DELETE_DEFINITIONS` | OFF | one definition set: libcxx's `new.cpp`, not libcxxabi's `stdlib_new_delete.cpp` |
| `LIBCXXABI_ENABLE_EXCEPTIONS` | OFF | `cxa_noexception.cpp` path |
| PSTL backend | `_LIBCPP_PSTL_BACKEND_SERIAL` | no parallelism; `std::execution::par` falls back to serial (LLVM 21 errors without a backend) |
| rune table | `_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE` | HobbyOS `ctype.h` is byte-mode with no rune table; libc++ uses its built-in C-locale table |

`__config_site` (committed in `config/`) carries the site defines; the
rationale for each is inline in that file.

## Build

```sh
bash third_party/libcxx-21.1.8/fetch.sh          # extract + overlay + verify
bash third_party/libcxx-21.1.8/build-target.sh \
     --arch arm   --objdir obj/arm/libcxx   --out obj/arm/libcxx.a
bash third_party/libcxx-21.1.8/build-target.sh \
     --arch intel --objdir obj/intel/libcxx --out obj/intel/libcxx.a
```

- One static archive per arch (libc++ + libc++abi + the long-double
  builtins), built from `sources.txt` (`L `= libcxx, `A `= libcxxabi,
  `R `= compiler-rt builtins) by a generated, single-line-variable
  Makefile (`Makefile.gen`); no CMake anywhere.
- The archive is linked **ahead of** `libc.a`:
  `ld.lld -T src/user/linker.ld -e _start cxx_t.o obj/<arch>/libcxx.a obj/<arch>/libc.a`.
  Archive-on-demand pulls keep the non-C++ userland unchanged (it only
  picks up the `R` builtins via the three objects libc.a now carries).
- The Makefile does the same via `$(OBJ_DIR)/libcxx.a` and `$(CXX_T_BIN)`.

## Integration decision: cxxrt vs libc++abi (F2.4 -> P3)

F2.4 shipped `src/libc/src/cxxrt.cpp` (committed `bd8b7e8`) as a minimal
freestanding C++ runtime archived in `libc.a`: `operator new/delete`,
`__cxa_guard_*`, `__cxa_atexit/__cxa_finalize`, `__cxa_pure_virtual`,
`__dso_handle`.  (No RTTI surface there: the cast machinery is libc++abi's,
shipped as the class-B closure in the section above.)

P3 keeps **libc++abi inside `libcxx.a` as the single owner** of:

- `operator new`/`operator delete` (incl. array + sized/aligned forms) --
  `libcxx/src/new.cpp` (weak, so a program may still override),
- `__cxa_guard_acquire/__cxa_guard_release/__cxa_guard_abort`,
- `__cxa_pure_virtual`, `__cxa_deleted_virtual`,
- `__cxa_atexit`/`__cxa_finalize`/`__cxa_thread_atexit*`, demangling, etc.

and `cxxrt.cpp` keeps only the weak fallbacks a `libc.a`-only link needs
(CXXSMOKE.BIN, `src/user/cxx_smoke.cpp`): `__cxa_guard_*` and
`__cxa_pure_virtual`, which libc++abi then overrides strongly in
libcxx.a-linked programs.  The duplicate definitions were enumerated
mechanically before trimming: `ar x obj/arm/libc.a cxxrt.o && nm -g
--defined-only cxxrt.o` versus `nm obj/arm/libcxx.a`, intersect with
`comm -12`.

Evidence: `obj/arm/cxx_t.bin` links clean (584,312-byte raw binary) and the
wave's CXXSMOKE.BIN still passes -- CXXSMOKE links `libc.a` only, CXX_T
links `libcxx.a` + `libc.a`; both agree on the ABI.

## RTTI closure: libc++abi cast machinery (l3-rtti lane, 2026-09-30)

The P3.1 "no RTTI" policy left exactly four symbols undefined for any
consumer of ICU 78.3's C++ surface (its `dynamic_cast`/`typeid` TUs:
`common/serv.cpp`, `normalizer2.cpp`, `rbbi.cpp`, `i18n/alphaindex.cpp`):
`__dynamic_cast` and the vtables of `__class_type_info`,
`__si_class_type_info`, `__vmi_class_type_info` — all defined in one
libcxxabi file, `private_typeinfo.cpp`.  The closure now ships as
`sources.txt` class `B`, compiled with `-frtti` — and only that class:

| TU | why it is in the closure |
|---|---|
| `private_typeinfo.cpp` | `__dynamic_cast` + the three `__*_class_type_info` vtables; cannot compile under `-fno-rtti` |
| `stdlib_typeinfo.cpp` | `std::type_info`'s key function — `_ZTISt9type_info` is the base of every `__*_class_type_info` typeinfo, and `-fno-rtti` never emits it |
| `stdlib_exception.cpp` | `std::exception`'s key function — `_ZTISt9exception` is the base of the `bad_cast`/`bad_typeid` typeinfos `stdlib_typeinfo.o` emits once RTTI is on |

Everything else stays `-fno-exceptions -fno-rtti`; the failure path is
still `cxa_aux_runtime.cpp`'s no-exceptions `std::terminate()`.  The ICU
link probe (`third_party/icu-78.3/build-target.sh`) previously reported the
four as `ld.lld: error: undefined symbol: ...`; after the closure they
resolve (the probe's remaining gaps are the libm transcendentals, a
separate workstream).

Two latent archive defects surfaced while wiring this and are fixed in the
same change: `LIBCXX_BUILDING_LIBCXXABI` must be defined for the whole
libc++ class (upstream `HandleLibCXXABI.cmake` adds it as an interface
compile definition; only class A had it here), otherwise `exception.cpp`,
`typeinfo.cpp`, `new_handler.cpp` and `stdexcept_default.ipp` take their
"no ABI library" fallback branches and the archive carries duplicate strong
definitions of `~type_info` / `std::exception` / `bad_cast` etc. that
collide the moment one link pulls both copies (the RTTI closure does).
Also `stdlib_new_delete.cpp`'s `std::get/set_new_handler` pair: with the
define, libc++ defers to libc++abi's (`cxa_default_handlers.cpp`).

Evidence: `src/host/rtti_test.cpp` (host; the class-B sources compiled
natively, linked closed-world with `-nostdlib++`: up/down/cross/virtual
bases, nullptr paths, typeid, and the `dynamic_cast<T&>` abort path in a
forked child) and `RTTI_T.BIN` (device wave; same battery minus the
fork-abort check, `src/user/cxx_rtti_t.cpp`, compiled `-frtti`).

## libc gaps the port exposed (P3.2)

Each gap-fill is committed under `src/libc/` with a host-parity test under
`src/host/` that compiles the same source with `-DHOST_TEST` and races it
against glibc (`hb_*` renames where a name collides):

| gap | source | parity test |
|---|---|---|
| C99 `<math.h>` subset (IEEE-754 bit techniques, no FP ABI) | `src/libc/src/math.c` | `libc_num_parity_test.c` |
| `strtod`/`strtof`/`strtold` + `*_l` | `src/libc/src/strtod.c` | `libc_num_parity_test.c` |
| byte-mode wide-character surface (`mbrtowc`/`wcrtomb`/`wcs*`/`wmem*`) | `src/libc/src/wchar.c` | `libc_wide_parity_test.c` |
| `towlower`/`towupper` (ASCII-only C locale) + `wctype()` | `src/libc/src/wctype.c` | `libc_wide_parity_test.c` |
| `strftime` (C-locale, C99/POSIX + GNU padding) | `src/libc/src/strftime.c` | `libc_wide_parity_test.c` |
| locale object + POSIX-2008 `*_l` wrappers | `src/libc/src/xlocale.c` | `libc_wide_parity_test.c` |
| `vswprintf`/`swprintf`, `vsscanf`/`sscanf`, `vasprintf`/`asprintf` | `src/libc/src/{wchar,stdio}.c` | `libc_scanf_wprintf_test.c` |
| `getentropy()` | `src/libc/include/sys/random.h` | (used by `random_device`) |
| `strxfrm`/`strcoll`/`strerror_r` | `src/libc/src/string.c` | `libc_string_test.c` (extended) |

Semantics model glibc byte-for-byte; every non-obvious rule was probed on
the host first (strxfrm/wcsxfrm buffer semantics, swprintf truncation,
`%lc` raw-wide output, scanf's "0x needs a hex digit" / "exponent marker
needs digits" commit-and-fail rules).

## Risks / open questions

- RTTI is now available: the class-B closure above ships libc++abi's
  `__dynamic_cast` + `__*_class_type_info` machinery.  Exceptions stay off
  by design, so a failing `dynamic_cast<T&>` aborts (`__cxa_bad_cast` ->
  `std::terminate()`) and `throw`/`catch` remain unsupported.
- x86_64 long double is x87 (needs no builtins); aarch64 long double is
  binary128 and pulls the `R` class -- both arches were built.
- `<charconv>` floating point pulls LLVM-libc `shared/` headers; whether
  LLVM 21.2+ keeps that layout is unknown (pin accordingly).
- `libcxx/test/` is extracted but not exercised; the acceptance gate is
  `CXX_T.BIN` plus the wave.
