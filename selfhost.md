# Toolchain Self-Hosting Program — in-OS clang + build toolchain (selfhost.md)

Status: living document — **v1 (plan + pins + first lane wave dispatched), 2026-10-14**.
Executors: read §0 and §7 before touching anything; the §7.3 frozen interfaces are binding.

**Repo note.** This document lives in the WORK COPY of HobbyOS (`~/hobbyos-selfhost`,
clone of the workstation repo at `64093f6`, remote `ws` → `~/Documents/GitHub/HobbyOS`).
A second worker is active in the workstation repo. **Merge-back is explicitly deferred**
(§9 R7): program work happens only here, local commits only, no pushes anywhere.

---

## 0. TL;DR and how to use this document

Goal (user ask, 2026-10-14): port the **Clang C/C++ compilers and the other build tools
required to build the HobbyOS kernel image**, so that **HobbyOS compiles its own operating
system and user-mode programs from within itself, from the terminal shell**.

Acceptance (frozen, §8.3): inside HobbyOS (ARM), at the shell, the in-OS toolchain builds
the kernel + an acceptance user set from a staged source tree, assembles a bootable disk
image in-OS, and the host boots that image — the new kernel reaches its boot marker and an
in-OS-built user program runs. Evidence = raw serial logs (§8.4); proxies don't count.

The pick: **clang/LLVM 21.1.8** — the exact compiler the OS build already uses (host clang
is 21.1.8; libc++ 21.1.8 is already ported and gate-proven), one vendor tree covers
compiler + linker (lld) + archiver (llvm-ar) + objcopy, and the integrated assembler
removes any separate binutils port for aarch64. The sources are **already vendored**:
`third_party/libcxx-21.1.8/llvm-project-21.1.8.src.tar.xz` (sha256 `4633a23617fa…`,
full monorepo — contains `llvm/`, `clang/`, `lld/`).

Publication rule (user directive): for every ported application, **port source goes into
`src/user/`, pristine upstream goes into `third_party/`** — the repo's existing convention.

Mountains already climbed (why this is feasible now — all verified at this tip):
FPU both arches (F1.5); threads/futex/pthreads (P1); VM v2 + demand paging + memfd +
MAP_SHARED (P2); libc++/libc++abi static, no exceptions/RTTI (P3); socketpair/sendmsg/poll
(P4); fork/execve/waitpid/signals (P5); SQLite + POSIX fill-in (P6); FAT16 >1MiB write fix
(`3c781c5`) with 30 MB loader load verified; **VFAT long-file-name read+write already in
fat16.c**; shell with `-c`, redirections, pipelines, and `test`/`[`.

How to use: milestones §6; lanes §7 (briefs in `docs/selfhost/lanes/`); evidence rules §8.4;
fix log §11. Every fact in §3 is anchored — if any of it has drifted when you start,
**update §3 first** (regeneration commands: Appendix B).

---

## 1. Selection (the compiler/toolchain pick)

### 1.1 Requirements (from the ask, turned into filters)

1. **C AND C++ compilers** — the OS userland includes C++ (libc++, desktop, browser).
2. **Must build the kernel image** — i.e. reproduce the Makefile's flags exactly
   (`--target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 [-mgeneral-regs-only]`,
   plus the C++ policy flags).
3. **"Other build tools required"** — linker, archiver, objcopy; the build driver
   (make); and the disk-image tooling (mkfs.fat + mcopy/mmd class) because the kernel
   image ships on a FAT16 boot disk.
4. Runs as an in-OS **userland** program (no kernel residency; no new arch work).
5. Port source → `src/user/`; upstream → `third_party/` (user directive).

### 1.2 Survey

| Candidate | License | Size (order of magnitude) | Verdict |
|---|---|---|---|
| **clang/LLVM 21.1.8** | Apache-2.0 WITH LLVM-exception | ~6M LOC monorepo; we build a trimmed target set | **SELECTED** (§1.3) |
| tcc | LGPL-2.1 | ~40k LOC | **Rejected as primary / dormant contingency**: small and quick, but its codegen is not the build's compiler — it cannot faithfully "compile the OS with its own toolchain"; still needs a linker/archiver/objcopy story for this ELF+flatbin flow. Activation gate in §1.4. |
| chibicc | MIT | ~8k LOC | **Rejected:** C-only (fails the C++ requirement and the userland). |
| cproc+QBE | ISC / MIT | ~25k LOC | **Rejected:** C11 only, same reason; less proven at this scale. |
| GCC | GPL-3+ | ~15M+ LOC | **Rejected:** massively heavier port (own build system, libstdc++, GCC-specific runtime); the OS build is not GCC-based; no upside over clang. |
| glibc/musl mini-bootstrap stack | — | — | **Rejected:** solves a different problem (replacing a libc); HobbyOS has its own libc, POSIX-ified by posix.md + P1–P6. |

### 1.3 Why clang/LLVM

1. **It is the OS's own toolchain** (host: clang 21.1.8 + ld.lld + llvm-objcopy; the
   Makefile's flags are clang flags). Self-hosting with it = same semantics end to end.
2. **One vendor tree, four tools**: `clang` (driver + integrated cc1), `lld`, `llvm-ar`,
   `llvm-objcopy` — all from llvm-project. No second project to port for binutils.
3. **libc++ 21.1.8 already runs in-OS at WebKit scale** (P3 gate; WK-7 browser UI ships).
   The C++ runtime risk class is retired; exceptions/RTTI are off repo-wide, matching
   LLVM's official `LLVM_ENABLE_EH=OFF` / `LLVM_ENABLE_RTTI=OFF` build mode.
4. **Integrated assembler** — no GNU `as` port needed for aarch64.
5. **Cross-build precedent in-house**: the WebKit fork
   (`~/webkit-hobbyos/HobbyOS/toolchain-hobbyos.cmake` +
   `~/webkit-hobbyos/HobbyOS/docs/toolchain-sysroot.md`) cross-builds huge C++23 code
   for this exact sysroot — the LLVM build reuses that recipe pattern.
6. **The sources are already here**, sha-pinned and verified (same container as libc++).

### 1.4 Runners-up and revisit triggers

- **tcc — dormant, not scheduled, not vendored.** Activation gate: the M2 in-OS clang
  smoke is blocked by a runtime issue L2's audit did not predict and that cannot be fixed
  within one lane-week. If activated it is a bootstrap scaffold only; clang remains the
  goal and the acceptance definition does not change.

### 1.5 Licenses

LLVM/clang/lld: Apache-2.0 WITH LLVM-exception (static linking fine; `LICENSE.TXT`
vendored). GNU make, mtools (mcopy/mmd), dosfstools (mkfs.fat): GPL-3+ — compatible with
the repo's GNU-ports policy (`docs/gnu-ports.md` §1: GPL/AGPL sources only).

### 1.6 Non-goals (v1)

- No dynamic linking, no dlopen — everything static (repo policy).
- No rebuilding clang inside HobbyOS (bootstrap-of-clang) — v1 ships the cross-built
  toolchain and rebuilds **the OS** in-OS. (Stretch idea only.)
- x64 self-host acceptance is a **stretch** (M5); ARM is primary (repo rule).
- No merge-back to the workstation repo this phase (R7).
- No browser/WebKit work in this program (separate effort; browser.md owns it).

---

## 2. Pinned sources & provenance

| Source | Pin | sha256 / commit | Where |
|---|---|---|---|
| llvm-project full monorepo | `llvmorg-21.1.8`, container `llvm-project-21.1.8.src.tar.xz` | `4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142` (local verify 2026-10-14; original pin record in vendor fetch.sh, 2026-09-30) | **Already vendored** `third_party/libcxx-21.1.8/`. New vendor `third_party/llvm-21.1.8/` **reuses this container** (fetch.sh re-verifies the sha before extraction). |
| libc++ / libc++abi | same container | anchor manifest `anchors.sha256` | `third_party/libcxx-21.1.8/` — builds to `obj/<arch>/libcxx.a` (Makefile:905) |
| GNU make | **L4 pins** (latest stable; record at lane time) | L4 records URL + sha256 + tag→commit | `third_party/` at port time (just-in-time vendoring) |
| mtools (mcopy/mmd) | **L4 pins** | L4 | same |
| dosfstools (mkfs.fat) | **L4 pins** | L4 | same |

Provenance rules: exactly `docs/gnu-ports.md` — tarball + sha256 + tag→commit recorded,
tarball committed when the port starts; the vendor fetch.sh is the integrity gate.

---

## 3. Current-state snapshot (grounding — verified 2026-10-14 at `64093f6`)

**Syscalls.** Through 89: `src/include/syscall.h:176` (`SYS_MAX 89`). fork: `src/user_include/libc.h:72`;
execv/execve/execvp/execvpe: `libc.h:103-108`; getcwd/chdir: `libc.h:165-166`. Threads
(72–75), mmap family (62/63/81/82), memfd (80), poll (76), socketpair/sendmsg/recvmsg
(77–79), signals (83/84), spawn_ex (86) — all in tree (P1/P2/P4/P5 done, browser.md §6).

**Memory/loader.** `USER_IMG_SIZE 128 MiB` (`src/include/vm.h:44`); legacy
`MAX_PROGRAM_SIZE = USER_INITIAL_CLEAR_SIZE` = 1 MiB (`src/include/process.h:42`,
`src/kernel/program_loader.c:26`) — but the **v2 demand-paging loader is the routing
default** (browser.md §6 P2.5 S5) and has loaded 30 MB images (`image mapped,
bytes=30167040`, browser.md:1871). Re-verify caps at execution time.

**FPU.** Userland FP enabled both arches (Makefile:184-189, F1.5); kernel stays
`-mgeneral-regs-only` (Makefile:183).

**C++ runtime.** libc++/libc++abi 21.1.8 static, `-fno-exceptions -fno-rtti`; archive
target `obj/arm/libcxx.a` (Makefile:905); policy flags Makefile:200-231.

**FAT16.** >1 MiB write O(n²) root-caused + fixed (browser.md:1861; `3c781c5`;
`src/kernel/fat16.c:1525,1793`). **VFAT long names: read+write already implemented**
(`fat16.c:36-72` helpers/comment; parse `:412,:454`; writers `:569,:615,:622`; `read_dir`
`:985`; `unlink :1100`; `rename :1222`; `mkdir :1332`). L3 verifies build-payload
completeness — do not re-implement.

**Disk.** 1 GiB FAT16 startup disk (`disk.img` recipe; comment "Startup disk: 1 GiB";
`mkfs.fat -F 16`). Measured 2026-10-14 (workstation image): **174 files /
133,032,359 B used / 939,114,496 B free**.

**Shell.** `src/user/sh.c` (3369 lines): `-c` at `:3346`; redirections (`> >> < 2> 2>>`),
pipelines, builtins incl. `test`/`[` (`sh.c:13-15`).

**Build system (host).** Tools: `clang`, `ld.lld`, `llvm-objcopy`, `ar` (Linux branch
Makefile:28-32; macOS branch uses llvm-ar:20). Kernel flags: Makefile:183; user flags
Makefile:189; hermetic include pattern (`-nostdinc -isystem <clang resource>/include`)
Makefile:145-156. ASM: pattern rules Makefile:600/:610. Disk recipe: `dd` + `mkfs.fat
-F 16` + `mmd` dirs + `mcopy` files (~Makefile:1741+). libcxx archive: Makefile:905.

**Trees.** `src/` = 18 MB / 613 files; kernel = 71 C/asm sources. Host: clang 21.1.8,
64 cores, ≈318 GB free on `/`. CI VM `hobytest` (192.168.0.20, 64 cores) reachable for
long runs.

**QEMU.** ARM: 8 cores / 16 GB, TCG only on x86 hosts (slow — budget accordingly);
x64: 12 GB, KVM-capable (Makefile:167-197).

---

## 4. Gap analysis → work chains

| W | Gap | Work | Milestone |
|---|---|---|---|
| W-A | FAT16 LFN: implemented — **verify** against the in-OS build payload's exact needs (deep long-name trees from mcopy, long-name outputs, collisions) + tests | lane L3 | M1 |
| W-B | clang/lld/+tools runtime gaps in libc/syscalls (**unknown — enumerate**) | lane L2 audit → itemized fixes (single-writer syscall rounds) | M1→M2 |
| W-C | Toolchain cross-build (clang, lld, llvm-ar, llvm-objcopy for aarch64-none-elf/HobbyOS) | lane L1 vendor + recipe + configure spike; build iterations wave 2 | M0→M2 |
| W-D | Build driver: GNU make port + `Makefile.hobbyos` + sh gap fixes | lane L4 survey → make-port lane | M3 |
| W-E | Disk tooling: mkfs.fat + mcopy/mmd ports (+ `dd` replacement decision) | lane L4 survey → port lanes | M4 |
| W-F | Acceptance: payload staging, OUT.IMG assembly, host boot-verify loop, budgets | lane L5 design → harness lane | M1→M4 |
| W-G | Perf/hardening: big-image load time, integrated cc1, x64 stretch, docs | M5 | M5 |

---

## 5. Architecture decisions (binding; revisit criteria inline)

- **AD-1** Toolchain runs as plain userland `aarch64-none-elf` binaries built against the
  repo sysroot (`src/libc/include` + `src/include` + `src/user_include` + `obj/arm/libc.a`
  + `obj/arm/libcxx.a`). No kernel residency; no arch changes.
- **AD-2** In-OS clang uses the exact host flag sets (Makefile:183/:189/:215/:226/:231) and
  the integrated assembler; lld links; llvm-objcopy makes flat `.bin`s. Byte-parity with
  the host build is NOT a v1 gate (functional boot is).
- **AD-3** Vendor = `third_party/llvm-21.1.8/` (fetch.sh + container.sha256 +
  anchors.sha256 + README committed; extraction + container gitignored; container reused
  from the libcxx vendor). Port glue/scripts → `src/user/toolchain/` (user directive);
  `Makefile.hobbyos` at repo root. No source patches to LLVM expected in v1 (build-config
  only); if one is ever needed: two-build rule + named patch files listed in the vendor README.
- **AD-4** Installed layout on the OS disk (frozen §7.3): `/TOOLS/{CLANG.BIN, LLD.BIN,
  OBJCOPY.BIN, LLVMAR.BIN, MAKE.BIN, MKFSFAT.BIN, MCOPY.BIN, MMD.BIN}` +
  `/TOOLS/RESDIR/include` (curated clang builtin headers, 8.3-checked) + source payload
  under `/SRC/` + outputs `/SRC/obj`, `/SRC/OUT.IMG`.
- **AD-5** Builtin-header search: `Makefile.hobbyos` passes `-isystem /TOOLS/RESDIR/include`
  explicitly (the hermetic pattern of Makefile:145-156). Do NOT rely on clang's relative
  resource-dir discovery (no `/proc/self/exe`; driver resolves via argv0 — L1 verifies).
  The kernel build needs this too: kernel sources include `<stdint.h>` and get it from the
  resource dir today (Makefile:183 has no `-nostdinc`).
- **AD-6** cc1: prefer integrated cc1 (`-fintegrated-cc1`) if the 21.1.8 build supports it
  (host behavior: verify); else fork+execve-of-self (P5-complete path). Decision recorded
  by L1; per-TU cost measured at M2.
- **AD-7** Build driver v1 = **GNU make port** + `Makefile.hobbyos` (subset, frozen targets
  `check/user/kernel/disk/all`); `SHELL=/TOOLS/SH.BIN -c` (supported, sh.c:3346). Native
  `hbuild` driver = dormant contingency; activation gate: M3 slips >1 week.
- **AD-8** Disk output: in-OS `MKFSFAT` + `MCOPY` assemble `/SRC/OUT.IMG` (FAT16, ≤256 MiB;
  exact size fixed by L5 from the space math — 939 MB free today). Host verifies by booting
  OUT.IMG under QEMU (Limine chain). Second-raw-drive alternative = stretch.
- **AD-9** Acceptance user set v1 (provisional; freeze at M3): `HELLO`, `SH`, `LS`, `CAT`
  (+ `DESKTOP` if it links cheaply). Stretch: editor/files/everything.
- **AD-10** No exceptions/RTTI anywhere (repo policy; LLVM built `LLVM_ENABLE_EH=OFF`,
  `LLVM_ENABLE_RTTI=OFF`).
- **AD-11** The in-OS build tree is the REAL source tree (no name mangling, no shadow
  tree) — enabled by LFN support; this fidelity is the point. Payload = repo subtree copy.

---

## 6. Milestones (goal → tasks → gate → evidence)

Format as browser.md: checkboxes for execution sessions; gates run **on the merged tree**
by the integrator; a lane may not tick a gate it did not run.

### M0 — Groundwork, pins, freezes (this wave)

- [x] **T0.1 Copy + seed + smoke** — `~/hobbyos-selfhost` @ `64093f6`, remote `ws`;
  container seeded; libcxx `fetch.sh` verified (sha + anchors); `obj/arm/hello.bin` built;
  unit-tier validation run kicked (record result in §11).
- [ ] **T0.2 Plan + briefs + lane dispatch** — this doc + `docs/selfhost/lanes/*`
  committed; L1–L5 dispatched.
- [ ] **T0.3 L1 vendor scaffold** — `third_party/llvm-21.1.8/` fetch.sh re-runnable from
  committed files; extraction sizes recorded.
- [ ] **T0.4 L4 pin table** — make/mtools/dosfstools pins (URL+sha256+tag→commit) recorded.

**Gate M0:** `bash third_party/llvm-21.1.8/fetch.sh` green from a clean checkout state
(marker + anchors verify); plan + briefs committed; L1–L5 reports committed.
**Evidence:** lane logs under `docs/selfhost/lanes/`; `git log` on `main`.

### M1 — OS prerequisites verified + staging

- [ ] **T1.1 LFN build-payload verification** (L3) — deep long-name tree (mcopy-written,
  nested: e.g. `/src/kernel/program_loader.c`, `/src/user_include/...`) fully usable in-OS:
  open/read/read_dir/rename/mkdir/unlink; long-name outputs creatable; edge cases
  (collisions, deep paths). Add the permanent tests (unit + in-OS `LFN_T.BIN`-class).
- [ ] **T1.2 Staging tool** (L5 first cut → harness owner) — `tools/selfhost-stage.sh`
  (host): copies the payload (`/SRC` tree: `src/`, `Makefile.hobbyos`, minimal libcxx
  include set, `bootloader/BOOTAA64.EFI`, limine.conf template) into `disk.img` via mcopy;
  idempotent; size report.
- [ ] **T1.3 Large-image load test** — synthetic ~96 MB flat bin loads + runs in-OS via v2
  loader; load time recorded (feeds M2 sizing).
- [ ] **T1.4 L2 audit closed** — every MISSING/PARTIAL item scheduled (or documented
  no-action); single-writer fix commits for anything quick.

**Gate M1:** one in-OS run shows: long-name tree listing + deep-path reads + the 96 MB
load + staged payload present. **Evidence:** serial log + artifact sizes.

### M2 — The toolchain runs on HobbyOS (critical path)

- [ ] **T2.1 Cross-build** clang + lld + llvm-ar + llvm-objcopy (wave-2 lanes continue
  L1's recipe; sizes + build log committed).
- [ ] **T2.2 Install to `/TOOLS` + `/TOOLS/RESDIR`** (staging path; 8.3 checks; curated
  resource headers).
- [ ] **T2.3 In-OS smoke A:** `clang --version` runs.
- [ ] **T2.4 In-OS smoke B:** compile `hello.c` → `.o` in-OS (freestanding flags).
- [ ] **T2.5 In-OS smoke C:** lld-link + objcopy + **run `HELLO.BIN` in-OS**.
- [ ] **T2.6** Fix whatever L2 predicted that actually fires (single-writer rounds).

**Gate M2:** T2.3–T2.5 green in one boot transcript; installed tool sizes recorded.
**Evidence:** serial log; `llvm-size` numbers.

### M3 — Build driver in-OS

- [ ] **T3.1** make port runs in-OS (trivial Makefile: `sh -c` recipes, `test`/`[`).
- [ ] **T3.2** `Makefile.hobbyos` v1: `check` + `user` (acceptance set) green in-OS from
  `/SRC`.
- [ ] **T3.3** sh fixes L4's matrix proves necessary.

**Gate M3:** in-OS `make -f Makefile.hobbyos user` builds HELLO+SH from source; both run
in-OS. **Evidence:** serial log.

### M4 — THE ACCEPTANCE: self-built OS

- [ ] **T4.1** `make -f Makefile.hobbyos kernel` in-OS: all 71 kernel TU compile, link
  `hobbyos.elf`, objcopy `hobbyos.bin` — in-OS.
- [ ] **T4.2** `disk` target: MKFSFAT+MCOPY/build `OUT.IMG` (kernel + limine.conf +
  BOOTAA64.EFI + user set) in-OS.
- [ ] **T4.3** Host verify loop `run_selfhost_build_test.py`: boot HobbyOS, drive the
  build, extract OUT.IMG (`fat16img.py`), boot OUT.IMG, assert the kernel marker + run an
  in-OS-built program.

**Gate M4 (frozen acceptance):** T4.3 green end-to-end with raw logs; OUT.IMG's kernel is
linked in-OS from objects compiled in-OS. **Evidence:** full serial transcript + image
sha256 + stage markers.

### M5 — Hardening, stretch, records

- [ ] **T5.1** Perf pass: load-time, integrated-cc1 verification, page-cache reuse;
  numbers recorded.
- [ ] **T5.2** x64 stretch: x86_64-none-elf toolchain; x64 acceptance run (KVM).
- [ ] **T5.3** Docs final pass; §11 records; merge-back prep notes (instructions only).

---

## 7. Parallel execution plan

### 7.1 Lanes (wave 1 — dispatched 2026-10-14)

| Lane | Branch | Worktree | Scope | Key deliverables | Deps |
|---|---|---|---|---|---|
| **L1** | `selfhost/l1-llvm-bootstrap` | `~/hobbyos-selfhost-lanes/l1` | W-C vendor + cross-build recipe + configure spike | `third_party/llvm-21.1.8/**`; `docs/selfhost/llvm-bootstrap.md`; `docs/selfhost/lanes/l1-dep-surface.txt`; configure logs | — |
| **L2** | `selfhost/l2-os-audit` | `~/hobbyos-selfhost-lanes/l2` | W-B runtime audit vs sysroot | `docs/selfhost/os-gap-audit.md` + work items | reads llvm src (own scratch extract) |
| **L3** | `selfhost/l3-lfn-verify` | `~/hobbyos-selfhost-lanes/l3` | W-A LFN payload verification + tests + gap fixes | fat16 tests + in-OS LFN test + report | — |
| **L4** | `selfhost/l4-buildtools-audit` | `~/hobbyos-selfhost-lanes/l4` | W-D/W-E surveys + sh matrix | `docs/selfhost/buildtools-audit.md` + `Makefile.hobbyos` sketch + pins | — |
| **L5** | `selfhost/l5-acceptance-design` | `~/hobbyos-selfhost-lanes/l5` | W-F acceptance design + staging first cut | `docs/selfhost/acceptance.md`; `tools/selfhost-stage.sh` | — |

Briefs (binding): `docs/selfhost/lanes/l1-llvm-bootstrap.md` … `l5-acceptance-design.md`.
Merge model: **one lane = one branch + disjoint files** (§7.4); parent merges to `main`.

### 7.2 Waves (critical path)

- **Wave 1 (now):** L1–L5 fully parallel (disjoint file sets; L3 is the only Makefile
  writer this wave).
- **Wave 2 (after wave-1 reports):** clang full-build iterations (continuation of L1);
  libc/syscall fix batches (from L2 work items, single-writer); make-port lane; mtools +
  mkfs.fat port lanes; harness lane (`run_selfhost_build_test.py` + staging v2).
- **Wave 3:** M3/M4 integration rounds, fixups, records.
- Integrator = parent session: merges lanes to `main` here, runs merged-tip gates, is the
  sole writer of `selfhost.md`, `.gitignore`, syscall.h/both trap.c.

### 7.3 Frozen interfaces (binding)

| # | Interface | Value | Owner / change protocol |
|---|---|---|---|
| F1 | OS install layout | `/TOOLS/{CLANG.BIN, LLD.BIN, OBJCOPY.BIN, LLVMAR.BIN, MAKE.BIN, MKFSFAT.BIN, MCOPY.BIN, MMD.BIN}` + `/TOOLS/RESDIR/include` | parent; change = plan §11 note |
| F2 | Payload root | `/SRC/` = repo subtree (real names; no mangling) | L5→harness owner |
| F3 | In-OS make targets | `check`, `user`, `kernel`, `disk`, `all` | `Makefile.hobbyos` owner |
| F4 | Disk output | `/SRC/OUT.IMG`, FAT16, ≤256 MiB | same as F3 |
| F5 | Staging script | `tools/selfhost-stage.sh` | L5 → harness owner |
| F6 | Syscall additions | single writer (parent): `syscall.h` + BOTH `trap.c` in one commit | parent |
| F7 | Vendor dirs | `third_party/llvm-21.1.8/` (new, L1); `third_party/libcxx-21.1.8/` (existing, **read-only to everyone**) | L1 / nobody |
| F8 | 8.3 policy | every name installed to the OS disk must be 8.3-representable OR proven reachable via LFN (L3) — RESDIR names curated to 8.3 as belt-and-braces | parent + L3 |

### 7.4 File ownership map (wave 1)

- **L1:** `third_party/llvm-21.1.8/**`, `docs/selfhost/llvm-bootstrap.md`,
  `docs/selfhost/lanes/l1-*`.
- **L2:** `docs/selfhost/os-gap-audit.md`, `docs/selfhost/lanes/l2-*`.
- **L3:** `src/kernel/fat16.c`, `src/kernel/fat16_test.c`, new in-OS test file(s),
  `Makefile` (test wiring only — **sole Makefile writer this wave**), `docs/selfhost/lanes/l3-*`.
- **L4:** `docs/selfhost/buildtools-audit.md`, `third_party/_staging/**` (downloads),
  `docs/selfhost/lanes/l4-*`.
- **L5:** `docs/selfhost/acceptance.md`, `tools/selfhost-stage.sh`,
  `docs/selfhost/lanes/l5-*`.
- **Parent:** `selfhost.md`, `.gitignore`, `tools/selfhost-seed-worktree.sh`, merges,
  syscall files.

### 7.5 Session briefs

See `docs/selfhost/lanes/` — each brief is self-contained (owns/depends/first tasks/DoD/
evidence/skills/constraints/report schema).

### 7.6 Integration protocol (git)

- Worktrees under `~/hobbyos-selfhost-lanes/` (outside any synced dir). **Local commits
  only; no pushes anywhere; never touch `~/Documents/GitHub/HobbyOS`.**
- Commit subjects: `selfhost: <lane/area>: <what>`. Commit cadence ≈ every 30–45 min of
  work; **stop at a committed checkpoint** if the tool budget nears; the report JSON is
  written to disk and validated (`python3 -m json.tool`) before finishing.
- Parent merges each lane to `main` after reviewing the report + evidence (diff read, no
  blind merges); gates re-run on the merged tree; `.gitignore` conflicts resolve as unions.
- Merge-back to the workstation repo: **deferred** (R7) — prep notes only (M5.3).

---

## 8. Testing & acceptance strategy

### 8.1 Tiers

| Tier | Command | Where |
|---|---|---|
| host logic | `make host_tests` | workstation |
| kernel unit | `./run_unit_tests.sh` / `_intel` | QEMU EL1 |
| in-OS wave | `make test` (ARM; `-intel` secondary) | QEMU EL0 |
| acceptance | `run_selfhost_build_test.py` (M4) | QEMU + host driver |

### 8.2 New tests inventory

| Test | Kind | Where | Owner |
|---|---|---|---|
| LFN deep-tree (mcopy payload shape) | unit + in-OS | L3 delivers | L3 |
| ~96 MB image load + timing | in-OS | T1.3 | wave-2 |
| toolchain smokes (version/compile/link/run) | in-OS | T2.3–T2.5 | wave-2 |
| staging script self-check | host | tools/selfhost-stage.sh | L5 |
| `run_selfhost_build_test.py` E2E | host E2E | M4 | harness lane |

### 8.3 Acceptance checklist (v1 "done" = all checked; ARM)

- [ ] In-OS: kernel objects all compiled, `hobbyos.elf` linked, `hobbyos.bin` objcopy'd —
      transcript shows each phase.
- [ ] In-OS: `OUT.IMG` assembled by the in-OS toolchain; **host boots it successfully**.
- [ ] The booted `OUT.IMG` kernel was linked in-OS from in-OS-compiled objects; an
      in-OS-built user program runs on it.
- [ ] Full raw logs + artifact hashes archived (evidence dir + §11 records).
- [ ] x64: stretch only (M5) — not required for v1 done.

### 8.4 Evidence standards

Raw serial logs with explicit markers (`[SELFHOST] phase=…`); **no proxy signals** (exit
codes, frame counts, file existence alone); wave runs judged by completion marker
(substring `System halt`) + failure-token scan (plain `FAIL` and `FAILED`); sizes/hashes
for every artifact; every "verify" item re-grepped at execution time (facts drift — §3 is
a snapshot, not scripture).

---

## 9. Risks & mitigations

| # | Risk | Mitigation |
|---|---|---|
| R1 | clang cross-build drag (CMake for a custom triple, EH/RTTI off, static) | In-house precedent (`toolchain-hobbyos.cmake` + sysroot doc); L1's configure spike surfaces blockers early; trimmed target set (`LLVM_TARGETS_TO_BUILD=AArch64`); if the CMake path is hopeless, fall back to a hand-driven build recipe (decide at M0 close, recorded). |
| R2 | Runtime rejection: libc/syscall gaps discovered late | L2 audit → itemized, scheduled fixes; M2 smoke ORDER (version → compile → link → run) to fail fast; single-writer syscall rounds. |
| R3 | Big-binary load/run (tool binaries tens of MB) | T1.3 measures ~96 MB load; integrated cc1 (one load per compile); loader-speed work slotted M5; worst case: larger RAM, smaller link units. |
| R4 | In-OS disk space (payload + OUT.IMG vs 939 MB free) | L5 size math; OUT.IMG ≤256 MiB; payload trim list; second-drive stretch if ever needed. |
| R5 | TCG slowness (whole-kernel compile in-OS) | Acceptance = wall-time with generous budget; CI VM (`hobytest`, 64 cores) for long runs; x64-KVM stretch; parallelism later (make -j needs jobserver care). |
| R6 | LFN edge gaps corrupting payload access | L3 verification-first + permanent tests; host-side `mdir`/`mtype` cross-checks; FATBIG_T stays green (regression floor). |
| R7 | Two-worker merge conflicts | This is an independent clone; no pushes; merge-back planned as its own exercise later (fetch `ws`, quiet window, shared-file mapping documented then). |

---

## 10. Open questions (close by M1/M2)

1. Exact `OUT.IMG` size + content set (L5 + M3 freeze).
2. make vs hbuild final (L4 recommendation; default: make).
3. Curated `RESDIR` header list (L1: which builtin headers the kernel/user builds
   actually resolve; everything unused is dropped).
4. Integrated cc1 availability/effect in the 21.1.8 config (L1/L2; affects per-TU cost).
5. clang binary size after Release + strip (L1; feeds R3).
6. lld threading needs on HobbyOS (`--threads=1` fallback; L2).
7. x64 self-host scheduling (M5 stretch).

---

## Appendix A — Frozen interface drafts

- **/TOOLS layout** (F1): as in AD-4. `RESDIR` = curated subset of
  `<src>/clang/lib/Headers` + `<src>/llvm/lib/...` builtin headers, only those the
  freestanding builds resolve, names 8.3-checked (F8).
- **In-OS make target semantics** (F3): `check` = one-TU compile smoke; `user` = libc +
  acceptance set → `.bin`; `kernel` = all kernel objects → link → objcopy; `disk` =
  `OUT.IMG`; `all` = user + kernel + disk.
- **Staging tree shape** (F2): `/SRC/{Makefile.hobbyos, src/**, tools/**, bootloader/,
  third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include/** (C++ set only),
  limine.conf}`.

## Appendix B — Regeneration commands (§3 refresh)

```bash
grep -n "SYS_MAX" src/include/syscall.h
grep -rn "USER_IMG_SIZE\|USER_INITIAL_CLEAR_SIZE\|MAX_PROGRAM_SIZE" \
     src/include/vm.h src/include/process.h src/kernel/program_loader.c
sed -n '180,199p' Makefile                    # arm kernel/user flags
grep -n "libcxx.a:" Makefile
grep -n "lfn_\|0x0F\|read_dir" src/kernel/fat16.c | head -40
mdir -i disk.img -/ ::                        # used + free bytes
for t in clang ld.lld llvm-objcopy llvm-ar mcopy mmd mkfs.fat make; do command -v $t; done
```

## Appendix C — Vendoring & patch policy

- **third_party/llvm-21.1.8/** — container reused from `third_party/libcxx-21.1.8/`
  (sha verified) or re-downloaded from the pinned release URL; subtrees: `llvm/`,
  `clang/`, `lld/`, `cmake/`, `LICENSE.TXT`; extraction + container gitignored; anchor
  manifest re-verified by fetch.sh; **no LLVM source patches in v1** (build-config only);
  any future patch: two-build rule + `patches/` + README listing.
- **Port glue** (`src/user/toolchain/**`): build/install scripts + wrapper C programs —
  first-party, cstyle-clean, committed.
- **`Makefile.hobbyos`** — first-party; kept deliberately shell-conservative (L4 defines
  the recipe constraints from sh's proven matrix); divergences from the host Makefile
  documented in its header comment; sync responsibility = the make-port lane.

---

## 11. Fix log (append-only, newest first)

- **2026-10-14 — Program start.** Copy created (`~/hobbyos-selfhost` @ `64093f6`; remote
  `ws`; container seeded; libcxx fetch verified; `hello.bin` smoke built). Plan v1
  committed; lanes L1–L5 briefed + dispatched. Planning-time fleet discoveries:
  (a) fat16 **LFN read+write already in tree** (fat16.c:36-72/412/454/569-622/985) —
  L3 rescoped from "implement" to "verify + extend tests"; (b) fat16 >1 MiB defect
  already fixed (`3c781c5`); (c) the full llvm-project 21.1.8 container (llvm/clang/lld)
  is already vendored — no new source downloads needed for the critical path;
  (d) host clang == pin 21.1.8 (no version skew).
- Clone-validation unit-tier run: result recorded here when it lands (kicked 2026-10-14;
  see `docs/selfhost/lanes/` for the raw log if archived).
