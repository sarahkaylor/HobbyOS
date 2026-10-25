**GB-097 — the DoD's precondition run exists in the archive, and it is shredded too.**

The DoD wants zero byte-interleaved lines *in a run that fires `[FREERACE]`/`[STALLBREAK]`*. My
current-tree run fires neither, so I looked for one that does — the GB-057 probe serials from worktree
`hermes-ccb03222` (older sha, named so nobody mistakes it for today's tree):

| serial | `[STALLBREAK]`/`[FREERACE]` | `console_shred.py` |
|---|---|---|
| `t3intel_probe24_smptest_frame_zeroed.log` | 6 | **3** |
| `t3intel_probe26_trapreturn_not_scheduler.log` | 5 | **8** |
| `t3intel_probe23_spawntime_wild_noas_silent.log` | 4 | **49** |

So the precondition leg is satisfiable — and the answer there is 3, 8 and 49, not zero. A specimen from
the last one, where the shred lands *inside* a fault report in a STALLBREAK run:

```
  entry=0x0000001000000000 sp=[KE0RNEL] Fx00ATAL: Exce0ption in000 Ke18000000rnel Mo00d resie!dent=1  tVecto
```

(`[KERNEL] FATAL: Exception in … Kernel Mode! Vecto…` interleaved with a loader line's fields.)

### Where the ticket stands

- **Instrument:** delivered, self-tested, exits 1 on any hit — `evidence/gb-097/console_shred.py`.
- **Precondition run:** exists (3 archived serials above), all non-zero.
- **Current tree (`9f89a23`):** 113 in one intel wave that fires neither marker.
- **Fix:** not landed — still the design choice (console ownership vs. build-time ban on raw-sink calls
  from returning functions). Nothing I can call GREEN: four independent runs, four non-zero counts.

Status **In Progress**. The one thing I cannot do from here is decide the fix's shape; that is the
Coordinator's call, and it is the only thing between this card and a gradeable GREEN.
