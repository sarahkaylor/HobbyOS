# GB-132 — family B: a reproducer exists, but only on the build that carries GB-125's guard

Claimant: `wgames-verify/gate` (DEEP verifier), 2026-10-29 19:22Z → 20:0xZ.
Head `215b79b`, worktree `.worktrees/gb125-verify`, this VM. Local runs only; no CI consulted.
Reproduce: `python3 evidence/gb-132/classify2.py <logs>` (`50-banner-classification.txt` is its output over every log cited).

## 1. The measurement that decides this card's shape

Seven intel T3 waves at `215b79b`, classified banner by banner. The classifier splits
a fatal banner into four states, because "impossible vector" was previously argued
from lines that were **shredded while being printed** (the fatal printer writes field
by field with `uart_puts_raw`, which does not take `uart_lock` — GB-097):

| wave | build | console lines | old-format #PF dumps | halting banners |
|---|---|---|---|---|
| 21 | **unguarded** | 18 973 | 14 | **0** |
| 23 | **unguarded** | 65 047 | 22 | **0** |
| 11 | **unguarded** | 9 278 | 3 | **0** |
| 12 | **unguarded** | 12 113 | 11 | **0** |
| 13 | **unguarded** (stalled) | 11 929 | 6 | **0** |
| 20 | **GB-125 guard** | 51 596 | 36 | **4** |
| 22 | **GB-125 guard** | 59 507 | 43 | **2** |

**0 banners in five unguarded waves; 6 banners in two guarded waves.** Every one of
the six came from a build that differs from `main` only by GB-125's
`call resume_kernel_copy_guard` in the kernel-mode resume path.

```
== 20-t3-intel-wave.log            (guarded)
   BADVEC   vector=2124482397 rip=0x31FFE938 cr2=0x17FFFFEFB8 pid=50 err=0 cr3=0x338FF000
   NONSENSE vector=6 rip=0x7 cr2=0x1000006130 pid=33 err=0 cr3=0x75515000
   NONSENSE vector=6 rip=0x7 cr2=0x10400012B0 pid=32 err=0 cr3=0x7550F000
   OK       vector=13 rip=0x7003CDDC cr2=0x100000FE20 pid=48 err=0 cr3=0x7550F000
== 22-t3-intel-guard-run2.log      (guarded)
   OK       vector=6 rip=0x31FFE5C0 cr2=0x1000009070 pid=32 err=0 cr3=0x75511000
   OK       vector=6 rip=0x29FFE5C0 cr2=0x1000025C3D pid=37 err=0 cr3=0x7550F000
```

`rip=0x7` with `vector=6`, `cr2` in the `0x100000….` image range — that is the carded
family-B signature, and these lines are **not** shredded (`TORN` column is separate;
the classifier's foreign-token test is on the pre-`cr3=` prefix).

## 2. What this does to the card's premise, stated plainly

The card assumes B is a **pre-existing intermittent defect** that GB-125 accidentally
provoked the wrong way. On this VM at this head the opposite is what the runs show:
B is **not** producible without my patch, and is producible **with** it, twice out of
two. So the leading suspect for B here is **GB-125's own instrument**, not a latent
defect in `main`.

I am the author of that patch. That is exactly why this is written as a measurement
with the label on it rather than as a hypothesis about the kernel: it would be
self-serving to hunt a latent defect while my own build is the only thing that
produces the symptom.

**And it retracts a claim I made on GB-125.** There I reported "guarded 46/48 FATALs
vs baseline 18/25, mechanism unnamed". The banners above are the missing explanation:
the extra FATAL lines counted on the guarded build are *these banners*. Both
observations are one observation, and neither is evidence about the underlying tear.

## 3. Why the guard could plausibly do this (named, not claimed)

In the kernel-mode branch the frame is copied to `[gs:[24]-320, gs:[24])` — the
**task's own** stack, which for a parked kernel task is the shared per-CPU stack region
the source comment already warns about ("that scratch region is the shared per-CPU
stack, where parked kernel tasks keep their live frames"). GB-125's guard calls a C
function with a `struct kline` (256 bytes) in that same path, so its transient frame
can live **inside the window the copy later writes**. A guard that is correct in
isolation can therefore still change what the resumed frame looks like.

That is a hypothesis with a testable consequence, not a finding: if it is right, the
symptom tracks the guard's **stack footprint**, not its page walk.

## 4. The experiment that would settle it (not run — budget)

1. Guard variant A: same `call`, first statement `return 1;` — the walk and the
   `kline` frame are compiled out (`-O2` drops them), so the frame shrinks to a few
   bytes. If B disappears, the footprint is the mechanism; if B stays, the `call`
   itself (or its timing) is.
2. Guard variant B: keep the walk, move the record buffer off the stack (a static
   `struct kline`, safe here because the path halts on refusal).
3. In both, print `rsp` at the copy and `dest = gs:[24]-320` for the same pid, so
   "does the guard's frame overlap the destination" is answered by two numbers
   instead of an argument.
4. Only after that, re-ask the original question about `main`: five unguarded waves
   with zero banners is a **hit rate of 0/5**, which does not prove absence — it
   prices a future reproducer campaign at roughly one in five runs or worse.

## 5. Item 2's instrumentation: deliberately not built yet

The card asks to instrument the **write** — frame pointer, the five slots the `iretq`
consumes, CR3, and where the frame was built. Building that now would add a second
modified build to a comparison whose only signal is "which build produced the
banner", and I would not be able to tell the two non-`main` builds apart. It becomes
the right next step the moment §4 tells us whether the footprint matters.

## 6. Item 3 — the discriminator against A, as measured

Forcing an absent destination on the unguarded head (GB-125 `11-falsifier-…`) gives
`vector=14 err=0x2` at `RDI = dest` and then `killing pid=1` + `System halt.` — a
**clean kill, no banner**. The guarded waves give the opposite: a banner whose vector
slot holds a CPU vector (`6`) while `rip=0x7` and `cr2=0x1000006130` are nonsense. Two
different signatures, so A and B are not one event, and any candidate mechanism for B
must explain why A is quiet.

## 7. Honest limits

- n is small: 5 unguarded, 2 guarded. The asymmetry is stark (0 versus 6) but it is
  not a rate estimate; §4 prices the follow-up.
- Wave 13 **stalled** (`[IDLESTUCK]`, killed at 14 min instead of waiting out the
  2400 s deadline). It is counted only for "did any banner appear" — none did — and
  its FAIL/FATAL totals are not used anywhere.
- The GB-125 guarded runs were made while I was also validating a control define; run
  19 (a false positive from my own `#ifdef` bug) is excluded from every table here.
- Nothing in this card changed the resume copy, per item 4.

**No RED/GREEN** — `debug` card. The result is a reproducer with its conditions and a
suspect named against my own patch; the mechanism is not claimed.