# HobbyOS WebKit port — toolchain file + sysroot layout (OQ-10, WK-1)

Written WK-1 (2026-10-02) by L8. This is the OQ-10 deliverable: the draft
toolchain file (`HobbyOS/toolchain-hobbyos.cmake` in this fork) and the
recorded sysroot layout the parent can land as
`src/user/browser/webkit/toolchain-hobbyos.cmake` in the OS repo.

## Sysroot layout (HobbyOS repo; read-only from this fork)

All paths relative to the HobbyOS repo root (`/home/sarah/Documents/GitHub/HobbyOS`).

| Item | Path | Notes |
|---|---|---|
| C headers | `src/libc/include`, `src/include`, `src/user_include` | `-I` in that order (repo USER_CFLAGS convention) |
| C++ headers | `third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include` | used with `-nostdinc++`; vendor tree is gitignored, fetched by `fetch.sh` |
| clang resource | `$(clang -print-resource-dir)/include` | x64 uses `-nostdinc -isystem <resource>`; ARM relies on the bare-metal triple's default |
| libc.a | `obj/<arch>/libc.a` | contains compiler-rt builtins (obj/<arch>/builtins), so `-nostdlib` links work |
| libc++ | `obj/<arch>/libcxx.a` | built `-fno-exceptions -fno-rtti`; l3-rtti lane added the `__cxa` closure members ICU's `-frtti` TUs need |
| crt0 | `obj/<arch>/crt0.o` | `_start`; walks `.init_array` (F2.4) |
| ICU | `obj/<arch>/icu/{libicuuc,libicui18n,libicudata}.a` | 78.3, static data packaging; `libicuuc.a` has `U icudt78_dat` (satisfied by libicudata.a, no whole-archive) |
| ICU headers | `third_party/icu-78.3/src/source/{common,i18n}/unicode` | merged into one `obj/icu-include/unicode/` view by the toolchain (0 basename collisions) |
| linker script | `src/user/linker.ld` | v2 image base `0x1000000000` (64 GiB); `.tls_meta` word carries `__tls_align` |

## Toolchain shape (HobbyOS/toolchain-hobbyos.cmake)

- `CMAKE_SYSTEM_NAME=HobbyOS`, `CMAKE_SYSTEM_PROCESSOR=aarch64|x86_64` per
  `HOBBYOS_ARCH=arm|intel`; triples `aarch64-none-elf` / `x86_64-none-elf`.
- **clang is both C and C++ driver** (this workstation has no clang++).
- `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` — no target code may run at
  configure time (H-9).
- Compile flags mirror the OS repo `USER_CFLAGS` per arch (+ `-mcmodel=large
  -mno-red-zone` for x86_64, `-mcpu=cortex-a53` for ARM).
- Link: `CMAKE_EXE_LINKER_FLAGS = -nostdlib -nostartfiles -Wl,-T,<linker.ld>
  -Wl,-e,_start`; `CMAKE_CXX_STANDARD_LIBRARIES =
  -Wl,--start-group crt0.o libcxx.a libc.a -Wl,--end-group` (lands at the END
  of the link line — resolves the mutual libc.a↔libcxx.a references).
- ICU character: `ICU_INCLUDE_DIR` + `ICU_{UC,I18N,DATA}_LIBRARY_RELEASE` cache
  vars (FindICU synthesizes the non-`_RELEASE` vars itself);
  `ICU_VERSION=78.3`.

## Configure invocation (M1 evidence)

```
cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<fork>/HobbyOS/toolchain-hobbyos.cmake \
  -DPORT=HobbyOS -DHOBBYOS_ARCH=arm -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_STATIC_JSC=ON -DUSE_SYSTEM_MALLOC=ON <fork>
```

Exit 0, writes build.ninja. JIT-off/C_LOOP/static/system-malloc come from the
port defaults in `OptionsHobbyOS.cmake` (no per-invocation flags needed).

## Known configure-graph traps (WK-1 record)

- `find_package(Threads)` passes compile-only (pthread.h present in sysroot);
  `CMAKE_THREAD_LIBS_INIT` ends up empty — pthread symbols come from libc.a
  (no libpthread exists). `Threads::Threads` in WTF_LIBRARIES then emits no
  link flags; a `-lpthread` would fail.
- `Source/ThirdParty/skia/CMakeLists.txt:5` demands Freetype 2.9 — skia must
  stay out of the WK-1 graph (USE_SKIA=OFF, D-9).
- unifdef is compiled for the TARGET but never run by the JSC-only graph
  (GLibMacros-only consumer); leave as-is, revisit at WK-2.
- The JSC offlineasm extractors (LLIntSettingsExtractor/OffsetsExtractor) are
  linked as target executables and consumed as DATA blobs by the Ruby scripts
  (`offsets.rb` reads magic numbers from the binary bytes) — they never RUN
  on the host, so no qemu-user bridge is needed.
