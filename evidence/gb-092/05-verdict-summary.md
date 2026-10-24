# GB-092-V — instrument verdict, revision b587a9d

Worktree `hermes-9642df51`, branch `verify-gb092v` at **b587a9d** (= `origin/ticket/092-glued-fp`, the
PR head the card names; not an ancestor of `origin/main`).

## DoD 1 — self-test proves every direction, including the shape v1 miscounted

```
$ make t4_glued_selftest
self-test: glued=[2 2] clean=[1 0] fusion=[1 1] fusion2=[1 1] (want glue>0 / 0,0 / >=1 / >=1)
self-test: absent-log exit=2 (want non-zero)
self-test: OK — genuine FAILs on glue, 0 on clean (incl [0]-subscript single records),
           catches injected fusions, refuses a missing log
exit=0
```

`clean=[1 0]` is the decisive pair: raw counts 1, **genuine counts 0** on a single
`tf->regs[0]`-shaped record — the exact false positive of 379-of-417 lines that v1 produced. Injected
fusions give genuine ≥ 1, so the counter cannot be painted to 0 by a clean input.

## DoD 2 — raw and genuine both printed on real ARM full waves, genuine ≤ raw

The card asks for the ARM full-fix serial. The ARM serials in this tree are the full waves from
GB-086's run (they are inputs; the counter is a pure log classifier):

```
$ bash tools/gate/t4_glued.sh /tmp/arm_smp8.log     # 24,526 lines, -smp 8, 570 [ARRIVED] records
t4_glued: raw=635 genuine=192 glued lines in '/tmp/arm_smp8.log'      exit=1

$ bash tools/gate/t4_glued.sh /tmp/arm_smp1.log     # 8,775 lines, T3_SMP=1
t4_glued: raw=276 genuine=5 glued lines in '/tmp/arm_smp1.log'        exit=1
```

Both runs print **both** numbers, `genuine <= raw` in both, and the script states the grading rule in
its own output: *"GB-082 DoD is graded on the GENUINE count; RAW is the v1 superset incl.
[0]-array-subscript single records v1 misread."*

## DoD 3 — a missing log exits 2 (proven twice, live)

```
$ bash tools/gate/t4_glued.sh evidence/gb-086/02b-serial-smp8.log
t4_glued: FAIL (exit 2) — serial log '…' absent. A missing log is not a clean log:
0 glued lines cannot be measured over nothing.
```

(That path was absent because I had switched branches — a real accident that produced a real
demonstration: absence is refused, not scored 0.)

## Verdict

**GREEN** for GB-092-V. The classifier distinguishes letter-led record tags from digit-led subscripts,
the raw/genuine split is printed on real ARM waves with genuine ≤ raw, the grading rule is stated, and
a missing log is refused. Most importantly the counter is **not self-serving**: on a post-GB-082 ARM
tree it reports **192 genuine** glued lines (8 CPUs) and **5** (1 CPU) rather than finding a
comfortable zero.

## Two things the Coordinator must read alongside this GREEN

1. **Those 192 genuine lines are a GB-082 fact, not a GB-092 defect.** `ad51ac5` contains the GB-082
   counter fix (`45abe1b` is an ancestor), and the 8-CPU ARM wave still shows 192 genuinely fused
   lines. GB-092 only makes the number honest; whether the number should be zero is GB-082's DoD.
2. **The input logs are from `ad51ac5`, not from this sha's own build.** The instrument under test is
   `b587a9d`; the logs are genuine full ARM waves from the GB-086 tree. Since the counter only reads
   text, that does not weaken the instrument verdict — but it means this GREEN says nothing about
   `b587a9d`'s runtime behaviour, only about its classifier.
