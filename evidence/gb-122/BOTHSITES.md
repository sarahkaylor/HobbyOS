# GB-122 — both-site, CS-gated commit: two complete waves, and a repeat-commit signature that blocks attribution

Head `215b79b` + patch `93-both-site-commit.patch`, this VM, intel. Both waves **completed**
(`halted=1`), unlike the earlier partials.

## What was built

`gb122_commit_resume(site, p, target_sp, cs)` called at **both** resume sites
(`schedule()` → `site=sch`, `start_scheduler()` → `site=boot`), gated on `local_tf.cs` so only the
branch that will actually be taken is examined (kernel ⇒ `gs:[24]-320`, user ⇒ `target_sp-320`),
scoped to `[USER_MAIN_STK_LIMIT_V2, USER_MAIN_STK_TOP_V2)`, committing only when the process's own
AS reports the page absent, and printing one `[GB122-COMMIT]` per occurrence.

## The waves

| wave | console | FAIL | FATAL banners | programs | halted | `Vector: 14` | commits |
|---|---|---|---|---|---|---|---|
| 91 | 35 966 | 372 | 2 | 88 | 1 | **0** | 142 |
| 92 | 33 101 | 186 | 3 | 86 | 1 | **1** | 139 |

Against this host's unmodified baselines (`Vector: 14` = 10, 17, 21, 26 in four separate waves),
that is the first time the copy fault has been near zero with full program completion.

## Why I am not claiming the fix

```
total commits: 142   distinct (site,br,pid,page): 68
('sch','u','1','17FFFFF000') x 13      ('sch','u','2','17FFFFF000') x 9
run 92: ('sch','u','1','17FFFFF000') x 21
```

**The same page is committed again and again for the same pid** — up to 21 times in one wave. A
commit that stuck would fire once per (pid,page) forever. So one of these is true, and I do not
know which:

1. **`vm_arch_walk` is reporting absent for pages that are present** — false negative. Then the
   commits are no-ops on already-mapped pages, and `Vector: 14 = 0/1` is wave variance rather than
   the fix's doing. (This would also contradict the earlier 375-sample, where 374 of 375 were
   present — the two samples disagree, and that disagreement is itself unexplained.)
2. **The page really is absent each time** — i.e. something *re-clears* it between resumes. Then
   the "never committed" answer from my earlier addendum is wrong, the page is being unmapped, and
   the commit is only papering over it.

All commits are branch `u` (142/142), so the CS gate itself is working — that part is confirmed.

## The one measurement that decides it (cheap, next)

Print the walk result **immediately after** `vm_touch`, for the same page:

```
[GB122-COMMIT] ... pre=0 post=1   -> commit sticks; repeats mean something clears it
[GB122-COMMIT] ... pre=0 post=0   -> vm_touch is not installing what the walk reads
```

If `post=0`, hypothesis 1 is right and the fault count result above must be withdrawn. If `post=1`
and the page is `pre=0` again at the next resume, hypothesis 2 is right and the card returns to
"find the unmap" with the whole population in hand.

## Clause status

| # | clause | status |
|---|---|---|
| 1 | commit on absent at every site, log once, window only, refusal untouched | **built at both sites, CS-gated**; "log once" **fails** (13-21× same page) |
| 2 | fatal count must not rise | 2 and 3 banners vs baseline 3-25; `Vector: 14` 0 and 1 vs baseline 10-26 |
| 3 | no behaviour change on normal resumes | commits fire ~140/wave — far more than the ~17 faults they replace |
| 4 | both arches build; host_tests with count | intel build exit 0 (`90`); **arm and host_tests not run for this patch** |
| 5 | five halting waves | **2 of 5**, both `halted=1`, but attribution blocked by the repeat signature |

## Disclosures

- Both waves are complete (unlike 61/71), so `halted=1` and the load counts are real.
- I am reporting a *favourable* fault count and then arguing against attributing it to my own
  patch. That is the only honest order: the repeat-commit log says the mechanism is not what the
  patch's comment claims.
- `main` untouched; the patch is evidence only.