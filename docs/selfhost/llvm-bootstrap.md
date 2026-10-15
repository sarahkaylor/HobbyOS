# LLVM/Clang cross-build recipe for the HobbyOS toolchain (lane L1)

Status: **wave-1 spike record** — vendor scaffold + toolchain file + configure
spike against the HobbyOS sysroot. Written 2026-10-14 by lane L1
(`docs/selfhost/lanes/l1-llvm-bootstrap.md`). Consumed by wave-2 build lanes.

## 1. Goal

Cross-build `clang`, `lld`, `llvm-ar`, `llvm-objcopy` for
`aarch64-none-elf` (the HobbyOS userland target) on the workstation, from the
pinned llvm-project 21.1.8 monorepo, so wave 2 can install them to
`/TOOLS/*.BIN` and boot the OS on the in-OS toolchain (selfhost.md M2).

Pins / ADs: selfhost.md §2 (container sha `4633a23617fa31a…`),
AD-1 (plain userland binaries vs the repo sysroot), AD-3 (vendor =
`third_party/llvm-21.1.8/`), AD-4 (install layout), AD-5 (hermetic builtin
headers), AD-6 (cc1), AD-10 (no exceptions/RTTI).

## 2. Reference reads

- WebKit-port toolchain precedent: `~/webkit-hobbyos/HobbyOS/toolchain-hobbyos.cmake`
  + `docs/toolchain-sysroot.md` — the in-house recipe for cross-building C++23
  against this sysroot. Reused: `CMAKE_SYSTEM_NAME=HobbyOS`,
  `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`, clang as both drivers,
  link closure in `CMAKE_CXX_STANDARD_LIBRARIES`, `-Wl,--start-group`.
- `third_party/libcxx-21.1.8/build-target.sh` — how libc++ 21.1.8 was built
  for the OS (flags mirror USER_CFLAGS; `-fno-exceptions -fno-rtti`).
- Repo `Makefile` lines 140-260 — per-arch flag blocks
  (`USER_CFLAGS` arm/intel).
- Upstream `clang/docs/CrossCompilation.rst` and
  `llvm/docs/HowToCrossCompileLLVM.rst` — single-stage cross build with an
  existing clang as host compiler.

## 3. Vendor scaffold (`third_party/llvm-21.1.8/`)

Committed (integrity model identical to `third_party/libcxx-21.1.8/`):
`fetch.sh`, `container.sha256`, `anchors.sha256`, `README.md`.
Container + extraction + `.fetched-ok` gitignored (block pre-seeded on main).

- Container **reused** from the libcxx vendor (no duplicate 152 MB download);
  `fetch.sh` re-verifies the sha before extraction.
- Extracted: `llvm/`, `clang/`, `lld/`, `cmake/`, `third-party/` (siphash —
  required via `LLVM_THIRD_PARTY_DIR`; discovered by the first native build
  attempt), `LICENSE.TXT`.
- Extraction: 1.7 GB / 107,622 files, full (no test-tree trim — under the
  ~3 GB threshold).
- `.fetched-ok` marker + anchor verification drive the early-exit.

## 4. Staged sysroot

Built the repo's own artifacts and staged a scratch sysroot (gitignored
`obj/`):

```
make obj/arm/libc.a obj/arm/libcxx.a            # Makefile:1011 / :905
mkdir -p obj/selfhost/sysroot/{include,lib}
cp -r src/libc/include  obj/selfhost/sysroot/include/libc-include
cp -r src/include       obj/selfhost/sysroot/include/kernel-include
cp -r src/user_include  obj/selfhost/sysroot/include/user-include
cp -r <clang-resource>/include  obj/selfhost/sysroot/include/builtins
cp obj/arm/{crt0.o,libc.a,libcxx.a}  obj/selfhost/sysroot/lib/
```

Sizes (2026-10-14): `crt0.o` 7.8 KB, `libc.a` 1.1 MB, `libcxx.a` 8.7 MB,
builtins include 15 MB, total sysroot 26 MB. `src/user/linker.ld` is used
directly from the repo (linker script, USER_IMG_BASE at 64 GiB).

## 5. Toolchain file

`src/user/toolchain/cmake/toolchain-llvm-hobbyos.cmake` (port glue lives in
`src/user/` per the publication rule; AD-3):

- `CMAKE_SYSTEM_NAME=HobbyOS`, `CMAKE_SYSTEM_PROCESSOR=aarch64`,
  triple `aarch64-none-elf`, compiler = host `clang` (21.1.8) for both C and C++.
- `-ffreestanding -mcpu=cortex-a53 -fuse-ld=lld`.
- Hermetic includes: `-nostdinc` + `-isystem <host clang resource>/include`
  (builtins staged under the sysroot for the OS-side install; the host
  resource dir is used at cross-build time) + `-isystem` of the three sysroot
  header trees.
- C++: `-nostdinc++` + `-I <libcxx have/include>` **as `-I`, not `-isystem`** —
  -isystem sorts after clang's builtin dir and libc++'s `<stddef.h>`
  self-check fails (verified in the spike).
- Link: `-nostdlib -nostartfiles -Wl,-T,src/user/linker.ld -Wl,-e,_start`;
  `CMAKE_CXX_STANDARD_LIBRARIES` = `-Wl,--start-group crt0.o libcxx.a libc.a
  -Wl,--end-group` (mutual libc.a↔libcxx.a refs, P3 closure).
- `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` (no target code may run at
  configure time).
- `LLVM_HOST_TRIPLE`/`CMAKE_{C,CXX}_COMPILER_TARGET` = `aarch64-none-elf`.

Spike probe (2026-10-14): a C++17 TU using `std::string` + `std::vector`
compiled and linked statically with these exact flags → aarch64 ELF
(**probe2.elf, text 127,752 / data 292 / bss 6,336**), proves the include
order, the sysroot, and the link closure all work before LLVM itself is fed
through. `main` must be `extern "C"` (freestanding clang mangles it — known
WK-1 issue, handled in the WebKit recipe).

## 6. CMake configure recipe (wave-2 starting point)

Native (host) tablegen first — the target binaries cannot run on the host
(no qemu-user for `aarch64-none-elf`), so `llvm-tblgen`/`clang-tblgen` are
host-built from the same tree:

```
cmake -G Ninja -S <src>/llvm -B obj/selfhost/tblgen-native \
  -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=AArch64 \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_CURL=OFF
ninja -C obj/selfhost/tblgen-native llvm-tblgen clang-tblgen
```

Cross configure:

```
cmake -G Ninja -S <src>/llvm -B obj/selfhost/llvm-build \
  -DCMAKE_TOOLCHAIN_FILE=<repo>/src/user/toolchain/cmake/toolchain-llvm-hobbyos.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=AArch64 \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_RTTI=OFF \
  -DBUILD_SHARED_LIBS=OFF -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_LIBPFM=OFF \
  -DLLVM_TABLEGEN=<repo>/obj/selfhost/tblgen-native/bin/llvm-tblgen \
  -DCLANG_TABLEGEN=<repo>/obj/selfhost/tblgen-native/bin/clang-tblgen \
  -DLLVM_NATIVE_TOOL_DIR=<repo>/obj/selfhost/tblgen-native/bin \
  -DLLVM_DISTRIBUTION_COMPONENTS="clang;lld;llvm-ar;llvm-objcopy;clang-resource-headers"
ninja -C obj/selfhost/llvm-build install-distribution   # with DESTDIR staging
```

Tablegen choice: `LLVM_NATIVE_TOOL_DIR` is the canonical mechanism
(`AddLLVM.cmake:2638`, `TableGen.cmake:181`); explicit `-DLLVM_TABLEGEN` /
`-DCLANG_TABLEGEN` are belt-and-braces and match the same native build.

### 6.1 Wave-2 actual commands (lane W2-LLVM — resume from the committed checkpoint)

Resumed from the L1 checkpoint (tip d48ada6; build dir already present, do NOT
re-configure from scratch — reconfigure in-place after toolchain-file edits,
3 s, warm cache):

```
# 0) Re-stage the sysroot FIRST when src headers changed after the stage was
#    made (the pid_t -> int fix, AD-12, landed AFTER L1 staged the sysroot;
#    the STALE copy kept `typedef long pid_t` -> Process.inc:64 assert again).
cp -a src/libc/include/. obj/selfhost/sysroot/include/libc-include/
cp -a src/include/.      obj/selfhost/sysroot/include/kernel-include/
cp -a src/user_include/. obj/selfhost/sysroot/include/user-include/
make obj/arm/libc.a obj/arm/crt0.o obj/arm/libcxx.a -j64   # pid_t ABI in libc.a
cp -a obj/arm/{crt0.o,libc.a,libcxx.a} obj/selfhost/sysroot/lib/

# 1) llvm-ar + llvm-objcopy (was blocked at 211/564 TUs on the pid_t assert)
ninja -C obj/selfhost/llvm-build llvm-ar llvm-objcopy -j64
#    fix: sysroot re-stage above.  Link-only gap then: -lpthreads -lrt -ldl
#    -lm (compile-only probes set HAVE_LIBDL/HAVE_LIBRT even without bodies).
#    -> empty stub archives + -L (toolchain file; see blockers).
#    Then WK-1: mangled C++ main (_Z4mainiPPc) vs crt0's C `main` -> weak
#    main bridge in gaps.c (WebKit-fork mainlink_shim.c precedent).

# 2) clang + lld (long phase; keep a log, resume with the same dir)
ninja -C obj/selfhost/llvm-build clang lld -j64
#    New blockers, all sysroot/build-config class (NO LLVM source patch):
#    - <fstream> compiled out of staged libc++21 by _LIBCPP_HAS_FILESYSTEM 0
#      (clang Frontend needs std::ifstream: CompilerInvocation.cpp:4678 &
#      co).  Toolchain file overrides the STAGED (gitignored generated)
#      include/__config_site to 1 + compiles sysroot-headers/fstream_inst.cpp
#      (3 <char> explicit instantiations) into the closure.  Tracked
#      config/__config_site untouched (L2 decision).
#    - fseeko/ftello (POSIX-2008, needed by <fstream>) -> _missing.h decls +
#      gaps.c bodies.
#    - lldMachO needs libunwind "mach-o/compact_unwind_encoding.h":
#      fetch.sh SUBTREES += llvm-project-.../libunwind/include; extract it
#      into the EXISTING tree:
#      tar -xJf third_party/llvm-21.1.8/llvm-project-21.1.8.src.tar.xz -C \
#        third_party/llvm-21.1.8/src/llvm-project-21.1.8.src \
#        --strip-components=1 llvm-project-21.1.8.src/libunwind/include
#    - clang-21 link: dlopen/dlsym/dlclose/dlerror, getrlimit/setrlimit,
#      math (exp/log10/log2/log2f/log1p/logb/acos/cosh/sinh/tanh/erf) ->
#      gaps.c bodies; setjmp/longjmp (mangled because OS setjmp.h lacks
#      extern "C") -> _missing.h extern-"C" decls + sysroot-headers/setjmp.S
#      (kernel-derived aarch64 asm) + closure wiring.
#    - "section: .tdata is not contiguous with other relro sections" ->
#      -Wl,-z,norelro (HobbyOS loader has no PT_GNU_RELRO/page-protect).

# 3) install-distribution with DESTDIR staging (the F1 /TOOLS layout)
DESTDIR=<repo>/obj/selfhost/dist-staging ninja -C obj/selfhost/llvm-build \
    install-distribution -j8
#    staged: usr/local/bin/{clang-21,clang*,lld,ld.lld,wasm-ld,llvm-ar,
#    llvm-objcopy} + usr/local/lib/clang/21/include (272 headers, 15 MB).
#    F8: 159/272 resdir names are >8.3 -> M2 curation item (§10).

Sizes (2026-10-14, unstripped Release ELF, aarch64): clang-21 120 MB,
lld 69 MB, llvm-ar 13 MB, llvm-objcopy 9.6 MB; staging total 227 MB.
These are the on-disk numbers to beat at M2 stripping/curation.

### 6.2 F8 decision: 8.3 curation is NOT required — LFN carries the RESDIR (lane W3-Tools, 2026-10-14)

**Question (F8 / llvm-bootstrap §10):** 205/239 top-level (221/272 incl.
wrapper-subdir files) RESDIR basenames violate 8.3 — must they be curated
to a mapped 8.3 tree (`tools/curate_resdir.py`) before /TOOLS staging?

**Decision: stage the tree FAITHFULLY with long names. No mapped/curated
tree is built. No `tools/curate_resdir.py` was created (not needed).**
`/TOOLS/RESDIR/include` is a byte-exact copy of the install-distribution
`lib/clang/21/include` tree (272 files, 14.7 MB, incl. the six wrapper
subdirs); every header name survives verbatim.

Evidence (all in-tree, re-verified this lane):

1. **LFN open-by-path is the compiler's only lookup, and it is proven.**
   clang's `-isystem` include resolution is a path-based `open()`; the HobbyOS
   FAT16 driver matches VFAT long names on open over the full 255-char name
   space (`fat16.c` `lfn_*`/`lfn_matches`; no component cap — the 31-char
   limit applies ONLY to `read_dir` *display* names).  Wave-1 lane L3's merged
   in-OS `LFN_T.BIN` opens the **36-char** `llvm-project-21.1.8.src-EXAMPLE.c`
   by long name and reads its content back (`src/user/lfn_test.c:100`), the
   worst-case class of the RESDIR's own longest basename
   (`__clang_cuda_math_forward_declares.h`, also 36 chars) — and 21.1.8's
   `install-distribution` RESDIR is exactly the tree previously staged with
   2322 LFN payload files by the harness (L5/§2).
2. **`read_dir` capping at 31 chars is listing-only and cosmetically
   truncates 5 RESDIR basenames** (32–36 chars: 4 at the include root +
   `openmp_wrappers/__clang_openmp_device_functions.h`).  Nothing in the
   S0–S5 acceptance *lists* the RESDIR: the compiler's include path is
   `-isystem /TOOLS/RESDIR/include` (AD-5), and make/cc1 never glob it.  If a
   later milestone copies the RESDIR in-OS via `read_dir`, this is the one
   item to revisit (AD-13 watch list: "revisit only if a staged/output name
   exceeds 31" — exactly these 5).
3. **Host-side staging fidelity verified this lane:** `selfhost-stage.sh
   --tools` staged the full RESDIR + 4 ELF tools to a scratch FAT16 image;
   `mtype`+sha256 of the 36-char names, the `openmp_wrappers` subtree and
   `CLANG.BIN` (126,060,320 B) are byte-identical to the staging sources
   (transcript: `docs/selfhost/lanes/w3-tools-*.md` / report §evidence).
4. In-OS `-print-resource-dir` / compile-a-TU *proof* is an M2 item (needs
   the M2 flat-.bin loader step, T2.3–T2.5); the *mounted-path* proof —
   resource dir staged at its frozen path, byte-faithful, and the L3-proven
   in-OS long-name open — is complete here.  M2's `-isystem
   /TOOLS/RESDIR/include` smoke (S0) is pre-validated by this staging.

Consequence for the /TOOLS recipe (wave-3 lane W3-Tools): the startup image
carries the RESDIR **uncurated** at `/TOOLS/RESDIR/include`; `T2.2` reduces
to "stage + verify mdir", and the hypothetical curation work (8.3-truncated
names + rewritten `#include` lines + collision disambiguation for the 19
8.3-truncation collision families) is dropped.  A `curate_resdir.py` is
still a useful fallback ONLY if an in-OS tool is ever found listing the
RESDIR — documented here so nobody re-derives the analysis.

## 7. Configure spike results

See `docs/selfhost/lanes/l1-configure-1.log` (committed; use `git add -f` —
`*.log` is globally gitignored).

**Outcome: configure GREEN** (exit 0, build.ninja written) against the
staged sysroot with the L1 option set. The cross-build build-system approach
is proven at the configure level; the first real blockers were logged
verbatim and iterated in bounded rounds (spike runs 1-18):

| run | blocker (verbatim) | resolution |
|---|---|---|
| 1 | `${CXXFLAGS}` carried `-isystem …;-isystem …` (CMake LIST `;` into ninja) → `/bin/sh: 1: -isystem: not found` | `list(JOIN … " ")` in toolchain file (WebKit-fork trap) |
| 1 | sysroot path resolved one level too high (`/l1/src/obj/…`) | toolchain file repo-root = 4 levels up |
| 2 | `HandleLLVMOptions.cmake:245` `Unable to determine platform` (CMake 4.x sets `UNIX` only in `Platform/<os>-Initialize` modules) | committed `Platform/HobbyOS.cmake` module (UNIX=1; `modules/Platform/`) |
| 4 | `LINKER_TYPE 'LLD' is unknown … by this toolchain` | drop `CMAKE_LINKER_TYPE LLD` (driver `-fuse-ld=lld` suffices) |
| build | `llvm/ADT/bit.h:53` `machine/endian.h not found` (freestanding triple) | compat `sysroot-headers/machine/endian.h` |
| build | `ExitCodes.h:30` `Exit code EX_IOERR not available` (no `<sysexits.h>`) | compat `sysroot-headers/sysexits.h` (+`HAVE_SYSEXITS_H` probe via fresh dir) |
| build | `dlfcn.h`, `sys/resource.h`, `sys/statvfs.h`, `sys/utsname.h` not found; `Dl_info`, `SA_ONSTACK`/`SA_NODEFER`, `_POSIX_ARG_MAX`, `PATH_MAX`, `MNT_LOCAL` undeclared | compat headers in `sysroot-headers/` |
| build | missing POSIX function *declarations* compiled into LLVMSupport unconditionally (alarm, gethostname, getsid, fork, setsid, wait4, pthread_sigmask, _Exit, strsignal, socket/bind/listen/accept/connect/pipe, fchown/fchmod/mkdir/symlink/link, getrusage, uname, statvfs, dladdr) | `sysroot-headers/_missing.h` (force-included via `-include`, build-config only) |
| build | same list's **bodies** absent from HobbyOS libc | `sysroot-headers/gaps.c` stub object in the link closure (WebKit-fork gaps.c precedent); every stub is a documented L2 work item |
| build | `Process.inc:64` `static assertion failed: sizeof(int) >= sizeof(long): Process::Pid should be big enough to store pid_t` | **NOT resolvable in-lane**: HobbyOS `sys/types.h` defines `typedef long pid_t` (8 B on aarch64) vs LLVM `Process::Pid = int32_t` (Process.h:46); POSIX/Linux uses `int`. No LLVM source patch, no src/libc edit (lane rules). **Recorded as the spike's first real blocker — L2 decides: change HobbyOS pid_t to int (Linux-parity) or carry an LLVM build patch.** |

The spike stops at this committed checkpoint (211/564 TUs compiled;
LLVMSupport is blocked on the pid_t assert). Every earlier blocker was a
toolchain-file or sysroot-gap fix in-lane; none needed an LLVM source patch
(AD-3 holds). Wave 2 resumes from here after L2 resolves pid_t (and lands
the B-section libc functions, which the gaps stubs currently satisfy only
for the toolchain's own link closure).

## 8. Dependency surface (feeds L2)

`docs/selfhost/lanes/l1-dep-surface.txt` — the OS-facing call surface of
`llvm/lib/Support/Unix/*.inc`, `llvm/lib/Support/{Path,Program,Memory,Signals}.cpp`,
`clang/lib/Driver/*.cpp`, `lld/Common/*.cpp`.

- **A. Calls declared in the HobbyOS headers: 53** (strlen 20, close 16,
  getenv 10, strcmp 9, free 9, realpath 8, open/read/memset/signal/sigaction
  5 each, …) — these must link from the staged sysroot.
- **B. POSIX/glibc calls NOT declared by the HobbyOS headers (link
  blockers): 6**:
  - `alarm` (5; `Unix/Program.inc:406`)
  - `getrlimit` / `setrlimit` (3 each; `Unix/Process.inc:140/160`)
  - `syscall` (3; `Unix/Signals.inc:426`)
  - `ioctl` (1; `Unix/Process.inc:325`)
  - `strsignal` (1; `Unix/Program.inc:492`)

  These make the L2 work-item list concrete. The build stops linking at the
  first open-archive member needing one; stubbing (return `-1`/`ENOSYS`
  where legal) vs real syscalls is L2's call.
- **C. HAVE_* config surface**: with the L1 option set, configure compiles
  the generic POSIX path; `llvm/include/llvm/Config/*.cmake.in` +
  `llvm/cmake/config-ix.cmake` gate the branches.

Regeneration: the generator is embedded in the lane report; re-run against a
re-extracted tree after any LLVM version pin change.

## 9. Sizes / estimates (wave-2 full build)

- Host: 64 cores, 251 GB RAM, ~299 GB free. Native tablegen-only build =
  minutes. Full `clang`+`lld` cross build ≈ **hours** (LLVM Release with only
  the AArch64 backend; ~6M LOC monorepo). Wave 2 continues in a fresh lane if
  this lane's budget runs dry — the recipe above is the handoff.
- Expected installed sizes (guesses to be replaced by `llvm-size`/`du` at
  M2): `clang` ≈ 70-100 MB static ELF, `lld` ≈ 30-50 MB, `llvm-ar`/`llvm-objcopy`
  ≈ 5-15 MB each, all stripped + flat `.bin`'d (`objcopy -O binary`) for the
  FAT16 install. `clang-resource-headers` ≈ 10-20 MB (curated to 8.3-safe
  names, F8).
- Wave-2 exact targets: `ninja clang lld llvm-ar llvm-objcopy` then
  `install-distribution`.

## 10. Known open items (wave 2 / L2)

- `main` mangling under `-ffreestanding` (applies to LLVM's own tools'
  `main` — same WK-1 fix shape if the linker objects).
- Whether clang's resource-dir *relative* discovery works in-OS (AD-5 says
  do not rely on it; `-isystem /TOOLS/RESDIR/include` explicitly).
- Builtin-header minimum set for `--target=aarch64-none-elf` must be curated
  (8.3 names, F8) at M2.
