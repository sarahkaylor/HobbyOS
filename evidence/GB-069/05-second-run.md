**GB-069 — second independent wave: still no fault, and the PD entry is intact once the instrument stops lying.**

Run 2 of the same instrumented tree (`83ff46b` family, worktree `hermes-289f3b84`), intel T3 with disk I/O:

```
kernel-mode FATAL faults ............ 0
vector=14 inside [0x1FC000000,0x1FDFFFFF) ... 0
[GB-069-PD7] divergences (A/D masked) ....... 0
```

That is two independent waves with the same result, so "one lucky run" is no longer the only support for the
negative half of the DoD.

**The masked comparison is the one that counts.** After correcting the check (`slot480 & ~0x60` vs
`init & ~0x60`), zero divergences remain: at every NVMe doorbell write the PD slot still carries
`0x81000000 | P | RW | PWT | PCD | PS`, with only the CPU's own Accessed/Dirty bits added. Mechanism (1)
("another mapper replaced the entry") is therefore **not** observable here — the earlier `0x9B → 0xFB` line was
A|D, not a rewrite, and I have said so on this ticket rather than leaving a false positive standing.

**Disclosure about these two runs' tier verdicts.** The tree also carries my GB-071 probes, and the
`[ENTRY-KBUF]` lines are counted by `t3_tier.sh` as `FAIL` lines (`FAIL: line startpoint[ENTRY-KBUF] …`,
3933 of them). So `T3_intel EXIT=1` on these runs is **my instrumentation talking**, not a property of `main`.
The fault census above is read from the serial directly, not from the tier verdict, and that is the only claim
I am making.

**Status unchanged:** negative half of the DoD holds across two runs; the mechanism clause needs a fault site
that does not exist on this sha. Still **In Progress**, still awaiting the Coordinator's decision on whether to
fund a load-varied reproducer hunt or close as not-reproducing.
