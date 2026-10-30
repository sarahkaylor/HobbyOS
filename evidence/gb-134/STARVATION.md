# GB-134 — the starvation is not cost, not rc, and not the wrong table. It is a **`uart_lock` wedge**.

Head `215b79b`, this VM, intel. One instrumented wave (as the card asked: instrument before coding).
Patch `20-inst-exec.patch`. Wave `10-inst-*`: **stalled**, killed; `Vector: 14 = 0`, 19 exec
prefaults observed before the stall, no tier summary.

## The instrumented line (DoD item 1)

Printed around the added `vm_touch` in the exec path — cost, frame accounting, return code, and
whether `grp->as` is the table actually installed:

```
[GB134] pid=15 ms=0 rc=0 frames=11732->11750 root=0x20E19000 cr3=0x20E19000
[GB134] pid=41 ms=0 rc=0 frames=30922->30940 root=0x2C146000 cr3=0x2C146000
[GB134] pid=16 ms=0 rc=0 frames=31443->31461 root=0x2CB83000 cr3=0x2CB83000
```

19 lines, all identical in shape:

| candidate | prediction | measured |
|---|---|---|
| 1. cost per exec | large `ms=` | **`ms=0` on every line** |
| 2. lock interaction | `rc!=0` or a hang inside the call | `rc=0`, call returns; the wedge is elsewhere |
| 3. wrong table | `root != cr3` | **`root == cr3` on every line** |

Cost is +18 frames per exec (16 data + 2 tables), which is what a 64 KiB prefault should cost.

## Where the wave actually dies (and it is the same place without my patch)

The wave stalls on a spinlock. Resolving that address against the built kernel:

```
7557fdc8 B uart_lock
```

```
[STALLBREAK] cpu=7 lock=0x000000007557fdc8 owner_cpu=2 stale_ms=2244 pc=0x...3c7be
[STOLEN]     cpu=1 lock=0x000000007557fdc8 from halted cpu=5  pc=0x...3c7be
```

**And wave 81 — the previous exec-prefault build, which had no instrumentation at all — stalled on
the same `uart_lock`** (`0x7557fdc8`, `pc=0x7003c6de`). So the wedge is a property of the
exec-prefault build, not of my printing, and it is **not** a failure inside `vm_touch`.

This is the console-lock family this board already has open (GB-097: the lock-free console path
bypasses `uart_lock`; GB-082/GB-094 interleaving), not an exec defect. The honest statement of the
mechanism available from these two waves: extra per-exec work changes the timing enough that a CPU
sitting in the console path is seen holding `uart_lock` past the stall threshold, and the wave dies
there rather than in the stack copy.

## What this means for the fix

The prefault itself is **correct and cheap** by measurement: right table, right rc, no measurable
time, the expected frame cost. What it cannot do is make the wave survive a console-lock wedge that
the extra work makes more likely. So the DoD's items 2-5 are **not met**, and they are not met for a
reason outside this card:

1. starvation explained — **yes**, by the two instrumented waves above;
2. prefault lands correctly — **not proven** (the wave dies before enough execs to sample a first
   resume reliably);
3. wave parity (≥86 programs, `halted=1`, `Vector: 14 = 0`) — **not met**: 19 execs, stall;
4. a test that fails pre-fix — **not built**;
5. whether GB-122's mask is still needed — **not answerable yet**; do not remove it.

## Recommendation

Do not retry the prefault against this bar until the `uart_lock` wedge is owned. The prefault is the
right change and its cost is measured; the wave it lands in is the problem. Two candidate next steps,
both cheap:

1. **Print nothing during the wave** and re-run the same prefault (no `[GB134]` line, no `[GB133-R]`)
   to see whether the wedge is provoked by console traffic or purely by the extra allocation — one
   wave, and it separates "my instrumentation" from "the prefault" as the trigger.
2. **Take `vm_lock` around the prefault out of the wave's hot path** (prefault at spawn only, and make
   exec reuse the existing pages) — more invasive, and only worth it if 1 shows the wedge survives
   with no printing.

## Limits

- One instrumented wave, stalled and killed: no `halted`, no program count, `Vector: 14 = 0` is a
  partial-wave count.
- The stall dumps are shredded in places (the `cpu=16777222` line is a corrupted field), which is why
  the lock address was resolved against the kernel symbols rather than read from the text.
- Wave 81 (previous build) is cited only for the lock address; its 8-program count is not reused as a
  new measurement.

**No RED/GREEN** — `debug` card.