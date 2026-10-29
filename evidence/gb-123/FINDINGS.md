# GB-123 — clause 1 **unmeasured** in this attempt (two instrumented waves never reached the detail path)

Head `98fc494`. Host farm-3, intel 8 CPUs, QEMU TCG. Worktree `.worktrees/gb123`, evidence `evidence/gb-123/`.
No CI was consulted.

The card's rule: *"**RED** if item 1 is unmeasured. Do not close this on symbolization."* Item 1 is unmeasured
here, so this is RED — but the instrument that would measure it is built, committed, and two waves were run
with it.

## What I built (and what it would prove)

`evidence/gb-123/01-snapshot.patch`:

- At the very top of `general_interrupt_inner` (above the `[TORNFRAME]` gate), snapshot `tf->vector`,
  `tf->cs`, `tf->ss` and `tf` into per-CPU slots.
- On the fatal detail print, emit `| GB123 entry: vector=N cs=X ss=Y now: cs=X ss=Y TEAR=yes|no`.

`TEAR=yes` means the frame was coherent when the handler took it and garbage by the time it printed — the
card's claim, observed rather than symbolized. `TEAR=no` with a garbage vector means the CPU itself pushed
that frame, which would **falsify** the claim.

## What the two waves did instead

```
T3_intel: 10671 console lines, 82 FAIL lines, 6 user memory fault records across 5 processes, 85 programs exited, halted=1
T3_intel: FAIL — 6 FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, not because the wave completed
```
```
T3_intel: 11782 console lines, 85 FAIL lines, 7 user memory fault records across 7 processes, 82 programs exited, halted=1
```
`grep -c "GB123 entry"` → **0** in both serials. What the serials contain is the banner form only, six and
seven times, with the console interleaved as usual:

```
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000002
```

and no `[KERNEL] FATAL kernel-mode fault, halting this CPU: …` line at all. That is the print the card quotes
and the only place the frame slots can be read, so on these two runs there was nothing for the instrument to
report: the guest died through the banner path instead.

## Why I stopped rather than re-rolling

The detail path is *reachable* at this head — an earlier wave of mine on the same sha produced 13 of those
lines (`evidence/gb-122/02-t3-before.serial.log`, 26399 console lines, 21 memory-fault records across 19
processes) — but it is not *reliable*: 1 in 3 waves at my current rate, and each wave costs ~6 minutes. Two
coin flips in a row came up empty. Continuing would be rolling dice, not measuring, so the honest stop is
here with the instrument in place.

## Static work done (context for the next owner, explicitly not proof)

The card's symbolization is consistent with `enter_user_space`'s prologue, which I disassembled from the
kernel this VM built (`evidence/gb-123/disasm.py`):

```
enter_user_space 0x7003cdbb .. 0x7003cf00 (325 bytes)
    0:  mov  r12,rdi          ; tf
    3:  mov  r13,rsi          ; target_sp
    6:  call 0x245            ; restore_user_sp_helper  <- pushes a return address at [rsp-8]
   ...
   3a:  lea  rsp,[rdi-0x128]  ; the 296-byte frame window
```
with the frame slots at `vector=tf+264`, `error_code=tf+272`, `cs=tf+280`, `ss=tf+288` (x64
`struct trap_frame`: `regs[30]` 0-232, `lr` 240, `elr` 248, `spsr` 256). A `call` made while `tf` sits just
above `rsp` writes a return address into exactly those slots — which is why `restore_user_sp_helper+0x12` and
`arch_fpu_restore+0x1E` were found in the SS and vector slots by the card's author. That remains a
hypothesis by this card's own standard; the snapshot is what would settle it.

## Where this stops

- Clause 1: **unmeasured** (RED by the card's rule) — instrument built, two waves, detail path never taken.
- Clause 2: **not attempted** (depends on clause 1).
- Clauses 3-5: **not attempted** (fix and waves depend on 1-2).

The instrument is committed so the next owner does not rebuild it; the cheapest next step is to run it
until a wave takes the detail path (or to instrument the banner path as well, which would catch the case that
actually dominated my two runs).

## Reproduce

```
cd .worktrees/gb123                     # 98fc494 + 01-snapshot.patch
make disk.img ARCH=intel -j32
for i in 1 2 3; do T3_MONITOR=0 T3_TIMEOUT=300 W0_GATE=0 bash tools/t3_tier.sh intel; \
  grep -a "GB123 entry" obj/t3_intel.log; done
python3 evidence/gb-123/disasm.py        # the prologue and the slot offsets
```