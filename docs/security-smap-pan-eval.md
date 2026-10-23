# SEC-4 evaluation: SMEP/SMAP (x64) and PAN (ARM) for HobbyOS

**Status:** evaluated; NOT enabled. Rationale below — enabling either bit today
would break the kernel without closing the hole it is meant to close, because
of how HobbyOS maps memory.

## What was checked

- **x64 (`src/kernel/arch/x64/fpu.c`):** CR4 is touched per core only for
  `OSFXSR`/`OSXMMEXCPT`. `SMEP` (bit 20) and `SMAP` (bit 21) are never set.
- **ARM (`src/kernel/arch/arm/boot.s`, `mmu.c`):** the boot path runs on
  cortex-a53 (QEMU `-M virt -cpu cortex-a53`). PAN is an ARMv8.1 feature;
  A53 is ARMv8.0 — **there is no PAN hardware to enable** on the primary test
  platform. SCTLR_EL1.PAN/EPAN are likewise not touched.

## Why enabling SMAP would not close issue #4's hole (as filed)

Issue #4's real concern is "any kernel bug that dereferences an
attacker-controlled pointer becomes arbitrary kernel read/write". On HobbyOS
the dominant reason that is true is **not** the user mappings — it is the
kernel direct map: kernel VA == physical address covers all of RAM on both
arches (that is what `vm_kwrite` and now `vm_user_read` exploit to touch any
frame). SMAP/PAN only guard accesses made *through user-attributed pages*; a
kernel that can walk the direct map can read or write every user frame with
SMAP fully enabled. SMEP likewise protects against executing user pages, but
the kernel never jumps into user code today (returns go via `eret`/`iretq`).

So SMAP/PAN are defense-in-depth *after* the direct map is restricted; before
that they add cost and breakage without removing the capability.

## What enabling would additionally require

Every syscall path that dereferences a user VA directly in kernel mode would
first have to move behind accessors:

- `vm_user_read()` (SEC-2) — fault-safe read via the direct map; SMAP-clean.
- `vm_kwrite()` — cross-context write via the target's tables + direct map.
- Remaining raw-dereference families still in tree: FAT16 path/`stat` string
  reads, `vfs_abs_path`, socket/iovec struct copies (`fs.c` range-checks then
  dereferences), GPU rect arrays (issue #7), and the signal-frame writes.

That audit is a multi-week change with no behavioral payoff until the direct
map shrinks; it should be sequenced as: (1) finish accessor conversion,
(2) fragment or restrict the direct map per-process (frame ownership),
(3) flip SMAP on x64 and require ARMv8.1+ hardware for PAN.

## What this commit does instead (the plan's other two bullets)

- **Stack canaries:** kernel-wide `-fstack-protector-strong` with a global
  guard (`src/kernel/ssp.c`, seeded at boot from the physical counter through
  splitmix64; `__stack_chk_fail` prints and halts). Stack smashes are no
  longer silent.
- **Guard page:** tracked separately as issue #10 (kernel-stack layout, not
  user-pointer dereference).

## Interim mitigations in effect today

- SEC-2/SEC-3 accessors (`vm_user_read`) never depend on the caller's active
  address space and cannot be defeated by remapping races.
- Canaries catch linear overflows of any buffer-bearing kernel function
  before its return executes.
