# GB-122 — site enumeration + population sample (step 1-2 of the new bar)

Head `215b79b`, this VM, intel. Instrument: `81-sampler.patch` (diagnostic only, reverted;
`main` untouched). Wave: `80-sampler-*`, **completed** — `25750 console lines, 133 FAIL,
17 kernel-mode FATAL, 88 programs exited, halted=1`.

## Step 1 — every site that builds a resume frame onto a stack

`enter_user_space` has **two branches** and is reached from **two call sites**:

| site | branch | destination |
|---|---|---|
| `schedule()` resume boundary (process.c ~1841) | kernel (CS ring ≠ 3) | `gs:[24] - 320` |
| `schedule()` resume boundary | user (CS ring = 3) | `target_sp - 320` |
| `start_scheduler()` resume boundary (process.c ~3601) | kernel | `gs:[24] - 320` |
| `start_scheduler()` resume boundary | user | `target_sp - 320` |

The sampler walks the **process's own AS** for both candidate destinations at **both** call sites
and prints only the absent ones, tagged `site=sch|boot br=k|u`.

## Step 2 — absent-destination frequency per site

```
absent samples: 144
  site=sch   140        site=boot    4
  br=k        74        br=u        70
```

Four lines from the boot site, quoted:

```
[GB122-S] site=boot br=k pid=1 parent=6 sp=0x17FFFFF4C8 gsp=0x17FFFFF4C8 page=0x17FFFFF000
[GB122-S] site=boot br=u pid=1 parent=6 sp=0x17FFFFF4C8 gsp=0x17FFFFF4C8 page=0x17FFFFF000
```

**The answer to "is every such site covered": no.** Absent destinations occur at **both** resume
sites, so any fix applied only at `schedule()` (which is where my route A patch lived) leaves the
`start_scheduler()` site uncovered. That is exactly the reviewer's point, and it is now measured
rather than suspected.

## What this sample does **not** establish, stated before anyone quotes the numbers

- **Branch attribution is not yet meaningful.** The sampler prints both candidate destinations
  for every resume, but only one of them is the branch actually taken. `br=k` absent entries on a
  *user* resume are `gs:[24]` pointing somewhere irrelevant, not a copy destination. The fix is
  two lines — pass `local_tf.cs` and gate on the branch — and it is the next thing I will run.
- Consequently **144 is an upper bound on absent copy destinations, not a count of them.** The
  wave's own `Vector: 14` count is **17**, and every one of those is a copy that actually fired.
- `pid=1 parent=6` at the boot site is the same `sp=0x17FFFFF4C8` as the carded address, which is
  worth a look on its own: a *parented* pid 1 is unusual and may mean the boot site resumes a
  forked child before the first user process runs.

## Against the fix bar

| # | clause | status |
|---|---|---|
| 1 | enumerate every resume-frame site; every one covered | **enumeration done (4 combinations), coverage disproved** — two sites, both hit |
| 2 | re-run sampling per site; `start_scheduler()` gets the same treatment or say why | sampling per site done; treatment **not** applied |
| 3 | five halting waves, `halted=1`, `Vector: 14 = 0`, commit count quoted | **not run** — this wave is the sampler build (17 `Vector: 14`, 88 programs, halted=1), within this host's baseline range of 3-25 |

## Next, in order

1. Gate the sampler on `local_tf.cs` (2 lines) → true per-branch counts.
2. Apply the commit-at-both-sites version of route A (route A previously covered `schedule()`
   only) and re-measure: `Vector: 14` at **any** site, plus commits per wave quoted uncapped.
3. Then the five halting waves.

**No RED/GREEN** — `debug` card. Two of the three clauses in the new bar need the CS gate first,
and I would rather run that than report a number I know is inflated.