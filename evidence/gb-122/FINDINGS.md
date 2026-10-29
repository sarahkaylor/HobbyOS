# GB-122 — DoD clause matrix: **RED** (the repair is partial; clause 1 and clause 3 fail)

Not a gate verdict — this is a repair card whose DoD defines RED itself ("RED if any item is unmeasured").
Head: `98fc494` (`origin/main`, 2026-10-29) + the patch in `evidence/gb-122/30-fix-final.patch`. Host farm-3,
intel 8 CPUs, QEMU TCG. Nothing here was measured against CI.

## What I changed

Both resume sites in `src/kernel/process.c` (`schedule()`'s resume boundary and the idle-core
`start_scheduler()` site) — after `spinlock_release(&proc_lock)`, before `enter_user_space`:

```c
#if defined(__x86_64__)
      int gb122_user_resume = ((local_tf.cs & 3u) == 3u);
#else
      int gb122_user_resume = ((local_tf.spsr & 0xFULL) == 0ULL);   /* EL0t */
#endif
      if (gb122_user_resume && proc_table[next].as)
        (void)vm_touch(&proc_table[next], target_sp - 1024, 1024, 1);
      enter_user_space(&local_tf, target_sp);
```

Plus a probe on the existing `[RESUME-DIAG]` line: `pte=`/`w=` from `vm_arch_walk` on the destination page.

## Clause matrix

| # | Clause | Result |
|---|---|---|
| 1 | copy no longer takes the #PF; `pte=0` before → `pte=1 w=1` after on the same probe | **RED** — the class is *reduced*, not removed: 2 of 3 intact fatals before were in the carded window, and 1 of 6 post-fix runs still took one (`cr2=0x17FFFFFAC0`). I never captured `pte=0` on the probe (all 20 sampled resumes had `pte=1 w=1`) |
| 2 | lock-order risk settled before shipping (a) | **PASS** — the call is *not* under `proc_lock`; both sites release it first, and `vm_touch` takes only `vm_lock` per page with no yield/sleep (`frame_alloc_zeroed` + `vm_arch_map`, `vm.c:660-720`) |
| 3 | five consecutive intel T3 runs, `halted=1` and **0** `FATAL` each | **RED** — 1 of 6 runs halted; fatals in 3 of 6 |
| 4 | the GB-121 recogniser sees it (a clean wave cannot be clean-by-blindness) | **PASS** — the tier counted 13 fatals in the before-run and 14 in the worst post-fix run, so the counter is live |
| 5 | `make host_tests` green; `trap.o`/`vm.o` both arches; arm stated | **PASS** — `Checks run: 482 / Checks failed: 0 / TEST EXIT: 0`; `obj/intel/trap.o` 221112, `obj/intel/vm.o` 97288, `obj/arm/trap.o` 154400, `obj/arm/vm.o` 104456 (the guard is ported to arm via `SPSR` EL0t rather than left x64-only) |
| 6 | (c) is a complement, not a substitute | n/a — no (c) shipped |

## The numbers

Before (head `98fc494` + probe, no repair) — `02-t3-before.log`:

```
T3_intel: 26399 console lines, 81 FAIL lines, 21 user memory fault records across 19 processes, 84 programs exited, halted=0
T3_intel: FAIL — 13 FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, not because the wave completed
```
Three intact fatal lines in that serial, two squarely in the carded window:
```
vector=15676 rip=0x0000000000000097 cr2=0x00000017FFFFF2F8
vector=0     rip=0x0000000000000000 cr2=0x00000017FFFFF388
```

After (five consecutive runs at the fix head, `evidence/gb-122/runs.txt`):

| run | fatals | halted | programs exited | note |
|---|---|---|---|---|
| 1 | 3 | 0 | 86 | |
| 2 | 14 | 1 | 87 | halted **because the kernel crashed** |
| 3 | 0 | 0 | 85 | no `System halt` line |
| 4 | 0 | 0 | 24 | wave cut short |
| 5 | 0 | 0 | 82 | no `System halt` line |
| post-guard confirmation | 5 | 0 | 86 | after the arm-portable guard, same x64 path |

Run 2 still carries the carded window once — `rip=0x0000000000FFE6EA cr2=0x00000017FFFFFAC0` — plus a
family I did not touch (`vector=13 rip=0x0000000000000093` at `cr2=0x1000002270`, `vector=9 rip=0x1E`).

## Why it is not the card's fix

1. The carded #PF is **still reachable** with the guard in place (run 2), so whatever produces that
   `target_sp` is not covered by a 1 KiB window below `target_sp` — the window is the wrong shape for at
   least one resume shape (the fault address can sit *above* the copied window, or the resume uses a
   different stack).
2. The dominant remaining fatal family is **not** the frame copy at all: it is torn frames with garbage
   vectors (`13`, `9`) and RIPs (`0x93`, `0x1E`) whose cr2 lands in the *user image* range
   (`0x1000002xxx`). That is GB-101 clause 1's other half, and it is untouched by this patch.
3. `halted=0` in 5 of 6 runs means clause 3 cannot be measured as written on this head even before the
   fix — the wave does not halt on `98fc494` for reasons independent of the resume copy (`02-t3-before`
   shows `halted=0` with 84 programs exited).

So the honest state of the repair: a real but **partial** mitigation (the loader-window class is reduced,
never eliminated), the lock question answered with evidence, and the card's own DoD still RED.

## What I recommend (for the next owner, not done here)

- Instrument the *failing* resume instead of the first five: capture `target_sp`, `context[33]`, `gs:[24]`
  and the leaf PTE at the moment of the fault, so the window shape is derived from the failing case rather
  than assumed.
- Treat the residual family as its own card: `vector=13 rip=0x93` with `cr2` in the user image range is a
  different torn-frame source and will keep clause 1 red no matter how the resume copy is fixed.
- If the guard is kept as defence in depth, it should be `vm_kwrite`-based (self-validating) rather than a
  bare `vm_touch`, so the materialise-then-copy window cannot be reopened by a peer.

## Reproduce

```
cd .worktrees/gb122                 # 98fc494 + 30-fix-final.patch
make disk.img ARCH=intel -j32 && T3_MONITOR=0 T3_TIMEOUT=300 W0_GATE=0 bash tools/t3_tier.sh intel
python3 evidence/gb-122/tally.py    # per-run fatals / carded-window hits / halted
bash evidence/gb-122/five_runs.sh   # the five consecutive runs
make host_tests && make disk.img ARCH=arm -j32
```