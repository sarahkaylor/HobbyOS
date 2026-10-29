**GB-122 — measured, and RED against its own DoD. The `#PF` is gone in one wave; the wave still does not halt, and method (a) is now proven unsafe rather than merely suspected.**

Three intel waves at `98fc494` + my patch. Nothing here is inferred: each claim is a wave summary or a counted line.

| wave | what it tests | result |
|---|---|---|
| w1 | `vm_touch` at resume, no user-window guard | **boot died**: `[RESUME-NOMAP] refusing to resume pid=1 target_sp=0x23FFF000` — the initial entry resumes on a low identity-mapped `target_sp`, not a user VA |
| w2 | `vm_touch` at resume, guarded to the user window | **hang**: 7003 lines, `halted=0`, `FAIL — 5 FATAL kernel-mode fault line(s)`, monitor had to sample the CPUs |
| w3 | page committed at AS build (loader) + read-only refusal at resume | **0 FATAL**, 83 programs exited, 17294 lines, `halted=0`, **6 `[RESUME-NOMAP]`** |

## Clause 2 is settled, with evidence — take (b), not (a)

DoD clause 2 said: settle the lock-order risk before shipping (a). **w2 settles it by hanging.** `vm_touch` takes `vm_lock` and allocates a frame; called from `schedule()` under `proc_lock` inside the scheduler's critical path, that closed a cycle and the guest stalled with no halt. The documented order (`proc_lock -> vm_lock -> frame_lock`, `vm.c:7`) is not sufficient here, because the *allocation* under that order is what another CPU is waiting behind. So (a) is out on measurement, not on taste.

## What (b') does, and what it does not

`program_loader.c` now commits **one page** — `USER_MAIN_STK_TOP_V2 - FRAME_SIZE`, the page the resume frame lands in — right after the stack region is inserted, before the process can be scheduled. The resume site keeps only a **read-only** walk (`vm_arch_walk`), and on an absent leaf prints `[RESUME-NOMAP] pid=… target_sp=…` and refuses to iretq into it (DoD clause 6's complement).

Effect, w2 → w3:

```
w2: FAIL — 5 FATAL kernel-mode fault line(s) … halted=0
w3: 0 FATAL kernel-mode fault line(s) … 83 programs exited … halted=0
```

and the healthy probe now walks clean, which is clause 1's requested quote:

```
[RESUME-DIAG] pid=2 region=yes prot=3 dest=0x00000017FFFFFEB8 covers=1 cr3=0x20C04000 as=0x20C04000 match=1 pte=1 w=1
```

**What it does not do:** six processes still arrive at resume with an absent leaf and get refused:

```
[RESUME-NOMAP] refusing to resume pid=23 target_sp=0x00000017FFFFF4C8
[RESUME-NOMAP] refusing to resume pid=29 target_sp=0x00000017FFFFF438
[RESUME-NOMAP] refusing to resume pid=41 target_sp=0x00000017FFFFFDF8
```

Those `target_sp-320` values are `0x17FFFFF388` / `0x17FFFFF2F8` — **the same addresses as the original 25 anomalies**. So the loader commit fixes the *first-touch* class, and a **second** class remains: the page is absent again later, i.e. something unmaps or tears down the top stack page while the region stays registered. That is the region/PTE divergence re-appearing after the fact, and it needs `[RESUME-NOMAP]` to be extended with the *reason* (was it ever present? who unmapped it?) before anyone guesses at a fix.

## Why the card is RED, not GREEN

DoD clause 3 asks for **five** consecutive intel waves with `halted=1` and 0 fatals. I have **zero** waves with `halted=1` — w3 has 0 fatals but `FAIL — the console has no 'System halt' line (idle/hung wave, or the 2700s deadline hit; a hang is a defect, not a flake)`. Killing six processes at resume is very likely *why* the wave idles: the tests that owned those processes never finish. A wave that stops crashing by refusing to run is not a wave that passes.

Clause 5 (`make host_tests`, both arches) is **not run** either — I am not going to quote a gate I did not execute.

## What I would do with this card next

1. Keep (b') — it removes the fatal class at first touch and costs one page per process.
2. Instrument `[RESUME-NOMAP]` with "ever present?" + the unmap path that clears it; that second class is now the whole defect.
3. Only when the refusals reach zero, run the five waves; `halted=1` is the acceptance signal, not the absence of the word "fatal".

Evidence: `evidence/gb-122/t3_w1.tier.txt`, `t3_w2.log(+gz)` / `.tier.txt`, `t3_w3.log(+gz)` / `.tier.txt`, `fix.patch` (195 lines), `nomap_w3.txt`, `diag_w3.txt`, `tier_summary.txt`. Branch `probe/gb101-clause1`. Local runs only; no CI used or cited.

— `wgfx-verify`, DEEP verifier, 2026-10-29T03:05Z