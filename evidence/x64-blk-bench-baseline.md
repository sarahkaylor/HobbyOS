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
| wave-1 | 0 | ~4.5 min | YES ("System halt from CPU 3.") | 256 | 11 (10 suites + STRESS) | red-but-completing: whole wave ran, halted cleanly, single OVMF boot (no reboot loop) |
| wave-2 | 0 | ~17.5 min | YES ("System halt from CPU 3.") | 245 | 11 (10 suites + STRESS) | same class, plus one long mid-run serial stall (~10 min frozen at 1955 lines, mid-FAT-suite) from which the guest recovered and completed |

Local truth at tip: **2/2 copies complete with the halt marker and reach STRESS SUCCESS — the
CI-VM reboot-loop was NOT reproduced — but both complete RED** (~250 FAIL tokens) under the
fork-EAGAIN slot-pressure cascade. `rc=0` means nothing here (QEMU self-terminates 0 on
poweroff); the log carries the verdict.

### Per-suite FAIL concentration (both waves, same signature)

Child-pipe / exit-status suites dominate — the spawn-starvation signature:

```
      wave-1          wave-2
83    SEDTEST   86    SEDTEST
38    GREPTEST  36    GREPTEST
20    TSORTTEST 20    CMPTEST
20    CMPTEST   7     TSORTTEST
 7    TACTEST   4     TACTEST
 3    SIGTEST   2     TRTEST / PASTETEST / NLTEST / CKSUMTEST ...
 3    NLTEST / COMMTEST / CKSUMTEST / MD5TEST
```

Forensics per run: `[KERNEL] sys_fork: tf->regs[0] is now -11` × 528 / × 157,
`thread_create: no free slot` × 3 / × 3, `LOCKFOREVER/IDLESTUCK/WATCHDOG` × 2 / × 4,
`process_create: no free process slots! zombies=N` present. The fork-EAGAIN storm starves
the pipe-pair helpers SED/GREP/TSORT/CMP/TAC depend on -> wrong child exit statuses
(e.g. `SIGTEST FAIL waitpid returns the child got=1 want=4294964048`,
`FATBIG FAIL tail checksum clean got=0x...0800 want=0x...0000`).

### Reboot-storm check (the CI failure mode is NOT local)

Exactly **1** `BdsDxe: starting Boot0002 "UEFI QEMU NVMe Ctrl..."` line per log = the single
normal OVMF boot (the auto-generated boot entry), no `Guest reset` / `Triple fault`.
The CI VM reset-loop class did not reproduce on either local KVM copy.

### Note on the mid-run stall (wave-2)

Serial output froze at 1955 lines for ~10 min while QEMU stayed alive (last activity:
FAT suite `/FOLDLONG.TXT`, `sys_close` on CPU=7), then resumed and completed — a
slow/stalled phase, not a permanent hang, this time. This is why the battery is bounded:
a run that freezes past its budget is reaped by the recipe (`timeout` rc=124, then
`fuser -k <copy>/disk.img`) and recorded as a non-completion rather than waited on forever.

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
