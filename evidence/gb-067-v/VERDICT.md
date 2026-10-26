# GB-067-V — VERDICT: **GREEN**

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-26 12:2x–12:4xZ.
Verifies GB-067 (#76) / ship **PR #182 @ `fcdf8ae`**, gated as `origin/main` `3c2d380` × PR #182
(`a9770b8`, `ort` merge, no conflicts), intel T3 on this VM. No CI consulted; I did not author GB-067 or
PR #182.

## One line

A host-staged 4 KiB `/STAGE4K.BIN` is read back byte-exact over **all 4096 bytes** on waves that reach
`System halt`, the ABSENT path the card warns about did **not** occur in any of three runs, and the
`attr != 0` half is present on main with its unit suite green.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | in-guest byte-exact read-back of a host-staged 4 KiB file, on a wave reaching `System halt` | **PASS** — 3/3 runs, 0 `[STAGE4K] FAIL` |
| 2 | honest reading of the 1-of-4 ABSENT reproduction | **0 of 3 runs** hit the ABSENT path (quoted) |
| 3 | `attr != 0` present (PR #48) with its unit test green | **PASS** — `fat16.c:1273 new_entry.attr = 0x20`, 374/0 unit tests |

## Clause 1 — the read-back, quoted (`02-stage4k-lines.log`, `01-three-runs.log`)

Fixture: 4096 bytes, `byte[i] = i % 251`, `sha256 d67c656e01756650d77717b0839985a056ec28ffe174601d690fc407a2ceffca`
— the same hash the previous verifier recorded — staged post-build through the tier's documented hook:

```
T3_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/stage4k.bin ::STAGE4K.BIN && mdir -i "$ROOT/disk.img" ::/ | grep -i stage4k'
```

```
T3_intel: 11132 console lines, 90 FAIL lines, 7 user memory fault records across 7 processes, 82 programs exited, halted=1   (run 1)
T3_intel: 32829 console lines, 138 FAIL lines, 25 user memory fault records across 20 processes, 86 programs exited, halted=1  (run 2)
T3_intel: 12822 console lines, 33 FAIL lines, 8 user memory fault records across 8 processes, 80 programs exited, halted=0   (run 3)
```

```
[WAVE-TRY] STAGE4K_T.BIN
[ARRIVED] STAGE4K_T.BIN rc>0
/STAGE4K.BIN
[STAGE4K] PASS size == 4096
[STAGE4K] PASS read full 4096 bytes
[STAGE4K] PASS byte-exact content (0 mismatches)
[STAGE4K] ALL PASSED
```
Per run: `run1 PASS=6 FAIL=0 ALLPASSED=2`, `run2 PASS=15 FAIL=0 ALLPASSED=5`, `run3 PASS=6 FAIL=0
ALLPASSED=2` — **no `[STAGE4K] FAIL` line in any run**, including the one that stalled before halting.

It is a **full-content** compare, not a header/first-N check (`src/user/stage4k_test.c:79-90`): a read
loop to `FIXTURE_SIZE`, `check("read full 4096 bytes", got, FIXTURE_SIZE)`, then a loop over all 4096
bytes comparing `(unsigned char)buf[i]` with `(unsigned char)(i % 251)` and `check("byte-exact content (0
mismatches)", bad, 0)`.

## Clause 2 — the ABSENT path did not occur here (stated, not hidden)

The card asks for the count and attribution. Measured: **0 of 3 runs** opened `/STAGE4K.BIN` as ABSENT —
the reader reached `PASS size == 4096` in every run, and a case-insensitive scan for `absent` in run 1's
whole serial returns exactly one hit, an unrelated test line:

```
2284:[REGTEST] PASS re_search reports -1 when absent
```
(`03-absent-and-attr.log`)

So on this tree the worker's 1-of-4 ABSENT reproduction (under `FATBIG_T` 2 MiB + `BLKTEST` 512 KiB churn
immediately before) **did not reproduce in 3 runs**, and I can attribute it to nothing — I did not
reproduce it, rather than explaining it away. Two of the three runs reached `System halt` (the card's
stated pass condition for clause 1); the third stalled without halting (the GB-057/GB-100 family) and
still produced all three PASS lines, which I quote rather than use as the green.

## Clause 3 — attr != 0 (`03-absent-and-attr.log`, `04-unit-tests-summary.log`)

```
$ grep -n "new_entry.attr" src/kernel/fat16.c
1273:    new_entry.attr = 0x20; /* ATTR_ARCHIVE, same as host-written regular files */

$ make ARCH=intel unit_tests_intel
fat16_test_suite: …
Tests run:    374
Tests failed: 0
UNIT TESTS PASSED
```
The created-entry attribute is on main (PR #48's half) and the suite that covers it is green on this tree.

## What is not claimed

The wave is still red on its own FAIL lines (90/138/33) and one run in three still stalls — that is the
GB-057/GB-100/GB-093/GB-095 family, not this card, and I have not counted it either way. The card's own
clauses are met on the evidence above.

## Reproduce

```
git worktree add /tmp/gb067v origin/main && cd /tmp/gb067v
git merge fcdf8ae            # PR #182, ort, no conflicts
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
python3 -c 'open("/tmp/stage4k.bin","wb").write(bytes(i%251 for i in range(4096)))'
for i in 1 2 3; do
  T3_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/stage4k.bin ::STAGE4K.BIN' \
  T3_LOG=$PWD/run$i.log T3_TIMEOUT=900 bash tools/t3_tier.sh intel
  grep -a "STAGE4K" run$i.log
done
grep -n "new_entry.attr" src/kernel/fat16.c && make ARCH=intel unit_tests_intel
```