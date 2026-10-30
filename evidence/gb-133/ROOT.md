# GB-133 — the root discriminator answers it: **no page is ever re-cleared. The AS is replaced.**

Head `215b79b`, this VM, intel. Probe now carries `root=` (the process's AS root) and `cr3=` beside
`pte=`. Patch `40-root-probe.patch`, analyzer `flips2.py`.

```
probe lines 5333
flips same-root (real unmap): 0
flips root-changed (new AS):   8
flips unknown:                0
```

**Every one of the eight `pte 1→0` transitions is a root change, and `cr3 == root` in every sample**
— so the walk is always reading the table that is actually in use, and the "absent" page belongs to
a **different address space** than the "present" one it was compared with.

Two of the eight, verbatim:

```
[GB133-R] sch pid=14 pg=0x17FFFFF000 pte=1 root=0x20DB2000 cr3=0x20DB2000      (line 344)
[GB133-R] sch pid=14 pg=0x17FFFFF000 pte=0 root=0x20E02000 cr3=0x20E02000      (line 366)
```

and inside that 22-line interval:

```
v2 loader: image mapped (lazy), bytes=55656 pages=14 first=in elap=1ms
```

That is **exec/pid reuse**: pid 14 was re-loaded with a new program and a brand-new AS. The page was
never taken away; the pid simply started pointing at a different table.

## What this settles, and what it changes

1. **GB-133's question is answered**: nothing re-clears the committed page. GB-122's page-watch saw
   a commit stick and the page "gone" at the next resume because the *next resume was in a new AS*.
   Both observations were true; the inference that joined them was mine and it was wrong twice
   ("never committed", then "cleared again").
2. **GB-122's commit-ceiling is explained** without invoking a mask: each new AS starts with a
   demand-zero stack top, so every load/resume pair commits the page once. 68-142 commits per wave
   is the cost of *that*, not of a leak.
3. **The real defect is narrower than "an unmap"**: a resume can reach a process whose AS exists but
   whose stack-top page has not been committed yet — the loader's own 64 KiB prefault
   (`program_loader.c:834-840`) is either after the process becomes runnable, or not applied on this
   path. That is a **race**, and it is testable: print in the loader before and after the prefault,
   and check whether a `[GB133-R] pte=0` for that pid falls between them.

## Does this close GB-122?

No, and it should not: GB-122's DoD asks for five halting waves with `Vector: 14 = 0`, and the
fix that would follow from this measurement is "commit the page before the process is runnable",
which is a loader change, not the resume-side commit I measured. The resume-side commit suppresses
the fault (0/1/0 across three complete waves) precisely because it fills the new AS's hole at the
first resume — a correct but late repair.

## Limits

- One wave, **`Vector: 14 = 2`**, and the wave was still running when I read the probe stream; the
  flip census (5 333 probes, 8 flips) is complete for what ran, the wave verdict is not.
- `root=` is `as->root_phys`; a root recycled by the frame allocator would look like "same root".
  With 0 same-root flips in this wave that risk did not materialise, but it is a real one.
- I have not yet instrumented the loader's prefault to prove the race; that is the next measurement
  and it is the one that would turn this from "the AS is replaced" into "the AS is replaced *while
  runnable*".

**No RED/GREEN** — `debug` card.