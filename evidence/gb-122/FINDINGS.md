# GB-122 — route (2) was built and falsified twice by the wave; the blocker is named

Claimant: `wgames-verify/gate` (DEEP verifier), 2026-10-29 22:47Z → 23:2xZ.
Head `215b79b`, this VM (the same host as the baseline comparison), intel, local runs only.
Patch: `10-refusal-patch.patch` (187 lines). Every build here is `make disk.img ARCH=intel`, exit 0.

## What I built

Route (2) of the card: refuse in the style of the user branch, **without** GB-125's shape.
A **register-only** inline page-table read on the normal path — no call, no stack frame, no
allocation, no lock — for the destination page of the 296-byte copy, followed on refusal by a
slow path that reports and takes the same landing pad the #PF path already uses
(`process_fault_exit(tf, 11)` → `process_exit` → `schedule`, which never returns). The point
of the register-only design is GB-132's measurement: GB-125's guard called a C function whose
256-byte `kline` frame landed in `[gs:[24]-320, gs:[24])`, the window the copy writes, and that
build produced family-B banners that do not occur without it.

## Three builds, three measurements — each one kills a version

| build | wave result | what it proves |
|---|---|---|
| **unscoped** (both branches, no window test) | 109 lines, **0 programs exited**, `halted=1` | refuses **pid=1** at `target_sp=0x23FFF000` — the initial entry resumes a kernel task on a low identity-mapped stack, exactly the w1 finding. The guest dies before the wave. |
| **scoped, kernel branch only** | 12 723 lines, 82 programs, **0 refusals** and **26 `Vector: 14` dumps** (`RIP 0x7003DA30`, `RDI 0x17FFFFF388`) | the measured `#PF` is **not** in the kernel branch. Guarding the kernel branch as the 22:39Z briefing directed changes nothing; the copy that faults is the **user** branch's (`rdi = r13-320`). |
| **scoped, both branches** | 3 waves: 737 / 737 / 735 lines, **68 refusals each**, 0 FAIL, `halted=1`, tier says **PASS** | **a fake pass**: 68 refusals is one per program. Nothing ran, so nothing failed. GB-122's own warning — "a wave that stops crashing by refusing to run is not a wave that passes" — caught my build, not a stranger's. |

## Why the both-branch build refuses everything — the actual blocker

Every refusal is at the same address, and it is the address GB-122 already measured as
**healthy**:

```
[RESUME-NOMAP] refusing to resume pid=2 target_sp=0x17FFFFFFF8 dest=0x17FFFFFEB8 reason=absent-or-ro at resume
```

GB-122 w3's own probe at that same dest, walking the **process address space**:

```
[RESUME-DIAG] pid=2 region=yes prot=3 dest=0x00000017FFFFFEB8 covers=1 cr3=0x20C04000 as=0x20C04000 match=1 pte=1 w=1
```

Mine walks **CR3**, and at that instant CR3 is not the target process's table — so a present
page reads as absent. That is the whole reason the check cannot be written this way at this call
site: `enter_user_space` receives `(tf, target_sp)` and **no address space**, and the CR3 in
force during the resume is not reliably the resumed process's.

**So route (2) is not "cheap enough to be safe" — it is currently unimplementable without
passing the target AS (or its root) into `enter_user_space`.** That is a signature/ABI change at
two call sites in `process.c`, not a guard. I did not make it: it changes the shape the card
authorised, and the card says it would rather have the right question than a wrong fix.

## Clause-by-clause, against the 22:39Z re-scope

1. **Five waves, `halted=1`, 0 resume-page FATALs** — **not met.** My builds produce either
   death-at-boot, or no change (26 faults), or refusal-of-everything. None of the three is a
   wave that ran.
2. **Wave fatal count must not increase vs baseline on the same host** — **not assessable on
   the builds above**, because none of them ran a wave. The one build that ran (kernel-branch
   only) had 24 FATALs against baselines of 18/25/3/11/6 on this host: inside the observed range,
   but it also **changed nothing**, so it is not evidence of anything.
3. **Other families are notes** — they were counted, not absorbed: `Vector: 14` dumps were 26 in
   the kernel-branch-only wave and 0 in the both-branch waves (because nothing ran).

## What the next card should be — two candidates, both cheap

1. **Give `enter_user_space` the AS** (`enter_user_space(tf, target_sp, struct addr_space *as)`)
   and walk `as` in the guard, exactly as GB-122's own probe does. That is the missing
   parameter, and it is the reason three attempts at this guard have failed on this path.
2. **Or answer the unmap question directly** — the card's route (1): the page is present at AS
   build and absent by resume; name the path that clears it. The addresses are unchanged
   (`0x17FFFFF388`, `0x17FFFFF2B8/FCB8`).

## Disclosures

- I wrote GB-125's guard; GB-132 showed it manufactured the banners it was built to catch. That
  is why this patch is register-only, and why I quote GB-122's healthy probe against my own
  refusal line rather than arguing my walk is fine.
- Baseline figures on this host are from waves 21/23/11/12/13 recorded on GB-132
  (`ae6efc5`), not from farm-2's numbers.
- Wave 25 was killed with the batch (qemu + tier pids) rather than waited out; its partial log is
  kept but is not used for any count.

**No RED/GREEN** — `debug` card, and its DoD did not obtain a gated verdict. Recommendation:
do **not** merge `10-refusal-patch.patch`; take candidate 1 (pass the AS) as its own change, or
re-scope this card to the unmap tracing.