# LLVM/Clang 21.1.8 vendor (selfhost.md §2/AD-3; lane L1)

Cross-build source for the HobbyOS in-OS toolchain (clang + lld + llvm-ar +
llvm-objcopy, target `aarch64-none-elf`), vendored per the same recipe as
`third_party/libcxx-21.1.8/` (fetch.sh + committed manifest + gitignored
extracted tree).

## Pin and integrity

| | |
|---|---|
| upstream | `https://github.com/llvm/llvm-project/releases` |
| tag | `llvmorg-21.1.8` (release tag; matches the workstation clang 21.1.8 and the P3 libc++ pin) |
| container | `llvm-project-21.1.8.src.tar.xz` (FULL monorepo release) |
| container sha256 | `4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142` (`container.sha256`) |
| content gate | `anchors.sha256` — sha256 of six untouched upstream files, re-verified on every `fetch.sh` run |
| sha pin verified | 2026-10-14 (lane L1; reused the libcxx vendor's already-verified container) |

The container tarball (152 MB) is **gitignored**; `fetch.sh` reuses the
libcxx vendor's copy when present and sha-matching (`third_party/libcxx-21.1.8/llvm-project-21.1.8.src.tar.xz`),
else downloads from the pinned release URL. `src/llvm-project-21.1.8.src/`
(1.7 GB extracted) is gitignored too; `fetch.sh` re-extracts and re-verifies
the anchors, then writes `.fetched-ok`.

## Extracted subtrees (what the toolchain build needs)

| subtree | size | why |
|---|---|---|
| `llvm/` | 1.3 GB | LLVM core: Support, Object, TableGen, llvm-ar, llvm-objcopy, lld-host pieces |
| `clang/` | 430 MB | the C/C++ compiler driver + frontend + clang resource headers |
| `lld/` | 24 MB | the ELF/COFF/Mach-O linker (ld.lld) |
| `cmake/` | 76 KB | upstream CMake glue (Modules, config templates) |
| `LICENSE.TXT` | 16 KB | Apache-2.0 WITH LLVM-exception (static linking fine) |

Full extraction — no `*/test*/` trees trimmed (measured 1.7 GB vs the ~3 GB
trim threshold; all test trees kept to keep future subtrees re-extractable
without re-fetching).

## Build configuration (cross; DESTDIR-staged)

Configure with CMake + Ninja against the HobbyOS sysroot, host clang as the
cross-compiler (`--target=aarch64-none-elf`), host tablegen:

- `-G Ninja -DCMAKE_BUILD_TYPE=Release`
- `-DCMAKE_TOOLCHAIN_FILE=<repo>/src/user/toolchain/cmake/toolchain-llvm-hobbyos.cmake`
- `-DLLVM_TARGETS_TO_BUILD=AArch64` (only the AArch64 backend; the OS is ARM-primary)
- `-DLLVM_ENABLE_PROJECTS="clang;lld"`
- `-DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_RTTI=OFF` (AD-10: no exceptions/RTTI)
- `-DBUILD_SHARED_LIBS=OFF -DLLVM_ENABLE_ASSERTIONS=OFF`
- `-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF`
- `-DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_LIBPFM=OFF`
- `-DLLVM_DISTRIBUTION_COMPONENTS="clang;lld;llvm-ar;llvm-objcopy;clang-resource-headers"`
  + `ninja install-distribution` with a DESTDIR staging dir.

Full build estimates are in `docs/selfhost/llvm-bootstrap.md` §sizes.

## What will be installed (OS disk layout, frozen F1)

`/TOOLS/{CLANG.BIN, LLD.BIN, OBJCOPY.BIN, LLVMAR.BIN}` + `/TOOLS/RESDIR/include`
(curated clang builtin headers, 8.3-checked) — see selfhost.md §5 AD-4/AD-5.

## Why vendored

There is no clang/lld for `--target=aarch64-none-elf` on the workstation and
no in-OS compiler yet; the self-hosting program cross-builds the toolchain
from the pinned sources exactly as the OS build already uses clang 21.1.8
(the Makefile's flags are clang flags — same semantics end to end, §1.3).

## Anchors

`anchors.sha256` pins `LICENSE.TXT`, `llvm/CMakeLists.txt`,
`clang/CMakeLists.txt`, `lld/CMakeLists.txt`,
`llvm/include/llvm/Support/thread.h`, `clang/docs/CrossCompilation.rst`.
