---
name: hobbyos-build-and-run
description: "Use when building, booting, or choosing a QEMU target for HobbyOS. Maps the ARCH×MODE build matrix, the right target for each job, and the deadlock/shutdown discipline."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos, linux]
metadata:
  hermes:
    tags: [hobbyos, osdev, qemu, make, build, arm64, x86_64]
    related_skills: [hobbyos-run-tests, hobbyos-screenshot, hobbyos-gui-test, hobbyos-kernel-constraints, hobbyos-proxmox-gpu]
---

# HobbyOS: Build & Run

## Overview

HobbyOS is a bare-metal ARM64 (and secondary x86_64) hobby OS built with LLVM/Clang and booted in QEMU. Every build is parameterized by two axes — **`ARCH`** (target CPU) and **`MODE`** (what the kernel does after boot). Picking the wrong pair silently wastes a full build+boot cycle, so choose deliberately before running anything. This skill is the entry point; delegate testing to [[hobbyos-run-tests]], display capture to [[hobbyos-screenshot]], and coding rules to [[hobbyos-kernel-constraints]].

Run all commands from the repo root (contains the `Makefile` and `disk.img`).

## When to Use

- Building the kernel or booting HobbyOS in QEMU for any reason.
- Deciding which `make` target fits a task (interactive desktop vs. a test tier).
- A build/boot hangs and you need to know whether that's normal or a defect.

Don't use for: interpreting test *results* (→ [[hobbyos-run-tests]]), taking screenshots (→ [[hobbyos-screenshot]]), or remote Intel/GPU work (→ [[hobbyos-proxmox-gpu]]).

## The Build Matrix

`ARCH` defaults to `arm`; `MODE` defaults to `desktop`. Override on the command line: `make run ARCH=intel MODE=test`.

| ARCH | CPU / QEMU | Notes |
|------|-----------|-------|
| `arm` (default) | `qemu-system-aarch64 -M virt -cpu cortex-a53`, 8 cores, 16 GB (2026-10 tip; was 4c/2 GB) | **Primary target.** All features must work here first. |
| `intel` | `qemu-system-x86_64 -M q35`, 8 cores, 3 GB | Secondary. Needed for PCIe/RDMA/GPU work only. |

| MODE | `-D` define | What the kernel does after boot |
|------|-------------|--------------------------------|
| `desktop` (default) | `KERNEL_MODE_DESKTOP` | Boots the window manager / desktop. Runs forever — interactive. |
| `test` | `KERNEL_MODE_TEST` | Scheduler auto-spawns userland integration tests (fork, smp, pipe…) then halts. |
| `unit_tests` | `KERNEL_MODE_UNIT_TEST` | Tests kernel subsystems in EL1 before the scheduler starts, then force-halts QEMU. |
| `desktop_test` | `KERNEL_MODE_DESKTOP_TEST` | Boots desktop, auto-launches a UI app to exercise the framebuffer, self-shuts down. |

Non-`desktop` modes automatically append `-display none`. `desktop` uses `-display cocoa` (a window opens on your Mac).

## Build matrix pitfalls

**`ld.lld` is invoked unqualified (Makefile:190), so the LLVM bin dir must be
on PATH.** Foreground shells in this environment have it; harness-spawned
background jobs may not, and then `make` dies with `ld.lld: No such file or
directory` (Error 127) at the kernel link — an environment failure, not a
build/code failure. Prefix background builds with
export PATH="$(dirname $(which clang)):$PATH”.

**The kernel ELF is a single shared root file (`hobbyos.elf`, `$(TARGET)`),
linked from whichever arch was built LAST** — `make clean` removes it (line
801), and a normal `make ARCH=arm`/`ARCH=intel` does not detect that the
shared file now holds the *other* arch's kernel (each arch tracks staleness
only via `obj/<arch>/.mode`). Symptom: after an ARM `make test`, an intel
boot (unit/integration/desktop) either boots the ARM kernel and hangs with
no OS output, or — worse — OVMF settles on a "UEFI QEMU NVMe Ctrl" boot
option it auto-generates. Fix: `rm -f hobbyos.elf` before the second
arch's build (forces relink + disk.img refresh), e.g. always run the two
   arches' gates as: ARM tier, `rm hobbyos.elf`, intel tier. x64 symptom to
   recognize (2026-10-02): OVMF spins in a hlt loop ~0% CPU at
   `BdsDxe: starting Boot0002` under BOTH TCG and KVM when the disk carries the
   other arch's ELF (Limine can't load an AArch64 image) — this is the stale-ELF
   case, NOT a qemu/firmware regression; fix is the relink, no host changes.
   x64 secondary: `-smp 4` fails AP-bringup (TCG hang / KVM stack-dump); use
   `-smp 8` for x64 boots (matches the green unit-x64 tier).
pristine `~/.local/share/OVMF/OVMF_CODE_4M.fd` from `/usr/share/OVMF/` if
the intel `-pflash $(EDK2_X86_64)` (code file is ALSO the vars file here)
accumulates stale boot entries.

## Canonical Commands

```bash
# Interactive desktop (ARM) — opens a Cocoa window, runs until you quit
make run

# Same, explicit arch
make run_arm
make run_intel

# Integration tests (userland syscall/SMP/IPC suite)
make test                 # ARM;   == make MODE=test run
make test_intel

# Kernel unit tests (fastest in-kernel checks)
make unit_tests           # or: ./run_unit_tests.sh  (wraps with a 20s timeout + pass/fail grep)
make unit_tests_intel     # or: ./run_unit_tests_intel.sh

# Automated desktop/framebuffer test (Python-driven screendump validation)
make desktop_test         # runs ./run_desktop_test.py

# Host "golden" tests — compile userland logic natively on macOS, no QEMU (seconds)
make host_tests

# Pass extra QEMU flags to any run (e.g. a QMP socket for screenshots/input)
make run QEMU_ARGS="-qmp unix:./qmp-sock,server,nowait"
```

Toolchain (already wired in the Makefile): `clang`/`ld.lld` from `/opt/homebrew/opt/llvm`, `mkfs.fat`/`mtools` and `qemu-system-*` from Homebrew. The disk is a 64 MB FAT-16 image (`disk.img`) assembled from the kernel + every userland `.bin`.

## Deadlock & Shutdown Discipline

These are hard rules from the project design doc — treat violations as defects, not flakes:

1. **The 30-second rule.** If a build+boot+test run exceeds ~30 s without a clear pass/fail, **stop it and treat the slowness itself as the symptom** — almost always a deadlock, a trap/fault, or a hung driver. Do not just re-run; investigate. A hang often produces *no* clean failure log.
2. **Tests must self-terminate.** `test`/`unit_tests`/`desktop_test` modes end by halting QEMU (`-action shutdown=poweroff`, ARM also uses `-semihosting`). If you add a test that leaves QEMU running, it's incomplete — a passing test that never exits will read as a 30 s "deadlock."
3. **The desktop runs forever by design.** `make run` won't return; that's correct. Quit the Cocoa window or `pkill -f qemu-system` when done.
4. **`make clean` also kills QEMU** (`pkill -f qemu-system`) and removes `obj/`, `disk.img`, `*.elf/*.bin/*.log`. Use it when a stale build or a zombie QEMU is suspected. A `MODE` switch is auto-detected and forces a rebuild via `obj/$(ARCH)/.mode`, so you rarely need a full clean just to change modes.

## Common Pitfalls

0. **The tier verdict is the guest's log, not the exit code.** A failed test
   tier (`Tests failed: N`, `UNIT TESTS FAILED`) can still leave `make` and the
   QEMU wrapper at exit 0 — QEMU self-terminates with 0 after `poweroff`
   regardless of what the kernel printed. Grep the run log for
   `Tests run:`/`Tests failed:`/`UNIT TESTS` — never trust the shell exit code.
0. **`origin/main` here lags the workstation's local `main`** (GitHub receives
   pushes only at wave checkpoints; local work sits unpushed for long
   stretches — e.g. `main` was 170+ commits ahead of origin, including the
   intel `fat16_test` fix that switches the UNEXPAND_T.BIN size from a
   hardcoded ARM value to a build-generated per-arch header). Clones/repos
   created from GitHub get a stale tree and can fail tiers for reasons already
   fixed locally. Sync a fresh machine from the workstation via
   `git bundle create repo.bundle main` → `git fetch <bundle> main:refs/remotes/ws/main`,
   not from origin.

1. **Forgetting `ARCH=intel` for PCIe/RDMA/GPU work.** Those subsystems are x86_64-only (`#ifdef __x86_64__`); on ARM they compile to empty translation units and the feature silently won't run.
2. **Expecting `make run` to return.** It's the interactive desktop — it blocks. For anything scripted/automated, use a test mode or add a QMP socket.
3. **Reusing a stale `disk.img`.** Any change to a userland program requires the disk to be rebuilt; the `disk.img` target depends on every `.bin`, so a normal `make run`/`make test` rebuilds it — but if you copied files by hand, rebuild.
4. **Vendor-tree bootstrap (P3 libc++ pattern).** Files created by a vendor
   script (e.g. `third_party/libcxx-21.1.8/src/.../compiler-rt/lib/builtins/*.c`
   from `fetch.sh`) must NOT appear as make prerequisites: on a fresh checkout
   make rejects the whole implicit rule during its search phase ("No rule to
   make target 'obj/<arch>/builtins/<x>.o', needed by 'libc.a'") because
   nothing can create the missing prerequisite — and an **empty-recipe
   pattern rule does not satisfy the search: the pattern must have a recipe**.
   Instead: gate the object rule on the vendor marker (`.../.fetched-ok`) and
   refetch inside the recipe.  `fetch.sh` early-exits when marker + tree
   verify, so to restore a partially-missing tree delete the marker first
   (`rm -f .../.fetched-ok; bash .../fetch.sh`) and `test -f` afterwards so a
   silent no-op fails loudly.  Mark script-generated sources `.SECONDARY` or
   make deletes them as intermediate files after use (forcing re-extraction).
   Recon that finds this: `make -d <target> 2>&1 | grep <target>` shows which
   rules make tried and why each was rejected.
5. **Zombie QEMU holding the disk/socket.** If a run refuses to start or the display is stale, `fuser -k -9 disk.img` (kills exactly the processes holding the image). **Never** run `pkill -f qemu-system-aarch64` from a scripted shell whose own command line contains that pattern — `pkill -f` matches the invoking bash too (only pkill itself is excluded), silently killing your own background job before make even starts. `fuser -k disk.img` cannot self-match. On a shared workstation never use `make clean` to reset (Makefile:1962-1965 runs `pkill -f qemu-system` = kills every lane's VMs globally).
6. **Most root `run_*.py` runners kill ALL QEMUs on the machine.** `run_desktop_test.py`, `run_desktop_apps_test.py`, `run_antfarm_test.py`, `run_apps_test.py`, `run_console_test.py`, `run_filedialog_test.py`, `run_files_nav_test.py`, `run_games_test.py`, `run_nano_test.py` (plain `qemu-system`), `run_pong_test.py`, `run_xcalc_test.py`, `run_xeyes_test.py` and `capture_screenshots.py` each run `pkill -9 -f qemu-system*` internally (some with the `[-]` self-match trick — still global). Only `run_unit_tests*.sh` kill scoped (`pkill -P`). On a shared host run the python tiers only with a **cwd-scoped `pkill` shim** first on PATH (signals only processes whose `/proc/<pid>/cwd` is inside the tree); proven pattern + working shim: `continuation/gx-launch/evidence-G3/README.md` item 9, shim copy at `HobbyOS-review/.review-artifacts/shim/pkill`.
7. **Every chained tier target recompiles the kernel end-to-end — that is the designed behavior, not staleness.** Top-level make parse applies default `MODE=desktop` (Makefile:42) and rewrites `obj/<arch>/.mode` when it differs; the sub-make for the tier rewrites it again — every kernel object depends on `$(MODE_FILE)`, so all rebuild. `hobbyos.elf` relinks on *every* invocation (`FORCE_ARCH` via `.EXTRA_PREREQS`) and `disk.img` depends on the ELF, so the (now 1 GiB) disk rebuilds too. Budget ~1-3 min per tier on a fast box. Consequence: do not "dry-run" these targets — recipe lines containing `$(MAKE)` execute even under `-n`, so `make -n test` runs the tier for real.
8. **1 GiB startup disk (2026-10 tip).** `disk.img` is 1024 MiB (was 64 MB); anything asserting disk geometry must use bounds covering it (`time_test.c`'s `< 256 MiB` bound went stale with this bump — a deterministic unit-tier red).

## [IDLESTUCK] false positives + live-guest probing (2026-10-06)

**A healthy desktop can log [IDLESTUCK] storms.** At `-smp >= 2` the ARM
desktop (DESKTOP.BIN) periodically appears "stale-claimed" to idle peers
under MTTCG: per-vCPU clock skew makes `cpu_heartbeat_ms[claimer]` look
>2 s old at single-sample instants (the owner's vCPU clock froze while a
peer idled, then refreshes). The L9 arm-and-confirm gates the DISPOSE, but
the unconfirmed arm used to print the full table dump every ~1000 idle
rounds anyway → console spam that reads as a hang. Fixed in process.c by
gating the [IDLESTUCK] dump on `anomaly_confirmed` (claimless reclaim, or
2nd consecutive stale claim) — the inline "[LOSTWAKE] reclaiming/disposing"
lines are untouched. Before calling a desktop "wedged": inject input and
watch it paint, or read `proc_table[1].state` live (state 2↔3 oscillation =
alive). `desktop_test` cannot discriminate this: it halts before the
desktop's first idle/flush.

**Runtime addresses ≠ ELF addresses.** Limine loads the ARM kernel at a
runtime slide (code executes around `0x43a6...`, not the ELF's `0x4008...`
link address). Compute the slide per boot from a KNOWN runtime PC — idle
`safe_wfi` = runtime `0x43a6ac9ac` == ELF `0x400ac9ac` → slide `0x3fa600000`
on this setup — then read .bss symbols at `elf + slide` (proc_table at
`0x442498e0 + slide`, stride `0xA80` for MAX_PROCESSES=64; cpu_current_pids
`0x442738e0`; cpu_heartbeat_ms `0x44273908`). It can change between boots.

**QEMU monitor limitations under MTTCG**: HMP `info registers` only shows
CPU0 (all vCPUs report the same PC), and a halted vCPU's register bank can
be a mid-TB stale mix (PC at the exception vector while SP is still the
user stack) — treat it as a sample artifact. Use the GDB stub
(`-gdb tcp::1234`) for true per-vCPU PCs. QMP screendump does NOT work on
GL windows — use `QEMU_GPU=soft` + QMP `input-send-event` to test desktop
interactivity headlessly (screendump hashes change on input).

## Verification Checklist

- [ ] Correct `ARCH` for the subsystem under test (ARM unless it's PCIe/RDMA/GPU).
- [ ] Correct `MODE` for the goal (interactive vs. which test tier).
- [ ] Run completed in <30 s for any test mode; if not, investigated the hang rather than re-running.
- [ ] No leftover `qemu-system-*` process after a non-interactive run.
