# GB-122 / GB-133 — the writer is found: **exec never prefaults the stack top**. The one-line fix I tried then broke the wave.

Head `215b79b`, this VM, intel. Three measurements and one code fact, in the order they arrived.

## 1. The loader is not racing (32 markers, 0 races)

Instrumented `program_loader.c` around its 64 KiB stack `vm_touch` (`[GB133-L] pid=… phase=pre|post`)
and crossed it with the resume probe:

```
loader markers 32
absent resumes INSIDE a pre..post window (race): 0
absent resumes with no open window:              7
```

So the spawn path's prefault is **not** raced: every absent resume happens outside a loader window.
The race hypothesis from GB-133's close is **falsified**.

## 2. The exec path has no prefault at all — code fact

`process_exec_current()` (`program_loader.c:1001-1290`) contains **zero `vm_touch` calls**, while the
spawn path prefaults `USER_MAIN_STK_TOP_V2 - USER_MAIN_STK_GUARD` for 64 KiB at line 846. Exec sets
`grp->context[33] = USER_MAIN_STK_TOP_V2 - 8` and returns — the stack top page in the **new** AS has
no leaf, and the resume copy is the first thing that writes it.

That is consistent with everything measured before it: 8/8 `pte 1→0` flips were **root changes**
(exec installing a new AS), `cr3 == root` in every sample, and the 7 absent resumes sit outside
every loader window.

## 3. The obvious fix, and the wave that rejected it

Added the same prefault to exec (`vm_touch(grp, TOP - GUARD, GUARD, 1)` right after
`arch_set_user_sp`):

```
T3_intel: 9326 console lines, 3 FAIL lines, 8 programs exited, halted=1
T3_intel: FAIL — 5 FATAL kernel-mode fault line(s)
Vector: 14 = 2
```

Against 86-88 programs exited in every recent wave. **8 programs is a broken wave, not a fixed one.**
So the fix as written regresses and is **not offered**; the next attempt has to explain why an
exec-time 64 KiB `vm_touch` starves the wave (cost per exec, a lock interaction, or `grp->as` not yet
being the table that will run) before it is retried.

## What this changes on the cards

- **GB-122** is no longer "someone re-clears the page" and no longer "a race with the loader". It is:
  **exec builds a stack with an uncommitted top page, and the resume copy writes it from CPL0.**
  The fix belongs in the exec path, and the first attempt at it failed for a reason nobody has named
  yet — that is the card's next question, not a five-wave campaign.
- The resume-side commit (3 complete waves at `Vector: 14 = 0/1/0`) remains a **mask**: it repairs
  the symptom at the first resume of each new AS and costs one page per AS.

## Limits

- Wave 81 was the only fix-build wave: 8 programs, 5 banners, `Vector: 14 = 2`. One wave, and it is
  a *negative* — I am not going to re-run it to look for a kinder number.
- Waves 71 (loader-marker build) and 81 both ran **after** a stale QEMU killed a first attempt
  ("REFUSING TO START … disk.img is in use"); the killed attempt produced no data and is not quoted.
- `root=` recycling can in principle mimic a same-root flip; 0 same-root flips were observed.

**No RED/GREEN** — `debug` work.