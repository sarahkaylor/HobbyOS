---
name: hobbyos-run-tests
description: "Use when running or interpreting HobbyOS tests. Covers the three tiers — host golden tests, kernel unit tests, userland integration tests — plus the two-VM RDMA test, pass/fail signals, and the deadlock timeout rule."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [hobbyos, testing, unit-tests, integration, host-tests, qemu, rdma]
    related_skills: [hobbyos-build-and-run, hobbyos-gui-test, hobbyos-kernel-constraints, hobbyos-proxmox-gpu]
---

# HobbyOS: Running & Interpreting Tests

## Overview

HobbyOS has three test tiers, fastest to slowest: **host golden tests** (userland logic compiled natively on macOS, no QEMU), **kernel unit tests** (subsystems checked in EL1 before the scheduler), and **userland integration tests** (real processes/syscalls/SMP/IPC under the booted kernel). Plus a **two-VM RDMA test** for the remote-PCIe path. Start at the cheapest tier that can catch the bug: reach for host tests for pure logic, unit tests for kernel internals, integration for anything touching processes or drivers. Every tier signals pass/fail in text and must halt QEMU itself — a run that just *hangs* is the failure.

## When to Use

- Running the suite after a change, or adding a new test.
- Deciding which tier fits what you changed.
- Interpreting a hang/timeout vs. a clean fail.

Don't use for: choosing build ARCH/MODE mechanics (→ [[hobbyos-build-and-run]]) or GUI validation (→ [[hobbyos-gui-test]]).

## Tier 1 — Host golden tests (seconds, no QEMU)

Userland programs are compiled natively on macOS with `-DHOST_TEST` against a mock libc/framebuffer (`src/host/compat.c`), then run directly. This is the fast baseline for game/app/logic bugs.

```bash
make host_tests    # builds + runs the editor and pong host tests
```

Pattern (see `src/host/pong_test.c`): the test `#include`s the userland `.c` with `#define main <name>_main`, then unit-tests its pure functions (ball physics, collision, scoring…) with `ASSERT`s, printing `PASS`/`FAIL` and a summary, returning non-zero on any failure. Add a new one by writing `src/host/<prog>_test.c`, adding a `*_host` link rule in the Makefile's host section, and listing it under the `host_tests` target. **Prefer this tier for any logic that doesn't need real kernel services** — it's the fastest signal by far.

## Tier 2 — Kernel unit tests (in-EL1, before scheduler)

```bash
./run_unit_tests.sh          # ARM: builds, runs headless, greps, 20s timeout
./run_unit_tests_intel.sh    # x86_64
# or directly:
make unit_tests              # ARCH=arm; make unit_tests_intel for x86_64
```

`run_unit_tests.sh` runs `make unit_tests` headless into `qemu.log` and polls for the signal strings, killing QEMU on either:

- **`UNIT TESTS PASSED`** → exit 0.
- **`UNIT TESTS FAILED`** → exit 1, dumps the log.
- Neither within **20 s** → "timed out," exit 1 (see the deadlock rule below).

## Tier 3 — Userland integration tests (booted kernel)

```bash
make test          # ARCH=arm;  make test_intel for x86_64
```

`MODE=test` makes the scheduler auto-spawn a fixed sequence of userland test binaries (`fork_test`, `smp_test`, `pipe_test`, …) to validate syscalls, multi-core scheduling, and IPC, then halt. Use when the change touches processes, scheduling, the filesystem, or drivers — things the host tier can't model.

## Two-VM RDMA test (remote-PCIe, x86_64)

```bash
./run_two_instances.sh
```

Boots two x86_64 instances locally — a **host/provider** (owns the `-device edu` card, listens on `:12345`) and a **receiver/consumer** (connects over a QEMU socket netdev, emulates the card via RDMA) — using `fw_cfg opt/pcishare=host|guest:0x1234:0x11e8`. It polls `receiver.log` for `UNIT TESTS PASSED/FAILED` with a 25 s timeout and prints both logs. This is the local rehearsal for the Proxmox GPU path in [[hobbyos-proxmox-gpu]].

## The Deadlock Timeout Rule (project-wide)

**If any test tier runs longer than ~30 s without a clear PASS/FAIL, stop it and treat the slowness as the defect** — almost always a deadlock, an unhandled trap/fault, or a hung driver waiting on an interrupt that never fires. Hangs frequently emit **no** failure log, so "no output yet" is itself the signal. Do not paper over it by bumping the timeout; find the root cause (see [[hobbyos-kernel-constraints]] for the common trap sources). The wrapper scripts already cap at 20–25 s for this reason.

## Common Pitfalls

1. **Adding a test that never halts QEMU.** A passing test that doesn't trigger shutdown reads as a 30 s deadlock. Ensure the tier's exit path fires (`-action shutdown=poweroff`; ARM `-semihosting`).
2. **Testing kernel-dependent logic on the host tier** (or vice-versa). Host tests use mock syscalls — they can't validate real scheduling/driver behavior; don't trust them for it.
3. **Ignoring a timeout as "flaky."** A timeout is a first-class failure here. Re-running without investigating wastes cycles and hides a real deadlock.
4. **Wrong ARCH.** RDMA/PCIe/GPU tests are x86_64-only; running them on ARM silently no-ops.
5. **Stale `disk.img` / zombie QEMU.** If results look impossible, `make clean` (kills qemu, wipes artifacts) and re-run.

## Verification Checklist

- [ ] Ran the cheapest tier that can actually catch the change.
- [ ] Saw an explicit `PASS`/`UNIT TESTS PASSED`/summary — not just "no error."
- [ ] Any run >30 s was stopped and investigated as a deadlock, not re-run blindly.
- [ ] New tests halt QEMU on completion.
- [ ] No leftover `qemu-system-*` processes.
