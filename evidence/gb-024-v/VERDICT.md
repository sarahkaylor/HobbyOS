# GB-024-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-25.
Revision: **`origin/main` @ `6b6f8a2`** (`GB-057: watchdog prints RFLAGS per CPU (#152)`, later than the
card's floor `618ef89`). Clean worktree, no local edits.

The W0 output clause works and the gate provably fails. The M1 gate still cannot be graded green, for two
reasons: **ARM does not link on main at all**, and three of the four W0 members are never compared — only
counted as "arrived".

## Clause 1 — W0 corpus, per-member output vs committed `.ref` — **FAIL (1 of 4 compared)**

```
=== t4_w0: W0 corpus gate over t3_intel_6b6f8a2.log ===
  PASS HELLO (arrived in guest)
  PASS HELLO output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS SH (arrived in guest)
  PASS CAT (arrived in guest)
  PASS LS (arrived in guest)
t4_w0: PASS — every W0 corpus member arrived in the intel wave; members emitting a
               '[W0] <member> linux=' marker had their output verified ...
rc=0
```

The DoD asks that "every W0 corpus member arrives in the wave **and its stdout is compared byte-exactly**
against the committed `.ref`". Only HELLO does both. The corpus is one member deep:

```
[W0] <member> linux= markers:  HELLO 3   SH 0   CAT 0   LS 0
tests/fixtures/abi/w0_output/: HELLO.ref   (no SH/CAT/LS refs exist)
```

So `PASS SH/CAT/LS` means "the program ran", not "the program produced the right bytes". The card's own
table calls W0 "hello / cat / sh / coreutils-static" — the same wrong-set trap GB-070 was minted to close,
one level down: the gate now checks the right *names* but only one of them for *content*.

## Clause 2 — intel T3 reaches `halted=1`, counts quoted — reached, not green

```
  System halt      = 1
  FAIL lines       = 165
  user mem faults  = 0
t4_glued: 0 glued console lines in 't3_intel_6b6f8a2.log'   (PASS — single writer)
  T3_intel EXIT=1
```

The halt happens and the glue count is finally clean on intel (it was 2 at `9f89a23`), but the tier exits 1
with 165 FAIL lines. Reaching the halt is not the same as passing; I am quoting the counts rather than
calling this green.

## Clause 3 — ARM T3 green — **RED: main does not link for ARM**

```
ld.lld: error: undefined symbol: resume_mark
>>> referenced by process.c:1687 (src/kernel/process.c:1687)  obj/arm/process.o:(schedule)
>>> referenced by process.c:3331 (src/kernel/process.c:3331)  obj/arm/process.o:(start_scheduler)
make: *** [Makefile:736: hobbyos.elf] Error 1
```

`resume_mark` is defined **only** in `src/kernel/arch/x64/trap.c:2274` (GB-057 instrumentation, #151) and
called unconditionally from the shared `src/kernel/process.c`. There is no ARM definition and no guard, so
`main` @ `6b6f8a2` cannot be built for ARM — the tier never reaches a boot. This is a merge-blocking defect
on the merged tip, found by the milestone gate, and it is not GB-024's code.

## Clause 4 — the gate can fail — **PASS**

One byte appended to a copy of `HELLO.ref`, gate pointed at it with `T4_LEGA_OUT`:

```
  ... expected '...X' (writes expected output: NO)
t4_w0: FAIL — W0 corpus incomplete or output mismatch
rc=1
```

It fails on a wrong `.ref` byte, and only for HELLO — consistent with clause 1: no `.ref` exists for the
other three, so nothing about their output can make this gate fail.

## Clause 5 — a missing member/probe FAILs, never skips — **PASS**

```
$ grep -av "ARRIVED] CAT.BIN" serial > /tmp/w0_nocat2.log
  FAIL CAT — not in the intel wave's exited-programs/arrival set (W0 corpus member never ran)
t4_w0: FAIL — W0 corpus incomplete or output mismatch   rc=1

$ : > /tmp/w0_empty.log ; t4_w0.sh --serial /tmp/w0_empty.log
  FAIL LS — not in the intel wave's exited-programs/arrival set
t4_w0: FAIL — no W0 member arrived in the guest (absent probe is a failure, never a skip)   rc=1
```

Self-test also passes all three legs (`good → PASS`, `missing CAT → FAIL`, `broken HELLO → FAIL`).

## DoD table

| # | clause | result |
|---|---|---|
| 1 | every W0 member's stdout byte-compared to its `.ref` | **FAIL** — HELLO only; SH/CAT/LS pass on "arrived", no `.ref` exists |
| 2 | intel T3 `halted=1` with counts quoted | **reached, not green** — `halted=1`, 165 FAIL, 0 user mem faults, 0 glued, `EXIT=1` |
| 3 | ARM T3 green | **RED** — `main` does not link for ARM (`resume_mark` undefined) |
| 4 | the gate can fail (wrong `.ref` byte) | **PASS** — `rc=1`, mismatch named |
| 5 | absent member / absent probe FAILs | **PASS** — both `rc=1`; self-test covers the three legs |

## What closes it

1. **Unblock ARM on main first**: give `resume_mark` an ARM definition (or guard the calls in `process.c`
   with the arch split that already exists for the x64 trap code). Nothing about the M1 gate can be graded
   until `hobbyos.elf` links for ARM.
2. Commit `.ref` files for SH, CAT, LS and make the `[W0] <member> linux=` marker mandatory — a member
   without a marker and a `.ref` should FAIL, not pass on arrival. Otherwise "W0 corpus output clause"
   describes one program, not four.
3. Re-run: intel T3 to `halted=1`, ARM T3 to `halted=1`, `t4_w0.sh` on both serials, plus the wrong-`.ref`
   falsification again.

Not this card's ground: the intel 165 FAIL lines (GB-057 attribution) and the ARM glue work (GB-082).
