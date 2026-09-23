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
| `arm` (default) | `qemu-system-aarch64 -M virt -cpu cortex-a53`, 4 cores, 2 GB | **Primary target.** All features must work here first. |
| `intel` | `qemu-system-x86_64 -M q35`, 8 cores, 3 GB | Secondary. Needed for PCIe/RDMA/GPU work only. |

| MODE | `-D` define | What the kernel does after boot |
|------|-------------|--------------------------------|
| `desktop` (default) | `KERNEL_MODE_DESKTOP` | Boots the window manager / desktop. Runs forever — interactive. |
| `test` | `KERNEL_MODE_TEST` | Scheduler auto-spawns userland integration tests (fork, smp, pipe…) then halts. |
| `unit_tests` | `KERNEL_MODE_UNIT_TEST` | Tests kernel subsystems in EL1 before the scheduler starts, then force-halts QEMU. |
| `desktop_test` | `KERNEL_MODE_DESKTOP_TEST` | Boots desktop, auto-launches a UI app to exercise the framebuffer, self-shuts down. |

Non-`desktop` modes automatically append `-display none`. `desktop` uses `-display cocoa` (a window opens on your Mac).

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

1. **Forgetting `ARCH=intel` for PCIe/RDMA/GPU work.** Those subsystems are x86_64-only (`#ifdef __x86_64__`); on ARM they compile to empty translation units and the feature silently won't run.
2. **Expecting `make run` to return.** It's the interactive desktop — it blocks. For anything scripted/automated, use a test mode or add a QMP socket.
3. **Reusing a stale `disk.img`.** Any change to a userland program requires the disk to be rebuilt; the `disk.img` target depends on every `.bin`, so a normal `make run`/`make test` rebuilds it — but if you copied files by hand, rebuild.
4. **Zombie QEMU holding the disk/socket.** If a run refuses to start or the display is stale, `pkill -f qemu-system` (or `make clean`) first.

## Verification Checklist

- [ ] Correct `ARCH` for the subsystem under test (ARM unless it's PCIe/RDMA/GPU).
- [ ] Correct `MODE` for the goal (interactive vs. which test tier).
- [ ] Run completed in <30 s for any test mode; if not, investigated the hang rather than re-running.
- [ ] No leftover `qemu-system-*` process after a non-interactive run.
