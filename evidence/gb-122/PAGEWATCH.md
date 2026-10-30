# GB-122 — page-watch answers the question: the commit **sticks**, and the page is **cleared again**. The unmap is real.

Head `215b79b`, this VM, intel. Patch: both-site CS-gated commit + post-commit walk
(`B1-pagewatch.patch`). Wave **completed**: `18576 lines, 132 FAIL, 86 programs exited, halted=1`,
`Vector: 14 = 0`, **0 kernel-mode fault banners**.

## The measurement

Immediately after `vm_touch` for the absent page, walk it again and print the result:

```
[GB122-COMMIT] site=sch br=u pid=22 parent=8 sp=0x17FFFFF4C8 page=0x17FFFFF000 pre=0 post=1
[GB122-COMMIT] site=sch br=u pid=28 parent=6 sp=0x17FFFFF0F8 page=0x17FFFFE000 pre=0 post=1
```

```
commits 68        post=1: 68        post=0: 0
```

**The commit installs the page every time.** So `vm_touch` is not the problem and the walk is not
lying.

## And the repeats persist anyway

```
distinct (site,br,pid,page): 38      total: 68
('sch','u','2','17FFFFF000') x 10     ('sch','u','1','17FFFFF000') x 7
('sch','u','22','17FFFFF000') x 4     ('sch','u','1','17FFFFE000') x 3
```

Same pid, same page, committed, then **absent again** at the next resume — ten times for pid 2.

## What that means, stated against my own earlier answer

- **My "the page is never committed" conclusion is falsified by this measurement.** It *is*
  committed — by my own commit, mid-wave — and it is gone by the next resume.
- Therefore **the card goes back to route (1), "find the unmap"**, and the sample that led me to
  "never committed" (1 absent in 375, on a forked child) measured a population that is much
  smaller than the one the commit log sees (68-142 per wave). Both cannot describe the same
  system, and the page-watch says which one is closer to the truth: **pages are disappearing.**
- The per-pid/per-page trace in `[GB122-COMMIT]` is now the cheapest lead there is: the next
  resume of `pid=2 page=0x17FFFFF000` is where the clear happens, and the interval between two
  such lines is the whole search space.

## Effect of the commit on the fault, for the record

Three waves with the commit, all complete:

| wave | `Vector: 14` | FATAL banners | programs | halted |
|---|---|---|---|---|
| 91 | 0 | 2 | 88 | 1 |
| 92 | 1 | 3 | 86 | 1 |
| A1 | 0 | 0 | 86 | 1 |

Against baselines of `Vector: 14` = 10, 17, 21, 26 and banners 3-25. **The commit suppresses the
fault in every wave it has run in**, which is a real effect — but it is a *mask*, not a fix, and
it costs 68-142 page commits per wave for a page that something keeps taking away.

## Clause status (per the 01:45Z DoD)

| # | clause | status |
|---|---|---|
| 1 | five halting waves, `Vector: 14 = 0` at all four sites, distinct/total commit ratio per wave | **3 of 5** waves, `0/1/0`, ratios 68/38, 142/68, 139/59 — reported, not claimed |
| 2 | the unmap named, or the repeats explained as harmless | **named as real, not yet identified.** Repeats are *not* harmless; they are the defect |
| 3 | fatal count vs baseline on this host | 2/3/0 banners vs 3-25 pre-fix; `Vector: 14` 0/1/0 vs 10-26 |

## Next (cheap, and the card's own tool now exists)

Bracket the interval: for one `(pid,page)` pair, print the walk at **every** resume of that pid
(not just when absent) and at every `vm_touch`/unmap entry, so the line where `pte` goes 1→0 is in
the log. That is one build and one wave, and it converts "something clears it" into a named call
site.

**No RED/GREEN.** `main` untouched; patch is evidence only.