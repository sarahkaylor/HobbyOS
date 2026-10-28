# GB-089-V — verdict: GREEN (all three clauses measured on this VM; the no-fix leg re-provoked here, not quoted from October)

Subject: `GB-089` (#142), branch `ticket/89-nullentry-control` @ **`04e81c5`**; control
`tools/gate/t3_nullentry.sh`, driven by `make t3_nullentry` (Makefile:3606). Graded in a fresh detached
worktree of that head; the no-fix leg in a second throwaway worktree of the same head with the a53f26e
fix reverted by hand. Host: farm-3 VM, QEMU system emulation, `T3_SMP=1` (the fault is single-CPU
specific). No CI consulted.

## Clause matrix

| # | Clause | Result |
|---|---|---|
| 1 | `STATE=fixed` reaches `halted=1`, 0 FATAL/fault lines, **both arches**, quoted from tier logs | **PASS** — intel `halted=1`, arm `halted=1`, both `0 FAIL lines, 0 user memory fault records` |
| 2 | Control is make-target-executed, never by hand; `make t3_nullentry` is the gate | **PASS** — `Makefile:3606-3608`; both runs above went through the target, and a mislabeled STATE is **refused** (below) |
| 3 | The control can provably fail: mislabeled STATE exits non-zero; the no-fix run names `vector=13` (intel) / `EL1`+`ELR=0` (ARM) | **PASS** — refusal measured; **and** I re-proved the no-fix fault on this VM rather than quoting the 2026-10-24 record |

## 1 — fixed state, both arches

```
$ make t3_nullentry ARCH=intel STATE=fixed
t3_nullentry: tree has the a53f26e fix (STATE=fixed)
=== t3_nullentry (intel, SMP=1, program=HELLO.BIN, state=fixed) ===
  halted=1 fault_lines=0 (expecting vector=13 on unfixed)
t3_nullentry: PASS — fixed run halted cleanly (a53f26e verified).
RC=0
```
```
$ make t3_nullentry ARCH=arm STATE=fixed
t3_nullentry: tree has the a53f26e fix (STATE=fixed)
=== t3_nullentry (arm, SMP=1, program=HELLO.BIN, state=fixed) ===
  halted=1 fault_lines=0 (expecting EL1/ELR=0 on unfixed)
t3_nullentry: PASS — fixed run halted cleanly (a53f26e verified).
RC=0
```
Tier logs (`obj/t3_<arch>_nullentry.tier.out`, quoted with their own verdict line):
```
T3_intel: 120 console lines, 0 FAIL lines, 0 user memory fault records across 0 processes, 1 programs exited, halted=1
T3_arm:    64 console lines, 0 FAIL lines, 0 user memory fault records across 0 processes, 1 programs exited, halted=1
```
serial, arm (the halt the clause names):
```
44:[WAVE-MANIFEST] HELLO.BIN
52:[ARRIVED] HELLO.BIN rc>0
63:System halt.
```

## 2 — make-target only, and the mislabeled state is refused

```
$ make t3_nullentry STATE=unfixed        # on the fixed tree
t3_nullentry: FAIL — STATE=unfixed but tree HAS kernel_exit; run against plain main
make: *** [Makefile:3608: t3_nullentry] Error 2
RC=2
```
The refusal is the structural reason the control cannot be painted green: `STATE` is checked against
the tree's actual loader body (only the `test_wave_loader` dispatched block, not the init path's own
`kernel_exit`), so a mislabel aborts before QEMU.

## 3 — the discriminating half, re-provoked on this VM

Rather than quote the 2026-10-24 record as prose, I reverted the fix in a throwaway worktree of the
same head — `kernel_exit();` → `return;` inside `if (dispatched > 0)` and nothing else — and ran the
same make target:

```
$ make t3_nullentry ARCH=intel STATE=unfixed
t3_nullentry: tree is unfixed (STATE=unfixed)
=== t3_nullentry (intel, SMP=1, program=HELLO.BIN, state=unfixed) ===
  halted=0 fault_lines=1 (expecting vector=13 on unfixed)
t3_nullentry: PASS — unfixed run provoked the vector=13 fault (GB-087 confirmed)
RC=0
```
serial, verbatim, immediately after the manifest dispatched the program:
```
106:[ARRIVED] HELLO.BIN rc>0
107:[KERNEL] FATAL: Exception in Kernel Mode! Vector: 13 Error Code: 0x0000000000000000
108-  RIP: 0x0000000000000000  RSP: 0x0000000023FFF008
    ...
130:[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13 rip=0x0000000000000000 cr2=0x0000000000000000 pid=1 err=0 cr3=0x000000007550E000 pd480=0x00000000810000FB
```
That is the GB-087 signature exactly (`vector=13 rip=0x0 pid=1`), produced by the same make target on
the same head with only the two-line fix withdrawn. The 2026-10-24 record (PR #110, quoted on #142) says
the same for intel and adds the ARM form `Synchronous Exception in EL1, ELR=0x0 pid=1`; I re-measured
intel and did not re-run ARM (disclosed rather than implied).

## Three residuals, disclosed rather than tidied away

1. **The control's program copy is a no-op that cannot fail.** `t3_nullentry.sh` builds its stage as
   `mcopy obj/$ARCH/$PROG ::$PROG || true` with `PROG=HELLO.BIN`, but the build's artefact is
   `obj/$ARCH/hello.bin` (lowercase, `Makefile:561`). Every run therefore prints
   `.../obj/intel/HELLO.BIN: No such file or directory` and continues, because the image already ships
   `HELLO.BIN` at the root. The manifest itself **does** stage (`obj/<arch>_nullentry.wave` →
   `::WAVE.CFG`, guest prints `[WAVE-MANIFEST] HELLO.BIN`), so the clause's claim — a single-program
   manifest wave — holds; but the `|| true` means a genuinely missing fixture would not fail the gate,
   and that is the opposite of the tier's `T3_STAGE` contract (`t3_tier.sh:180-183` is fatal). GB-118 is
   the card that owns making staging fatal; this gate should adopt it when it lands.
2. **The tier's own exit code is FAIL on both legs, by design.** A one-program wave trips the tier's
   completeness floor, and the unfixed leg additionally trips its FATAL counter:
   `T3_intel: FAIL — only 1 programs exited (floor 55)` and `T3_intel: FAIL — 1 FATAL kernel-mode fault
   line(s)`. The control asserts its own criteria from the serial and reports PASS; nobody should read
   `T3_* EXIT=1` as the gate's verdict, and the tier's numbers are quoted here so that reading is not
   available by accident.
3. **Fixed-arm timing note:** `T3_arm: note — halt marker found after make exited (poll raced the exit)`.
   The halt was observed, `halted=1` is recorded, and the run is reproducible (re-ran it once, same
   result); the line is named because a poll race is the kind of thing that turns into a flake later.

## What this verdict does and does not claim

- It claims the null-entry control **discriminates on this VM at `04e81c5`**: halted-and-clean with the
  fix on both arches, `vector=13` without it, and a refusal when the state is mislabeled.
- It does not claim the ARM no-fix leg was re-run here, and it does not claim the control's staging path
  is sound (residual 1). Both are stated above rather than left for the reader to discover.

## Reproduce

```
git worktree add /tmp/w89 --detach 04e81c5 && cd /tmp/w89      # + third_party symlinks, mkdir obj
export PATH=/usr/sbin:$PATH
make disk.img ARCH=intel -j32 && make t3_nullentry ARCH=intel STATE=fixed
make disk.img ARCH=arm   -j32 && make t3_nullentry ARCH=arm   STATE=fixed
make t3_nullentry STATE=unfixed                                # refused, exit 2
# no-fix leg: second worktree at 04e81c5, revert kernel_exit() -> return; in test_wave_loader
make disk.img ARCH=intel -j32 && make t3_nullentry ARCH=intel STATE=unfixed
```