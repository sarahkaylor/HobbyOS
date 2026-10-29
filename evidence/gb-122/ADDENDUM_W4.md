**GB-122 addendum, wave 4 — scoping the walk was correct and the vec=14 class is gone; what is left is GB-123's crash, and the wave still does not halt. RED stands.**

One change since my 03:05Z verdict: the resume-site walk is now scoped to resumes landing in the **stack top page** (`target_sp - 320 >= USER_MAIN_STK_TOP_V2 - FRAME_SIZE`). It was walking on *every* user resume, which is the pricing regression I suspected — the per-resume page-table walk is what stretched w3 to the 2700s deadline.

Wave 4 (same tree, scoped walk):

```
T3_intel: 15079 console lines, 30 FAIL lines, 6 user memory fault records across 5 processes, 85 programs exited, halted=0
T3_intel: FAIL — the console has no 'System halt' line (idle/hung wave, or the 2700s deadline hit; a hang is a defect, not a flake)
T3_intel: FAIL — 2 FATAL kernel-mode fault line(s)
```

Four waves, one table:

| wave | change | console | FAIL lines | programs exited | fatals | halted |
|---|---|---|---|---|---|---|
| w2 | `vm_touch` in `schedule()` | 7003 | 52 | 79 | 5 | 0 |
| w3 | commit at AS build, walk on every resume | 17294 | 85 | 83 | **0** | 0 |
| w4 | commit at AS build, walk scoped to top page | 15079 | **30** | **85** | 2 | 0 |

**The vec=14 write fault is gone in both w3 and w4.** The two fatals in w4 are a *different* signature:

```
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x0000000000000007 cr2=0x000000100000ECD0 pid=50 err=0
```

`vector=6` (invalid opcode) with `rip=0x7` — a jump through a null-ish pointer, not a write to an absent page. That is the **late tear** GB-123 (#269) describes, and it is not this card's defect.

**Refusals:** 5 in w4 (down from 6), all `target_sp=0x17FFFFF4C8`, and the log around them carries `[KERNEL] process_create_v2: no address space!` **184 times** in w3. So the residual class is processes whose AS was never built — refusing to resume them is correct behaviour, and it is a separate defect from this card's (the page being absent while the AS exists).

**Why this is still RED against the DoD:** clause 3 wants five consecutive intel waves with `halted=1` and 0 fatals. Four waves, zero halts, and the remaining fatal belongs to another card. A wave that fails to halt for reasons outside this card still cannot be called clean, and I am not going to count w3's `halted=0` as "clean because no vec=14".

What would make this card gradable: GB-123's fix landing, then five waves on a head that carries both. Until then the honest state is "the repair works on its own defect; the tier cannot show it because a second defect halts the guest".

Evidence: `t3_w4.log` (+`.gz`), `t3_w4.tier.txt`, `fix2.patch`, `fatal_w4.txt`, `tier_summary.txt` (all four waves). Branch `probe/gb101-clause1`.

— `wgfx-verify`, DEEP verifier, 2026-10-29T04:25Z