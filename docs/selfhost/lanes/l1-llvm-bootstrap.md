# Lane L1 — LLVM/clang bootstrap: vendor + cross-build recipe + configure spike

**You are lane L1 of the HobbyOS self-hosting program.** Your COMPLETE instructions are
this file. Read `~/hobbyos-selfhost/selfhost.md` §0, §2, §4, §5 (AD-1..AD-6), §7 — it is
the program plan; this brief is its L1 slice and binds you.

## Identity & tree

- **Worktree:** `/home/sarah/hobbyos-selfhost-lanes/l1` (branch `selfhost/l1-llvm-bootstrap`,
  based on `main` of the clone repo `/home/sarah/hobbyos-selfhost`).
- The clone's remote `ws` points at the WORKSTATION repo `/home/sarah/Documents/GitHub/HobbyOS`.
  **NEVER touch the workstation repo** (a second worker is active there). **No pushes**
  anywhere. Local commits on your branch only.
- Your worktree is pre-seeded: `third_party/libcxx-21.1.8/{llvm-project-21.1.8.src.tar.xz,
  src/, .fetched-ok}` are present (gitignored). Re-run
  `bash third_party/libcxx-21.1.8/fetch.sh` if you ever doubt it (it verifies sha + anchors).
- Skills to load first: `hobbyos-build-and-run` (build/run discipline), `lane-orchestration`
  (commit discipline contexts), and skim `hobbyos-port-planning` (house rules).

## Mission

Establish the **cross-build machinery and vendor tree** for the HobbyOS clang toolchain
(clang + lld + llvm-ar + llvm-objcopy, target `aarch64-none-elf`), from the **already
vendored** container — and run the **configure spike** that proves (or disproves) the
build-system approach, recording every blocker exactly. You are the critical path: L2,
the M2 lanes and the acceptance all build on your artifacts.

## The container (do not re-download if present)

`third_party/libcxx-21.1.8/llvm-project-21.1.8.src.tar.xz`
— sha256 `4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142`
(record lives in `third_party/libcxx-21.1.8/container.sha256`). It is the FULL monorepo
release (`llvm/`, `clang/`, `lld/`, `cmake/`, `libcxx/`, …).

## Deliverables (in order — commit after EACH)

### 1. Vendor scaffold `third_party/llvm-21.1.8/`

Model it on `third_party/libcxx-21.1.8/` (read its `fetch.sh`, `container.sha256`,
`anchors.sha256`, `README.md` first — same integrity model):

- `fetch.sh` — reuses the container from `third_party/libcxx-21.1.8/` when present and
  sha-matching (this is the intended path; do NOT duplicate 152 MB when the sibling
  exists), else downloads from the pinned release URL and verifies sha256 before use.
  Extracts subtrees: `llvm-project-21.1.8.src/{llvm, clang, lld, cmake, LICENSE.TXT}`
  into `src/llvm-project-21.1.8.src/`. Writes `.fetched-ok` marker; early-exits when
  marker + tree verify. Full extraction (no test-tree exclusion) unless it measures
  beyond ~3 GB — if you trim, trim only `*/test*/` trees and note it.
- `container.sha256` (copy the pin), `anchors.sha256` (pick 4-6 upstream anchor files you
  actually rely on — e.g. `llvm/CMakeLists.txt`, `clang/CMakeLists.txt`, `lld/CMakeLists.txt`,
  `llvm/docs/CrossCompiling.rst` — and sha256 them from the extracted tree),
  `README.md` (pin, provenance, what's extracted, how it's built, what will be installed).
- **The `.gitignore` block for `third_party/llvm-21.1.8/` is ALREADY on `main`** — check;
  if missing, add exactly the block from `selfhost.md` §4/AD-3 pattern (container, `src/`,
  `.fetched-ok`) and say so in your report. Do not touch any other ignore rules.
- **Run `fetch.sh` yourself** (extraction is the first real check) and record
  `du -sh` of the extracted tree in the design note. Commit: fetch.sh + container +
  anchors + README (extraction is gitignored — verify with `git status`).

### 2. Design note `docs/selfhost/llvm-bootstrap.md`

The complete cross-build recipe, grounded:

- **Reference reads (do them):** `/home/sarah/webkit-hobbyos/HobbyOS/toolchain-hobbyos.cmake`
  and `/home/sarah/webkit-hobbyos/HobbyOS/docs/toolchain-sysroot.md` (the in-house precedent
  for cross-building C++ for this sysroot), `third_party/libcxx-21.1.8/build-target.sh`
  (how libc++ was built for the OS), the repo `Makefile` flag blocks (lines 140-260), and
  the extracted `llvm/docs/CrossCompiling.rst`.
- **Toolchain file** `src/user/toolchain/cmake/toolchain-llvm-hobbyos.cmake` (port glue
  lives in `src/user/` per the program's publication rule): sets
  `CMAKE_SYSTEM_NAME=HobbyOS`, `CMAKE_SYSTEM_PROCESSOR=aarch64`,
  compiler = host `clang` (21.1.8), target flags `--target=aarch64-none-elf -ffreestanding
  -mcpu=cortex-a53 -nostdinc` + explicit `-isystem` list of the repo headers
  (`src/libc/include`, `src/include`, `src/user_include`, plus the builtin-header dir you
  stage), `-nostdlib`-style link recipes against the staged `libc.a` + `libcxx.a` +
  `libbuiltins` objects, `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`, and whatever else
  the precedent requires. You will iterate on this file; that's the point of the spike.
- **Staged sysroot**: build the repo's own artifacts the target build must link against —
  `make obj/arm/libc.a obj/arm/libcxx.a` (verify target names in the Makefile) — and stage
  `{include dirs, libc.a, libcxx.a}` into an obj-dir scratch sysroot (e.g.
  `obj/selfhost/sysroot/`; `obj/` is gitignored). Record exact paths + sizes.
- **CMake configure recipe** (proposed starting point — refine with evidence):
  `-G Ninja`, `-DCMAKE_BUILD_TYPE=Release`, `-DLLVM_TARGETS_TO_BUILD=AArch64`,
  `-DLLVM_ENABLE_PROJECTS="clang;lld"`, `-DLLVM_ENABLE_EH=OFF`, `-DLLVM_ENABLE_RTTI=OFF`,
  `-DBUILD_SHARED_LIBS=OFF`, `-DLLVM_ENABLE_ASSERTIONS=OFF`,
  `-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
  -DLLVM_INCLUDE_DOCS=OFF`, `-DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF
  -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_LIBPFM=OFF`, and a **minimal install set** via
  `-DLLVM_DISTRIBUTION_COMPONENTS="clang;lld;llvm-ar;llvm-objcopy;clang-resource-headers"`
  + `ninja install-distribution` (DESTDIR into a staging dir). Host tools: resolve the
  tablegen/try-compile mechanics per `CrossCompiling.rst` — most likely a small native
  build (`llvm-tblgen`, `clang-tblgen`) on the workstation and
  `-DLLVM_TABLEGEN=<path> -DCLANG_TABLEGEN=<path>`; document exactly what you chose and why.
- **Dependency surface appendix** (feeds L2): grep the extracted tree for the OS-facing
  call surface of the components we build and write `docs/selfhost/lanes/l1-dep-surface.txt`
  — one item per line: `function_name  <count>  <example file:line>`; start from
  `llvm/lib/Support/Unix/*.inc` (the POSIX shim), `llvm/lib/Support/Path.cpp|Program.cpp|
  Memory.cpp|Signals.cpp`, `clang/lib/Driver/*`, `lld/Common/*`, and the `HAVE_*` config
  headers (`llvm/include/llvm/Config/*.cmake`/`config.h.cmake`) to know which branches
  compile. This list is L2's input; make it complete enough to be auditable, not exhaustive
  to the last call site.
- **Sizes/estimates**: expected binary sizes (clang with AArch64-only, Release, stripped),
  build time estimate on the 64-core host, and the exact `ninja` targets for the wave-2
  full build.

### 3. Configure spike (empirical, bounded)

- Configure into `obj/selfhost/llvm-build/` (gitignored). **Log everything**:
  `docs/selfhost/lanes/l1-configure-1.log` (commit it). Record the first REAL errors
  verbatim (toolchain-file mistakes you fix are fine — a *bounded* number of iterations);
  do not patch LLVM sources — a needed source patch goes in the note as a blocker.
- If configure turns green: attempt the quickest real artifacts first in this order —
  `ninja llvm-ar llvm-objcopy` (proves target compile+link path end-to-end), then if the
  budget is healthy, kick the **full clang+lld build in the background** (`ninja clang lld`
  under `timeout`), keep writing up the note while it runs, and record whether it completed.
  Estimated full build: hours; if more than ~2h wall is likely, note the estimate and stop
  at a committed checkpoint — wave 2 continues the build in a fresh lane.
- Check the built binaries when present: `llvm-size`, `.bin` sizes as flat images
  (objcopy -O binary), and a `file`-level sanity list. Record numbers.

## Constraints (hard)

- Commit cadence ≈ every 30-45 min of work; subjects `selfhost: l1: <what>`. **Stop at a
  committed checkpoint** if the tool budget nears (write the report first).
- Never `make clean` (kills shared-host QEMUs). Never broad `pkill`; pattern-scoped kills
  only. No QEMU needed in this lane.
- Do not edit: `Makefile` (report hunks you need), `src/kernel/**`, `src/libc/**`,
  `selfhost.md`, `.gitignore` (beyond the L1 block if missing), `third_party/libcxx-21.1.8/**`
  (read-only), other lanes' paths.
- Disk is fine (318 GB free) but keep scratch under the worktree or
  `~/.hermes/cache/scratch/` — no `/tmp` marathons.

## Report (write to disk BEFORE finishing)

`docs/selfhost/lanes/l1-report.json` — commit it, and validate with
`python3 -m json.tool` before your final message. Schema:

```json
{"lane": "l1", "status": "complete|partial|blocked",
 "commits": ["sha"], "artifacts": ["path"],
 "evidence": ["path"], "blockers": ["exact error + where"],
 "next_steps": ["what wave 2 should do first"], "notes": "free text"}
```
