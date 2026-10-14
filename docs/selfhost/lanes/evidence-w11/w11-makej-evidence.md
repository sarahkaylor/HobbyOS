# Lane W11-MAKEJ — parallel in-OS builds (`make -jN`): enable, verify, measure

Lane: `selfhost/w11-makej` — GNU make 4.4.1 parallel-build test bed.
Parent brief: every in-OS build is serial (~90 s/TU); make the 8 cores usable.

## 1. What was changed (port level)

`src/user/make/config.h` (this tree) —
- `MAKE_JOBSERVER` now defined (was the audit's "-j1 v1" `#undef`): the
  jobserver section of `posixos.c`/`os.h` compiles in.
- `HAVE_PSELECT 1` + `HAVE_SYS_SELECT_H 1`: make's modern jobserver token
  acquisition (`posixos.c jobserver_acquire`, pselect branch) is used.
- `JOBSERVER_USE_FIFO 0`: pin the pipe jobserver.  The default-style fifo
  jobserver would `mkfifo()` in `/tmp` (absent on this OS) and fall back
  noisily.
- Serial (-j1 / bare) behavior is untouched: `job_slots == 1` takes the
  proven blocking-waitpid path; the jobserver pipe is only set up when
  `job_slots > 1`.

`src/user/make/hb_stubs.c` — new `pselect()` stub (declared in the new
sysroot header `src/libc/include/sys/select.h`).  Why a stub and not
upstream's blocking read:

- This kernel never returns EINTR from a syscall and defers signal
  delivery for a process parked in a pipe read until the syscall completes
  (`src/kernel/process.c` D8.4).  Make's classic jobserver blocks in
  `read()` on the token pipe expecting SIGCHLD to interrupt it — but a
  token is only written back by `reap_children()`, which only runs after
  that `read()` returns.  That is a deadlock on this OS.
- The kernel *does* complete `select()` timeouts (10 ms re-probe slices),
  so the stub waits via `select()` with a bounded interval
  (25 ms, `HOBBYOS_PSELECT_POLL_MS`) when no user timeout is given, and
  returns 0 ("timeout") so make can reap children, recover their tokens
  and start the next job.  `sigmask` is ignored (sigprocmask is a no-op);
  a token-ready fd makes select() return immediately, so there is no
  artificial latency when work is available.
- The 25 ms poll bounds how quickly a finished child's slot is noticed —
  a few tens of ms per token re-acquisition, negligible vs multi-second
  TUs.

`src/libc/include/sys/select.h` (new) — `fd_set` (256-bit, layout identical
to the frozen `libc.h` ABI / kernel `fd_set_k`), `FD_*`, `pselect()`
prototype.  Keep the fd_set typedef single under `libc.h` inclusion.

No kernel, libc implementation, or upstream make source was modified.

### Why the pselect branch specifically is exercised

`jobserver_acquire` polls with our bounded-timeout pselect; when it returns
0 the `new_job` loop reaps children and retries.  `F_GETFL/F_SETFL` on a
pipe is `-ENOTSOCK` on HobbyOS, so `set_blocking()` no-ops and the
jobserver pipe silently stays blocking — harmless, because make is the
only token consumer (select-ready ⇒ token present ⇒ read succeeds) and the
exit-time `jobserver_acquire_all()` drain wants blocking anyway.

## 2. Synthetic project (in-OS)

Staged at `::/SRC/BM/` (sources in `docs/selfhost/lanes/evidence-w11/bench/`):

- 16 tiny C "units" `TU01.C..TU16.C` (byte-distinct marker payloads).
- `MK16.MK` / `MKFAIL.MK`: **explicit rules only** (the empty dir cache on
  FAT disables pattern/implicit rules — the known v1 contract), 8.3 names,
  `SHELL=/TOOLS/SH.BIN`, absolute `/TOOLS/*.BIN` paths, `OUTDIR` override
  so each run writes its own output dir (O1 serial, O4 -j4, O8 -j8).
- Per-TU recipe = `BURN.BIN 700` (in-OS busy-wait on the uptime-ms clock —
  deterministic duration on TCG) then `CP.BIN src → OUTDIR/TU.N.O`
  (deterministic output bytes).
- `BURN.BIN` (`bench/bench_burn.c`): compiled on host with crt0+libc.a as
  a normal user binary.
- `-B` on every run so timestamps never skip a rebuild (FAT is
  whole-second).

Driver: `SELFHOST-RUN-w11-bench.txt` (one stage per mode), wrapper
`BENCH.SH`, host runner `tools/w11_bench_boot.py` (per-phase host wall
times + timestamped capture).

Probe (`PROBE.SH`/`probe2.serial.txt`) pinned the in-OS sh dialect before
the finale: **empty double-quoted args are dropped** (they shifted
function positionals — this is what initially made make see `/SRC/BM/` as
its makefile/target), and `while` + `n=$((n+1))` never advances (infinite
loop).  The final BENCH.SH uses only proven constructs (functions with
non-empty positionals, `case`, `[ ]`, `$?`, unrolled 16 compares).

## 3. Results — correctness (in-OS, run4/run5 serials)

| check | result |
|---|---|
| `make -B -j1` (serial) | rc=0, 16/16 outputs produced |
| `make -B -j4`, `-B -j8` | rc=0 every run (8 runs of -j across 4 boots) |
| outputs vs sources (`cmp -s`, in-OS) | 0 mismatches in every success-mode run |
| **byte-identity serial vs -j8 (host-side)** | all 16 `O1/TU*.O` == `O8/TU*.O` (md5) |
| fail propagation, serial | TU13 recipe `exit 9` → `Error 9`, make rc=2 |
| fail propagation, -j8 | `Error 9` + `*** Waiting for unfinished jobs....`, make rc=2, no hang |
| failed TU leaves output | correct: `OF2/TU13.O` absent (cmp rc=2), all other TUs built |
| child reaping / zombies | `ps` after -j8 shows only SELFHOST.BIN + stage SH.BIN (+ transient PS); every `[KERNEL] Process N: ... exited` line is a clean exit |
| console interleaving | expected under -j (make's per-line echo + kernel prints interleave; noted, not a defect) |

The `*** Waiting for unfinished jobs....` + `Error 9` pair is GNU make's
stock -j fail path: the failing recipe stops the goal, make waits for the
in-flight children (all reaped), then exits 2.  This is exactly the
semantics a self-hosting build needs (a failed TU must not wedge make, and
must not leak processes).

Peak concurrency observed in the -j8 stage: 16+ distinct child PIDs
(make + 8 sh + 8 BURN + 8 CP waves) — ~17-25 live processes against the
63-slot PCB table at -j8.  No `no free slot` event appeared.

## 4. Results — wall time (host-side per-phase, QEMU aarch64 TCG, 8 vCPUs)

Stage wall includes make + script overhead (small).  Four full boots
(run2..run5), each running the 16-TU project serial(-j1) ×2, -j4 ×2, -j8
×2, plus both fail modes.

| config | runs | avg (s) | min | max | speedup vs serial |
|---|---|---|---|---|---|
| serial (-j1) | 8 | 19.67 | 19.42 | 20.02 | 1.00× |
| -j4 | 7 | 10.01 | 9.21 | 10.61 | **1.96×** |
| -j8 | 8 | 9.29 | 9.01 | 9.61 | **2.12×** |
| fail-serial | 4 | 15.77 | 15.62 | 15.82 | — |
| fail-j8 | 4 | 9.16 | 9.01 | 9.41 | — |

Per-run detail: serial 19.42–20.02 s (spread 3 %); -j8 9.01–9.61 s
(spread 7 %).  Serial and -j8 timings are stable enough to be meaningful
at the 0.2 s resolution of the runner.

Interpretation: at 0.7 s/TU burn the TCG fork/exec + scheduler overhead
dominates (16 × ~1.23 s serial = ~0.7 s burn + ~0.5 s spawn).  -j4 already
amortizes the burns onto other vCPUs; -j8 only adds the remaining
schedule/spawn parallelism, hence the plateau at ~2×.  On real builds
(~90 s per cc1 TU) this fixed overhead disappears, so the ceiling is the 8
cores and the 63-slot PCB table — keep `-j8`.

## 5. Optional item — real Makefile.hobbyos `user` target

Skipped as permitted: ~1 h serial for the 5-TU user set (sh.c −O2 ≈ 53 min
in-OS), too long for this bounded lane; the post-merge acceptance lane
owns the real run.

## 6. Deliverables in this lane

- Port: `src/user/make/config.h`, `src/user/make/hb_stubs.c`
  (`pselect`), `src/libc/include/sys/select.h` (new).
- Tooling: `tools/w11_bench_boot.py` (boot + per-phase timing runner).
- Evidence: this file + `run2..run5.serial.txt` (+ `.ts` timestamped
  captures), `runN-phases.json`, `probe1/probe2.serial.txt`, frozen
  `SELFHOST-RUN-frozen.txt`, benchmark tree under `bench/`.
- Runbook note: `docs/selfhost/acceptance.md` §9.6.
- Serial evidence files: `evidence-w11/runN.serial.txt`, 246–270 KB each.

Reproduce: `make MODE=selfhost disk.img` → stage bench payload (BURN.BIN +
MK16/MKFAIL + BENCH.SH + SRC) → `tools/w11_bench_boot.py --image disk.img
--script-copy docs/selfhost/lanes/evidence-w11/SELFHOST-RUN-w11-bench.txt
--restore-script docs/selfhost/lanes/evidence-w11/SELFHOST-RUN-frozen.txt
--serial <out> --phases <out-json> --timeout 600`.
