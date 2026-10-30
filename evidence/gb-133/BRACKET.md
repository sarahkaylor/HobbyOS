# GB-133 — first bracket: 17 `pte 1→0` transitions, and **zero** region-removal markers covering the page

Head `215b79b`, this VM, intel. Instrument: `[GB133-R]` at every resume (both call sites) printing
the copy destination page's leaf, plus `[GB133] op=` markers on `vm_region_remove_locked` when the
removed range covers `0x17FFFFF000`. Patches: `20-bracket.patch`.

Wave: **killed at 02:5xZ, not completed** — `13025` resume probes, `Vector: 14 = 8`, no tier
summary. Everything below is from the probe stream, which is unaffected by where the wave ended.

## The measurement

```
probe lines: 13025
op markers:  0
1->0 flips:  17
```

Example flip (line 6032):

```
[GB133-R] sch pid=28 pg=0x17FFFFF000 pte=1
[GB133-R] sch pid=29 pg=0x17FFFFF000 pte=0
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000002
```

**The page becomes absent with no `vm_region_remove_locked` call covering it.** So the disappearance
does not go through the region list at all — which rules out the most obvious writer (munmap /
stack-region teardown) for this wave, by absence of evidence in the instrumented path rather than by
argument.

## The two surviving explanations, and the one line that separates them

1. **The address space under the pid changed** — exec/fork installs a new AS at the same VA, so
   `pte=0` is a *different table*, not a cleared one. The page was never "re-created" because the AS
   is not the same object. This fits the data: no region op fires, and the flips sit next to
   `Loading program for scheduler (v2 AS)` / `Process N: … exited` lines.
2. **A direct unmapping path** that does not call `vm_region_remove_locked` — `vm_arch_unmap` from
   another caller, or a teardown of the AS that clears pages without touching the region list.

The discriminator is one field: print `as->root_phys` (or CR3) beside `pte`. Same root across a
1→0 flip ⇒ an unmap happened inside one address space (explanation 2). Different root ⇒ the AS was
replaced and there is nothing to explain (explanation 1, and GB-122's "unmap" framing collapses).

That is a two-line change to the probe I already have, and it is the next run.

## Why this is worth reporting before the discriminator

- It **narrows** the writer search from "any kernel code" to "either the AS lifecycle or a direct
  unmap path", and it does so with a number (0 markers over 13 025 resumes).
- It also explains why GB-122's commit "sticks" and yet the page keeps coming back absent: if the
  AS is replaced between resumes, committing into the old AS is correct and useless — which is
  exactly the "mask, not a fix" behaviour observed.

## Limits

- Wave killed; `Vector: 14 = 8` is a partial-wave count, not a verdict, and no `halted` is claimed.
- `vm_region_remove_locked` is **one** path. `vm_as_teardown`, `vm_arch_unmap` callers and
  `process_exec`'s AS swap are **not** instrumented yet; the 0-marker result says something about
  the region list, not about the whole kernel.
- The flip detector keys on `(pid, page)`; a pid reused after exit would look like a flip. The root
  field in the next probe also fixes that ambiguity.

**No RED/GREEN** — `debug` card.