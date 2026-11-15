# java.md — proving out the OpenJDK port, surface by surface

OpenJDK 27 (Adoptium 27.0.1.0.2+1, linux/x64 hotspot) runs on the x86_64
Linux-ABI layer: it starts, compiles with `javac`, runs what it compiled, and
passes `tools/jdk/tests/Verify.java` 18/18 with class-data sharing on and off.
Gate: `tools/jdk/sdktest.sh` (version / javac / run / suite), 4 boots.

That is a **proven subset**, not "Java fully works". This plan exists to replace
the word "believe" with evidence, one workstream at a time. Every workstream
runs real programs in the guest and records the console as the witness.

## Rules that keep the evidence honest

1. **One program per boot.** `ARCH=intel MODE=test` dispatches every `/WAVE.CFG`
   line concurrently, and the console is the only output channel. A "test" is
   one program, one boot, one log, one deadline.
2. **Never stage and boot at the same time.** A guest reading a half-written FAT
   image looks exactly like an infinitely slow program.
3. **`boot.sh` loads `hobbyos.elf` from inside `disk.img`.** A kernel you did not
   repack is a kernel you did not test. Use `cycle.sh` (build + repack + boot)
   when the kernel changes; use `stage.sh` + `boot.sh` for pure payload changes.
4. **A hang is a failure, not a flake.** `boot.sh` escalates TERM to KILL and
   exits non-zero when the marker never appears.
5. **Workers do not edit `src/`.** Parallel workers share one kernel; a fix is
   planned and applied once, centrally, after the diagnosis is on the table. A
   worker's job is the experiment and the diagnosis, not the patch.
6. **Report the log path with every claim.** No number without a file behind it.
7. **A boot is only evidence if the kernel you booted contains your edit.** Twice
   now a "result" came from a stale kernel: first `boot.sh` was loading
   `hobbyos.elf` from inside `disk.img`, so a rebuild that never repacked was
   invisible; then a compile error (`array type uint64_t[4] is not assignable`)
   left the previous `hobbyos.elf` in place and three boots silently measured the
   old code. After any kernel change: check the build's exit code, and
   `strings hobbyos.elf | grep '<a literal you just added>'` before believing a
   single line of console output.
8. **Program stdout is NOT ordered by execution order.** The wave driver is
   strictly sequential -- `[WAVE-TRY] MMapTouch`, `[ARRIVED]`, `[WAVE-TRY]
   PlainRead`, `[ARRIVED]` (`obj/jdk/mmaptouch.log:109-305`) -- but MMapTouch's six
   output lines are written to the log AFTER both PlainRead runs that followed it.
   A program's buffered stdout can surface anywhere. Attribute output by its own
   prefix (`MT `, `PR `), never by where it sits relative to the driver's markers,
   and read execution order off `[WAVE-TRY]`/`[ARRIVED]` only. Anything that
   depends on ordering between two programs' prints is unmeasurable.

9. **The harness runs one program at a time, so "concurrency" needs fork.** Wave
   lines are launched sequentially, and record locks are keyed per process, so
   neither two wave lines nor two threads in one JVM can produce contention. Any claim about blocking,
   cross-process locking or parallel writers is untestable until fork exists --
   say "unverified", not "fixed".

## Workstreams

Each workstream owns a git worktree and its own `obj/`, so builds and boots never
collide. Shared read-only inputs: the JDK at
`/Users/sarah/Documents/GitHub/HobbyOS-openjdk/third_party/jdk-27` and the glibc
2.31 pin (both referenced by absolute path from `tools/jdk/stage.sh --jdk`).

| # | Surface | Why it is on the list | Done means |
|---|---|---|---|
| 1 | Virtual threads, park/unpark, monitors | JDK 21+ flagship; leans hardest on the park/wake and futex paths that were broken here (`exit_group`, parked-thread teardown). Zero coverage today | 10k virtual threads (blocking sleep + queue handoff + structured concurrency) run to completion and the VM exits on its own; thread dumps consistent |
| 2 | `ProcessBuilder` / exec, pipes, signals | fork+exec of a child JVM is a different path from `Runtime.exec` of a static binary; exit codes, pipe EOF and SIGTERM/SIGINT handling are all untested | child JVM runs and its stdout is captured; non-zero exit propagates; SIGTERM runs shutdown hooks; no lost output |
| 3 | Mapped files, file locks, channels | kernel-internal mmap works (jimage), *user* mmap does not have evidence; `fcntl` locks and `transferTo` are unexercised | `MappedByteBuffer` read/write/`force()` round-trips; two VMs contend on a lock file with the expected blocking; `transferTo/From` and `truncate` correct |
| 4 | JDK tooling and attach | `keytool`/`jar`/`jlink`/`javadoc`/`javap`/`jshell` and `jcmd`/`jstack` exercise crypto, zip, module images, `javax.tools` and the attach mechanism | each tool produces its documented artifact; attach either works or fails with a precise, explainable reason |
| 5 | jtreg tier1 subset | the honest meaning of "standard tests": OpenJDK's own suite, not our 18 checks | a bounded, named subset of tier1 runs in the guest with a pass/fail/skip count and every failure classified |

## Blockers found (missing primitives, not bugs)

| Primitive | Evidence | Consequence |
|---|---|---|
| `fork` / `execve` in the Linux-ABI layer | `java.io.IOException: Cannot run program "/JDK/bin/java": fork failed, error: 38 (Function not implemented)` (`obj/jdk/B7-p1spawn-fork.log`, worktree `HobbyOS-jdk-exec`) | `ProcessBuilder`/`Runtime.exec` can never start a child process until the ABI grows fork+exec. The kernel can spawn its own `.BIN` programs (`SYS_SPAWN`); the Linux-ABI syscall surface has no equivalent |
| executable bit on FAT16 | same probe: `/JDK/bin/java -> exists size=16296 exec=false` | Even with fork available, a Java caller sees every binary as non-executable; worth knowing before implementing exec |

## Known pre-existing blocker (not this port)

`tools/t3_tier.sh intel` is RED on this host **with and without** the port work:
the baseline kernel wedges with `[IDLESTUCK] lock_addr=0x748D8148` (`proc_lock`)
and `[YLOCK-WEDGE] fat_wr_lock` with a writer parked in `SYS_WRITE`. That is an
SMP lock-bookkeeping bug on TCG. Consequences for this plan:

- A single JVM per boot is trustworthy. **Two processes running concurrently
  beside each other are not** — treat multi-process results as suspect until the
  wedge is fixed.
- Any workstream that needs two VMs at once (file locks) must say so explicitly
  and validate its result a second way (e.g. lock contention inside one VM).

## Results log

Append one row per experiment as it lands: workstream, probe, verdict, log path.
Failures stay in the table — a deleted failure is a lost bug report.

| # | probe | verdict | log |
|---|---|---|---|
| — | (baseline) `sdktest.sh` on `72477b57` | 4/4 PASS, suite 18/18 | `obj/jdk/sdk-*.log` |
| 5 | 19 OpenJDK test sources (`java/lang/{String,Integer,Math}`), one VM | **19/19 PASS in one clean run** (`SUMMARY pass=19 fail=0 total=19`); `Split` 21.3 s, the rest 19 ms–1.7 s. An earlier 15 s cap misreported `Split` as a hang — the cap was wrong, not the VM | `obj/jdk/jtreg-subset2.log` (clean), `obj/jdk/jtreg-subset.log` (first, capped run) |
| 5a | `Split` alone, 240 s budget, default JIT | **PASS, elapsed=20094 ms** | `obj/jdk/split-jit.log` |
| 5b | `Split` alone, 240 s budget, `-Xint` | HANG-after-240s — consistent with the interpreter costing ~10-30x the JIT, i.e. 400 s+ for this test | `obj/jdk/split-int.log` |

### Workstream 5 method notes

**Lesson recorded the hard way: a timeout is a budget, not a verdict.** `String/Split`
was reported as a hang on the strength of a 15 s cap. Given a 240 s budget it
passes in 20.1 s — 24x the host's 0.82 s, which is the ordinary TCG emulation
factor for CPU-bound string/regex work, not a defect. Before calling anything a
hang, measure what it costs when given room; the `-Xint` run then behaved exactly
as the model predicts (interpreter ≫ JIT, so it exceeds even 240 s).

- Tests are OpenJDK's own sources, fetched with a blob-filtered sparse checkout of
  `github.com/openjdk/jdk` (`test/jdk/java/lang/{String,Integer,Math}`), compiled
  with the host JDK and run in-guest. They are the real assertions, not rewrites.
- Three adaptations, all recorded because they change what a PASS means:
  `Tests.java` needed the `jdk.internal.math.DoubleConsts/FloatConsts` imports that
  jtreg injects via `@modules` (logic untouched); `Encodings` was dropped because it
  uses `String.encodedLength(Charset)`, a JDK 27 API the host javac 26 cannot
  resolve; the runner needs `--add-exports java.base/jdk.internal.math=ALL-UNNAMED`
  at both compile and run time.
- One VM for all 19 tests (the one-program-per-boot rule), each test on a daemon
  thread with a timeout. A test that calls `System.exit` would take the runner with
  it, so those are excluded — jtreg forks per test and this does not.
- The host baseline is run first, always: 19/19 on the host, so a guest failure is
  the guest's.

## Workstream results (parallel agent runs, 2026-11-14)

Each worker built its own worktree and ran its own boots; the probes and logs are
committed on `jdk-vt`, `jdk-exec`, `jdk-mmap`, `jdk-tools`.

### 1. Virtual threads — WORKING, no kernel change needed

| probe | verdict | evidence |
|---|---|---|
| `VtCore` (9 checks) | **9/9** | two initial failures (`null threadGroup` after termination, `dumpAllThreads` listing 0 virtual threads) reproduced **identically on host JDK 26** — JDK-specified behaviour, not a kernel bug |
| `VtDump` (4) | **4/4** | `ThreadContainers.root()` walks 50 virtual + 8 platform threads; `VirtualThreadSchedulerMXBean` parallelism=8 poolSize=8; a parked VT is WAITING with the correct 5-frame stack |
| `VtLocks` (12) | **12/12** | 400k contended monitor increments exact on both thread kinds; 20k items through a 4-slot monitor buffer; `wait(300)`→305 ms; `notifyAll` handoff 205 ms; 2000 park/unpark handoffs in 301 ms; pre-granted permits honoured; 64 VTs holding per-VT monitors across `sleep(100)` → **no pinning (JEP 491)**, while `Object.wait` pins as specified |
| `VtScale` 10k / 50k | **PASS** | 10k created 1480 ms, heap 17 MB; 50k created 2145 ms, finished 3536 ms, heap 54 MB; blocking-queue totals exact (50 005 000 and 1 250 025 000) |
| `VtExit` (3) | **3/3** | `System.exit(3)` **from a virtual thread** runs the shutdown hook and the kernel reports `[EXITPC] code=0x3`; 20 VTs × 5 concurrent file reads byte-exact |
| structured concurrency | **not run** | preview types renamed in 27; the guest's own `javac --enable-preview --release 27` compiles it cleanly, the run boot is missing |

### 2. Process creation — BLOCKED on missing primitives, pipes fine

| probe | verdict | exact evidence |
|---|---|---|
| kernel loader baseline (`LEAF.EXE --exit=42`) | **PASS** | `[EXITPC] code=0x2A` — the loader is fine, so exec's target is not the problem |
| `PIPE` (6 checks) | **6/6** | 300 KB round-trip in 167 ms, byte-exact, EOF on last-writer close, blocking read waits, `EPIPE` → `IOException: Broken pipe` |
| glibc `fork()` | **FAIL ENOSYS** | `[LCOMPAT] clone refused flags=0x1200011 (no CLONE_VM\|CLONE_THREAD...)` → `errno=38` |
| `posix_spawn` | **FAIL ENOSYS** | `clone refused flags=0x4111`; this is the JDK's *default* launch mechanism |
| raw `fork`+`execve` | **BROKEN CHILD** | child misrouted every syscall (`close`→`SYS_FORK`, `dup2`→`SYS_FTRUNCATE`), user fault vector 13, and silently spawned grandchildren at pids 3,4,5 |
| `execve` itself | **implemented** | maps the image (`v2 loader: image mapped … bytes=18680`), but the exec'd *dynamic* image then faults at `VA=0, PC=0x1000000047` |

### 3. mmap / file locks / channels — NO BOOT (host build defect)

The worker never got a guest run: `make` dies in the ICU host build because
`Makefile:1331` gates `ICU_CC` on `$(filter macos,$(OS))` while `OS` is never
defined, so `ICU_CC` inherits the kernel compiler and the stale CommandLineTools
SDK then fails to link (`ld: tapi error: malformed file ... libSystem.B.tbd:
unknown architecture`). Workaround that works: `make OS=macos` with
`SDKROOT=/Applications/Xcode.app/.../MacOSX.sdk` exported. **Two of four workers
burned their entire budget on this**, so it belongs in the build system, not in
each worker's head.

Source-level hypotheses for the next run (not boot evidence):
`MAP_SHARED` on FAT16 never reaches disk (`file_obj.c` has no write path);
any FAT16 write drops the whole page cache; **`F_SETLKW` returns `-EAGAIN`
instead of blocking** (`src/kernel/fs.c:1214`), so `FileChannel.lock()` may hand
back a `FileLock` the kernel never granted; an empty file's `map()` should raise
`IOException`.

### 4. JDK tooling — partially usable, two kernel defects on the path

| tool | verdict | evidence |
|---|---|---|
| guest `javac` | **PASS** | `/JDKOUT/CLASSES/Att.class` 5079 B + `CLASSES/demo/Welcome.class`, exit 0, clean halt, 19 s |
| `keytool -help` | **PASS** | real help text, exit 0, 11 s |
| `keytool -genkeypair` (18 args) | **FAIL** | `[LCOMPAT] argv argc=16` → `-storepass` truncated → "password is too short" ×3 |
| `keytool -genkeypair` (12 args) | **HANG** | `[YLOCK-WEDGE] addr=0x702B4650` = `f1_pos_lock`, owner `pid=27 osys=17` (pread64) |
| `jlink` | **impossible here** | the pinned JDK ships **no `jmods`** (464 entries, none on disk) |
| jar / javadoc / javap / jshell / jcmd / jstack / jmap | **not run** | iterations exhausted; payload staged |

### Kernel fixes applied from these findings

| fix | file | why |
|---|---|---|
| `COMPAT_MAX_ARGV` 16 → 128 | `program_loader.c` | every Linux-ABI process silently lost argv[17]+; measured 21 in → 16 delivered |
| positioned-I/O lock: one global → **per file descriptor** | `fs.c` | it is held across the read, and a blocked read wedged the whole machine (`keytool -genkeypair`) |
| exec path buffer 32 → 256 + `-ENAMETOOLONG` | `arch/x64/trap.c` | `u_strcpy` truncated silently, so a long path failed with a lying `ENOENT` |
| fork child inherits `abi_flags` (+ `fd_cloexec`) | `process.c` | without it a fork child misroutes every syscall and spawns phantom grandchildren |

## Block B -- the silently-wrong list (in progress)

These are not "missing" surfaces; they are surfaces that answer wrongly, which is
worse, because a program cannot detect the lie.

| # | defect | state | evidence |
|---|---|---|---|
| 1 | `Files.isExecutable()`/`access(X_OK)` false for every file | **FIXED, VERIFIED** | `EXECBIT isExecutable=true canExecute=true` / `VERDICT PASS` (`obj/jdk/execbit2.log`). Two FAT16 stat fills carry the mode; the *path* one is what `file_stat_path()` (and therefore `access`) reads -- patching only the fd one changed nothing, which is what the first FAIL run showed |
| 2 | `F_SETLKW` returned `-EAGAIN` instead of blocking | **IMPLEMENTED, UNVERIFIED** | engine now waits: halt loop (`safe_wfi`), 10 s ceiling, lock table released across every wait, dead-owner sweep each round. glibc's `F_SETLKW` is one blocking syscall, so the old one-attempt version handed back a `FileLock` the kernel never granted |
| 3 | `MAP_SHARED` never reaches disk | **CONFIRMED BY MEASUREMENT, NOT FIXED** | `MMapWrite` wrote `ZZZZ`@0 and `QQQQ`@4092 through a `READ_WRITE` mapping and `force()` returned normally; the next boot reads the file at its original 4096 `AAAA` bytes (`obj/jdk/durability.log`: `mm.txt size=4096 read=4096 head=AAAA tail=AAAA`).  The `Files.write` content survived, the mapped bytes did not -- so it is exactly "the page cache never writes through", not a lost file.  In-boot read-back is NOT a durability test: the same boot's reader saw **0 bytes** (`obj/jdk/mmap-shared.log`: `StringIndexOutOfBoundsException: Range [0, 0 + 8) out of bounds for length 0`) while the disk held 4096 |
| 4 | empty-file `map()` returns `-ENOMEM` | **NOT DONE** (low value) | should be `-ENODEV`/`IOException`; no known program depends on it |
| 5 | `make` needed `OS=macos` or ICU died in configure/link | **FIXED, VERIFIED** | `OS` was never defined anywhere; now derived from `UNAME_S`. `make -n -p` shows `OS := macos` and `ICU_CC` = `tools/cc-x86-none-elf.sh` |

Why #2 is the one that matters: a database that opens its store with
`FileChannel.lock()` gets a `FileLock` object either way. If the kernel grants
nothing, both writers proceed and the corruption is silent and late.

## #3 measured properly: the mapping and the read path are two copies

`obj/jdk/mmaptouch.log` (one process, every step printed):

    MT after-write size=4096      MT after-map size=4096
    MT after-force size=4096 byte0=Z      MT after-close size=4096
    MT in-process-reread len=4096 first=A     <- plain read() returns the OLD byte
    PR size=4096 read=4096 first=A VERDICT visible   <- and so do two other processes

So this is not merely "no write-through": **the page cache and the file read path
are independent copies**.  POSIX requires a mapping and `read()` on the same file
to agree; here a `MappedByteBuffer.put()` followed by a `FileInputStream.read()`
in the same process returns stale bytes.  That is the defect to fix, and it makes
the acceptance test stronger than "the bytes land on disk": after `force()`, a
plain read in the same process must see them too.

The 0-byte read from `mmap-shared.log` did NOT reproduce in the near-identical
`mmaptouch` run (size=4096 at every step, both readers saw 4096).  One
unexplained occurrence is not a rule; it is a reason to keep the cross-boot check.

Order of work from here: #3 (durability + mapping/read coherence), then verify #2
once fork lands, then `MAP_SHARED`-on-`O_RDONLY` and the empty-file case.

Two more things this cost, worth reading before diagnosing a "hang":

* **`[ALIVE-SET]` on its own means nothing.** Healthy shutdowns print it too
  (`verify-blockb2.log`, `verify-blockb3.log`: 18/18 and a clean halt, with
  `[ALIVE-SET] slot=36 ... GC Thread#7` above the summary). The wedge is
  `[ALIVE-SET] no READY work` **plus** the boot hitting its timeout
  (`boot: rc=3`, killed by signal 15) -- a guest that faults to a panic ends the
  boot instead.
* **Suite flake rate on this host: 1 wedge in 7 runs** (4 before Block B, 3
  after: `verify-lo2`, `verify-noshare2`, `verify-mk2`, `verify-final2`,
  `verify-blockb` WEDGE, `verify-blockb2`, `verify-blockb3`). One green run is
  not evidence; two is the minimum, and a wedge should be re-run before it is
  blamed on whatever was last committed.

## The disk image is not a laboratory bench

`make disk.img` recreates the FAT image from scratch, so everything staged or
guest-written (`/JDKTMP`, `/JDKOUT`) disappears. Measured: `PlainWrite` created
`/JDKTMP/x.txt`, and two boots later `ReadBack` reported it **absent** -- the
write-through build had rebuilt `disk.img` in between, while `t.txt` (written
after that rebuild) survived its own reboot.

A durability claim is therefore only meaningful for boot N -> boot N+1 with **no
`make` in between**; otherwise the comparison silently measures an empty disk.

## Suite status on the write-back kernel (83781f3c)

Two consecutive green runs, which is the bar this lane set after measuring the
1-in-7 wedge flake:

| run | log | result |
|---|---|---|
| 1 | `obj/jdk/verify-writeback.log` (6238 lines) | `SUMMARY pass=18 fail=0 skip=0`, `System halt`, no `[ALIVE-SET] no READY work` |
| 2 | `obj/jdk/verify-writeback2.log` | `SUMMARY pass=18 fail=0 skip=0`, `System halt` |

msync writing back did not disturb the files-nio check, which is the one that
exercises mappings hardest.
