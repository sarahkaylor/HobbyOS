# x86_64 HobbyOS test tiers — local KVM baseline (block-cache re-validation receipt)

Date: 2026-10-16
Host: workstation (251 GB RAM, 64 cores), `/dev/kvm` present, qemu 10.2.1, clang 21.1.8
Lane: `blkcache/x64` — worktree `/home/sarah/hobbyos-lanes/blkcache-x64` (never main)
Purpose: establish what the x64 unit tier and a bounded x64 integration wave do **today, locally**, so a later block-cache change can be re-validated against the same receipts.

## Baseline tip

- Branch `blkcache/x64` based on `main@71b980c` (parent fixed the exec-bit bug on main @695552d).
- Lane commits added by this baseline run:
  - `tools/x64_blk_bench.sh` (the reusable recipe) — see "Recipe" below.
  - `evidence/x64-blk-bench-baseline.md` (this note).

## Build-blocker found & fixed (by parent, on main)

`tools/selfhost-src-tar.sh` was committed **non-executable** (index mode 100644) while the
`disk.img` recipe invokes it directly:

```
tools/selfhost-src-tar.sh obj/selfhost/SRC.TAR
make: tools/selfhost-src-tar.sh: Permission denied
make: *** [Makefile:2183: disk.img] Error 127
```

Every fresh-worktree disk build (both arches) dies at this line. Parent chmod'ed the worktree
copy and committed the mode fix on main (`695552d`). Commit range: `71b980c..695552d`.

## 1. Cold intel build

Command (exactly the task's cold-build form; run as a tracked background job):

```
python3 -c "import os; [os.remove(f) for f in ['hobbyos.elf'] if os.path.exists(f)]"   # stale ARM elf
make ARCH=intel MODE=unit_tests hobbyos.elf disk.img BROWSER_SKIP=1 TOOLS_SKIP=1 -j16 > build.log 2>&1
```

- Seeded `obj/` contained ARM objects only (`obj/arm`); `obj/intel` did not exist — this was the true cold intel build (kernel + userland + vendor).
- ICU + SQLite intel vendor builds were paid once under `obj/intel/icu`, `obj/intel/sqlite*.o` (cache for future runs).
- Browser: `third_party/webkit-hobbyos/cache/browser-x64.bin.xz` existed, so `browser-staged` resolved from cache (`[browser] ... (cache) -> obj/intel/browser.bin`) — no WebKit build. `TOOLS_SKIP` never engaged because `third_party/llvm-21.1.8/dist-staging` already existed.
- First attempt died at the disk recipe (exec-bit bug above) with 693 objects already built under `obj/intel`; the resume re-ran only the tail of the disk.img recipe — **`BUILD_RC=0` at 14:35:23** (resume-run build.log mtime; cold object compile portion ~4 min to the disk failure, then ~3-4 min resume).
- Verify the result ELF is x86: `file hobbyos.elf` → `ELF 32-bit LSB executable, Intel i386 ... statically linked, with debug_info`.

## 2. x64 unit tier — VERDICT: PASS

Command (the script's own acceleration knob — it forwards `EXTRA_QEMU_ARGS`, not `QEMU_ARGS`):

```
UNIT_TESTS_TIMEOUT=1800 EXTRA_QEMU_ARGS=-enable-kvm ./run_unit_tests_intel.sh > unit-outer.log 2>&1
```

Decisive lines from `qemu.log` (run under KVM):

```
    RUNNING KERNEL UNIT TESTS
Tests run:    328
Tests failed: 0
UNIT TESTS PASSED
```

- QEMU line confirms `-enable-kvm` and the canonical intel config: `-M q35 -smp 8 -m 12288M -pflash $HOME/.local/share/OVMF/OVMF_CODE_4M.fd ... -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu ... -action shutdown=poweroff -display none -enable-kvm`
- Duration: **25 s** wall from script start (build portion incremental — mode already unit_tests and objects cached — so this is mostly the KVM guest run; the ~328 in-EL1 checks).
- 328 / 0 — no FAIL tokens. Unit tier is a clean local PASS on x64 @ tip.

## 3. x64 integration wave — BOUNDED battery (2 copies, this baseline)

Runner: the recipe below with `--skip-build --skip-unit --copies 2`, seeded from the MODE=test template built after the unit run (kernel mode-flips once: 00:04).

Per-copy command (bounded, own disk.img):

```
( cd <copy> && timeout 1800 make MODE=test run ARCH=intel QEMU_ARGS='-enable-kvm -display none' ) \
  > wave-<n>.log 2>&1
```

Kill/reap: `fuser -k <copy>/disk.img` (exact file, per copy — never broad pkill).

### Verdicts (recorded honestly — this is what the x64 wave does TODAY, locally)

| copy | rc | duration | System halt. | FAIL tokens | suite summaries | notes |
|------|----|----------|--------------|-------------|-----------------|-------|
| wave-1 | 0 | ~6.5 min | YES ("System halt from CPU 3.") | 256 | 11 (10 suites + STRESS) | red-but-completing: whole wave ran, halted cleanly, single OVMF boot (no reboot loop) |
| wave-2 | 124 (timeout-bounded) | ~30 min budget | NO | 2 | 3 | wedged: serial froze at 1955 lines ~6.5 min in, mid-FAT-suite (FOLDLONG.TXT / sys_close), QEMU alive, timeout-bound by design; see forensics below |

wave-1 is the strongest local x64 result available at tip: the wave does not reboot-loop
locally (unlike the CI VM), it reaches and passes STRESS, but it completes RED under the
slot-pressure cascade.

### wave-1 highlights (decisive lines, quoted)

```
System halt from CPU 3.
 pages=[STRESS TEST] SUCCESS: ALL TESTS PASSED SUCCESSFULLY!
[ERRNO] ALL TESTS PASSED
[REGTEST] ALL TESTS PASSED
```
- 11 `ALL TESTS PASSED` lines: 10 per-suite summaries + STRESS SUCCESS.
- Exactly **1** `BdsDxe: starting ...` line = the single normal OVMF boot (Boot0002 "UEFI QEMU NVMe Ctrl" — the auto-generated boot entry, not a reset loop). No `Guest reset` / `Triple fault`.
- FAIL-token concentration (child-pipe / exit-status suites — the spawn-starvation signature):
  `SEDTEST 83, GREPTEST 38, TSORTTEST 20, CMPTEST 20, TACTEST 7, SIGTEST 3, NLTEST 3, COMMTEST 3, CKSUMTEST 3, MD5TEST 2` (+ scattered).
- Forensics: `[KERNEL] sys_fork: tf->regs[0] is now -11` × 528, `thread_create: no free slot` × 3, `LOCKFOREVER/IDLESTUCK/WATCHDOG` × 2.  The fork-EAGAIN storm starves the pipe-pair
  helpers that SED/GREP/TSORT/CMP/TAC depend on -> wrong child exit statuses (e.g. `SIGTEST FAIL waitpid returns the child got=1 want=4294964048`, `FATBIG FAIL tail checksum clean`).

### wave-2 highlight (the hang class, bounded)

Serial output froze at 1955 lines while QEMU stayed alive (etime grew, zero new output —
the skill's "no output yet is itself the signal"). Last kernel activity was the FAT suite
(`/FOLDLONG.TXT`, `sys_close` debugs on CPU=7, `PIDs=[2,40,22,58]`). The recipe's
`timeout 1800` is the bound that converts this hang into a receipt (rc=124) instead of a
forever-wait; the copy is then reaped with `fuser -k <copy>/disk.img`. This is the
documented "local copies historically mostly clean, but the tier does not complete
reliably" reality — exactly why the battery is bounded.

## 4. Recipe (reusable, committed)

`tools/x64_blk_bench.sh` — usage:

```
tools/x64_blk_bench.sh [--copies N] [--skip-build] [--skip-unit] [--skip-wave]
LOGDIR=/path WAVE_TIMEOUT=1800 UNIT_TESTS_TIMEOUT=1800 tools/x64_blk_bench.sh ...
```

Does: python-delete any stale cross-arch `hobbyos.elf` → intel template build (`BROWSER_SKIP=1
TOOLS_SKIP=1`, `-j$(nproc)`) → unit tier via `run_unit_tests_intel.sh` with
`EXTRA_QEMU_ARGS=-enable-kvm` → wave template (MODE=test) → seed N copies (full tree incl.
`obj/`; each copy owns its `disk.img`) → run each boundedly in parallel → reap with
`fuser -k <copy>/disk.img` → print verdicts (halt marker, FAIL count, suite-summary count,
STRESS line, reboot-burst evidence). It never broad-pkills and never runs `make clean`.

Raw logs for this baseline: tree root `build.log`, `unit-outer.log`, `qemu.log`; battery under
`/tmp/x64-blk-bench-wave/` (`recipe-outer.log`, `build_wave.log`, `wave-1.log`, `wave-2.log`,
`wave-1.rc`, `wave-2.rc`).

## Open risks

- x64 wave FAILs may be the documented ambient class (slot-pressure at wave start, KVM-firing SMP races). Attribution rule: one clean re-run before blaming a change; `no free slot` / `sys_fork -11` forensics are chronic and not diagnostic alone.
- CI-VM reboot-loop behavior was NOT reproduced locally (KVM copies boot and run); but local copies have historically been "mostly clean," and today's run should be read in that context.
- The unit tier's `fresh_disk` step is stateful — a re-run in the same tree without refresh will false-fail fat16; `run_unit_tests_intel.sh` handles this itself.
- Cross-lane: other lanes' QEMU VMs were observed running concurrently (e.g. `blkcache-browser` ARM VM). Never broad-pkill; reap only `fuser -k <copy>/disk.img`.
