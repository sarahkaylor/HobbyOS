# GB-099-V (round 3) — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-099-V (#164) · verifies GB-099 (#161) · PR #155 (`d7c3c5e`) is now **merged on `main`**
Gate tree = `origin/main` @ `96ba7d2` (`GB-057: guard the cpu_locals diagnostic to x86`, PR #180)
Run window 2026-10-25T13:49Z–14:00Z · worktree `hermes-443f0e7c` (swarm-farm-1)

Tier demanded: **T2 both-arch build** (+T1 host). T3/T4/T5 not named by GB-099, not run.

## VERDICT: GREEN

| # | Clause | Result |
|---|---|---|
| 1 | `make ARCH=arm hobbyos.elf` links; `ARCH=intel` still links | **PASS** — ARM rc=0, 0 `error:` lines; intel rc=0, 0 `error:` lines |
| 2 | ARM record arch-appropriate; `nm obj/arm/trap.o \| grep -w resume_mark` = `T` | **PASS** — `T`, not a stub |
| 3 | `make ARCH=arm unit_tests_arm` 0 failed; `make host_tests` green | **PASS** — 369 run / 0 failed; 482 checks / 0 failed |
| — | Self-test: pre-fix tip fails with `undefined symbol: resume_mark` | **PASS — control fires** (rc=2) |

### Clause 1

    ### A: ARM link @ 96ba7d2
    ld.lld -T linker.ld -o hobbyos.elf obj/arm/boot.o … obj/arm/trap.o obj/arm/uart.o
    rc=0                       (0 `error:` lines)

    ### B: intel link
    llvm-objcopy -I elf64-x86-64 -O elf32-i386 hobbyos.elf
    rc=0                       (0 `error:` lines)

The ARM blocker I reported in round 2 (`cpu_locals` used from shared code) was fixed by the guard in
`96ba7d2`; `main` now compiles and links for both arches. This gate ran on plain `main`, **not** on a
merged branch — the condition round 2's RED asked for.

### Clause 2

    ### C: nm
    $ nm obj/arm/trap.o | grep -w resume_mark
    0000000000000000 T resume_mark
    $ nm obj/arm/process.o | grep -w resume_mark
                     U resume_mark
    $ nm hobbyos.elf | grep -w resume_mark
    7003f4f0 T resume_mark

Defined in the ARM object, referenced from shared `process.c`, resolved in the image — arch-appropriate,
not a silencing stub.

### Clause 3

    ### D2: make ARCH=arm unit_tests_arm
    Tests run:    369
    Tests failed: 0
    UNIT TESTS PASSED
    rc=0

    ### E: make host_tests
    Checks run:    482
    Checks failed: 0
    ALL APPS SUITE TESTS PASSED
    TEST EXIT: 0
    rc=0

### Control — the gate can fail (pre-fix tip `d189a9b`, its own worktree)

    ### F CONTROL: make ARCH=arm hobbyos.elf
    ld.lld: error: undefined symbol: resume_mark
    >>> referenced by process.c:1687 (src/kernel/process.c:1687)
    >>> referenced by process.c:3331 (src/kernel/process.c:3331)
    rc=2

So the PASS is not vacuous: the same build command fails with the exact symbol error on the pre-fix tip
and links on `main`.

### Environment note (honest, not a tier result)

My first `unit_tests_arm` run died with `mkfs.fat: No such file or directory` (exit 127) — the known
§11.2b #5 PATH defect (`/usr/sbin` omitted), not a code failure. Re-ran with
`PATH=/usr/sbin:/sbin:$PATH` → PASS. Logs: `d-unit-arm.log` (env failure) and `d2-unit-arm.log` (pass).

### Non-blocking finding (carried from round 1, still true)

The ARM copy of the record is written but never read: `src/kernel/arch/arm/trap.c:55-61` defines
`resume_mark_pid` / `resume_mark_sp` and stores into them, and no ARM code prints them — the only
consumer is the x64 watchdog dump. Clause 2 is met literally (same field set, arch-appropriate store);
making GB-057's ARM diagnosis readable would need an ARM dump line. That is a follow-on, not this gate.
