# Lane L2 — OS-side gap audit: can HobbyOS host clang/lld?

**You are lane L2 of the HobbyOS self-hosting program.** Your COMPLETE instructions are
this file. Read `~/hobbyos-selfhost/selfhost.md` §0, §3, §4, §5 (AD-4..AD-6), §7 — this
brief is its L2 slice and binds you.

## Identity & tree

- **Worktree:** `/home/sarah/hobbyos-selfhost-lanes/l2` (branch `selfhost/l2-os-audit`,
  based on `main` of `/home/sarah/hobbyos-selfhost`).
- **NEVER touch the workstation repo** `/home/sarah/Documents/GitHub/HobbyOS`. **No pushes.**
  Local commits on your branch only.
- Read-only lane for the HobbyOS tree itself (you audit; you do not fix HobbyOS code —
  fixes are itemized as work items for wave-2 lanes).
- Skills to load: `hobbyos-development` (userland API + libc surface facts),
  `hobbyos-kernel-constraints` (what the kernel can/can't do — context for gaps).

## Mission

Produce the definitive **audit of HobbyOS's runtime surface vs what clang/lld/llvm-ar/
llvm-objcopy need when they RUN AS HobbyOS USERLAND PROGRAMS**: every missing/partial
libc function, syscall, header, or behavior, each anchored to file:line, each with a
suggested fix location and a size estimate. L1's `docs/selfhost/lanes/l1-dep-surface.txt`
is a *starting point if it exists* — verify and extend; do not depend on it having landed.

## Method

1. **Get your own LLVM source copy** (scratch — do NOT extract into the repo):
   reuse the container `~/hobbyos-selfhost/third_party/libcxx-21.1.8/llvm-project-21.1.8.src.tar.xz`
   → extract `llvm-project-21.1.8.src/{llvm, clang, lld}` into
   `~/.hermes/cache/scratch/llvm-osaudit-src/` (~1-2 GB; fine). This is your grep source;
   cite paths relative to it as `llvm/...`, `clang/...`, `lld/...`.
2. **Enumerate the OS-facing surface per component** using the source, not memory.
   Priority files: `llvm/lib/Support/Unix/{Process,Path,Memory,Signals,Threading,
   DynamicLibrary,Watchdog,Program,Time,RandomNumberGenerator,Host,Main}.inc`,
   `llvm/lib/Support/*.cpp` (CommandLine/Program/FileSystem), `llvm/include/llvm/Config/
   *.h.cmake` (feature-detection truth — only compiled branches matter),
   `clang/lib/Driver/*.cpp` (ToolChains/Driver/Compilation), `clang/tools/driver/*`,
   `lld/Common/*`, `lld/ELF/Driver.cpp`, `lld/MachO` (skip), plus `llvm/lib/Support/
   PrettyStackTrace` / crash handling (what does it call on signal?).
   Build the call inventory: function + how used + example `file:line`.
3. **Check each against HobbyOS** — anchors REQUIRED from the actual tree:
   - sysroot headers: `src/libc/include/**` (`unistd.h`, `fcntl.h`, `sys/stat.h`,
     `signal.h`, `pthread.h`, `mman.h`, `poll.h`, `dirent.h`, `stdio.h`, `stdlib.h`,
     `string.h`, `time.h`, `errno.h`, `sched.h`, `sys/wait.h`, `sys/types.h`…)
   - implementations: `src/libc/src/*.c`; userlib: `src/user/libc.c`,
     `src/user_include/libc.h`; syscalls: `src/include/syscall.h` + both
     `src/kernel/arch/{arm,x64}/trap.c` dispatch.
   - Record per item: **FULL / PARTIAL / MISSING**, anchor, note (e.g. "stat has no
     st_blocks", "no access()", "no symlink (N/A?)", "fstatat absent").
4. **Runtime-flow analysis for the clang DRIVER specifically** (this is the highest-value
   part — the driver is what runs first):
   - How the driver invokes cc1: `posix_spawn` vs `fork+execve`; what the 21.1.8 config
     does when `HAVE_POSIX_SPAWN` is 0 (`clang/lib/Driver/Driver.cpp`, `Job.cpp` →
     `llvm::sys::ExecuteAndWait`); what `ExecuteAndWait` needs (`Process.inc`: fork, execv,
     waitpid, pipes, signals).
   - How cc1 is located (`Driver::GetProgramPath`, argv0-based search) — confirm that
     `-fintegrated-cc1` (check `clang/lib/Driver/Driver.cpp` for the option in 21.1.8) or
     an explicit `-resource-dir` makes it path-independent (no `/proc/self/exe` on HobbyOS).
   - Resource-dir header resolution (which builtin headers cc1 actually opens for our
     freestanding flag sets — cross-check with the Makefile's include flags lines 140-260).
   - Temp-file behavior (`llvm/lib/Support/Path.cpp` + `clang/lib/Driver/Compilation.cpp`:
     where temp files go without `/tmp` env; deterministic names needed?).
   - `isatty`/terminal, locale (`setlocale`), env vars read (`TMPDIR`, `PATH`, `HOME`?),
     `getcwd`, `getenv` usage; `uname`?; `sysconf` calls (`_SC_NPROCESSORS_ONLN`,
     `_SC_PAGESIZE`, …) — which are reachable in our build config.
   - lld specifics: threading (`llvm/lib/Support/ThreadPool` → pthreads ✓ but note any
     `std::thread` + futex needs, `--threads` flag), memory maps of outputs
     (`FileOutputBuffer`: mmap private/shared, fallocate!!, ftruncate, rename-as-commit!),
     file locking (`FileLocker`: fcntl locks) — flag every one.
   - llvm-ar/llvm-objcopy specifics: file I/O patterns, `lstat`, `chmod`, `utimes`,
     `readlink` (ignore-if-absent analysis).
5. **Make the blockers actionable**: for every MISSING/PARTIAL, write a work item:
   `W2-x: <fix> — <file to change> — size S/M/L — priority (M1-quick / M2-pre-smoke /
   M2-when-hit / M5)`. Quick ones (small libc shims) get proposed exact implementations in
   the doc (they'll be lifted by the wave-2 fix lane). Syscall additions are flagged as
   single-writer items (parent writes header + both trap.c).

## Deliverable

`docs/selfhost/os-gap-audit.md` + commit. Structure:

- §1 Summary table (count per status; the top-10 riskiest seams ranked with reasons).
- §2..§N per-area tables (filesystem, process/exec, memory, signals, threads, env/time,
  tty, misc) — each row: {call, used-by, status, HobbyOS anchor, work item id, note}.
- §Last: the work item list (W2-x), ordered by priority; plus explicit "no action" list
  (things that look scary but are unreachable in our build config — with the reason).

## Constraints (hard)

- No QEMU, no repo builds needed; do not modify `src/**`, `Makefile`, `selfhost.md`,
  `.gitignore`, other lanes' paths. Downloads no; your extraction source is local.
- Commit cadence ≈ every 30-45 min; subjects `selfhost: l2: <what>`; stop at a committed
  checkpoint if budget nears.
- Claims without an anchor don't go in the doc. If you couldn't verify → mark it
  `VERIFY` with the exact command someone should run.

## Report (write + validate before finishing)

`docs/selfhost/lanes/l2-report.json` (commit; `python3 -m json.tool` it):

```json
{"lane": "l2", "status": "complete|partial|blocked",
 "commits": ["sha"], "artifacts": ["path"], "evidence": ["path"],
 "blockers": ["..."], "next_steps": ["the wave-2 fix order"],
 "notes": "counts: FULL/PARTIAL/MISSING; top seams"}
```
