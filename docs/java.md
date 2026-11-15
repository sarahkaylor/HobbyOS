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
