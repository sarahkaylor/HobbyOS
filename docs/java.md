# java.md — proving out the OpenJDK port, surface by surface

OpenJDK 27 (Adoptium 27.0.1.1+4, linux/x64 hotspot) runs on the x86_64
Linux-ABI layer: it starts, compiles with `javac`, runs what it compiled, and
passes `tools/jdk/tests/Verify.java` 18/18 with class-data sharing on and off.
Gate: `tools/jdk/sdktest.sh` (version / javac / run / suite), 4 boots.

The runtime is **27.0.1.1+4** because of CVE-2026-114938 (see
[the pin is a security decision](#the-pin-is-a-security-decision-cve-2026-114938));
`tools/jdk/fetch-jdk.sh` reproduces it from the pinned tarball and checksum.

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

## The pin is a security decision: CVE-2026-114938

"Java4Shell", disclosed 2026-11-17: a heap out-of-bounds write in HotSpot's
x86-64 AVX-512 VBMI Base64 decode stub.  For a tail shorter than one complete
4-character group that ends in `=`, the stub computes `output_size = (len/4)*3`
= 0, the padding branch decrements it to -1, `BZHI` leaves the store mask all
ones, and the final `evmovdqub` writes 64 bytes from a destination with room for
2 -- 62 bytes past the decoded array -- and *then* Java throws
`IllegalArgumentException`.  Reachable wherever an application decodes
attacker-controlled Base64 (auth headers, SAML, uploads), on x86-64 CPUs
reporting `avx512f+avx512bw+avx512vbmi+bmi2`; **not** reachable on ARM64 or
without AVX-512 VBMI.

This lane used to pin **27.0.1.0.2+1**, which the OpenJDK advisory of
2026-11-17 lists as affected ("27.0.1.0.3 and earlier").  It now pins
**27.0.1.1+4**, the fixed release.  Four separate facts, each with a file
behind it:

| claim | how it was measured | result |
|---|---|---|
| the old pin is in the affected range | OpenJDK advisory 2026-11-17, CVE-2026-114938 row (17/21/25/27) | 27.0.1.0.2+1 < 27.0.1.1 |
| the new binary contains the fix | disassembled `StubGenerator::generate_base64_decodeBlock` in both builds' `lib/server/libjvm.so` and diffed the calls it makes | OLD 279 calls, NEW 281: exactly `+Assembler::cmpl(Register,int)` and `+Assembler::jcc(Condition,Label&,bool)`, nothing removed -- the two emissions upstream commit `5177a70b385c` (JDK-8393212) adds (`__ cmpl(length, 4); __ jcc(Assembler::less, L_bruteForce);`) |
| malformed tails are still rejected, and decoding still works | `tools/jdk/tests/Base64Probe.java`, one boot | `B64 SUMMARY pass=13 fail=0` (`obj/jdk/base64.log`) |
| the new runtime did not regress the port | `tools/jdk/sdktest.sh --no-build` | version / javac / run / suite all PASS, `SUMMARY pass=18 fail=0` (`obj/jdk/sdktest-cve.log`) |

**Reachability in this guest, stated so nobody overreads the patch.**  Two
independent conditions decide it, and they must be kept apart:

- **The CPU feature is the one that decides it.**  `java -XX:+PrintFlagsFinal
  -version` in the guest prints `UseAVX = 0` and `UseBASE64Intrinsics = false`
  (`obj/jdk/flags.log`): `boot.sh` boots q35's default CPU model under TCG,
  which exposes no AVX at all, so the vulnerable stub was never generated here
  even on the old pin.
- **The network is not a mitigation, and an earlier version of this section
  wrongly said it was** ("no TCP stack, loopback only").  `net.c` (1607 lines)
  plus `virtio_net.c` carry ARP, IPv4, ICMP, UDP, a DHCP client and a TCP
  *client* with SYN retransmit; the probe boot's own console shows
  `DHCP Ack received` and `DHCP DNS server: 10.0.2.3`, and `boot.sh` attaches
  `-netdev user` (QEMU slirp).  Outbound connections therefore exist, which is
  exactly the shape of Java4Shell's **client-side** variant -- a JVM dialing a
  malicious TLS server.  What does not exist is an externally reachable
  listener: AF_INET `bind`/`listen` route to `net_socket_bind_lo` /
  `net_socket_listen_lo`, a bind to anything but `127.0.0.1` is refused with
  `-EINVAL` (`fs.c`: "this stack has no NIC-bound listener to back it"), and no
  launch line passes `hostfwd`.  So no attacker can dial in; a malicious server
  the guest connects to is a real path, and what closes it is hardware feature
  detection, not the network stack.

The pin moved anyway: a known heap overflow should not be what the image runs,
and "we are not reachable" is a claim that has to keep being true.

Reproduce: `tools/jdk/fetch-jdk.sh --glibc` (verifies the tarball's published
SHA-256 `005e57ee...86d107` before unpacking), then `sdktest.sh` and the probe
boot:

```bash
tools/jdk/stage.sh --dir obj/jdk/classes:/C --wave "JDK/bin/java -cp /C Base64Probe"
tools/jdk/boot.sh --timeout 420 --log obj/jdk/base64.log
```

If a future pin has to sit in the affected range for some reason, the interim
mitigation is `-XX:-UseBASE64Intrinsics` (preferred) or `-XX:UseAVX=2` on x86-64
only -- and it belongs in the launch line, not in a document.  Do not use
`-XX:DisableIntrinsic=_base64_decodeBlock` on its own: it stops the JVM from
starting.

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

## Open: a file created by one process is sometimes invisible to the next

Found while proving munmap write-back, and it may outrank every JDK feature left,
because an in-guest toolchain (javac writes classes, java reads them) depends on
it and every build tool does too.

Measured with MKFILES -> READFILES, two wave lines, same boot, six files each
closed by the writer before the next:

| boot | reader | note |
|---|---|---|
| 1 | `found=0/6 PARTIAL-VISIBILITY` | none visible |
| 2 | `found=6/6` | |
| 3 | `found=6/6` | |
| 4-11 | `found=6/6` (8 boots) | "with sync()" -- but see below: sync() does nothing |

Two things are established and one is not:

* the writer's own view is correct -- it re-opens every file it just created and
  reads back 8 bytes (`inproc=8` on all six), so creation and write both worked;
* the failure is all-or-nothing, not per-file: the six dirents share a directory
  sector, and the reader was served a stale view of the whole directory. That
  points at the *directory sector* reaching disk on some boots and not others,
  not at an individual entry being dropped;
* **`sync()` is not implemented.** There is no `L_SYS_sync` anywhere in
  `src/kernel/`, so the eight "with sync()" boots tested the same thing as the
  three without it: the calls are no-ops. I briefly read 8/8 versus 2/3 as
  evidence for a fix and it is not evidence of anything. Corrected rate over all
  eleven boots: **1 failure in 11 (~9%)**, all-or-nothing when it happens.
  Consequence worth its own line: a program cannot currently ask for durability
  at all -- fsync/fdatasync/sync are the POSIX way to say "now", and none of them
  exists, so a toolchain cannot make a file durable on purpose.

Protocol for whoever picks this up: `tools/jdk/{mkfiles,readfiles}.c` (already in
the tree), stage both, boot twice per sample, and count. `mkfiles` calls `sync()`
today; to test the no-sync arm, drop that call and rebuild the probe. Until it is
fixed, any test whose verdict depends on a file written by an earlier process in
the same boot is unreliable -- which is why every durability claim in this lane is
cross-boot.

### Where the directory flake is NOT, and the next experiment

Checked and ruled out: `virtio_blk_write_sector()` already invalidates the written
range in the block cache (`blk_cache.h:11`, with its own `invalidations` counter),
and the create path in `fat16.c` (alloc_entry_in_dir, ~910-985) writes directory
sectors through exactly that call -- both the root-directory run and the
subdirectory run.  So this is not "the writer bypassed the cache and a reader kept
a stale copy", which was my first guess.

That leaves the two views of a name that a lookup can consult:

* a directory **scan** (`getdents`-style walk of the sector), and
* **path resolution** (`fat16_resolve_path`) for `open()`, which may keep its own
  view of the directory.

The experiment that separates them, one boot: the reader prints BOTH -- the result
of listing `/` and the result of `open("/FL0.TXT")`.  If the scan sees the six
files while `open` says ENOENT, the defect is in path resolution (a cached name
map or an entry snapshot, the same family as the `disk_seen` clobber guard).
If both are empty, look at the write itself and at the device's completion, since
the cache is not the explanation.

## UI frameworks: measured, not assumed

`tools/jdk/tests/ui/UiHeadless.java` reports each capability separately, because
the useful answer is which part breaks, not whether all of it works.

Boot `obj/jdk/ui-fonts3.log` (headless, fonts and fonts.conf staged):

| capability | result |
|---|---|
| Toolkit (headless) | PASS |
| Graphics2D shapes, antialiasing | PASS, 5446 painted pixels |
| image pipeline (scale, colour model) | PASS |
| Swing components + layout | FAIL -- fontconfig |
| Swing look and feel (Metal) | PASS |
| Event Dispatch Thread | PASS, `onEdt=true` |
| offscreen component painting | PASS, px=ffff0000 |
| font families / text rendering | FAIL -- fontconfig |

So java.desktop's 2D core, image pipeline, Swing LAF, EDT and component painting
work with no display at all. Everything that needs a font dies on one message:
`Fontconfig head is null, check your fonts or fonts configuration`.

What that message does and does not mean, established:

* `libfontmanager.so` does not link fontconfig -- its NEEDED list is libawt, libc,
  libfreetype, libjava, libjvm, libm -- and the message string lives in
  `lib/modules`. The JDK therefore dlopen()s fontconfig itself at runtime.
* the library IS in the image: `libfontconfig.so.1` (285840 bytes, from
  libfontconfig1 2.13.1-2ubuntu3, the focal build that matches the glibc 2.31 pin)
  and `libexpat.so.1`, both verified present with `mdir`.
* `tools/jdk/fcprobe.c` then dlopen()ed it by soname and by three absolute paths,
  including `/lib/x86_64-linux-gnu/libfontconfig.so.1`: all four FAILED. Its own
  dlerror() printing is buggy (two calls, second clears the message), so the next
  step is to fix that probe and read the loader's actual complaint -- most likely
  the guest loader failing to resolve libexpat.so.1 for the dlopen'd object, or
  the LFN/short-name mapping (the image stores it as LIBFON~1.1).
* the config side is fine: `/etc/fonts/fonts.conf` lands (after the mkd fix below)
  and points at `/FONTS`, where DejaVuSans.ttf is staged.

Fixed on the way: `stage.sh`'s mkd() swallowed every error and had no -p, so
`--dir ...:/etc/fonts` was a silent no-op -- ::/etc did not exist, ::/etc/fonts
failed, nothing was copied, and the only symptom was the same "head is null"
message as a missing font. It now creates each path component.

Open, in order: read the real dlopen error; make libfontconfig loadable (dependency
resolution or LFN mapping, whichever it turns out to be); then re-run UiHeadless
expecting 9/9; then AWT/X11 on-screen, which needs libX11/libXext/libXrender as
well as a display server the JDK can connect to.

### The font stack, solved

`obj/jdk/ui-fc7.log` -- **UI 11/11 PASS**, fonts included:

```
FCP dlopen libfontconfig.so.1 OK      FCP version=21301
FCP FcInitLoadConfigAndFonts=0x1040002c90   FCP VERDICT fontconfig-works
UI PASS font-families count=6 first=DejaVu Sans
UI PASS font-dialog name=Dialog       UI PASS text-metrics stringWidth=64 ascent=13
UI PASS text-render non-white-pixels=156
UI PASS swing-components children=3 pref=51x59 btnText=go
UI PASS swing-laf                     UI PASS edt onEdt=true
UI PASS swing-offscreen-paint px=ffff0000
```

How it fell out, one layer per boot, each layer discovered only by unwrapping the
previous one with `tools/jdk/fcprobe.c` (the JDK's own message names nothing):

| boot | dlopen of libfontconfig says | what it meant |
|---|---|---|
| fcprobe.log | `(null)` | my probe called dlerror() twice -- probe bug |
| fcprobe2.log | `libfreetype.so.6: cannot open shared object file` | stage freetype |
| ui-ui.log (after) | `libuuid.so.1: ...` | freetype present, uuid missing |
| fc5 | `undefined symbol: FT_Get_BDF_Property` | the JDK's bundled freetype has no BDF; use Ubuntu's |
| fc6 | `libpng16.so.16: ...` | Ubuntu freetype needs png |
| fc7 | **OK** | fontconfig loads, FcInit returns a config, 6 families |

`tools/jdk/fetch-font-stack.sh` reproduces all six libraries from the same focal
ABI as the glibc pin, so this is not a hand-assembled guess: rerun it and the
state is the same.  Note the general lesson for this lane: a missing .so surfaced
as a *font* error three layers away, and the only reliable tool was a probe that
asked the loader directly.
