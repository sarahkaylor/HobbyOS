# ICU 78.3 usage on HobbyOS — wiring record + data strategy

Lane `browser/l3-icu-wire`, 2026-10-01. This note records (1) what was wired
to make the ICU target cross-build usable from HobbyOS code, (2) the
before/after reproduction of the C-mode header clash, and (3) the deferred
plan for `libicudata.a` — **documented, not implemented** (the task's
explicit scope). Background: `third_party/icu-78.3/README.md` (pin, host
build, trimming) and `cross-notes.md` (cross-build recipe, flag rationale,
gap list).

## What is wired

1. **Sysroot C11 `<uchar.h>`** (`src/libc/include/uchar.h`). ICU's
   `common/unicode/ptypes.h` does `#include <uchar.h>` in C mode
   (`platform.h` hardcodes `U_HAVE_CHAR16_T 1` for conformant C, non-Darwin),
   so this header is what makes `#include <unicode/utypes.h>` — and every
   ICU C API header — compilable from a target C TU. The guard is
   `HOBBYOS_UCHAR_H`; on `HOST_TEST` it `include_next`s the host header.
2. **Makefile rules** (additive block after the libcxx.a rule): a manifest
   rule that runs `bash third_party/icu-78.3/build-target.sh --arch $(ARCH)`
   (self-healing: fetch → host tools → target configure/make → nm spot-check
   → link probe → `obj/$(ARCH)/icu/MANIFEST.txt`), plus compile/link rules
   for the smoke. `disk.img` gains `$(ICU_SMOKE_BIN)` and an `mcopy` to
   `::/ICUSMK.BIN`.
3. **`ICUSMK.BIN`** (`src/user/icu_smoke.c`, wave registration in
   `src/kernel/main.c`, last entry): a data-free EL0 acceptance —
   `u_strlen`, `U8_NEXT`/`U8_APPEND_UNSAFE`/`U8_LENGTH`/`U16_NEXT`,
   ASCII `u_tolower`, `u_errorName`, `u_getVersion` — linked against
   `libicuuc.a` + `libcxx.a` + `libc.a` with `-ffunction-sections` /
   `-Wl,--gc-sections`. **No `libicudata.a`**: the image is 260,088 B on
   ARM (llvm-size: text 259,412 / data 656 / bss 11,428) against the 1 MB
   user-image loader cap (`program_loader.c MAX_PROGRAM_SIZE` ==
   `USER_INITIAL_CLEAR_SIZE`), and `icudt78_dat` is absent from the
   symbol table. `libicui18n.a` is also not linked (deferred with the
   Intl work).

## The C-mode header clash (reproduced before the fix)

`#include <unicode/utypes.h>` from a target C TU failed with **20 errors**:

```
ptypes.h:60:17: error: 'uchar.h' file not found with <angled> include; use "quotes" instead
  ...then: ucpmap.h:12:1: error: unknown type name 'U_CDECL_BEGIN' (and 18 more)
```

Two mechanisms, both verified with a minimal TU compiled under the repo's
target flags (`-nostdinc -isystem <clang-resource>/include`, `-std=c11`,
`-I src/libc/include ...`):

- **Same-directory fallback.** clang reports the missing angled include, but
  still *processes* `common/unicode/uchar.h` found next to `ptypes.h` — ICU's
  OWN uchar.h, which includes `ucpmap.h`/`utypes.h` recursively before
  `umachine.h` has defined `U_CDECL_BEGIN`/`U_CAPI`. Hence the cascade.
- **Guard collision (the trap when adding the header).** ICU's
  `common/unicode/uchar.h` uses `UCHAR_H` as its include guard. A sysroot
  `<uchar.h>` naively guarded `UCHAR_H` (file-name-derived) makes the two
  headers silently suppress each other: `utypes.h` → ptypes → sysroot
  uchar.h defines `UCHAR_H`; the later `#include <unicode/uchar.h>` is then
  skipped whole and every `u_charType`/`U_UNASSIGNED` use fails with
  "call to undeclared function". This is the recorded clash; the committed
  header uses `HOBBYOS_UCHAR_H` (same failure class as the old signal.h
  `_SIGNAL_H` wrapper bug). Verified both include orders now compile clean;
  C++ consumers are untouched (ptypes.h's include is C-only).

Re-verification one-liner (after any sysroot change):

```sh
clang --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 \
  -nostdinc -isystem "$(clang -print-resource-dir)/include" -O2 -std=c11 \
  -Isrc/libc/include -Isrc/include -Isrc/user_include \
  -Ithird_party/icu-78.3/src/source/common -Ithird_party/icu-78.3/src/source/i18n \
  -c <a C TU with #include <unicode/utypes.h>> -o /dev/null   # rc 0; was 20 errors
```

## Data strategy — `icudt78.dat`, later (NOT implemented here)

The trimmed `libicudata.a` is **10,195,700 B** (single member
`icudt78l_dat.o`; trimmed to root+en, no brkitr). It cannot ride in a user
image: `MAX_PROGRAM_SIZE` is 1 MB and the loader reads the flat `.bin`
wholesale into the 32 MB process region. Linking it would put ~10 MB of
relocated `.rodata` in every consumer — not viable.

**Chosen direction for later work: load the data blob from disk at runtime
and hand it to ICU via `udata_setCommonData()`.** Sketch:

- Rebuild the data with `--with-data-packaging=archive` (a sibling of
  `build-target.sh` mode, not implemented) so `icudt78l.dat` (~10 MB, same
  trimmed filter) is produced next to the static archives; `mcopy` it to
  `::/ICUDT78L.DAT` (8.3-safe: `ICUDT78L` is exactly 8 chars).
- In a consumer, read the whole file into a static/malloc'd buffer
  (`open`/`read` from the sysroot) and call `udata_setCommonData(buf, &err)`
  once, before any data-using API. This bypasses ICU's own file I/O
  entirely — the bare-metal target has no `ICU_DATA` directory machinery
  and we do not want to port ICU's `uprv` file layer.
- Budgets: 10 MB in the 32 MB process region is fine; the 64 MB FAT16 disk
  holds it; boot-time read is tens of ms from VirtIO. The user-image cap
  stays untouched.
- Until then, only the data-free subset is usable from HobbyOS code — the
  case-mapping tables are compiled into `ucase.ao`
  (`common/ucase_props_data.h`), which is why `u_tolower` works without the
  blob; `u_charType`, property queries, `u_strToUTF8` (via normalization
  data), break iteration, collation etc. all need the data and stay
  deferred. `ICUSMK.BIN` is the acceptance boundary of the shipped subset.

Open items recorded (not scheduled): whether the browser loader should
pre-map the blob read-only instead of copying; whether the data should be
subset further (conversion tables) against the F0 flash budget; and the
still-open Intl-side gaps (`libicui18n.a` is built but not yet consumed —
it needs the data blob for every API).
