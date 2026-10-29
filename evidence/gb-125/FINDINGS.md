# GB-125 — candidate (c) **eliminated**, one latent skew found, and a better candidate named

Head `f2cdf5c` (`origin/main`, the exit-frame fix). Static work from the head's own source; no new wave was
needed to answer (c), and none is claimed.

The card's instruction was: *"(c) has not been ruled out and is the cheapest to test — check that first."*
This is that check.

## (c) is ruled out for every vector this CPU can raise

The entry stubs normalise the error-code asymmetry at the source, so the frame has the same shape whether or
not the CPU pushed an error code (`src/kernel/arch/x64/trap.c:3312-3343`):

```c
#define EXCEPTION_NO_ERR(num)  "push 0\n" "push " #num "\n" "jmp common_trap_wrapper\n"
#define EXCEPTION_ERR(num)     "push " #num "\n" "jmp common_trap_wrapper\n"
```

For a no-error-code vector the stub pushes `0` (error code) then the vector; for an error-code vector the CPU
has already pushed the error code and the stub pushes only the vector. Both leave `[vector][error_code]` at the
top of the frame, which is what `struct trap_frame` and the printer expect (`vector` at `tf+264`, `error_code`
at `tf+272`).

So a *valid* frame cannot have a garbage vector from a skew. What was printed was genuinely not the vector the
CPU pushed — which is the card's premise, not a reporting bug.

## The one real skew in that list (latent, not the cause)

`EXCEPTION_NO_ERR(21)`, `EXCEPTION_NO_ERR(29)`, `EXCEPTION_NO_ERR(30)` are wrong *in principle*: `#CP` (21),
`#VC` (29) and `#SX` (30) push an error code on CPUs that implement CET/SEV-ES/SGX. If one of those fired, the
frame would be skewed by 8 bytes and the printer would read the error code as the vector.

They cannot fire on this guest — QEMU TCG without CET, SEV-ES or SGX — so they are not the source of the
observed values, but they are a real latent defect worth a one-line fix (`EXCEPTION_ERR` for all three) before
anything enables those features.

## What the observed frame actually looks like

From the wave that produced the carded values (`evidence/gb-122/02-t3-before.serial.log`, pre-exit-fix head):

```
vector=15676 rip=0x0000000000000097 cr2=0x00000017FFFFF2F8 pid=28 err=0
vector=0     rip=0x0000000000000000 cr2=0x00000017FFFFF388 pid=26 err=0
```

Three things do not fit "corruption with no structure":

- `err=0` — a real #PF at those cr2 values would carry `err=2` (write, not-present). The *error slot* is a
  clean, plausible-looking zero while the vector slot is nonsense: the words were written independently, not
  shifted.
- `cr2` lands in the top stack page (`0x17FFFFF2F8`, `0x17FFFFF388`) — the same window GB-122 measured, i.e.
  the frame copy's destination — and `rip=0x97` is the carded below-the-NULL-page value.
- the values are *other code's data*: `0x7003B82E` is a kernel text address, `0x97`/`0x7` are small integers.
  Those are what a neighbouring writer's own words look like.

## The candidate I would test next (named, not claimed)

**Two CPUs writing one stack.** If the same process is resumed on two CPUs (a lost-claim / double-run bug),
both `enter_user_space` calls copy a frame into the *same* `target_sp` window, and each overwrites the other's
words after the `[TORNFRAME]` gate has already passed. That explains every property above at once: coherent at
entry, garbage at print, `err=0` (the other writer's zero, not a real fault's 2), cr2 inside the shared stack
page, and values that are code addresses and register contents rather than shifted frame fields.

The probe that would settle it, in one wave:

```
[RESUME] cpu=N pid=M sp=0x…        (at both enter_user_space call sites)
```
then check for two different `cpu=` values carrying the same `pid=` and the same `sp=` in the same window, or
for the fatal's pid appearing in two CPUs' claims at the moment of the fault. `[IDLESTUCK] … claims:` lines
already print per-CPU claim state and are the natural place to correlate.

I did not run it: it needs a build plus a lucky wave, and the honest handover is the probe, not a claim I did
not make.

## Summary

- (c) printer-offset: **eliminated** for every vector this CPU raises, by construction (quoted above).
- latent skew at 21/29/30: **real**, documented, not the cause on this guest.
- next candidate: **one stack, two CPUs** — probe specified above.