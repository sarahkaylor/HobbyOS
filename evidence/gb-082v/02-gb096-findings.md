# GB-096 — measured state at 9f89a23 (intel): the 0x7 record did not appear; a different unbreakable stall blocks the DoD

Worktree `hermes-9642df51`, branch `verify-gb082v` at **9f89a23**, intel T3 wave, swarm-farm-2.

## 1. No `lock=0x7` record in this run

```
$ grep -ac 'lock=0x0000000000000007' obj/t3_intel.log
0

$ grep -a "lock=0x" obj/t3_intel.log | sed 's/.*lock=\(0x[0-9a-f]*\).*/\1/' | sort | uniq -c
    110 0x0000000070200010
```

One lock value, 110 records, all the same address — and it is **not** the corruption the card
describes.

## 2. What is actually spinning: a valid spinlock_t, held with owner=0

```
[LOCKFOREVER] cpu=0 lock=0x0000000070200010 owner=0 owner_hb_age=4294967295 pc=0x70000dfd pc2=0x700046ce
[LOCKFOREVER] cpu=4 lock=0x0000000070200010 owner=0 owner_hb_age=4294967295 pc=0x70001432 pc2=0x70008772
[LOCKFOREVER] cpu=6 lock=0x0000000070200010 owner=0 owner_hb_age=4294967295 pc=0x70000dfd pc2=0x700096ad
```

Resolved against `hobbyos.elf`:

```
$ nm -n hobbyos.elf | awk 'strtonum("0x"$1) <= strtonum("0x70200010")' | tail -1
70200010 b fill_lock

$ addr2line -f -e hobbyos.elf 0x70000dfd 0x700046ce 0x70001432 0x70008772
blk_cache_read        src/kernel/blk_cache.c
find_entry_in_root    src/kernel/fat16.c:479
blk_cache_read        src/kernel/blk_cache.c
fat16_sync_entry      src/kernel/fat16.c:2030
```

So the pointer is `fill_lock` (blk_cache.c) — **valid**, not 0x7 — and the wave stalls on it with
`owner=0` and `owner_hb_age=-1`. The DoD's first clause ("the lock pointer at the spinning site is a
valid spinlock_t address") happens to hold here, but only because this is a *different* bug; the run
never reaches `halted=1`, so it cannot satisfy the clause as written.

## 3. Why nobody breaks it — this shape is unbreakable by construction

`spinlock_acquire` stamps the owner only *after* the lock is taken:

```
if (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) { … }   /* locked=1, owner still 0 */
lock->owner = get_cpuid() + 1;                                       /* owner stamped here */
```

and both escape hatches refuse an unowned lock:

```
int lock_steal_ok(uint32_t owner_slot, uint32_t cpu) {
  if (owner_slot == 0) return 0;          /* locks.c:50-51 */
  …
}
int lock_stallbreak_ok(uint32_t owner_slot, uint32_t cpu, uint64_t now_ms) {
  if (owner_slot == 0) return 0;          /* locks.c:78-79 */
  …
}
```

A CPU that dies (or is halted by a fault) between the test-and-set and the owner store leaves
`locked=1, owner=0` — and then **both** recoveries decline, forever. The 110 records are that state:
age -1 means "owner never seen alive", which the code itself calls out as the case where
`[STALLBREAK]` will never fire.

I have not proven which of the two routes produced it — (a) a halt inside the two-instruction window,
or (b) something clearing `owner` later. Proving that needs one probe: a counter of acquires that
enter the window with `locked==1 && owner==0` observed by a spinner, or stamping the owner with a
compare-exchange at acquire time (which also removes the window entirely).

## DoD scorecard

| clause | result |
|---|---|
| the spinning site's pointer is a valid `spinlock_t`, 3 consecutive runs reaching `halted=1` | **NOT MET** — pointer is valid (`fill_lock`), but the wave stalls; 0 of 3 runs reach `halted=1` |
| the corrupting write is named | **NOT MET** — no 0x7 record to name in this run |
| a run with the fix shows no `lock=0x7` record | **vacuous** — no 0x7 record appeared without a fix either |

## Status

**In Progress, DoD unmet.** Two separable problems now:

1. **The 0x7 corruption did not reproduce** at 9f89a23 on this configuration — one run, so this is not
   proof it is gone (GB-069 taught that lesson); it is proof it is load-dependent.
2. **A permanent `fill_lock` stall with `owner=0`** blocks every `halted=1` run this card needs, and it
   is unbreakable by design: both recovery paths return false for `owner_slot == 0`. That is worth its
   own card even if 0x7 never returns — and its fix (stamp the owner with the acquire, or make the
   unowned-locked state recoverable) is the same code the 0x7 path would need.
