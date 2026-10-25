# GB-099-V — gate evidence and verdict (VERIFIER, deep, swarm-farm-1, round 2)

Ticket GB-099-V (#164) · verifies GB-099 (#161) · PR #155 head `d7c3c5e`
Base `origin/main` @ `30178e5` · merged gate tree `6913917` (`ort` merge, no conflicts)
Run window 2026-10-25T07:03Z–07:17Z · worktree `hermes-de27f562` (swarm-farm-1)

Tier demanded by the DoD is **T2 both-arch build** (+ T1 host). T3 QEMU/T4/T5 are not named by
GB-099 and were therefore not run — a stated scope, not a skipped gate.

## VERDICT: RED

Clause 1 fails on ARM. The failure is a **compile** error, not a link error, and it is **not**
`resume_mark`.

| # | Clause | Result |
|---|---|---|
| 1 | `make ARCH=arm hobbyos.elf` links on the fix; `ARCH=intel` still links | **RED (ARM)** rc=2 · intel rc=0, 0 `error:` lines |
| 2 | ARM record arch-appropriate, `nm obj/arm/trap.o \| grep -w resume_mark` = `T` | **PASS** |
| 3 | `make ARCH=arm unit_tests_arm` 0 failed; `make host_tests` green | unit_tests_arm **rc=2** (same compile error) · host_tests **rc=0** `ALL APPS SUITE TESTS PASSED` |

### The full `rc=2` block, once (merged tree `6913917`, `make ARCH=arm hobbyos.elf`)

    clang -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -fstack-protector-strong -mstack-protector-guard=global -DKERNEL_MODE_DESKTOP -c src/kernel/process.c -o obj/arm/process.o
    src/kernel/process.c:1827:27: error: use of undeclared identifier 'cpu_locals'
     1827 |           int live_pid = (cpu_locals[cl].current_proc)
          |                           ^~~~~~~~~~
    src/kernel/process.c:1828:32: error: use of undeclared identifier 'cpu_locals'
     1828 |                              ? cpu_locals[cl].current_proc->pid
          |                                ^~~~~~~~~~
    2 warnings and 2 errors generated.
    make: *** [Makefile:744: obj/arm/process.o] Error 1
    rc=2

Every `error:` line in that log is `cpu_locals`; there is **no** `ld.lld: error:` line anywhere in
it, so no undefined symbol is reported at all — the link is never reached. Plain `origin/main`
@ `30178e5` (no PR #155) fails identically: same two lines, same rc=2 (`a-main-arm.log`).

### Answer to the Coordinator's consistency check

The two readings resolve to the second one, and the previous verdict's RED cell was in fact the
`cpu_locals` block; the `ld.lld: error: undefined symbol: resume_mark` text quoted back was from
that verdict's **Control** section (pre-fix tip), which this round re-ran to confirm:

    ### I: CONTROL — pre-fix tip d189a9b, make ARCH=arm hobbyos.elf
    ld.lld: error: undefined symbol: resume_mark
    >>> referenced by process.c:1687 (src/kernel/process.c:1687)
    >>> referenced by process.c:3331 (src/kernel/process.c:3331)
    rc=2

So: `resume_mark` was the pre-fix blocker and PR #155 does define it; the residual ARM blocker on
`main` is a **second x64-only symbol, `cpu_locals`** (declared inside `#ifdef __x86_64__` at
`src/kernel/process.c:350-358`; the `[ALIVE-SET]` block from `9ebfbbd` reads it at 1827-1828).
The card's scope therefore grows by exactly that one symbol — and it is not GB-099's to fix.

### Proof that `cpu_locals` is the only remaining ARM blocker (throwaway, not a deliverable)

Replacing the three-line expression with the arch-neutral `int live_pid = cpu_current_pids[cl];`
(`process.c:246`, `src/include/process.h:623`), rebuilding the **merged** tree:

    ### E: THROWAWAY diagnostic — merged tree, make ARCH=arm hobbyos.elf
    rc=0
    --- nm hobbyos.elf | grep -w resume_mark
    00000000400bbfb0 T resume_mark

Tree reverted immediately after the run (`git diff` empty). This is diagnosis, not implementation:
GB-099-V is a gate, and the fix belongs to the card that owns `cpu_locals`.

## Quotes for the clauses that pass

    ### C: nm on the merged tree's ARM objects
    $ nm obj/arm/trap.o | grep -w resume_mark
    0000000000000000 T resume_mark

    ### D: merged tree — make ARCH=intel hobbyos.elf
    llvm-objcopy -I elf64-x86-64 -O elf32-i386 hobbyos.elf
    rc=0                       (0 `error:` lines)

    ### F: merged tree — make host_tests
    ALL APPS SUITE TESTS PASSED
    TEST EXIT: 0
    rc=0

    ### G: merged tree (unmodified) — make ARCH=arm unit_tests_arm
    src/kernel/process.c:1827:27: error: use of undeclared identifier 'cpu_locals'
    src/kernel/process.c:1828:32: error: use of undeclared identifier 'cpu_locals'
    rc=2

`unit_tests_arm` cannot be evaluated on `main` until `process.c` compiles for ARM; the throwaway run
of it exceeded the verifier's 7-minute window and was terminated, so `h-*.log` is marked INCOMPLETE
and is not used as evidence.

## What would make this GREEN

A fix for the `cpu_locals`-on-ARM instance landed on `main` (accessor or ifdef), then this gate
re-run **on a tree merged with `main`**: `make ARCH=arm hobbyos.elf` rc=0, intel rc=0,
`make ARCH=arm unit_tests_arm` → `Tests failed: 0`, `make host_tests` green. GB-099's own fix
(`d7c3c5e`) needs no change to pass clauses 2/3 — only a base that compiles.
