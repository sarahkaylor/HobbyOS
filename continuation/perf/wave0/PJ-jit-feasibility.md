# Lane PJ — JavaScriptCore C_LOOP → LLInt → JIT feasibility (read-only)

Lane: **PJ** · Status: **gated** (LLInt + baseline JIT viable on both arches with small, precise edits;
gates: (a) ARM64 `cacheFlush` compile block needs an OS cache-flush route; (b) DFG tier needs OS
signal delivery that does not exist yet (P5.3 is design-only); (c) image-size budget near the 128 MiB cap).

Verified against:
- fork `~/webkit-hobbyos` @ `e9a040c5b4` (rp-f, WebKit 2.54-era tree)
- OS `~/Documents/GitHub/HobbyOS`

Every claim below carries a `file:line`; anything not directly verified is marked **UNVERIFIED**.

---

## 1. C_LOOP vs LLInt vs JIT in this tree

### 1.1 Feature-flag graph

- Defaults keyed off CPU in `Source/cmake/WebKitFeatures.cmake:79-127`:
  `WTF_CPU_ARM64 OR WTF_CPU_X86_64` → `ENABLE_JIT_DEFAULT=ON`, `ENABLE_C_LOOP_DEFAULT=OFF`
  (:92-98). The HobbyOS port overrides both: `Source/cmake/OptionsHobbyOS.cmake:213-214`
  (`ENABLE_JIT PRIVATE OFF`, `ENABLE_C_LOOP PRIVATE ON`), plus `ENABLE_DFG_JIT OFF` /
  `ENABLE_FTL_JIT OFF` (:217-218).
- Conflict/depend rules `WebKitFeatures.cmake:334-345`: `ENABLE_JIT` ⇔ `ENABLE_C_LOOP` conflict
  (:334), `ENABLE_SAMPLING_PROFILER`/`ENABLE_WEBASSEMBLY` conflict with C_LOOP (:336-337);
  `ENABLE_DFG_JIT` requires `ENABLE_JIT` (:342), `ENABLE_FTL_JIT` requires DFG (:343); wasm
  BBQ/OMG require FTL (:344-345).
- So **C_LOOP and JIT are mutually exclusive build modes** in this tree. There is NO separate
  `ENABLE_LLINT` switch: the LLInt layer is compiled in both modes.
  - C_LOOP mode: `llint/LowLevelInterpreter.cpp` compiles the C/C++ interpreter
    (`#if ENABLE(C_LOOP)` at `LowLevelInterpreter.cpp:32`), built from the same
    `llint/LowLevelInterpreter.asm` via the `offlineasm` C_LOOP backend.
  - JIT mode: the identical `.asm` is lowered by offlineasm into native machine code
    (`LLIntAssembly.h`, embedded in the binary via the `LowLevelInterpreterLib` object lib —
    `Source/JavaScriptCore/CMakeLists.txt:496-497`), and `LowLevelInterpreter.cpp` compiles the
    non-C_LOOP branch (`#else // !ENABLE(C_LOOP)` at `LowLevelInterpreter.cpp:497`) that exposes
    the `llint_function_for_call_prologue` etc. entry points as inline-asm labels in `.text`.
    **The native LLInt runs from the process image `.text` — it needs no runtime executable-after-
    load mapping, so LLInt alone requires no exec-memory syscalls.**
  - This is the core architectural fact: LLInt(C) → the only ask is a rebuilt binary; native LLInt
    is a build-flag flip; only the JIT tiers (baseline/DFG/FTL) allocate executable memory at runtime.

### 1.2 What must be true per tier (macros)

Checked against `Source/WTF/wtf/`:
- CPU: `WTF_CPU_X86_64` from `__x86_64__` (`PlatformCPU.h:114-116`); `WTF_CPU_ARM64` from
  `__aarch64__`/`__arm64__` (`:122-123`). The cross toolchain compiles with
  `--target=x86_64-none-elf` / `aarch64-none-elf`
  (`HobbyOS/toolchain-hobbyos.cmake:71-84`), so both macros already fire — the HobbyOS build
  **already** satisfies the CPU branch that WebKitFeatures keys its JIT defaults off
  (`WebKitFeatures.cmake:92`). ✓
- Value model: `USE_JSVALUE64` from `CPU(REGISTER64)` (`PlatformUse.h:130-134`); REGISTER64 from
  ADDRESS64 (`PlatformCPU.h:306-320`) — both 64-bit targets get JSVALUE64. ✓
- `ENABLE_JIT` is auto-set ON for x86_64/ARM64 unless a port overrides it
  (`PlatformEnable.h:723-726`); `ENABLE_C_LOOP` auto-OFF when JIT/64-bit (`:742-748`). The port's
  CMake override is what pins the current C_LOOP build.
- Offlineasm backends: `OFFLINE_ASM_BACKEND` chosen in `Source/JavaScriptCore/CMakeLists.txt:306-324`
  — X86_64 for `WTF_CPU_X86_64`, ARM64 for `WTF_CPU_ARM64`, and C_LOOP only when
  `NOT ENABLE_JIT AND ENABLE_C_LOOP`. All backend `.rb` files (`offlineasm/{x86,arm64,risc,cloop}.rb`
  et al.) are already unconditionally listed in the build (`CMakeLists.txt:242-262`) — no new
  offlineasm machinery needed.
- `--binary-format=ELF` is only added for `CMAKE_SYSTEM_NAME MATCHES "Linux"`
  (`CMakeLists.txt:223-225`); it only drives `$emitELFDebugDirectives` in
  `offlineasm/asm.rb:331` (debug annotations, harmless to omit). Not a blocker.
- `ENABLE_LLINT_EMBEDDED_OPCODE_ID` auto-ON for the JIT build on these CPUs
  (`PlatformEnable.h:1020-1022`) — needs no OS support (it's a `.int opcode` word embedded in the
  generated asm).

### 1.3 What is gated by `ENABLE(JIT)` runtime that the port currently compiles out

The port is `ENABLE(JIT)=0` today, so all of `jit/`, `dfg/`, `ftl/`, `bytecompiler/` tiers are
stubbed or excluded (`JavaScriptCore/CMakeLists.txt:1858` lists the JIT dirs; `ExecutableAllocator.cpp`
is entirely `#if ENABLE(JIT)` at :29). Enabling JIT adds those sources to the build — the JSC
`CMakeLists.txt` already contains them, no file-list surgery needed. Runtime knobs default:
`useLLInt=true` (`runtime/OptionsList.h:84`), `useJIT=jitEnabledByDefault()` (:85),
`useBaselineJIT=true` (:86), `useDFGJIT=is64Bit()` (:87) — on 64-bit, DFG flips on by default once
the build has JIT, so the A/B plan must explicitly pin `--useDFGJIT=false` for the baseline-only run.

### 1.4 OS() surface (the real macro gap)

The port deliberately does **not** set `WTF_OS_UNIX` (DECISIONS D-5; `OptionsHobbyOS.cmake:28-29`,
`PlatformOS.h:144-148` derives OS(UNIX) from `__unix*`, absent for the `none-elf` targets) and ports
code by adding `OS(HOBBYOS)` branches instead (precedent: `WTF/wtf/posix/ThreadingPOSIX.cpp:75-77`).
The JIT needs the same per-site treatment at a small number of sites; the known OS-gated JIT sites
are enumerated in §8. Setting `WTF_OS_UNIX=1` wholesale is **not** recommended: it would pull in
`threads/Signals.cpp` (`#if OS(UNIX)` at `Signals.cpp:29`) and other unix files that need a signal
delivery layer the OS does not yet have.

---

## 2. Executable memory path + OS syscall surface

### 2.1 WebKit-side path (JIT modes only)

1. `ExecutableAllocator::initializeJITPageReservation` reserves one fixed pool:
   `PageReservation::tryReserveWithGuardPages(size, OSAllocator::JSJITCodePages, hint,
   EXECUTABLE_POOL_WRITABLE, /*executable=*/true, …)` (`jit/ExecutableAllocator.cpp:393-407`).
   `EXECUTABLE_POOL_WRITABLE` is `true` unless `ENABLE(MPROTECT_RX_TO_RWX)` is on
   (`jit/ExecutableAllocator.h:52-56`; `ENABLE_MPROTECT_RX_TO_RWX` defaults 0,
   `PlatformEnable.h:961-962`). **So the default request is one `mmap(PROT_READ|PROT_WRITE|PROT_EXEC,
   MAP_PRIVATE|MAP_ANON)` region — RWX, no W^X dance.**
2. Pool sizes: x86_64 → **1 GB** (`ExecutableAllocator.cpp:106-107`), ARM64 → 128 MB (or 512 MB with
   jump islands; `:94-99`), overridable via `FIXED_EXECUTABLE_MEMORY_POOL_SIZE_IN_MB` (`:92-93`) —
   the port's `PlatformHobbyOS.cmake:14-16` TODO explicitly flags this (PlayStation pins 64,
   `PlatformPlayStation.cmake:5-7`).
3. `PageReservation` → `OSAllocator::tryReserveAndCommitImpl` (`WTF/wtf/posix/OSAllocatorPOSIX.cpp:75-133`):
   builds `PROT_*` bits from writable/executable (:78-82), `MAP_PRIVATE|MAP_ANON` (:84), fd from
   `vmTagFd` (= −1 off-Darwin, `bmalloc/BVMTags.h:115`); guard pages are re-`mmap`ed to `PROT_NONE`
   with `MAP_FIXED` (:122-129). On non-Darwin/non-Linux it skips `MAP_NORESERVE` (:93-100) — fine.
4. Allocation from the pool: `MetaAllocator` (since `ENABLE_LIBPAS_JIT_HEAP` requires
   `BENABLE(LIBPAS) && (OS(DARWIN)||OS(LINUX)||OS(WINDOWS))` — `jit/ExecutableMemoryHandle.h:40-42` —
   this fork does NOT get libpas JIT heap, so `#include <wtf/MetaAllocator.h>` at
   `ExecutableAllocator.cpp:57`), handing out RWX chunks within the reservation.
5. Code emission: `LinkBuffer::finalizeCodeDisassembly` → `MacroAssembler::cacheFlush(code(),
   m_size)` (`assembler/LinkBuffer.cpp:581`) is the single cache-flush choke point.
   - x86_64: `cacheFlush` is a no-op (`assembler/X86Assembler.h:6657`) — correct, x86 I-cache is
     coherent (plus `mfence` where the assembler needs ordering, `X86Assembler.h:4209-4214`). ✓
   - ARM64: `ARM64Assembler.h:4021-4048` — DARWIN→`sys_icache_invalidate`, FUCHSIA→`zx_cache_flush`,
     LINUX→`__builtin___clear_cache`, WINDOWS→`FlushInstructionCache`, **else `#error "The cacheFlush
     support is missing on this platform."` (:4047)**. **Compile blocker for the ARM64 JIT.**

### 2.2 HobbyOS kernel/libc surface (verified)

Syscalls present and dispatched on **both** arches:
- `SYS_MMAP 62` (`src/include/syscall.h:77`; dispatch `src/kernel/arch/arm/trap.c:1792-1796`,
  `src/kernel/arch/x64/trap.c:1685-1693`; impl `src/kernel/process.c:3007-3028` → `vm_mmap`
  `src/kernel/vm.c:1200-1288`).
- `SYS_MUNMAP 63` (`syscall.h:78`; arm trap :1797-1798, x64 trap :1694-1696;
  `vm_munmap_range` `vm.c:1301-1318` — supports partial ranges).
- `SYS_MPROTECT 81` (`syscall.h:130`; arm trap :1799-1800, x64 trap :1697-1700; `vm_mprotect`
  `vm.c:1361-1395` via `vm_prot_set_span` `vm.c:1323-1359`, which preserves flags/kind/obj and
  rewrites per-page PTEs).
- `SYS_MADVISE 82` (`syscall.h:131`; arm trap :1801-1802, x64 trap :1701-1704; `vm_madvise`
  `vm.c:1397-1414` — DONTNEED/FREE zap resident pages).
- `SYS_MEMFD_CREATE 80` (not needed by JIT).

Protection bits **already match** the JIT's needs end-to-end:
- libc `PROT_NONE=0, READ=1, WRITE=2, EXEC=4` (`src/libc/include/sys/mman.h:15-18`) == kernel
  `VM_PROT_READ/WRITE/EXEC` (`src/include/vm.h:78-80`) == glibc values.
- `mmap_prot_sanitize` caps to READ|WRITE|EXEC only (`vm.c:1196-1198`); **PROT_EXEC is accepted**
  (`vm.c:1210`). Flags mask accepts SHARED|PRIVATE|FIXED|ANONYMOUS|NORESERVE (`vm.c:1211-1212`).
- `MAP_FIXED` replacement works (`vm.c:1249-1256`) — the WebKit guard-page remap
  (`OSAllocatorPOSIX.cpp:122-129`) is supported.
- Exec permission is enforced/encoded in PTEs: x64 `x64_v2_pte_desc` sets NX iff no EXEC
  (`src/kernel/arch/x64/mmu.c:393-402`); ARM sets UXN iff no EXEC (`src/kernel/arch/arm/mmu.c:491-500`);
  fault classifier checks EXEC (`vm.c:831-838`). So an RWX anonymous mapping genuinely executes in
  user mode on both arches. ✓
- Arena: user window 32 GiB with an **18 GiB mmap arena** (`src/include/vm.h:27-29,47-48`); a 1 GB
  (x64) or 128 MB (ARM64) JIT pool fits virtually. Physical RAM is demand-paged 4 KiB; only real
  code pages commit (JIT pools commit ~KB–MB, not the reservation size). ✓

Cache/maintenance for ARM64 at runtime:
- Kernel has a full `__clear_cache` (dc cvau + dsb ish + whole-I-cache invalidate, EL1-safe)
  `src/kernel/arch/arm/mmu.c:447-458`. There is **no userland-visible cache-flush syscall row today**
  and no evidence EL0 `dc cvau/ic ivau` is enabled (no SCTLR_EL1.UCI set — **UNVERIFIED**, but the
  OS sets minimal EL1 state). So the ARM64 JIT needs one of:
  (a) a tiny `SYS_CACHEFLUSH` row routing to the kernel `__clear_cache` (recommended, TCG-agnostic),
  (b) enabling UCI so JSC can emit inline `dc cvau`/`ic ivau`, or
  (c) rely on QEMU TCG not modeling I-cache incoherence (see §5.2).
- **TCG note (critical):** QEMU TCG does not model incoherent I-caches; it invalidates translated
  blocks on guest stores to addresses with live TBs (self-modifying-code detection). A JIT that
  writes into an RWX page and runs it works under TCG even if the explicit flush is a no-op. The
  flush still matters for correctness on real hardware (future bare-metal, KVM x64 is unaffected).

Gaps → precisely:
- **None** on x64: mmap RWE / mprotect / madvise / munmap all present with matching semantics.
- **ARM64**: the JSC `cacheFlush` `#error` (`ARM64Assembler.h:4047`) must be resolved, and a
  userland cache-flush route must exist (§8 edit E5). That is the only OS-level prerequisite unique
  to ARM64 JIT.

---

## 3. Threading / RunLoop requirements

- `ThreadingPOSIX.cpp` is compiled by the port (`PlatformHobbyOS.cmake:29`) and already carries
  `OS(HOBBYOS)` branches (`ThreadingPOSIX.cpp:75-77`) — pthreads work (the windowed driver runs a
  two-thread split, `WK5WindowDriver.cpp:2821-2831`). Futexes exist (P1, ground state).
- JIT worklist: `JITWorklist` spawns up to `Options::maxNumberOfWorklistThreads` real threads
  (`jit/JITWorklist.cpp:59-61`); defaults `min/maxNumberOfWorklistThreads =
  computeNumberOfWorkerThreads(3, 2)` (`runtime/OptionsList.h:288-289`) — i.e. 2-3 pthreads
  (`Options.cpp:436-447`, non-Darwin cap 32). GC parallel markers default `computeNumberOfGCMarkers(8)`
  (`OptionsList.h:402`) → small on low core counts. No OS-specific guard found that would break on
  HobbyOS. `useConcurrentJIT` defaults true (:287) — under 1-vCPU TCG it is pointless but harmless;
  recommend `--useConcurrentJIT=false` in TCG measurements (compile-on-main costs are then explicit).
- Watchdog is **signal-free** in this tree: `Watchdog::startTimer` uses `VMTraps::queue().dispatchAfter`
  (`runtime/Watchdog.cpp:129-153`) — thread/timer-queue based, not SIGALRM. ✓
- Signals: `threads/Signals.cpp` is `#if OS(UNIX)` (:29) and the OS has **no signal delivery at all**
  ("every signal except 0 ignored", `docs/browser/p5-exec-signals-design.md:68-72`; P5.3 design-only).
  This is the DFG-tier gate (§5.3). LLInt + baseline JIT use explicit inline stack checks and compile
  fine without WTF Signals (as the current C_LOOP build already does); the DFG tier's POSIX
  stack-overflow/OSR signal paths are the risk spot — keep DFG off until a signal row lands.

---

## 4. The windowed-shell page-script gate (and the RLCONF spin)

- **The gate:** `HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp:2814`
  `webPage->corePage()->settings().setScriptEnabled(false);` — set to stop a first-script JSC hang
  (comment :2807-2813). WebCore's enforcement: `ScriptController::canExecuteScripts`
  (`Source/WebCore/bindings/js/ScriptController.cpp:842-863`) — non-normal worlds return true
  (:849-850), the normal world is gated by `loader().client().allowScript(settings().isScriptEnabled())`
  (:862). Driver JS therefore runs in an **isolated world** (`WK5WindowDriver.cpp:422-426`; `executeScriptInWorldIgnoringException` at :1901-1903, :1956, :1982, :2518, :2540) which bypasses the gate — matches the fs-r4 findings (commit `b3ef10222e`, `fs-r4-REPORT.json:7,31`).
- **The RLCONF spin** (history): a ~4.8 KB inline `RLCONF` runtime-config script on the full-skin
  page spun ~100% CPU for 30+ min under **TCG + interpreter-mode (C_LOOP) JSC**
  (`WK5WindowDriver.cpp:2807-2813`). It was a hang, not a test of page JS being required — the T2
  gate explicitly didn't need JS. Note the isolated-world eval also "aborted under TCG"
  (`WK5WindowDriver.cpp:1262-1269`) — separate symptom of the same interpreter perf/slowdown
  problem, which is precisely what LLInt/JIT targets.
- **Control points to flip for page JS:**
  1. `WK5WindowDriver.cpp:2814` — `setScriptEnabled(false)` → `true` (or remove) after tier validation.
  2. `Source/WebCore/loader/FrameLoader.cpp:1006-1016` — the `OS(HOBBYOS)` skip of the
     `ScriptDisallowedScope` RELEASE_ASSERT was justified **only while no script can run**;
     re-enable (or scope-guard) it once page scripts run, or re-entrant `checkCompleted` can fire
     inside a script-disallowed scope (RELEASE_ASSERT with security implication — a real trap).
  3. Subresource/loader assumptions that lean on "no scripts": `FrameLoader.cpp:1006-1013` comment
     chain, and R1c's parser-completion fix history (fs-r1c, `fs-r1c-REPORT.json:14-18`) — scripts
     becoming live re-opens the parser-blocking-script path the port worked around by disabling JS.
     Re-verify `readyState=Complete` with scripts on.
  4. The `[WIN] load-ok`/paint sequencing (R7) assumed zero script-induced work between last
     subresource and completion — script exec between those markers may re-pend completion; keep
     the `requestCount==0 && !parsing` completion check honest instead of relying on the assert skip.
- Risks when flipping: (a) any interpreter/JIT hang reappears in a different form under page scripts
  (must validate RLCONF + a CNN corpus in the `jsc` harness first); (b) re-entrancy/assert skips
  above; (c) render slowness from script-driven layout (CSSOM/DOM churn) even when the JS engine is
  fast — new measurement surface, not a correctness blocker.

---

## 5. Cost/benefit per environment + measurement plan

### 5.1 Environment facts
- **x64 runs under KVM (native)** — JIT compile cost is real-host cost; all tiers are cheap at
  compile time. Best first target.
- **ARM runs under QEMU TCG (emulated)** — guest instructions (interpreter *and* JIT-compiled code)
  both run at TCG's per-instruction emulated cost; JIT *compilation* itself is also emulated guest
  work (assembling, memory writes, cache flush) — so compile cost is multiplied by roughly the TCG
  slowdown factor vs a native build of the same logic.

### 5.2 Reasoning
- **LLInt-native vs C_LOOP** (same CPU): fewer C-level indirections, register-pinned interpreter
  state, computed-goto not needed — historically 2-6× on hot loops. Under TCG the multiplier is
  damped because the emulated instruction cost dominates, but it is still the single cheapest
  guaranteed win and it needs no exec-memory OS support at all.
- **Baseline JIT vs LLInt**: JIT per-opcode code is more direct but the compile step must be paid
  guest-side. For CNN-scale pages (DOM/JSON-heavy, mostly cold one-shot scripts) baseline-JIT
  compile may not amortize under TCG; on KVM-x64 it nearly always wins.
- **DFG vs baseline**: only pays for sustained hot loops. On a 1-vCPU TCG guest, concurrent JIT
  cannot hide compile cost and the signal-based OSR/stack machinery is unavailable (§3) — DFG on
  ARM-TCG is not recommended this milestone. On x64-KVM, DFG is the expected end state.
- **TCG top-line**: for script-heavy-but-cold page load, the practical ARM ceiling is **native
  LLInt, or baseline JIT with `--useConcurrentJIT=false` and a `useDFGJIT=false` pin**; measure
  before choosing. The question the milestone asks ("does JIT beat LLInt on TCG for CNN-scale
  loads?") is empirically answerable only by the A/B below — no a-priori win is guaranteed because
  compile cost is also emulated.

### 5.3 Recommended order + A/B commands (do NOT run — specify only)

Builds (two, one checkout each):
- **Build A — C_LOOP (today)**: `bash third_party/webkit-hobbyos/build.sh --arch arm --targets "jsc"`
  (also `--arch intel`). (`third_party/webkit-hobbyos/build.sh:8-11,42,182-183`; `jsc` target exists:
  `Source/JavaScriptCore/shell/CMakeLists.txt:1-6`, gated by `ENABLE_JAVASCRIPT_SHELL` default ON,
  `WebKitFeatures.cmake:235`.)
- **Build B — JIT**: same command after the §8 config edits (JIT ON, C_LOOP OFF, DFG/FTL OFF);
  option: `-DFIXED_EXECUTABLE_MEMORY_POOL_SIZE_IN_MB=64` (keep the pool small — §2.1/§6).

Tier matrix on **Build B** (one binary, runtime knobs from `OptionsList.h`):
- native LLInt: `jsc --useJIT=false --useLLInt=true` (compiled JIT ON, runtime JIT off — the LLInt
  still runs the native asm interpreter).
- baseline JIT: `jsc --useJIT=true --useBaselineJIT=true --useDFGJIT=false --useFTLJIT=false
  --useConcurrentJIT=false`
- DFG probe (x64 only): `jsc --useDFGJIT=true`

Benchmark corpus (fixed, both arches):
1. SunSpider 1.0.2-style micro suite (or the fork's existing fixture if present — **UNVERIFIED**).
2. JSON/DOM-ish microbench (e.g. `JSON.parse/stringify` loop + `document.createElement` storm is a
   page-adjacent proxy; if a WK5 fixture page exists, use it).
3. The exact 4.8 KB `RLCONF` script (recovered from the full-skin page corpus) run in a loop N times
   with a wall-clock timeout — this is the historic hang repro, must be first to go green.
4. A real CNN page-script bundle (the `browser.md:1555` CNN corpus/`5,707,014 B` fetch already
   exists in the evidence tree as a raw corpus).

Metrics: per-bench wall time (guest ticks via the OS clock probe), `--dumpBytecodes`/tier counters
to confirm which tier ran, and `jsc --dumpDisassembly` spot-check; on-shell: page load-to-`[WIN]
load-ok` marker delta with the gate flipped (§4). Run each config 3× on TCG-ARM (and KVM-x64 for the
x64 branches), report median.

Expected signals: `LLInt-native < C_LOOP` everywhere (biggest on policy-bench loops); `baseline`
beats LLInt on x64-KVM, ambiguous-or-loses on TCG for cold script loads; DFG wins only on sustained
loops (probe-only on x64).

### 5.4 Gate ordering (recommended)
1. x64: LLInt-native (Build B `--useJIT=false`) — zero OS risk, immediate win.
2. x64: baseline JIT + DFG — exec-mem path is fully supported (§2.2), enable DFG after baseline is
   stable; this is the KVM-native flagpole.
3. ARM: LLInt-native — needs only Build B; no cacheFlush involvement for LLInt (it runs from .text).
4. ARM: baseline JIT — **after** edit E5 (cacheFlush) + the OS `SYS_CACHEFLUSH` row (§8); TCG
   measurement decides keep-vs-`useJIT=false`.
5. ARM DFG: **defer** until OS signal delivery (P5.3) or a non-signal DFG config is validated.

---

## 6. Estimate, budget, risk

- **Files touched:** ~10-14 total: 2 config (`OptionsHobbyOS.cmake`, `PlatformHobbyOS.cmake`
  FIXED_EXECUTABLE_MEMORY_POOL_SIZE_IN_MB), 2-3 JSC/WTF source
  (`ARM64Assembler.h` cacheFlush; optional `OSAllocatorPOSIX.cpp` NORESERVE/commit branch alignment;
  optional `PlatformOS.h` D-5 note), 1-2 OS side (syscall row + libc wrapper for ARM cache flush),
  2 shell/driver (`WK5WindowDriver.cpp:2814`, `FrameLoader.cpp:1006-1016` re-enable assert), 1-2
  docs. No offlineasm, no new JIT files, no new WTF platform files.
- **LOC:** ~150-400 (mostly the ARM cache-flush shim + config deltas; the C_LOOP→LLInt/x64-JIT path
  is almost pure build-flag flip).
- **Binary/space risk:** flat image must stay under `USER_IMG_SIZE 128 MiB` (`vm.h:44`); the ARM
  flat was ~80 MiB and the first intel ELF was ~183 MB (`browser.md:1566`) — JIT adds thunks/code
  (~5-20 MB). Verify the flat size each tier; if the cap is hit, raise `USER_IMG_SIZE` (one define,
  precedent `vm.h:33-44` note) or trim feature surface.
- **Pool budget:** 1 GB x64 default pool is virtual-only and fits the 18 GiB arena, but pin
  `FIXED_EXECUTABLE_MEMORY_POOL_SIZE_IN_MB` (64-128) to match the f0-budgets RAM style and avoid
  foot-guns; resolves the standing TODO (`PlatformHobbyOS.cmake:14-16`).

### Risk list
| # | Risk | Level | Mitigation |
|---|------|-------|-----------|
| R1 | ARM64 `cacheFlush` `#error` (compile) | Med | E5 + OS `SYS_CACHEFLUSH` (both present, small) |
| R2 | DFG/Final tier needs OS signals; none exist | Med-High | Keep DFG off; LLInt/baseline don't need signals (watchdog is thread-based, §3) |
| R3 | `FrameLoader.cpp:1006-1016` assert-skip becomes unsafe with live scripts | Med | Re-enable as part of the same change set |
| R4 | Historic RLCONF hang class reappears under a tier | Med | Repro in `jsc` with timeout before flipping `:2814` |
| R5 | TCG compile cost makes ARM baseline/DFG net-negative | Med | A/B decides; default ARM to LLInt if so |
| R6 | Image size over 128 MiB cap | Low-Med | Guard in CI; raise define if needed |
| R7 | `OS(HOBBYOS)` branch drift (D-5 style) at new JIT sites not enumerated here | Low | Compile gate on both arches per tier keeps it visible |

---

## 7. Bottom line per tier

- **LLInt (both arches):** verdict **green to attempt** — pure build-flag change
  (`OptionsHobbyOS.cmake:213-218`), no OS prerequisite beyond what exists, no exec-mem syscalls.
  Immediate flagpole on x64 (KVM) and the TCG-ARM ceiling candidate.
- **Baseline JIT:** verdict **gated** — all exec-mem syscalls present and correct on x64 (KVM path
  ready); ARM requires edit E5 (cacheFlush) + OS cache-flush row. Recommend x64 first, ARM gated on
  the TCG A/B.
- **DFG:** verdict **gated** — x64 viable after baseline stabilizes; ARM-TCG deferred (signals,
  compile-cost).

---

## 8. Concrete change list (file:line referenced, proposed)

**E1 — `Source/cmake/OptionsHobbyOS.cmake:213-218`** (build gate)
```cmake
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_JIT PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_C_LOOP PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_DFG_JIT PRIVATE OFF)   # ON later (x64 DFG probe)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_FTL_JIT PRIVATE OFF)
```
(keep WEBASSEMBLY/SAMPLING_PROFILER OFF; WEBASSEMBLY conflicts with C_LOOP at
`WebKitFeatures.cmake:337`, SAMPLING_PROFILER at :336 — both already OFF.)

**E2 — `Source/JavaScriptCore/PlatformHobbyOS.cmake:14-16`** (resolve the standing TODO)
```cmake
add_definitions(-DFIXED_EXECUTABLE_MEMORY_POOL_SIZE_IN_MB=64)
```
(else x64 defaults to a 1 GB virtual reservation, `ExecutableAllocator.cpp:106-107`.)

**E3 (optional) — `Source/WTF/wtf/posix/OSAllocatorPOSIX.cpp:93-100`** — add `|| OS(HOBBYOS)` to the
`OS(LINUX)||OS(HAIKU)` branch for identical NORESERVE/`madvise` semantics; functionally not required
(HobbyOS demand-pages) — keep for exactness.

**E4 — OS: ARM64 cache-flush route** — add `SYS_CACHEFLUSH` row (dispatch in
`src/kernel/arch/{arm,x64}/trap.c` alongside :1792-1804/:1685-1705, impl calling the existing
`__clear_cache` `src/kernel/arch/arm/mmu.c:447-458`; no-op on x64) + libc wrapper
(`src/libc/src/mman.c` pattern :141-149). Alternatively set SCTLR_EL1.UCI and let EL0 use
`dc cvau`/`ic ivau` directly — the syscall route is TCG-agnostic and preferred.

**E5 — `Source/JavaScriptCore/assembler/ARM64Assembler.h:4044-4048`** — add before the `#else #error`:
```cpp
#elif OS(HOBBYOS)
        cacheFlushHobbyOS(code, size);   // -> SYS_CACHEFLUSH (kernel __clear_cache semantics)
```

**E6 — `Source/WebCore/loader/FrameLoader.cpp:1006-1016`** — re-enable the `ScriptDisallowedScope`
RELEASE_ASSERT for the JIT milestone (or keep the OS(HOBBYOS) skip only for the interpreter builds).

**E7 — `HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp:2814`** — flip `setScriptEnabled(false)`
→ `true` (or `allowScript` default) after the `jsc` RLCONF/CNN repro is green; re-run the fs-r4/r7
nav + completion gates.

**E8 — docs** — record D-5 exception: JIT adds `OS(HOBBYOS)` branches; do not set `WTF_OS_UNIX`.

---

*Lane PJ · read-only · companion data: JSON summary returned as the channel's final message.*
