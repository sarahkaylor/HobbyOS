# Toolchain Self-Hosting Program — in-OS clang + build toolchain (selfhost.md)

Status: living document — **v1.1 (wave 1 complete; all five lanes merged; merged-tip battery running), 2026-10-14**. Wave-1 results: §11 + `docs/selfhost/lanes/`; wave-2 roster: §7.2.
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

**Trees.** `src/` = 18 MB / 613 files; kernel = 59 ARM TUs (71 C/asm sources incl. the x64 branch; L4 count). Host: clang 21.1.8,
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
- **AD-8** Disk output: in-OS `MKFSFAT` + `MCOPY` assemble `/SRC/OUT.IMG` — **frozen:
  128 MiB** (L5 sizing from the space math; real-disk staging measured 2.02× cluster
  slack; `-C` + explicit size; `dd` replaced by ftruncate-only creation; byte-match bar =
  BPB + both FATs + root entries, not host-identical images). Host verifies by booting
  OUT.IMG under QEMU (Limine chain). Second-raw-drive alternative = stretch.
- **AD-9** Acceptance user set v1 (provisional; freeze at M3): `HELLO`, `SH`, `LS`, `CAT`
  (+ `DESKTOP` if it links cheaply; L5: the staging tool's `STAGE_LIBCXX` default flips at
  freeze). Stretch: editor/files/everything.
- **AD-10** No exceptions/RTTI anywhere (repo policy; LLVM built `LLVM_ENABLE_EH=OFF`,
  `LLVM_ENABLE_RTTI=OFF`).
- **AD-11** The in-OS build tree is the REAL source tree (no name mangling, no shadow
  tree) — enabled by LFN support; this fidelity is the point. Payload = repo subtree copy.
- **AD-12** `pid_t` is **int32** (Linux/glibc parity; `src/libc/include/sys/types.h`).
  Forced by LLVM 21.1.8's `Process::Pid` static_assert; the kernel's byte-wise
  `struct flock` already stores `l_pid` as a 32-bit value at offset 24 (`K_FLOCK_SIZE`
  stays 32). Landed 2026-10-14 (`4a30ca4`).
- **AD-13** The `read_dir` syscall's 31-char name cap STAYS (frozen ABI; payload basenames
  ≤26 chars today). Revisit only if a staged/output name exceeds 31 (L3 note).

---

## 6. Milestones (goal → tasks → gate → evidence)

Format as browser.md: checkboxes for execution sessions; gates run **on the merged tree**
by the integrator; a lane may not tick a gate it did not run.

### M0 — Groundwork, pins, freezes (this wave)

- [x] **T0.1 Copy + seed + smoke** — `~/hobbyos-selfhost` @ `64093f6`, remote `ws`;
  container seeded; libcxx `fetch.sh` verified (sha + anchors); `obj/arm/hello.bin` built;
  unit-tier validation run kicked (record result in §11).
- [x] **T0.2 Plan + briefs + lane dispatch** — this doc + `docs/selfhost/lanes/*`
  committed; L1–L5 dispatched (2026-10-14).
- [x] **T0.3 L1 vendor scaffold** — `third_party/llvm-21.1.8/` fetch.sh re-runnable from
  committed files; extraction 1.7 GB; anchors + early-exit verified by re-run (L1).
- [x] **T0.4 L4 pin table** — make 4.4.1 / mtools 4.0.49 / dosfstools 4.2 (URL + sha256 +
  tag→commit recorded; mtools' upstream is SVN — tarball sha256 + published SHA-1 is the
  integrity gate).

**Gate M0:** `bash third_party/llvm-21.1.8/fetch.sh` green from a clean checkout state
(marker + anchors verify); plan + briefs committed; L1–L5 reports committed.
**Evidence:** lane logs under `docs/selfhost/lanes/`; `git log` on `main`.

### M1 — OS prerequisites verified + staging

- [x] **T1.1 LFN build-payload verification** (L3; merged `5a9967b`) — five real gaps found
  and fixed, failing-test-first: G1 read_dir long names; G2 mkdir writes 0x0F runs;
  G3 rename rewrites runs; G4 cross-sector unlink; G5 read_dir accumulator across
  sector/cluster boundaries. `LFN_T.BIN` 25 checks + 7 unit tests; unit 309/0; ARM wave
  zero-FAIL; pristine attribution in `docs/selfhost/lanes/l3-*`.
- [x] **T1.2 Staging tool** (L5 first cut, merged; harness owner refines) —
  `tools/selfhost-stage.sh` tested on a scratch image AND a real 1 GiB copy (mdir arrival +
  content spot-checks); payload = 2322 files / 30,108,339 B; 1362/2322 basenames exceed 8.3
  (informational; LFN carries them).
- [ ] **T1.3 Large-image load test** — synthetic ~96 MB flat bin loads + runs in-OS via v2
  loader; load time recorded (feeds M2 sizing).
- [x] **T1.4 L2 audit closed** (merged `06d5b44`) — 66 FULL / 20 PARTIAL / 16 MISSING /
  12 NO-ACTION; work items W2-1..W2-15; load-bearers = argv envelope 256 B/32 args,
  temp dir, M1 headers; cc1 runs IN-PROCESS (compile half needs no fork/exec).

**Gate M1:** one in-OS run shows: long-name tree listing + deep-path reads + the 96 MB
load + staged payload present. **Evidence:** serial log + artifact sizes.

### M2 — The toolchain runs on HobbyOS (critical path)

- [x] **T2.1 Cross-build** clang + lld + llvm-ar + llvm-objcopy — COMPLETE (wave 2;
  relinked against merged libc post-merge: clang-21 126 MB, lld 72.6 MB, llvm-ar 13.6 MB,
  llvm-objcopy 10.0 MB; zero LLVM source patches, AD-3 held).
- [x] **T2.2 Install to `/TOOLS` + `/TOOLS/RESDIR`** — COMPLETE (wave 3: 12 entries /
  223 MB on the 1 GiB image; LFN carries the resdir — 8.3 curation NOT required (§6.2,
  decided with byte-fidelity evidence); in-OS MAKE.BIN exec proof on the staged image).
- [x] **T2.3 In-OS smoke A:** `clang --version` runs. — COMPLETE (w5-flat pipeline staging +
  w5-nothrow closure fix; full 21.1.8 banner from the staged pipeline, rc=0).
- [x] **T2.4 In-OS smoke B:** compile `hello.c` → `.o` in-OS (freestanding flags). —
  COMPLETE (w5-nothrow: `phase=w5-compile-done rc=0`; HELLO.O 2272 B valid AArch64 REL,
  pulled off the image + host-validated).
- [x] **T2.5 In-OS smoke C:** lld-link + objcopy + **run `HELLO.BIN` in-OS**. — COMPLETE
  (w6-run: in-OS-built HELLO.BIN ran in-OS, `[HELLO] entering main`, `phase=t25-run-done
  rc=0`; LLD fix-forward `-flavor gnu`). **Gate M2: T2.3–T2.5 all green** (banner w5-flat;
  compile w5-nothrow; link+run w6-run) — the combined transcript roll-up is part of w7's
  acceptance driver.
- [ ] **T2.6** Fix whatever L2 predicted that actually fires (single-writer rounds).

**Gate M2:** T2.3–T2.5 green in one boot transcript; installed tool sizes recorded.
**Evidence:** serial log; `llvm-size` numbers.

### M3 — Build driver in-OS

- [ ] **T3.1** make port runs in-OS (trivial Makefile: `sh -c` recipes, `test`/`[`).
  Partial: MAKE.BIN loads + executes in-OS; w5-src's S0/S1 dry-runs show make parsing
  `Makefile.hobbyos` + issuing compile lines; the recipe-shell exec bug (missing-image
  exec after AS teardown) is FIXED by w5-kernel (vii). Full merged-tip S0/S1 = w6-run.
- [x] **T3.2** `Makefile.hobbyos` v1: `check` + `user` (acceptance set) green in-OS from
  `/SRC`. — COMPLETE (w6-run: S0 + S1 green; HELLO.BIN byte-identical; pattern-rule/
  argv/lld-flavor fix-forwards).
- [x] **T3.3** sh fixes L4's matrix proves necessary. (w3-shell: redirect fd-close fix +
  file-based byte assertions; w6's in-OS runs exercised make→sh recipes.)

**Gate M3:** in-OS `make -f Makefile.hobbyos user` builds HELLO+SH from source; both run
in-OS. **Evidence:** serial log.

### M4 — THE ACCEPTANCE: self-built OS

- [x] **T4.1** `make -f Makefile.hobbyos kernel` in-OS: all 59 ARM kernel TU compile, link
  `hobbyos.elf`, objcopy `hobbyos.bin` — in-OS. — COMPLETE (w6-run; 1,372,912 B elf /
  219,564 B bin; ≈1.5–2 h under TCG).
- [x] **T4.2** `disk` target: MKFSFAT+MCOPY/build `OUT.IMG` (kernel + limine.conf +
  BOOTAA64.EFI + user set) in-OS. — COMPLETE (w6-run; 128 MiB OUT.IMG in 365 s; layout
  mdir-verified; sha256 `7e24b4ca…`).
- [ ] **T4.3** Host verify loop `run_selfhost_build_test.py`: boot HobbyOS, drive the
  build, extract OUT.IMG (`fat16img.py`), boot OUT.IMG, assert the kernel marker + run an
  in-OS-built program. Pre-tested (wave 3, hand-built OUT.IMG): extraction byte-identical
  (sha256), extracted image boots to kernel marker (6.5 s), program launches from the
  OUT.IMG Apps menu; kernel-less case classifies `stage_failure`. OUT.IMG must-contain
  list = `acceptance.md` §8 (limine/AAVMF layout + `/DESKTOP.BIN` MANDATORY + CONSOLE/SH +
  8.3-named S1 program).

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
- **Wave 2 (at the merged tip `4a30ca4`):** (a) LLVM build continuation — resume
  `ninja llvm-ar llvm-objcopy` → `ninja clang lld` → `install-distribution`
  (recipe: `docs/selfhost/llvm-bootstrap.md` §6; sizes recorded); (b) libc/syscall M1
  batch — L2 W2-1..W2-10 (headers/shims, argv envelope, temp dir; syscall surfaces
  single-writer = parent); (c) make-port lane; (d) mtools-port lane; (e) mkfs-port lane;
  (f) harness lane — `SELFHOST.BIN` + `run_selfhost_build_test.py`
  (`docs/selfhost/acceptance.md` §5); (g) merged-tip + CI-VM batteries (running; §11).
  **STATUS: COMPLETE — all seven lanes merged on `main` (`0c272a4`; see §11 entry);
  per-lane reports at `docs/selfhost/lanes/w2-*-report.json`; wave-2 merged-tip
  batteries (local + CI VM) running.**

**Gate etiquette (hard rule, learned wave 2).** One QEMU per tree at a time;
`fuser -k disk.img` may only run when NOTHING of yours is using the image — firing it
while your own background run is in flight kills YOUR run (observed: a lane fuser-killed
its own mid-suite ARM wave and misread it as a regression; memory was fine). Serialize:
wait for the current run's log to show its final line, then start the next.
- **Wave 3 (roster, carried from lane reports + merges):** (a) kernel write-path gap for
  ftruncate-extended FAT16 files (in-OS MKFSFAT `-C` writes; deterministic repro in
  `w2-mkfs-report.json`); (b) sysroot `dirent` (opendir/readdir) — unblocks make
  wildcards/VPATH + `mcopy -s`; (c) sh external redirect `cmd ARG > FILE` fix
  (w2-make report; blocks file-based byte assertions); (d) socket API reconciliation
  (legacy `connect` vs POSIX / `raw_socket_stream` reachability); (e) `MODE=selfhost`
  kernel boot mode + serial-console-fd stdout (acceptance 5.2/5.3); (f) RESDIR 159>8.3
  name curation + `/TOOLS` staging of the toolchain (M2 sizes); (g) `shell_test`
  echo-token drain (QoL); (h) M3 driver milestones once the toolchain is bootable.
  **STATUS: COMPLETE — all four merged (`994116a`; §11 entry).** Gap-A outcome: the
  “extend-write gap” was a sub-minimum probe size (1 MiB < FAT16 floor), NOT a kernel bug;
  dirent landed; sh redirect fixed; token family drained; /TOOLS staged (in-OS MAKE.BIN
  exec); S4/S5 pre-tested. Socket reconciliation (d) and `MODE=selfhost` (e) rolled to
  wave 4.
- **Wave 4: COMPLETE — all three merged (`82d4d81`; §11 entry).** MODE=selfhost delivered
  (serial-evidenced); FAT16 O(n²)→linear (128 MiB MKFSFAT 284 s); Makefile.hobbyos v1 +
  /SRC staging + S0/S1 dry-runs (blocked at the 1 MiB exec gate); clang-in-OS walls
  root-caused (raw-ELF staging; `_Znwm` link config; prefetch PROT race).
- **Wave 5 (roster — the unblockers):** (a) w5-flat: stage-time `objcopy -O binary` flat
  conversion of the /TOOLS toolchain + re-stage + in-OS `clang --version` from the staging
  pipeline (T2.3); (b) w5-kernel: exec gate (`MAX_PROGRAM_SIZE` vs v2 128 MiB cap),
  fault-killed-child rc=0 (process_fault_exit), pid=5 housekeeping fault, L9-prefetch PROT
  instruction-fetch race, spawn-arg 255-char cap; (c) w5-nothrow: clang `_Znwm` inside
  `__lcxx_override` link config → flat clang completes a compile (T2.4); (d) after a+b+c:
  the real S0/S1 dry-run (first true in-OS compile) then S2/S3; (e) watchdog-stall hardening
  (shell_test); (f) socket reconciliation; (g) image budget/stripping.
  **DISPATCHED 2026-10-14 (`deleg_6283bc5f`, 3 lanes from `92815fa`):** w5-flat = (a);
  w5-kernel = (b); w5-nothrow = (c), working in the warm l1 worktree (branch
  `selfhost/w5-nothrow`). After these land: the real S0/S1 in-OS run, then S2/S3.
- **Wave 5: COMPLETE — all three merged (`42c6c0b`; §11 entry).** Flat pipeline conversion
  proven in-OS; all 7 kernel blockers fixed (318/0 + 322/0 + wave green); **T2.3 + T2.4
  green (clang compiles in-OS)**.
- **Wave 6 (roster):** (a) w6-run: merged-tip end-to-end drive — S0 `check` → S1 `user` →
  T2.5 link+run → S2 kernel attempt (bounded), fix-forward; plus the `src/libc/src/
  cxxrt.cpp` latent-fix follow-up; (b) after S2: S3 OUT.IMG in-OS + S4/S5; (c) watchdog
  hardening; (d) socket reconciliation; (e) budget/stripping.
  **DISPATCHED 2026-10-14 (`deleg_326875fa`, 1 lane from `42c6c0b`):** w6-run carries (a)
  in full (fixes `cxxrt.cpp`, drives S0→S2, closes T2.5). → **COMPLETE + merged
  (`288995c`, §11).**
  **DISPATCHED 2026-10-14 (`deleg_36d019f2`, 1 lane from `288995c`):** w7-accept — the
  acceptance: (A) boot the w6-produced OUT.IMG (kernel marker + in-OS-built program run),
  (B) the full T4.3 loop (complete fresh in-OS build → extract OUT.IMG → boot it → run),
  (C) acceptance.md reconciliation (TCG reality + runbook + §8.3 verdict table).
- **Wave 6.5 (roster):** (a) w8-torture: root-cause + fix the VM-only tip-scoped `TORTURE
  exec-wait-cycles` red (§11); (b) then the normal battery re-verify of the fix.
  **DISPATCHED 2026-10-14 (`deleg_689f3db2`, 1 lane from `288995c`):** w8-torture.
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

1. ~~OUT.IMG exact size~~ **CLOSED: 128 MiB** (L5; content set freezes at M3 with the
   `STAGE_LIBCXX` flip).
2. ~~make vs hbuild~~ **CLOSED: make** (port breakdown in `docs/selfhost/buildtools-audit.md`).
3. Curated `RESDIR` header list (wave 2, with `install-distribution`; keep only the builtin
   headers the freestanding builds actually resolve).
4. ~~integrated cc1~~ **CLOSED: cc1 runs in-process by default** (L2; `Job.cpp`
   `CC1Command::Execute` spawns nothing).
5. clang binary size — probe closure GREEN (127,752 B `.text`); final size at full build
   (wave 2; feeds R3).
6. ~~lld threading~~ **CLOSED: `hardware_concurrency()` (≤16) over the existing pthreads is
   sufficient** (L2).
7. x64 self-host scheduling (M5 stretch).
8. Temp-dir strategy in-OS (L2 W2-9: disk dir vs `TMPDIR` env; prerequisite of the first
   in-OS clang smoke).
9. `raw_socket_stream` closure: are the socket calls truly unreachable in the shipped tool
   set? (L2; ENOSYS stubs acceptable if so.)

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

- **2026-10-15 (~01:45) — Battery round 10 on the merged tip (`9beedc4`): LOCAL cleanest
  yet; VM green with one classified drop.** Local: host/unit-arm/unit-x64/wave ALL rc=0 —
  **single WATCHDOG token only** (no IPC_T, no LFN, no CMPTEST); TORTURE PASS + `exec_ok=8`.
  VM (`merged-w11`): host rc=0 57 s / unit-arm rc=0 / unit-x64 rc=0; test-arm 440 s, 1
  token (WATCHDOG) BUT **`TORTURE.BIN` was dropped at BOOT LOAD — `Loader (v2) starved
  (slot pressure): TORTURE.BIN`, never retried → suite silently absent** (the documented
  boot-load-bail class: no caller to retry a caller_pid<0 load; no FAIL token, wave still
  halted). 8 suite summaries + STRESS PASS otherwise. Attribution: ambient pressure class
  (todo §5); a clean VM test-arm sample on this tip exists from `merged-w10r` (`exec_ok=8`).
  w12-rcut (final acceptance receipt) in flight.

- **2026-10-15 (~00:05) — Wave 8c (w9-dirent) COMPLETE + merged (`9beedc4`): THE S2 CROWN
  PASSES — the in-OS kernel stage compiles 59/59, links and objcopies on the fixed kernel;
  zero twins.** Two-sided root cause: **(writer)** clang writes objects via an LLVM TempFile
  whose fd stays open across `rename(temp->final)` and is closed after; the rename frees the
  temp's slots and the clean entry often reuses the freed slot, then the later CLOSE's dirty
  `fat16_sync_entry` wrote the stale temp dirent back over it — twin resurrected + clean
  entry clobbered (the deterministic nfs_test.o/cpu.o loss). Fix: `fat16_sync_entry` reads
  the on-disk slot; skip when freed (0x00/0xE5) or the name changed. **(reader, exposed by
  crown #1)** `net_rdma.o`/`net_rdma_test.o` share the 8.3 alias; the resolver returned
  first-dir-order (the small test object) -> `undefined symbol: net_rdma_fast_write`. Fix:
  prefer an LFN-free exact-8.3 entry over a colliding long-name alias. Failing-first:
  `test_fat16_clang_temp_rename_no_twins` (319/1 on tip -> pass) + collision test. Gates:
  **320/0 ARM + 324/0 x64**. In-OS proofs: probe zero twins; **S2 crown `phase=s2-kernel-done
  rc=0` + `ACCEPTANCE-COMPLETE` (4321 s); `hobbyos.elf` 1,371,192 B + `hobbyos.bin`
  218,036 B (sha `8126b413...`); exactly 59 objects, zero twins.** Battery 10 launched on
  `9beedc4`. NEXT: the FINAL acceptance re-cut (full S0->S3 + extract + boot on the merged
  lineage with incremental + -j8). **DISPATCHED (`deleg_8d010caf`, 1 lane from `9beedc4`): w12-rcut** — the culminating receipt run.

- **2026-10-14 (~23:20) — w9-dirent S2 crown attempt #1: PROGRESS + remaining wall.** The
  dirent fix eliminated the `cannot open` class — lld now WALKS all 59 in-OS objects (new
  territory) and fails at symbol level: `undefined symbol: net_rdma_fast_write` (ref.
  net.c:447 via /SRC/obj/arm/net.o). Integrator forensics on the lane's disk.img: staged
  net_rdma.c byte-perfect; mtype-extracted `net_rdma.o` = a valid ELF with `.text` size 0
  (near-empty twin) while mdir shows a 10,776-B entry and the host object (10,624 B, 18
  syms) has the symbol → hypothesis: duplicate/broken twin still created for some objects;
  lld opens an EMPTY twin → clean undefined-symbol instead of parse error. Steered to the
  lane (dirent-per-entry dump + rewrite-path re-examination + re-crown). This supersedes the
  earlier two-missing-objects failure mode.

- **2026-10-14 (~22:40) — Battery round 9 + post-merge hotfix (`8aa7b7c`).** LOCAL: host
  rc=2 — **w11's new `src/libc/include/sys/select.h` shadowed glibc's in HOST_TEST builds**
  (glibc <sys/types.h> -> our select.h -> signal/unistd chain -> `intptr_t` unknown); fixed
  (`#ifdef HOST_TEST` -> `#include_next <sys/select.h>`), then local host_tests **482/0**
  green. unit-arm rc=0, unit-x64 rc=0, wave rc=0 (tokens: 1x `IPC_T received fd...` inside
  a starved-spawn window WITH `no free slot` forensics = documented pressure family; 2x
  WATCHDOG; TORTURE PASS `exec_ok=8`). VM (`merged-w10`): host rc=2 same select.h cause
  (6 s); units rc=0; test-arm 3 tokens = `[LFN] FAIL rename: new long name listed...` +
  `[LFN] FAILED: 0x1` + WATCHDOG; rerun `merged-w10r` = **watchdog only, LFN did NOT
  reproduce** (flare; same "listed with long name" class as the pre-w6 sighting; re-sample
  after w9 — dirent-writer family suspect). VM host re-verify `merged-w10h`: rc=0, 55 s — hotfix confirmed on the VM.
  **Hygiene finds:** (a) 20 orphaned root-dir `EDITOR.BIN_host` processes (ppid=1, ~20 h
  old) + one fresh one wedged this box's `make host_tests` — the host editor test can hang
  on an anonymous pipe read with no watchdog (swept by exact /proc/comm match; todo item);
  (b) an accidental `pkill -f "./EDITOR.BIN_host"` matched the runner's own command line
  and killed it — reinforces the "never broad pkill" rule; sweep via pid lists filtered by
  /proc/<pid>/comm.

- **2026-10-14 (~21:50) — Waves 8a/8b (w10-fstime + w11-makej) merged (`465c9b4`,
  `1ac7c7d`) — THE IN-OS BUILD IS NOW INCREMENTAL AND PARALLEL.** w10: writes left
  mtime=0/1980 so make's dependency tracking was dead; `f9c6771` stamps FAT dirents from
  the RTC (`fat16_stamp_entry`, UTC; create/mkdir/write/append/truncate) and `fs.c` stat
  returns real times. Serial acceptance: 2nd make → **`MAKE.BIN: Nothing to be done for
  'all'.`**; rewriting one source of the 5-TU project compiles exactly that TU. Units
  322/0 ARM + 326/0 x64; wave green ×2 (one load-flake create-budget red absent on rerun).
  w11: `MAKE_JOBSERVER` was hard-undef'd; naive re-enable deadlocks (kernel never EINTRs;
  signals defer on pipe-read parks) → fixed with a bounded-poll `pselect()` stub (25 ms) +
  config.h enables + new `sys/select.h`; verified 16/16 byte-identical outputs, fail
  propagation (`Error 9`, rc=2, no zombies); measured serial 19.7 s vs -j4 ~10.0 / -j8
  ~9.3 on the synthetic (≈2× spawn-bound; near-8× expected on ~90 s/TU real builds; cap
  -j8 at the 63-slot table). New tools: `fatdir_dump.py`, `w11_bench_boot.py`. In-OS sh
  quirks recorded (drops empty quoted args; no `$(( ))` in while; no case alternation).
  Battery 9 launched on `1ac7c7d`. Next: w9-dirent still in flight; then the acceptance
  re-cut (todo §15) on the fast build path.

- **2026-10-14 (~20:45) — USER PRIORITY CHANGE: pause the acceptance re-cut; raise
  build-velocity first — FAT timestamps (incremental in-OS make) + parallel in-OS builds
  (`-j`) — with dirent continuing; all other session issues parked in `todo.md` (new,
  committed).** Wave 8 is now: **w9-dirent** (dirent-writer fix, in flight) +
  **w10-fstime** (FAT timestamps → incremental make; failing-first in-OS "second make =
  up to date" acceptance) + **w11-makej** (MAKE.BIN -j8 viability/fix + measured speedup).
  The acceptance RC re-cut (todo §15) is sequenced AFTER these merges.

- **2026-10-14 — Wave 7 (w7-accept) merged (`4f96d0f`): S4/S5 PASS + the last red
  root-caused.** (A) **The w6 in-OS-assembled OUT.IMG (sha `7e24b4ca…`) BOOTS standalone**
  (6.5 s, `Mode: DESKTOP`) and **launches the in-OS-built OUTPROG.BIN** from its menu
  (`[LAUNCH] OUTPROG.BIN`, clean exit); OUTPROG/HELLO sha `61b3be6a…` == w6 in-OS HELLO ==
  host hello.bin — the full circle closes. New helper `tools/w7_accept_boot.py`. (B) T4.3
  full loop: S0 ✓, S1 ✓ (~61 min), **T2.5 ✓ full markers** (`[HELLO] entering main; argc=1;
  argv[0]="./HELLO.BIN"; main returned 0`), then the S2 link failed TWICE (fresh disks incl.
  post-w8 cherry-pick) on `cannot open nfs_test.o/cpu.o` → root-caused as a **FAT16
  dirent-WRITER bug**: dump `t43-s2-fix.dirents.txt` shows every file receives a clean
  dirent + a mangled twin sharing its cluster, and for exactly 2/59 names only the mangled
  twin survives (`NFS_TEST.O.T`, `CPU-8167.O.T`) — deterministic, filename-keyed.
  (C) acceptance.md reconciled (TCG 900–1500×, S1→7200 s/S2→10800 s timeouts,
  non-incremental caveat, MODE=selfhost runbook §9, §8.3 verdict table). (D) reports +
  evidence committed.
- **2026-10-14 — Wave 8 (w9-dirent) dispatched (`deleg_e891f889`, 1 lane from `4f96d0f`):**
  fix the dirent writer (failing unit test with the trigger names + name set → fix
  `fat16.c` → unit gates + in-OS dump (zero twins) → S2-stage re-run crown). After w9: the
  acceptance RC re-cut (S3 + extract + boot on the fixed lineage).

- **2026-10-14 — w7-accept T4.3 full-loop intermediate: S0 PASS + S1 PASS + T2.5 PASS +
  S2 blocked at the link (its best run yet; 2.4 h).** `t43-loop.serial` in the w7 worktree
  logs all 59 compile/ascompile phases rc=0, then `LLD.BIN: error: cannot open
  /SRC/obj/arm/nfs_test.o` + `/SRC/obj/arm/cpu.o` → make Error 1 →
  `phase=s2-kernel-fail reason=tool-rc rc=512` → `FAILED-AT stage=s2-kernel`. Integrator
  verified in-serial: the two objects' compile commands are complete/correct and echoed
  before the link; the rsp (w6 reference) lists both names right; w6's identical stage
  passed on a FRESH disk while w7 re-ran on an already-built one → steered the fresh-disk
  kernel-stage re-run + mdir-of-failing-image discriminator (FAT create/overwrite-loss
  class vs code). w7 to report either way.

- **2026-10-14 — Wave 6.5 (w8-torture) merged (`bd58474`): the `TORTURE exec-wait-cycles`
  red is ROOT-CAUSED and FIXED — and the class was deterministic ON ARM all along (the
  "local pass" earlier was the HOST_TEST build: `suite_ms=301`/`total_kb=65536` mock lines,
  not the ARM wave).** Root cause: the exec-argv marshallers (`pl_strcpy` in
  `program_loader.c`, `u_strcpy` in both `trap.c`s) validated the WHOLE cap up front
  (`sys_user_range_ok(src, cap)`); w6's 64→4096 bump made that check cross the
  mapped-region top for short strings near the user stack top (torture child's `code[]` at
  `0x1800000000−0x238`) → the arg was silently dropped → exec'd child saw argc=2, took the
  code=7 default → `exit(7)≠want(10..17)` → every cycle `-1`, `exec_ok=0`. Proof:
  `w8-probe-fail.serial.txt` — child prints `argv2=10` intact (argv fine in-child), parent
  sees `st=0x700 exited=1 code=7 sig=0` ×16 forks; rules out fault-kill, kernel stacks
  (64 KiB), and the cxxrt delta. **Fix (`9159e86`)**: per-page validation at all three
  sites (validate exactly the page of each byte before the load; stop at NUL) — w6's
  long-argv purpose (`sh -c '<recipe>'`) unaffected; probe commit `ac59b9c` kept in
  history. Verified: A/B unfixed 6/6 FAIL ↔ fixed 3/3 PASS (`exec_ok=8`); VM test-arm ×3
  PASS; local ARM wave PASS + marker rc=0; unit gates 318/0 ARM + x64 clean. Battery
  round 7 launched on `bd58474`. **Battery round 7: GREEN both machines** — local 4/4
  rc=0 (`exec_ok=8` in host AND ARM wave; 1 WATCHDOG); VM 3/4 rc=0 + test-arm gate rc=1
  on 2× WATCHDOG only (157 s, no wedge; TORTURE PASS `exec_ok=8`, marker present).
  **Queued:** re-cut the frozen-acceptance RC on the final tip after the last red is
  merged (w7's loop runs the 288995c lineage). Skills updated (kernel-constraints #7
  whole-cap-check pitfall; run-tests host-vs-ARM battery-log attribution pitfall).

- **2026-10-14 — Battery round 6: local = host/arm/x64 green, wave WEDGED (guest CPU-dead
  after GREPTEST's full pass; `WAVE_ARM_RC=124`; first LOCAL wedge, b4-class) → single-tier
  rerun = rc=0, marker present, but the rerun ALSO hit `TORTURE exec-wait-cycles FAIL`
  (exec_ok=0) — see the red entry below. Rerun log
  `merged-battery6-wave-rerun-2026-10-14.log`; other tokens = 2× WATCHDOG, ~20 per-test
  ALL-PASSED lines.
- **2026-10-14 — `TORTURE exec-wait-cycles` failure at `288995c` — NOT VM-only: intermittent
  on both machines, heavily biased to the new tip. Local wave #1 PASS (exec_ok=8, 301 ms);
  local wave rerun FAIL (exec_ok=0); VM fail 2/2; pre-w6 `42c6c0b` passes on both (VM slow
  run incl.). Investigation lane w8-torture dispatched (`deleg_689f3db2`) + steered with
  the local-repro update.** Evidence: VM merged-w7 + merged-w7r
  both FAIL (`exec_ok=0`, `exec_rejected=0`, suite 44 s; children DO run — 16×
  `[TORTURE.CHILD] ok`; one fail-run had a NORMAL 179 s wall). Controlled A/B same night:
  pre-w6 tip `42c6c0b` on the VM PASSES (`exec_ok=8`) even in a slow 322 s run. Local at the
  new tip: PASSED once (`exec_ok=8`, 301 ms) then FAILED (`exec_ok=0`) on the wave rerun —
  intermittent on both machines. Failure-class signature = all cycles `-1`
  (waitpid≠pid ∥ !WIFEXITED ∥ status≠want) ×both retry attempts; candidates: exec-marshal
  argv loss (`pl_strcpy` 64→4096), child exit-path fault vs new status rules, `sys_exec`
  4096-wide stack locals, cxxrt.o binary delta. Side-note: the pre-w6 A/B run also showed a
  new `[LFN] FAIL long dir listed with long name` token (first sighting — watch).

- **2026-10-14 — Wave 6 (w6-run) merged (`288995c`): THE IN-OS BUILD RUNS END-TO-END.**
  Verified by the integrator against raw evidence before merge: every stage marker on raw
  serial (`s0-check-done rc=0`, `s1-user-done rc=0`, `t25-run-done rc=0`, `kernel-link-done
  rc=0`, `s2-kernel-done rc=0`, `s3-disk-done rc=0`, `ACCEPTANCE-COMPLETE`), and the
  artifacts are real: **S1**: all 5 user objects compiled + 4 links + 4 objcopies in-OS;
  **HELLO.BIN byte-identical (sha256) to the host build** — determinism proof. **T2.5**:
  in-OS-built HELLO.BIN ran in-OS (`[HELLO] entering main`). **S2**: 59 TUs + lld link +
  objcopy → hobbyos.elf (1,372,912 B) + hobbyos.bin (219,564 B, ARM64 Image header).
  **S3**: OUT.IMG 134,217,728 B assembled in-OS in 365 s (sha256 `7e24b4ca…`), full
  acceptance layout verified by mdir: `/boot/hobbyos.bin` + limine.conf,
  `/EFI/BOOT/BOOTAA64.EFI`, root HELLO/SH/LS/CAT/DESKTOP/CONSOLE/OUTPROG.BIN + OUTMARK.TXT.
  OUT.IMG + products also copied to `obj/selfhost/w6-out/` and committed as evidence.
  Fix-forwards (9 commits): cxxrt.cpp os-wide followup; **execv per-arg cap 64→4096**
  (broke make→`sh -c` recipe lines); **lld `-flavor gnu`** (bare lld refuses); **in-OS make
  pattern rules never match** (FAT readdir returns UPPERCASE 8; make's check is
  case-sensitive) → 59 explicit kernel rules; fpu.o drops `-mgeneral-regs-only`; **crt0
  32-arg link cap** → lld response file (`/SRC/OBJS.RSP`); S3 stage gaps (RM.BIN, ::/bin).
  **TCG reality (corrected):** in-OS clang under ARM TCG ≈900–1500× slower than host
  (sh.c 1.38 s host → 53 min in-OS; kernel ≈1.5–2 h) — acceptance.md's 8× note is off by
  ~2 orders; in-OS make is non-incremental (FAT timestamps). **S4/S5 = next (w7): boot the
  real OUT.IMG + run the in-OS-built program — the acceptance moment.** Battery round 6
  launched on `288995c`.

- **2026-10-14 — Merged-tip battery round 5 on `42c6c0b`: GREEN both machines (ambient-class
  residuals only).** Local: host rc=0, unit-arm **318/0**, unit-x64 (KVM) **322/0**, wave
  rc=0 with 3 token-lines: `[CMPTEST] FAIL identical files` (cmp exit 2 under slot pressure —
  `no free slot (used=63/63)` + starvation forensics present in-window = the documented
  pressure-fragility family; single check; 0 occurrences in b3/b4/w4-w6 runs → watch, not
  regression) + 2× `SHELLTEST WATCHDOG`. CI VM (`merged-w6`): host/unit-arm/unit-x64 rc=0;
  test-arm rc=1 by gate math on `CXX_T condvar_wait_notify` (documented slot-pressure check
  class — `no free slot` forensic grep confirmed) + `SHELLTEST WATCHDOG`; 173 s, no wedge.

- **2026-10-14 — Wave 5 (3 lanes) all merged (`42c6c0b`); zero conflicts. THE TOOLCHAIN RUNS IN-OS.**
  (1) **w5-flat**: `selfhost-stage.sh` converts every LLVM dist ELF (clang-21/lld/llvm-ar/
  llvm-objcopy) to flat at `--tools` stage time (host `llvm-objcopy -O binary` + sanity checks:
  AArch64 ELF, e_entry==0x1000000000, first PT_LOAD vaddr==entry, flat head==LOAD content;
  failure aborts staging). Pipeline proof in-OS: full clang banner, `phase=…done rc=0`,
  `ACCEPTANCE-COMPLETE`. Flat sizes: CLANG 94,192,000 / LLD 53,250,208 / LLVMAR 8,348,856 /
  OBJCOPY 4,982,328 B (byte-identical to the hand conversions).
  (2) **w5-kernel — all 7 blockers fixed with pre/post serial pairs**: (i) exec gate
  1 MiB→128 MiB on the v2 path; (ii) fault-killed child → non-zero status
  (`process_fault_set_status`; +test); (iii) pid=5 housekeeping EL1 fault = L9-prefetch vs
  group teardown dead page-table walk (`vm_arch_walk` deref) → zero-root guard + alive
  recheck (+test); (iv) L9-prefetch PROT instruction-fetch race → bounded (4)
  TLB-flush-and-retry in `vm_handle_fault`; (v) IDLESTUCK dump only on actual reclaim;
  (vi) spawn/exec args 255→1024 (kernel + crt0); (vii) exec of a *missing* image now fails
  -ENOENT BEFORE the AS teardown (was: teardown → bad-size-0 → AS-less fault loop). Gates:
  ARM 318/0, x64 322/0 (KVM), wave green (`ALL TESTS PASSED`).
  (3) **w5-nothrow — the in-OS compile works**: root cause = libc.a's `cxxrt.o` (pulled for
  `__cxa_atexit`) carries STRONG `operator new/delete` beating libc++'s weak
  `__lcxx_override`-section defs → nothrow override assert (brk #1). Fix: new
  `cxxrt_shim.cpp` at the head of the closure (strong `__lcxx_override` new/new[] + cxa
  atexit/finalize/dso/placement/oom; lld never extracts cxxrt.o) + delete→`free(ptr)` (raw
  malloc payloads have no stash). Verified: `_Znwm == __start___lcxx_override` in all four;
  **T2.3 banner PASS + T2.4 compile PASS (hello.c → HELLO.O, 2272 B valid ELF, pulled off
  the image and host-validated)**. Follow-up queued: `src/libc/src/cxxrt.cpp` same latent
  issues (section attribute + delete→free) → w6. Merged-tip battery round 5 launched on
  `42c6c0b`.

- **2026-10-14 — Battery round 4: local GREEN; VM test-arm wedged (investigating).** Local
  (`82d4d81`): host rc=0, unit-arm 316/0, unit-x64 320/0, wave rc=0 ('System halt.'; 2
  WATCHDOG ambient lines). CI VM (`merged-w5`): host/unit-arm/unit-x64 rc=0; test-arm hit
  the full 1800 s timeout with NO halt marker — a mass-failure cascade rooted in the KNOWN
  chronic slot-pressure condition (v2 loader starvation + `sys_fork` -11 + `used=63/63`):
  starved spawns failed downstream suites en masse (PASTETEST 24, FOLDTEST 45, SEDTEST 12,
  IPC_T, SQLTEST…) and the run never cleared. Starvation lines are chronic across ALL runs
  (local b3/b4: 60/54; VM w4: 86; VM w5: 61) — a flaky ESCALATION of a known fragility,
  not obviously wave-4-caused. **Clean rerun (`merged-w5r`): NO wedge — 338 s, halt marker
  present, 0 `FAILURES:` lines** → the wedge was a flaky escalation, not reproducible.
  Rerun tokens = 2× `SHELLTEST WATCHDOG` + 2× `[NFS] FAIL read PATTERN.BIN`; the NFS class
  also appears in pre-selfhost VM logs (`fs-w6b`/`l9p-wave`/`fs-w1a-r2`) → attributed
  network-timing ambient. **Queued: slot-pressure cascade robustness** (loader backoff/
  retry, slot recycling, or capacity) — CI determinism item.

- **2026-10-14 — Wave 4 (3 lanes) all merged (`82d4d81`); zero conflicts (Makefile auto-merged).**
  (1) **MODE=selfhost DELIVERED** (w4-kernel): Makefile plumbing + `main.c` branch launching only
  SELFHOST.BIN (argv[1]=`/SRC/SELFHOST.RUN`); driver stdout/stderr reach serial via the
  pre-existing console-fd fallback. Serial-evidenced run: `Mode: SELFHOST` → `[SELFHOST]
  DRIVER-UP` → `phase=s0-check-start` → tool relay → `phase=s0-check-done rc=0` →
  `ACCEPTANCE-COMPLETE` → `System halt.` (QEMU rc=0, 0 FAIL tokens;
  `docs/selfhost/lanes/w4-a1-selfhost-hello-serial.log`). Acceptance 5.2/5.3 primary path works.
  (2) **FAT16 TCG slowness FIXED** (w4-kernel): alloc_cluster free-cluster hint + bulk extension
  in `fat16_truncate_to` (O(n²)→linear): 16 MiB 78.7→34.6 s, 64 MiB 347→142.6 s, 128 MiB
  >720 s(killed)→**284 s** (clears the 600 s S3 budget). Units 316/0 ARM (317 w/probe) + 320/0 x64.
  (3) **Makefile.hobbyos v1 + /SRC staging** (w4-src): check/user/kernel/disk/all, `[SELFHOST]
  phase=s0..s3` markers, absolute `/TOOLS/*.BIN`, `SHELL=/TOOLS/SH.BIN`, explicit 59-TU kernel
  list (corrections vs sketch: `-T linker.ld` repo-root, no obj glob). Staging = 307 files /
  4.55 MiB subset + carried products + `SELFHOST.SH` cwd wrapper (processes start at cwd=/ →
  run via `SH.BIN /SRC/SELFHOST.SH <target>`). S0/S1 dry-runs: make runs + parses + issues
  compile lines in-OS, blocked at `sh: exec failed: /TOOLS/CLANG.BIN` → **the exec gate**:
  `program_too_large()` = `MAX_PROGRAM_SIZE` 1 MiB (process.h:42; program_loader.c:63/:855-860
  → `-ENOEXEC`) vs v2_map_image's 128 MiB cap — self-contradiction; rejects clang 126 MB (and
  LLD/LLVMAR/OBJCOPY). Kernel-side → wave 5. **AD-9 FROZEN: `STAGE_LIBCXX=0`** (S0–S3 all C).
  (4) **clang-in-OS walls root-caused** (w4-clang): (a) `/TOOLS/*.BIN` are raw ELF while the OS
  runs flat .bin (entry=file offset 0) → execute `\x7fELF` → fault EC=0 @ ELR=0x1000000000;
  **fix = the M2 `objcopy -O binary` conversion — proven: flat CLANG2.BIN runs in-OS, prints
  its full version banner, exit 0** (94 MB flat; prefetch 22,127/22,997 pages in 4.7 s);
  (b) flat clang compiled HELLO.C past parsing into codegen, then `brk #1` at
  `operator new(std::nothrow)` — `_Znwm` outside clang's `__lcxx_override` link list;
  OS/libc++ counter-proven healthy (NPNEW.BIN runs); (c) flat LLD2.BIN killed on
  instruction-fetch classified PROT at a page the L9 prefetch was filling (timing-sensitive,
  kernel); (d) fault-killed children can report rc=0 (process_fault_exit bypass; 2-line fix);
  (e) driver spawn args capped at 255 chars; (f) deterministic pid=5 housekeeping EL1 fault +
  IDLESTUCK false-positives on driver pipe-wait. Merged-tip battery round 4 launched on
  `82d4d81`; **wave 5 = the unblockers: w5-flat (stage-time flat conversion), w5-kernel (exec
  cap / fault-rc / pid5 / prefetch), w5-nothrow (clang `_Znwm` link config).**

- **2026-10-14 — Merged-tip battery round 3 on `994116a`: GREEN.** Local: host rc=0,
  unit-arm **315/0** (+3 dirent tests), unit-x64 (KVM) **319/0**, wave-arm rc=0 ('System
  halt.') with exactly ONE FAIL token — the `SHELLTEST WATCHDOG: protocol stalled 25 s`
  variant of the ambient family (the echo and shell_test3-cat variants are GONE — the
  w3-shell drain holds on the merged tree); residual watchdog-stall hardening queued as a
  minor follow-up. CI VM (`merged-w4`): host rc=0 (54 s), unit-arm rc=0 (211 s), unit-x64
  rc=0 (130 s, KVM); test-arm rc=1 by gate math on TWO `SHELLTEST WATCHDOG` lines (same
  ambient class — MTTCG-timing; marker `System halt from CPU 5.` present). Residual to
  harden: the watchdog-stall sub-class itself (queued for wave 5 / minor lane).

- **2026-10-14 — Merged-tip battery RERUN after `1920dc9`: GREEN (local, all tiers).**
  Local: host rc=0 (all suites), unit-arm 312/0, unit-x64 (KVM) 316/0, wave-arm rc=0
  ('System halt from CPU 5.') with exactly ONE FAIL token — `shell_test3: FAILED cat
  nested.txt` under `thread_create: no free slot (used=63/63)` (the same slot-pressure
  fragility family as the echo token; this run drew the shell_test3 variant instead of
  the echo one). CI VM (`merged-w3`): host rc=0 55 s, unit-arm rc=0 175 s, unit-x64 rc=0
  118 s (KVM); test-arm rc=1 by gate math on exactly ONE attributed token — `SHELLTEST
  WATCHDOG: protocol stalled 25 s` (documented ambient class; the echo and shell_test3
  variants were absent this run) with `System halt.` present. So both merged-tip runs
  now bottom out at a single ambient token from the same slot-pressure/timing family.
  w3-shell lane steered to drain the whole family (echo + shell_test3 cat + watchdog
  interaction), goal = wave with ZERO FAIL tokens.
- **2026-10-14 — Wave 3 (4 lanes) all merged (`994116a`); zero conflicts.** Merges:
  w3-kernel `d4b5402`, w3-shell `e6bedf8`, w3-tools `8ff6241`, w3-accept `994116a`.
  Headlines:
  (1) **Gap A (FAT16 extend-write) re-diagnosed — NOT a kernel bug.** mkfs.fat `-C`
  ftruncates BEFORE `setup_tables()` rejects a too-small filesystem: the w2 probe's 1 MiB
  image is below FAT16's 4087-cluster floor, so only the ftruncate zero-fill persisted
  (with a "too small" warning). Failing-test-first kernel tests (open→ftruncate→lseek0→
  write→close→reopen→raw-compare at file+fat16 layers) PASS on the pre-fix tree — write
  path exonerated, no kernel change made. In-OS proof: 1 MiB → zeros; 16 MiB minimum →
  full FAT16 + non-zero content surviving graceful quit; host-port parity covers 128 MiB.
  Watch: 128 MiB in-OS ran too slow under TCG (QMP broke mid-ftruncate, not persistence).
  (2) **dirent landed end-to-end**: libc `opendir/readdir/closedir` over SYS_READ_DIR (kernel
  `read_dir` already fine); in-OS DENTEST enumerated `[., .., ONE.TXT, SUB, ., .., TWO.TXT]`
  incl. subdir descent; units **315/0 ARM + 319/0 x64**.
  (3) **sh redirect root-caused + fixed**: `eval_simple()` closed the child's stdout when
  `lerr == 1` in `> F 2>&1` shapes; every redirect-close now guards `>= 3`. Host-parity
  byte proofs + in-OS SHTEST file-based assertions (`cat SHTEST.TXT > OUT2.TXT` ⇒ 194 B;
  `make --version > M.TXT` ⇒ GNU Make banner). New finding: w2-make's console symptom was
  a QMP keystroke artifact (guest translates `>`→`=`, drops chars) — `tools/run_w3kbd_probe.py`.
  (4) **Slot-pressure token family drained**: drain-before-write + pressure-classified
  SKIP; wave-arm **ZERO FAIL tokens across 3 full runs**.
  (5) **/TOOLS staged**: `selfhost-stage.sh --tools` + Makefile; 12 entries / 223 MB;
  in-OS harness boot shows `/TOOLS/MAKE.BIN` executing in the guest. **Resdir decision:
  LFN suffices — no 8.3 curation** (fat16 full-name matching + LFN_T.BIN + sha256 fidelity;
  §6.2; watch: 5 basenames >31 chars, cosmetic only). Image budget: 696 MiB free (M3 note).
  (6) **S4/S5 pre-tested** with a hand-built image: extraction byte-identical; extracted
  image boots (marker 6.5 s); program launches from the OUT.IMG Apps menu; kernel-less case
  classifies `stage_failure`. Exact OUT.IMG must-contain list in `acceptance.md` §8
  (limine/AAVMF layout + **`/DESKTOP.BIN` mandatory** + CONSOLE/SH + 8.3 S1 program).
  Pitfall recorded: mformat `-F` = FAT32, not FAT16. `fat16img.py` extract linearized
  (>120 s O(n²) → ~1 s). Merged-tip battery round 3 launched on `994116a`.

- **2026-10-14 — Post-merge closure re-validation GREEN + first merged-tip battery fallout
  fixed (`1920dc9`).** Closure: re-staged sysroot from merged src, `cmake` reconfigure
  (closure objects are configure-time — see entry below), then llvm-ar / llvm-objcopy /
  clang-21 / lld ALL relinked clean against the merged libc.a with the reconciled gaps.c —
  concrete proof the 24-symbol reconciliation holds for the full toolchain;
  `install-distribution` restaged 227 MB. First merged-tip battery (finished in 2 min,
  red): host rc=2 — `compat.c` host mock mkdir vs the new variadic `ho_mkdir` decl, then
  a glibc C++ `<sys/stat.h>` __THROW mkdir clash; unit-arm/unit-x64 rc=1 + wave rc=2 —
  MAKE.BIN duplicate `alarm` (hb_stubs.c) + `strsignal` (signame.c) vs the now-real libc
  symbols; mtools carried the same `alarm` fallback. Fixes: compat mkdir variadic;
  `unistd.h` mkdir decl HOST_TEST-guarded; make `config.h` `HAVE_STRSIGNAL 1`; alarm
  stubs dropped in make + mtools. Post-fix pre-checks green (host ALL APPS SUITE PASSED;
  arm make/mcopy/mdel/mmd relink clean). Full reruns (local + CI-VM `merged-w3`) in
  flight. **Class lesson:** every symbol that newly lands in libc must be re-checked
  against every port's private fallbacks — grep the symbol across `src/user` whenever
  libc grows. **Wave 3 dispatched (4 lanes, `deleg_1fa8515c`):** w3-kernel (FAT16
  extend-write gap + opendir/readdir), w3-shell (external redirect + echo-token drain),
  w3-tools (resdir curation decision + /TOOLS staging; sole Makefile writer),
  w3-accept (S4/S5 runner pre-test with a hand-built OUT.IMG + real-OUT.IMG requirements).

- **2026-10-14 — Wave 2 (7 lanes) all merged; closure re-validation + merge-time fixes.**
  Merges on `main`: w2-kernel `7abb674`, w2-userland `9587c45`, w2-llvm `a06daf7`,
  w2-harness `4130325`, w2-make `b70be62`, w2-mtools `3234c0d`, w2-mkfs `0c272a4`. Merge
  friction was exactly the predicted pair (`sysroot-headers/_missing.h` + `gaps.c`,
  Makefile var/rule/disk blocks, `tools/cstyle.py` EXCLUDE_PREFIXES) — all reconciled by
  hand. **Closure reconciliation (nm-verified):** the 24 symbols the userland lane made
  real in libc.a (alarm, strsignal, setsid, wait4, getrusage, getrlimit, setrlimit,
  fchown, fchmod, mkdir, symlink, link, pipe, pthread_sigmask, _Exit, statvfs, fstatvfs,
  dlopen, dlsym, dlclose, dlerror, dladdr, socket, connect) were dropped from gaps.c;
  true gaps remain (~20; gaps.o recompiles to 21 FUNC). **Closure rebuild gotcha (new
  hard rule):** the three closure objects (`hobbyos_gaps.o` / `hobbyos_setjmp.o` /
  `hobbyos_fstream_inst.o`) are built by `execute_process` at CMake **configure** time
  (toolchain-llvm-hobbyos.cmake:151-257) — there is NO ninja edge — so after editing
  gaps.c/_missing.h/setjmp.S you MUST re-run `cmake` in-place (warm, ~3 s) or the link
  silently uses a stale gaps.o (observed: duplicate wait4/link/strsignal/symlink/setsid/
  fchown/pthread_sigmask/alarm vs merged libc; fixed by reconfigure → llvm-ar +
  llvm-objcopy relink CLEAN). **Socket caveat (wave-3 item):** libc's `socket/connect`
  are the LEGACY ip-form API while `_missing.h` declares POSIX `connect(fd,addr,len)` —
  harmless at link for the toolchain's raw_socket_stream path (currently unreachable)
  but a semantic mismatch for the socket-reachability closure.
  **Lane findings folded into the wave-3 roster (§7.2):** mkfs — MKFSFAT `-C` BLOCKS are
  1024-byte units → acceptance uses `DISK_BLOCKS=131072` for the frozen 128 MiB OUT.IMG
  (L4's sketch math 524288 would make a 512 MiB image); cluster arms 64/128 MiB→spc4,
  256 MiB→spc8; in-OS ftruncate-extended writes do NOT persist (kernel write-path gap,
  deterministic, zeros after graceful quit). make — crt0 two-arg-main envp caveat
  (in-lane z/OS fallback), sysroot dirent ENOSYS → v1 = explicit-rule makefiles only,
  sh `cmd ARG > FILE` redirect lossy for externals (blocks file-based byte assertions).
  harness — S4/S5 gated on OUT.IMG (can pre-test with a hand-built OUT.IMG); the
  driver's tool-present relay path is implemented but not exercised; MODE=selfhost
  kernel boot mode still open (acceptance 5.2/5.3).

- **2026-10-14 — Wave 2 dispatched** (7 lanes, `deleg_aaf311af`): w2-llvm (LLVM build
  continuation from the 211/564 checkpoint), w2-kernel (wait4 + argv envelope + rmdir),
  w2-userland (setjmp/longjmp + resource/decl shims + wait4 wrapper), w2-make, w2-mtools,
  w2-mkfs (ports), w2-harness (SELFHOST.BIN + runner + staging v2). All branch off
  `d48ada6`; integration + merged gates when reports land.
- **2026-10-14 — Wave 1 (L1–L5) complete; all lanes merged clean (`059997a`…`5a9967b`);
  `pid_t`→int landed (`4a30ca4`); merged-tip battery RESULTS:** local = host_tests PASS,
  unit-arm 309/0 PASS, unit-x64 313/1 (single `patched==1` red in
  `test_fat16_lfn_truncated_run` — **byte-proven test-side gap**: `fat16_open` creates the
  fixture at runtime (fat16_test.c:533) and its LFN run can split across the root-dir sector
  boundary; the post-run dump shows the short entry at sector 557 offset 0 with its 0x42/0x01
  chunk slots as the last slots of sector 556, so the test's sector-local backward scan starts
  at k=-1 and never sees the run — OS behavior correct on both layouts; fix steered to
  w2-kernel), wave-arm marker + halt with
  ONE `shell_test: FAILED echo validation` token. CI VM = host / unit-arm / unit-x64
  (KVM, 114 s) all rc=0 — x64 313/0 there; test-arm = marker present ('System halt.') but
  the runner's FAIL-token gate counted exactly the two attributed ambient lines (SHELLTEST
  WATCHDOG + the same echo token) → verdict 'FAILURE (2 lines)' by gate math, i.e.
  attributed-ambient, not a product red (both lines are in the documented classes above). **Echo token attributed:** the local log shows
  `thread_create: no free slot (used=63/63)` immediately before it — under slot pressure the
  pre-echo pipe step SKIPs and SHTEST's echo step then reads a stale prompt buffer → its
  echo guard fires. Test-side fragility of the pressure-skip-guard family, NOT a product
  regression (L3 run5's zero-token result = lucky timing, same tree content). Follow-up:
  guard the echo step / drain fix (wave-3 QoL). L1: configure GREEN (18 spike runs; zero LLVM
  source patches); vendor + toolchain + sysroot machinery committed; the one hard gate was
  the `Process::Pid` static_assert → resolved by AD-12. L2: audit 66 FULL / 20 PARTIAL /
  16 MISSING / 12 NO-ACTION; cc1 runs IN-PROCESS; W2-1..W2-15 fix list. L3: five real LFN
  gaps found+fixed (G1–G5); `LFN_T.BIN` 25 checks; unit 309/0; ARM wave zero-FAIL; ambient
  flakes attributed via a same-window pristine gate. L4: pins (make 4.4.1 / mtools 4.0.49 /
  dosfstools 4.2); sh matrix (12 SUPPORTED / 4 PARTIAL / 1 ABSENT); `Makefile.hobbyos`
  sketch. L5: staging tool tested against a real 1 GiB image copy (2322 files / 30.1 MB
  payload); OUT.IMG = 128 MiB; TCG boot ≈9 s.
- **2026-10-14 — `pid_t` → `int` (AD-12).** Linux/glibc parity; unblocks LLVM 21.1.8's
  `Process::Pid` static_assert; kernel `struct flock` byte layout unchanged (32 bytes,
  `st32` at offset 24). Single-writer commit `4a30ca4`.

- **2026-10-14 — Program start.** Copy created (`~/hobbyos-selfhost` @ `64093f6`; remote
  `ws`; container seeded; libcxx fetch verified; `hello.bin` smoke built). Plan v1
  committed; lanes L1–L5 briefed + dispatched. Planning-time fleet discoveries:
  (a) fat16 **LFN read+write already in tree** (fat16.c:36-72/412/454/569-622/985) —
  L3 rescoped from "implement" to "verify + extend tests"; (b) fat16 >1 MiB defect
  already fixed (`3c781c5`); (c) the full llvm-project 21.1.8 container (llvm/clang/lld)
  is already vendored — no new source downloads needed for the critical path;
  (d) host clang == pin 21.1.8 (no version skew).
- Clone validation **GREEN** (2026-10-14): `./run_unit_tests.sh` on the copy —
  `Tests run: 302 / Tests failed: 0 / UNIT TESTS PASSED` (evidence trimmed to
  `docs/selfhost/lanes/l0-clone-unit-tier.log`; full log = the gitignored `qemu.log`
  in the clone root).
