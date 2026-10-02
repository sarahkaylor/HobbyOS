# L8 x64-loadfix — root cause + fix (browser/l8-x64loadfix)

## Status: FIXED + VERIFIED (KVM repro green, unit + ARM wave + lane gate)

## Root cause (one paragraph)

The x64 KVM fault is NOT a loader/mapping/phys-map bug and does not depend on
image size.  The watchdog's caller-chain diagnostics in
`src/kernel/arch/x64/trap.c` (`watchdog_tick` -> WD2 stall dump + deep-freeze
forensics) scanned each core's kernel stack with:

    uint64_t top = cpu_locals[c].kernel_stack;
    for (uint64_t a = top - 8; a > (top - 24576) && hits < N; a -= 8) {
      uint64_t v = *(volatile uint64_t *)a;   // unguarded
      ...

with NO validity guard on `top` (the sibling `frp`/`wd_last_tfp` scan above it
IS guarded).  Any core that never powered on — cores 4-7 under `-smp 4` (used
by the WK-1 jsc runner and by the M4 fault box), or cores that time out on a
slow machine — leaves `cpu_locals[c].kernel_stack == 0` in BSS.  The loop then
starts at `a = top - 8 = -8` and the very first dereference raises Vector 14,
err=0, CR2=0xFFFFFFFFFFFFFFF8.  The `~30 MB v2 load` only matters as the thing
that keeps the console silent for >20 s, so the watchdog fires mid-load and
executes the scan on a never-booted core.  TCG was green because in those runs
the load/diagnostics did not line up (fast loads finish before the 20 s
threshold / cores booted), not because of any MMU difference; my TCG repro of
the same config shows cores 4-7 time out identically, and the watchdog simply
never fired before the load completed.

## Evidence

- Repro (synthetic 30,167,040-byte BIG.BIN = 7365 x 4 KiB pages, MODE=bigload,
  generated at build time; no WebKit assets):
  `-smp 4 -m 4096M -enable-kvm` reproduces the M4 fault 1:1 (RIP
  0x7002E600 = `mov 0x8(%rbx),%r12` in general_interrupt_handler =
  watchdog_tick's scan loop, RBX=-16 -> CR2=-8; same RSP/STACK shape as the
  M4 log).  See continuation/l8/kvm-repro-BEFORE-fix.log.
- Disassembly of the RUNNING (MODE=bigload) binary (trap.c line 2076-ish range):
  0x7002E5C0 loads cpu_locals[c].kernel_stack into RBX; the loop at 0x7002E600
  reads [RBX+8]; with RBX=-16 (top==0) that is [-8].
- Before fix: `[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 ... CR2:
  0xFFFFFFFFFFFFFFF8` during the v2 load (pid=-1).
- After fix: same command is GREEN — watchdog prints
  `CSTK cpu4..7: (uninitialized kstack, skip)`, the 30 MB load completes
  (`v2 loader: image mapped, bytes=30167040 pages=7365`), synthetic image runs
  and is killed, `System halt`.  TCG same config: green.
- Unit-x64 (KVM): UNIT TESTS PASSED.  Unit-arm: UNIT TESTS PASSED.  ARM wave: 0 FAIL + System halt.

## Fix

- `src/kernel/arch/x64/trap.c`: new `watchdog_stack_scan_ok(uint64_t top)`
  (declared in `trap.h`) = `top >= 0x70000000ULL && top < 0x75000000ULL`,
  applied to both unguarded scans (WD2 `stk` scan at ~line 1838 and deep-freeze
  CSTK scan at ~line 1860).  Skips never-booted cores instead of faulting.
- Regression unit test: `trap_test.c` `test_watchdog_stack_scan_guard`
  (top==0, garbage, above-region tops rejected; in-region tops accepted).

## How to reproduce / re-verify

    make ARCH=intel MODE=bigload disk.img
    qemu-system-x86_64 -M q35 -smp 4 -m 4096M -pflash ~/.local/share/OVMF/OVMF_CODE_4M.fd \
      -display none -serial stdio -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
      -drive file=disk.img,format=raw,id=disk0,if=none -device nvme,drive=disk0,serial=1234,bus=pcie.1 \
      -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
      -action shutdown=poweroff -enable-kvm
    # before fix: FATAL Vector 14 CR2=-8; after: image mapped + System halt

## Risks / notes

- The guard uses the kernel/stack window [0x70000000, 0x75000000); if a future
  build moves kernel stacks elsewhere (linker.ld / USER_*/kmap changes) update
  watchdog_stack_scan_ok and trap.h comment together.
- The same watchdog-fire condition can still mask a genuinely wedged core
  (scan is skipped for uninitialized stacks by design - it only reports what
  it can safely read).
- Repro infra (MODE=bigload, BIG.BIN, bigload_intel target) is build-time self
  contained in the lane; the synthetic image never runs real code.
