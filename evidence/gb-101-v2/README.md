# GB-101-V2 — gate evidence (VERDICT: RED — the numbered clauses pass, the mechanism does not)

Ticket: GB-101-V2 (#187) — re-gate the torn-frame detector's below-32 clause
Verifies: GB-101 (#179), PR **#187** head **`9c7484b`** (confirmed: the head is the one the card
names), `src/kernel/arch/x64/trap.c` +38 as the card says.
Note on the PR: against today's `origin/main` (`20084d8`) the branch also carries 14 unrelated
CI/tooling files (`.github/workflows/*`, `Makefile`, `tools/cache/*`, `src/host/host_pthread_compat.h`,
`src/user/wk1c_test.c`, …) because it is based on `c71cb87`. I graded the `trap.c` change (checked out
over a clean `20084d8` worktree) and did not grade the CI files — CI is not a gate.
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2). I authored neither GB-101 nor #187.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | below-32 injection on a 0-FAIL halted wave log → FAIL by name | **PASS** (classifier) |
| 2 | above-32 still caught (`0x4E20`, `0x7703A33E`) | **PASS** |
| 3a | the gate does **not** judge SS/RSP on same-privilege traps | **PASS** (only `cs` and `vector` are read) |
| 3b | `[TORNFRAME]` = 0 on live runs (no false positives) | **0 in 86 726 lines / 4 completed runs — PASS literally** |
| 4 | three runs, four counts quoted per run | **4 completed runs quoted** (+2 aborted by an instrument defect, excluded and named) |
| 5 | ARM `unit_tests` links, arm `trap.o` untouched | **PASS** |
| — | **the detector names a real torn frame** (the card's purpose) | **FAIL** — two fatal frames with garbage fields, zero `[TORNFRAME]` |

## Clause 1/2 PASS — injections on a real 0-FAIL halted wave

Baseline is a real guest log (single-program manifest, `obj/t3_gb101v2_clean.log`, 130 lines, 0 FAIL,
halted=1, kfault=0):

```
$ T3_MIN_LINES=0 T3_MIN_PROGRAMS=0 bash tools/t3_tier.sh police evidence/gb-101-v2/clean_wave.log
T3_intel: PASS (0 FAIL lines, halted)          T3_intel EXIT=0

$ … police evidence/gb-101-v2/inj_below32.log        # + [TORNFRAME] … vector=6 rip=0x7 cs=0x0
T3_intel (police …): 132 lines, 1 FAIL lines, … halted=1
130:[TORNFRAME] FAIL - interrupt/fault handler read a torn frame: vector=6 rip=0x7 cs=0x0
T3_intel EXIT=1

$ … police …/inj_4E20.log                            # vector=20000 rip=0x97 cs=0x08
130:[TORNFRAME] FAIL - … vector=20000 rip=0x97 cs=0x08      T3_intel EXIT=1
$ … police …/inj_7703A33E.log                        # vector=1996485438
130:[TORNFRAME] FAIL - … vector=1996485438 rip=0x7003A33E cs=0x08   T3_intel EXIT=1
```

## Clause 3a PASS — only `cs` and `vector` are judged

```c
int bad_vec = ((uint64_t)tf->vector > 0xFFu);
int bad_cs  = (frame_cs != 0x08u && frame_cs != 0x1Bu);
if (bad_vec || bad_cs) { … }
```
`ss`/`rsp` appear only in the comment and the printed text. The stub does copy the hardware-pushed CS
into `tf->cs` (`trap.c` "3. CS ([rsp + 288]) -> cs ([rsp + 280])"), so the field is authoritative.

## Clause 4 — runs (four completed, two aborted and excluded)

`T3_MONITOR=1 T3_TIMEOUT=1500 bash tools/t3_tier.sh intel`, counts from each run's own serial:

| run | tier script | lines | `[TORNFRAME]` | `System halt` | `FATAL kernel-mode fault` |
|---|---|---|---|---|---|
| 1 | shipped | 31 377 | **0** | **0** | 1 — **`vector=4987` rip=0x…** |
| 4 | probe-safe copy | 17 560 | **0** | **1** | 1 — **`vector=6 rip=0x0000000000000007`** |
| 5 | probe-safe copy | 11 513 | **0** | **1** | 0 (clean halting wave) |
| 6 | probe-safe copy | 26 276 | **0** | **1** | 1 — `vector=6 rip=0x25FFE5C0` |
| 2, 3 | shipped | 7 726 / 18 643 | — | — | — — **aborted, exit 141, no verdict** |

Runs 2 and 3 are the instrument defect described at the bottom, not the detector's doing; they are
excluded and the reason is measured.

## The finding that decides the card: the detector never names a real torn frame

86 726 lines, four completed waves, **zero** `[TORNFRAME]` lines — while two of those waves produced a
fatal fault whose frame is garbage:

* **run 1**: `[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=4987 …` — 4 987 cannot come
  out of a 256-gate IDT, i.e. exactly the `vector > 0xFF` case the guard is supposed to catch at entry.
  No `[TORNFRAME]` line exists in that log (`grep -c '[TORNFRAME]'` = 0).
* **run 4**: `… vector=6 rip=0x0000000000000007 …` — the card's own below-32 shape (`vector=6 rip=0x7`,
  quoted verbatim in the card body), with `cs` evidently 0x08/0x1B because the guard did not fire.
  Again zero `[TORNFRAME]`.

So the CS test does not reach the frames that actually occur: on the real population the garbage field
is `rip` (and sometimes `vector`, but only after the handler is already running), while `cs` stays
plausible. The clause-1 injection is a *classifier* test — it proves the tier fails when someone pastes a
`[TORNFRAME]` line into a log; it cannot distinguish "the kernel detects the frame" from "the line was
pasted", and the live runs are the evidence that discriminates. They say the kernel does not detect it.

## Clause 5 PASS

```
$ make ARCH=arm MODE=unit_tests hobbyos.elf
ld.lld -T linker.ld -o hobbyos.elf obj/arm/… obj/arm/trap.o obj/arm/uart.o
ARM_LINK_EXIT=0
```
No `arch/arm` file appears in the diff (`git diff --name-only origin/main...pr/187 | grep arch/arm` →
empty), so the arm `trap.o` is untouched by construction.

## Instrument defect found on the way (worth its own card)

With `T3_MONITOR=1`, a run dies inside `probe_guest()` the first time a stall marker fires: the sample
block is cut off mid-way (no `=== end guest sample ===`), the cleanup trap SIGTERMs QEMU
(`qemu-system-x86_64: terminating on signal 15`) and the script exits **141** with no verdict.
Measured 3 of 5 monitored runs today (GB-101-V run3; GB-101-V2 runs 2 and 3). Cause is a probe command
substitution returning non-zero under `set -e`, which aborts the whole run.

To still get the card's three runs, runs 4–6 used `t3_tier_noerrexit.sh` — a copy of the shipped tier
whose only differences are `set -uo pipefail` instead of `-euo` and a pinned `ROOT`; every verdict rule,
count and message is the shipped one, and the graded kernel is untouched. The shipped-tier run (run 1)
is quoted alongside them.

## Verdict

**RED**, and the reason is not in the card's numbered list — it is the card's purpose. Clauses 1, 2, 3a,
3b, 4 and 5 all pass as written; what fails is that a detector which has never fired in 86 726 lines of
live waves, while `vector=4987` and `vector=6 rip=0x7` fatal frames walked straight past it, cannot be
certified as "the torn-frame detector". I state the counter-argument plainly: the card's RED triggers are
"below-32 injection passes silently / above-32 regressed / fires on a real frame / SS-RSP judged", and
none of those fired — a coordinator who intended the injection test to be sufficient can overrule this
and call it GREEN. My reading is that a GREEN here would be the "looks green" failure gaming.md §4.2
forbids, because the one measurement that distinguishes a detector from a log classifier says no.

What would change the verdict: a run in which the detector fires on a garbage frame the guest produced,
or a recorded reason why entry-time `cs` is the only reachable corruption given run 1's `vector=4987`
(which suggests the frame degrades *after* entry, i.e. the check is on the wrong side of the corruption).