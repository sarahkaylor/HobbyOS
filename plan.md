# HobbyOS GNU Terminal Program Ports — Remaining Work Plan

Status date: 2026-09-25. Working tree: `/home/sarah/Documents/GitHub/HobbyOS`.
All work is committed locally **only** — never pushed to GitHub.

## 1. Current state

### Committed (local git, never pushed)
- **textutils (GNU textutils 2.1, byte-exact):** `wc`, `head`, `tail`, `cut`, `nl`,
  `tr`, `paste`, `fold`, `comm`, `tsort`, `expand`, `unexpand`, `cksum`, `md5sum`,
  `tac` — 16 tools with strict host parity (`make <tool>_parity_strict`,
  ~1,500 byte-exact cases, 16/16 PASS) plus on-device test binaries run in the
  ARM test wave.
- **sed 4.8** (full port + GNU regex engine + wchar/locale sysroot work), 59-case
  golden suite.
- **grep 2.5.4**, 65-case golden suite (libc dirent/getopt surface added).
- **diffutils `cmp`**, 55-case parity (required `st_ino` synthesis in `kernel/fs.c`).
- **Kernel/libc support:** POSIX open semantics + FAT16 entry durability, synthesized
  inodes, load-the-wave-from-a-thread + pool headroom, x64 TSC timer + LAPIC multi-core
  fixes, monotonic timer.

### In flight (uncommitted)
- **FAT16 long-file-name (LFN) support** — `src/kernel/fat16.c` + `fat16_test.c`
  (see §2).
- **x64 kernel-task resume fix** — `src/kernel/process.c` (save `context[33]` as the
  interrupted RSP for kernel-mode traps) + `src/kernel/arch/x64/trap.c` (in
  `enter_user_space`: a kernel-mode resume currently lands RSP on the per-CPU scratch
  stack instead of the task's own stack; fix relocates the return frame to the task's
  stack and iretq's from there). See §2.

### Test status
- Host unit tests: PASS. Host strict parity: 16/16 PASS.
- ARM (`make test`): green; UNEXPAND_T suite was **silently running the wrong binary**
  (FAT16 8.3 truncation bug — `UNEXPAND_T.BIN` resolved to `UNEXPAND.BIN`); now fixed
  by LFN support, needs a fresh green run to confirm `[UNEXPTEST]`.
- x64 (`make test_intel`): **BROKEN** — the kernel-task resume bug (§2). Blocking all
  x64 work, including the second-arch verification of the remaining ports.

## 2. Immediate steps (M0 — unblock both architectures)

1. **Finish FAT16 LFN** (in progress): strict 8.3 matching (no silent truncation),
   minimal VFAT long-name read support (mtools-created records), long-name create
   (writes 0x0F chains mtools can read back), unlink clears the preceding 0x0F
   records. Unit tests added in `fat16_test.c`.
   - Verify: `make unit_tests` (all 44+, incl. new LFN cases),
     host-side `mdir`/`mcopy` of the OS-created LFN files, then a fresh
     `make test` full wave with **`[UNEXPTEST] PASS`** on ARM.
   - Commit: `fat16: VFAT long-file-name support + strict 8.3 matching (fixes
     UNEXPAND_T silently loading the wrong binary)`.
2. **Fix x64 kernel-task resume** (root-caused, §1): implement in
   `enter_user_space` + commit together with the `process.c` save_context patch.
   - Verify: `make test_intel` full wave on x64 (currently watchdog-freezes);
     no `WATCHDOG` messages, `EXIT=0`.
3. Optional follow-up (defer unless cheap): LFN-aware `rename` (long→long) and
   `DIR`-entry iteration (`ls -l` long names still show 8.3 aliases).

## 2b. Concurrency & test-run timing work (2026-09-25/26)

Goal: reliable ≤5-minute full ARM waves, stable on up to 16 cores. Best clean
full wave so far: **6:39 (615 PASS, 19 ALL-PASSED, 0 FAIL, 8c/8GB/MTTCG)**.

Landed (uncommitted):
- **Pipe/proc ABBA deadlock — fixed.** Cycle was `process_fork`(proc_lock) →
  `fs_reopen`(f->lock) → `pipe_reopen`(p->lock) vs `pipe_read`(p->lock) →
  `process_wakeup`(proc_lock). Global lock order is now
  **proc_lock ≺ f->lock ≺ p->lock**; `pipe.c` collects a wake mask under
  `p->lock`, releases it, and only then calls `process_wakeup()`. Verified by
  clean parallel completions (rc=0, "System halt").
- **Wake-vs-schedule race — fixed (core).** A wake landing in the
  block-to-`schedule()` gap (the `-2`/EAGAIN retry path) set a still-running
  process READY; `schedule()` then skipped the save and resumed a **stale
  context**, silently replaying/skipping syscalls (root cause of stress
  ping-pong protocol drift → endgame deadlocks, shell-test validation
  flakes, and leftover processes blocking system halt). `schedule()` now
  saves the LIVE trap frame for the READY case too, and a cross-CPU
  "taken elsewhere" guard prevents two CPUs from running/saving one process.
- **`pipe_close` stale masks:** masks are now cleared when collected.
- **Default stderr fd inheritance:** no longer inherits a *pipe* end the
  parent happens to hold on fd 2 (was: child kept a duplicate read end of its
  own stdout pipe alive → full pipe + dead drainer = self-deadlock;
  observed SH.BIN wedged).
- **GICv3 support** (`gic.c`): QEMU `virt` silently switches to GICv3 above
  8 PEs; 16-core boot now works. `MAX_CPUS` back to 8 (16 still regressed).
- QEMU flags: ARM `-smp 8 -m 8192M -accel tcg,thread=multi` (MTTCG is the
  single biggest lever; before it, TCG ran single-threaded).
- `virtio_blk` no longer holds `blk_lock` across the device wait.

Remaining (this thrust):
- **Load-path collapse (DONE, verified):** the wave spent ~all of its wall
  time in program loads because each load cost ~3000 block-device round
  trips: fat16_read walked the cluster chain from the start for *every
  sector* (O(n^2) FAT reads, uncached) and every sector was its own
  virtio request.  Fixes: one multi-sector device request per contiguous
  cluster run (`virtio_blk_do_op` count), a one-sector FAT cache, a
  forward cluster walk, and `fat16_read_direct()` (loader reads straight
  into the child's physical block).  Result: **full wave 19/19 verdicts +
  System halt in ~40 s** (was 480 s / best-ever 6:39).
- **Net TX wait bounded (DONE):** `virtio_net_send` waited on the TX used
  ring with an unbounded wfi loop; a lost/coalesced completion left
  NFSTEST "RUNNING" with no CPU on it, blocking system halt ~1 run in 3.
  Now capped at 500 ms (returns -1, resyncs the ack).
- **Atomic block-to-deschedule refactor**: make [set BLOCKED+mask] and
  [save+deschedule] one proc_lock critical section (schedule variant that
  assumes the lock).  The current fix makes same-CPU resumes exact and
  cross-CPU takeovers corruption-free; a cross-CPU takeover can still replay
  from the previous save point.  Low occurrence; do before calling recovery
  airtight.
- **16-core stability (retried 2026-09-26 with the fixed tree; still open):**
  boots and runs; the failure is now understood as scale pressure, not the old
  lock bugs: (a) the 64 process slots (capped by 64-bit pipe pid masks) fill
  under 16-way concurrency — SHTEST-family children + spawn workers + unreaped
  EXITED slots starve new creates ("no free process slots"); (b) `sys_spawn`
  retries creation in a hot loop while the table is full, livelocking its CPU;
  (c) a lost-owner RUNNING process (no CPU on it) can stop the wave driver.
  Captured live via HMP + guest-memory process-table dumps.  Code supports
  MAX_CPUS 16 (1MB stack region); reintroduce only after slot-liveness work
  (spawn backoff, faster reap) — the 8-core path is the fast, verified one.
- **Lost-owner watchdog (NEW):** the scheduler idle loop now reclaims a
  process stuck in RUNNING with no CPU owner (a leaked wake — the wedged
  network tests' signature) back to READY after ~500 idle rounds and logs
  `[LOSTWAKE]` so the underlying race stays diagnosable.  Also fixes the
  [IDLESTUCK] dump trigger (monotone counter guard; the old modulo could be
  skipped when CPUs raced the shared counter).
- **≤5-min reliable waves**: ACHIEVED (0.7 min clean runs); verify stability
  across a 5x parallel battery, then re-commit.
- Uncommitted deliverables to commit after verification: net fix, load-path
  fixes, plan.md updates.

Lessons:
- `make test_arm` returns 0 even when the guest is killed — always grep the
  log for `ALL PASSED`/`System halt`.
- Kill QEMU by exact pattern (`pkill -f 'qemu-system-aarch6[4]'`) — both
  `pkill -x` (name >15 chars) and plain `-f` self-matches have bitten.
- Freeze taxonomy: class A = spinlock ABBA (locks held, CPUs spinning);
  class B = idle dead-lock (all CPUs in `safe_wfi`, an alive-but-blocked
  process holds the system open). HMP `info registers -a` + guest-memory
  reads (`xp`) of `proc_table`/`global_file_table`/`pipes` distinguish them.

## 3. OS / libc support needed for the next ports

| Capability | Needed by | Effort | Notes |
|---|---|---|---|
| Raw-mode TTY: termios-like (ICANON off), input queues | nano, less, ed | M | Biggest piece; also enables interactive tests to be driven |
| Keyboard: arrows, function keys, escape sequences | nano, less | M | Keyboard driver already delivers scancodes; decode at TTY layer |
| Terminal size (rows/cols) query | nano, less | S | Expose framebuffer/text size via a terminal ioctl |
| `opendir`/`readdir` on FAT | find, ls -l | S | FAT dir iteration exists; expose via libc dirent |
| Signal delivery (`SIGINT`, `SIGTERM`, handlers) | xargs, ed, nano | M | Needed for courtesy kills and ^C |
| `getline`, `fnmatch` | find, sort | S | fnmatch for find `-name` |
| wchar/collation locale | sort, join, diff | L | Already partially in sysroot for sed; sort can ship LC_ALL=C first |
| Compression (deflate) | gzip | M | zlib is GPL-incompatible; use the small zopfli/deflate core or write a minimal deflate |
| Archive reader/writer | tar | M | ustar is trivial; GNU tar adds long-name/dir features |

Legend: S = small, M = medium, L = large.

## 4. Candidate ports — priority + plan

Each item: vendor source (GNU, GPL/AGPL), port, host parity harness
(`src/host/build_*_ref.sh` + `*_parity.sh`), in-OS test binary in the wave, both
architectures, then a commit. "Nothing as ambitious as emacs" — gawk/nano are the
ceiling, not the floor.

### Wave 1 — textutils companions (small, high value, no new OS work)
1. **sort** — textutils 2.1. High value; the biggest remaining textutil. Ship
   `LC_ALL=C`/byte order first, keep the 2.1 algorithm; `-k`/`-t`/`-n`/-u` etc.
   Needs some libc (temp files already exist).
2. **uniq** — trivial; pairs with sort for dedup pipelines.
3. **join** — straightforward relational-join on sorted files; same file machinery
   as comm (already ported).
4. **look / fmt / sum / users / logname / whoami** — near-zero effort, trivial
   libc; cheap to batch with sort in one commit.

### Wave 2 — file-tree / process tools (medium)
5. **find (findutils 4.9)** — high value. Requires `opendir/readdir` (small) and
   recursively walking subdirs; implement `-name/-type/-size/-print/-exec` (non-GNU
   cosmetic parity first). Use findutils' own `fnmatch`.
6. **xargs (findutils 4.9)** — high value; composes with find/grep. Needs
   fork/exec/wait (exists via spawn), arg batching, and a `SIGCHLD`-ish reap
   (existing wait plumbing).

### Wave 3 — archives & compression (medium, self-contained)
7. **gzip** — deflate + gzip framing + CRC32 (CRC32 already in tree for cksum).
   Port gzip-1.13's core with the license-preserved bits; in-OS tests: round-trip
   gzip→gzip -d byte-exact, sizes, exit codes.
8. **GNU tar** — ustar create/extract/list; directory+link records on FAT.
   Possible friction: long names (LFN now supported!), symlinks (none on FAT —
   degrade to hardlink/plain record). Good after LFN lands.

### Wave 4 — terminal work + interactive editors (the headline)
9. **Terminal subsystem** (§3): raw mode, cursor addressing, arrow/function keys,
   screen size query. This is the OS-side prerequisite for the next two.
10. **nano** — the named headline target. After terminal work: port nano's core
    (GPL); byte-parity is not the bar here — functional parity (edit, save,
    navigate, search) plus a scripted end-to-end edit-driving test.
11. **ed** — classic line editor; smaller than nano, pure byte-exact parity
    possible; can land before or after nano.

### Wave 5 — remaining high-value GNU staples (as appetite allows)
12. **diff / sdiff** (diffutils) — diff compares via the same line-buffer
    machinery as cmp; the LCS/Hunt–Szymanski algorithm is the meat. Middle size.
13. **awk (gawk)** — the classic "big four" companion to grep/sed. Large but
    self-contained (no TTY). Strong candidate after the small waves.
14. **less** — pager; needs the terminal work (raw + keys + size). Post-nano.

### Explicitly out of scope
emacs (user), vi-family clone (vim is huge; consider `ex`/`ed` instead),
perl/python/ruby, X11/GUI tools, anything non-terminal.

## 5. Verification standards (apply to every port & kernel change)

- **Host parity:** `make <tool>_parity_strict` must PASS against the vendored
  reference build (byte-exact stdout+stderr+rc). Negative controls: a wrong
  reference must fail the harness.
- **In-OS wave:** every tool ships a `*_test.c` binary in the wave; the full
  `make test` (ARM) and `make test_intel` (x64) suites must end `EXIT=0` with every
  `[<TOOL>TEST]` verdict present. Watch specifically for silent mis-execution
  (the UNEXPAND_T.BIN lesson: verify the verdict exists and names the right file).
- **Kernel/libc changes:** `make unit_tests` all green (44+), plus host unit tests.
- **Style:** `python3 tools/cstyle.py check` on every touched file.
- **Both arches:** any non-arch-specific code compiles and passes on arm + intel;
  arch code builds and runs on its target (x64 gate: `make test_intel`).
- **Independent verification:** subagent parity claims are re-run by the parent
  (proven policy after batch-2 delegation drift).
- **Determinism:** vendor the exact upstream source version; never "modernize"
  semantics mid-port (the tr/nl divergences are documented deliberate spots).

## 6. Milestone ordering

- **M0** (this session): FAT16 LFN commit → x64 trap fix commit → `make test`
  green on ARM **and** `make test_intel` green on x64. Both arches healthy again.
- **M1** (wave 1): sort + uniq + join (+ trivial textutils) — one or two commits.
- **M2** (wave 2): find + xargs (opendir/readdir + wait plumbing).
- **M3** (wave 3): gzip + tar.
- **M4** (wave 4): terminal subsystem → nano → ed.
- **M5** (wave 5): diff/sdiff, gawk, less (deprioritized until M1–M3 per appetite).

Each milestone ends with: strict host parity PASS, full ARM + x64 wave green,
unit tests green, cstyle clean, one or more local commits.

## 7. Known landmines / lessons (carry forward)

- **FAT16 names:** never truncate a query onto an 8.3 short entry (wrong-binary
  bug); LFN records are skipped-by-attr traps — now resolved.
- **x64 trap path:** same-ring (kernel) interrupts push no RSP; any resume path
  must reconstruct the interrupted RSP explicitly (process.c + trap.c fix).
- **GETOPT macros:** keep the brace-less transcription from `sys2.h` (braced
  variants zero `val`, silently swallowing `--help`/`--version`).
- **Inode synthesis** for `cmp` (st_ino was always 0).
- **Deterministic refs:** build reference binaries from the vendored textual
  source, not the distro's (semantics drift).
- **Do not push** — local commits only, per original instruction.
