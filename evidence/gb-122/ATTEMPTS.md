# GB-122 attempt log — two fix routes measured, neither reaches the DoD bar

Head `215b79b`, this VM, intel. Both patches are in `evidence/gb-122/` (`52`'s source state is
`53-fork-prefault.patch`; the commit-on-resume build was reverted, its numbers are below).
**Both waves were cut short** (killed rather than waited out) — so these are partial-wave
measurements, stated as such, not wave verdicts.

## Route A — commit the destination page on resume (the card's *preferred* route)

Two sites in `process.c`, after the address space is installed and `proc_lock` released:
walk **the process's own AS** for `(target_sp-320) & ~0xFFF`; if absent and the resume lands in
`[USER_MAIN_STK_LIMIT_V2, USER_MAIN_STK_TOP_V2]`, `vm_touch(p, page, 0x1000, 1)` and emit one
`[GB122-COMMIT] pid=… parent=… sp=… page=…`.

Measured in a partial wave (`61-fix-wave.log`):

```
[GB122-COMMIT] pid=20 parent=7 sp=0x17FFFFF4C8 page=0x17FFFFF000
[GB122-COMMIT] pid=22 parent=9 sp=0x17FFFFF4C8 page=0x17FFFFF000
[GB122-COMMIT] pid=22 parent=9 sp=0x17FFFFF0F8 page=0x17FFFFE000
...
157 commits in the partial wave        Vector: 14 = 0
```

**The copy fault class disappeared in the wave that ran — and the commit fired 157 times**, not
the ~1 the card's clause 3 assumed. That is the honest headline: the fix works on the fault and
*fails* the "no behaviour change on the 374/375 normal resumes" clause as written.

But the clause's premise is now in doubt and the board should see why: the user stack is a
demand-zero reserve, so a resume onto a page the process has not touched yet is **normal**, not
an anomaly — committing it is what the hardware would have done had the process touched it
itself. "Fires on normal resumes" therefore does not distinguish a good fix from a bad one here;
the distinguishing measurement would be *faults removed* versus *pages committed*, and 157
pages for 10-ish faults is a real cost worth stating rather than hiding.

## Route B — prefault at fork (the card's *alternate*)

After `vm_as_clone_into`, `vm_touch(child, USER_MAIN_STK_TOP_V2 - USER_MAIN_STK_GUARD,
USER_MAIN_STK_GUARD, 1)` — parity with the loader's own prefault, on the reasoning that
`vm_as_clone_into` copies holes as holes.

Measured in a partial wave (`71-forkfix-wave.log`):

```
fork: stack prefault failed = 0
Vector: 14 = 10      RDI ∈ {0x17FFFFFCB8 ×3, 0x17FFFFF388, 0x17FFFFF2F8, 0x17FFFFF000}
```

**It does not remove the fault.** Every residual `dest` is inside the 64 KiB the prefault covers,
so the faults are not simply "a forked child below its parent's window" — the class is wider than
the one my 375-sample named, or the sampler (which instrumented **`schedule()`'s resume site
only**) does not see the `start_scheduler()` site at all. That is the next measurement, and it is
cheap: instrument both sites and print the sp at the fault.

## Where this leaves the card, plainly

| clause | status |
|---|---|
| 1. commit on absent, log once, stack-window only, refusal path untouched | **built** (two variants), not accepted: A fires 157×, B does not fix |
| 2. wave fatal count must not rise | **not assessable** — both waves partial; A showed 0 `Vector: 14`, B showed 10 |
| 3. no behaviour change on normal resumes | **A fails** (157 commits); B has no resume-side change but leaves the fault |
| 4. both arches build, host_tests green with count | **builds: intel exit 0, arm exit 0** (`50`/`51`). `host_tests` **not run** — I will not quote a gate I did not execute |
| 5. a test that fails before the fix | **not built.** The fork-depth case is not yet deterministic enough here (see below) |

## Why I am stopping rather than running five waves

The card's own rule: a wave cannot pass a bar whose cause is not fully named. Route B's residual
faults prove the cause is not fully named — the sampler looked at one of two resume sites. Five
waves now would measure a fix whose target population I cannot yet count.

## Disclosures

- I wrote GB-125's guard and my GB-122 route-(2) refusal; both are precedents this card has to
  beat. I reverted both from the tree; `main` is untouched and only evidence is committed.
- Waves 61 and 71 were **killed**, not completed: no `halted=1` and no five-run count is claimed
  from them. Their `Vector: 14` counts are quoted because the count is what the route changed.
- `host_tests` was not run for this attempt; the earlier tail-quoted 1096 PASS belongs to the
  GB-132 build, not to this one.

**Recommendation:** take route A as the direction (it removed the fault in the wave that ran),
re-frame clause 3 as "pages committed vs faults removed" instead of a rarity test, and run the
two-site sampler before any five-wave campaign.