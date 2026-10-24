**GB-096 — the stalled-run chain, measured (same run as the findings above, `9f89a23`, intel).**

The run that spun 110 times on `fill_lock` also took **17 kernel-mode faults**, 12 of them at a single
address:

```
$ grep -ac "Exception in Kernel Mode" obj/t3_intel.log
17
$ grep -ao "RIP: 0x[0-9A-Fa-f]*" obj/t3_intel.log | sort | uniq -c | sort -rn
     12 RIP: 0x000000007003A94F

$ addr2line -f -e hobbyos.elf 0x7003A94F
enter_user_space

$ nm -n hobbyos.elf | awk 'strtonum("0x"$1) <= strtonum("0x7003A94F")' | tail -1
7003a8fb T enter_user_space
```

So the shape of this stall is:

1. **12 × kernel `#PF` (vector 14) inside `enter_user_space`** — the CPU faults in kernel mode there.
2. A kernel-mode fault halts that CPU (`FATAL kernel-mode fault, halting this CPU`).
3. Whatever lock that CPU held stays held. `fill_lock` is observed as `locked=1, owner=0`.
4. Every other CPU spins on it forever, because `lock_steal_ok` and `lock_stallbreak_ok` both
   `return 0` for `owner_slot == 0` (`locks.c:50-51`, `78-79`).
5. The wave never reaches `halted=1` — which is the precondition GB-096's DoD and GB-082-V's gate
   both need.

**What is proven vs inferred.** Proven: the fault address and count, the faulting function, the spin
address (`fill_lock`), `owner=0`, and that both recoveries refuse that owner value. **Inferred, not
proven:** that the halted CPU was inside the two-instruction window between
`__atomic_test_and_set(&lock->locked, …)` and `lock->owner = get_cpuid() + 1`. An alternative is that
the owner was stamped and later zeroed. One probe separates them — record, at acquire, whether the
lock was already `locked=1 && owner==0`, or stamp the owner with the compare-exchange so the window
does not exist at all.

**Why this matters beyond this card.** Three separate tickets I have gated today hit this same wall:
GB-094-V's intel run stalled, GB-096's `halted=1` is unreachable, and GB-082-V's intel leg needs a
clean run. They are not three stalls; they are one `enter_user_space` fault plus one unbreakable lock
state. Fixing the acquire ordering makes every one of them measurable again — even before the
`enter_user_space` fault itself is fixed.
