# GB-024-V — M1 gate on the merged tip (DEEP Verifier, 2026-10-25)

Verdict **RED**. Ran on `origin/main` @ **`6b6f8a2`** (the card says `618ef89` or later; main had moved on).
Fresh worktree, tree clean.

| # | file | what |
|---|---|---|
| 01 | `01-tier-intel.log` | intel T3: `halted=1`, **257 FAIL lines**, 11 memory faults, 86 programs |
| 02 | `02-w0-gate.log` | `t4_w0.sh --serial` → PASS, but see what it actually compared |
| 03 | `03-w0-badref.log` | deliberately-wrong `HELLO.ref` → **FAIL** (gate can fail) |
| 04 | `04-w0-empty-refdir.log` | **empty ref dir → still PASS** ("guest … == Linux-leg ''") |
| 05 | `05-w0-missing-member.log` | `[ARRIVED] SH.BIN` removed → **FAIL** (never a skip) |
| 06 | `06-tier-arm-buildfail.log` | ARM **link failure**: `undefined symbol: resume_mark` |
| — | `gb024v-ev-serial-intel.log` | the intel serial the gate was run over |

## 1. The W0 gate passes — on one member's output and three arrivals
```
  PASS HELLO (arrived in guest)
  PASS HELLO output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS SH (arrived in guest)
  PASS CAT (arrived in guest)
  PASS LS (arrived in guest)
```
`grep -ao '\[W0\] [A-Z]* linux='` over the whole serial → **only `[W0] HELLO linux=`**. SH, CAT and LS emit no
output marker, and `tests/fixtures/abi/w0_output/` holds **only `HELLO.ref`**. The gate's own code treats a
member as PASS when it arrived and either has no marker or has no `.ref`, so **three of the four W0 members
are never output-compared**. The DoD says "every W0 corpus member ... its stdout is compared byte-exactly
against the committed refs" — that is true of one member.

## 2. The comparison silently degrades to a pass when a ref is absent
```
$ T4_LEGA_OUT=<empty dir> bash tools/gate/t4_w0.sh --serial …
  PASS HELLO output — guest 'entered-main:main-returned' == Linux-leg ''
t4_w0: PASS                                                                   exit 0
```
An empty reference is reported as an equality. So a missing, renamed or empty `.ref` turns the output half of
the gate off without turning it red — the exact "passes everything it is shown" shape this card exists to
prevent. (With a *wrong* ref it does fail: `03`.)

## 3. Intel reaches the halt but the tier is red
```
T3_intel: 17762 console lines, 257 FAIL lines, 11 user memory fault records across 10 processes,
          86 programs exited, halted=1
T3_intel: FAIL — 257 FAIL line(s)
```
The card asks for the counts to be quoted; these are them. The serial also shows character-level
interleaving (`[W0A0VE0-0TR7Y00]3 BH1EDFA P TRSEPS: T.0BIxN0`), i.e. two writers inside one line, so some of
those 257 cannot be attributed to the program in front of them.

## 4. ARM cannot even be built on main
```
ld.lld: error: undefined symbol: resume_mark
>>> referenced by process.c:1687 (src/kernel/process.c:1687)   obj/arm/process.o:(schedule)
>>> referenced by process.c:3331 (src/kernel/process.c:3331)   obj/arm/process.o:(start_scheduler)
make: *** [Makefile:736: hobbyos.elf] Error 1
```
`resume_mark()` is defined only in `src/kernel/arch/x64/trap.c:2274` (added by #151), and `process.c` calls it
from shared code with no arch guard. **ARCH=arm does not link at `6b6f8a2`**, so clause 3 is not "red" — it is
unrunnable, and that is a merge-blocking break in its own right.

## 5. What the gate does prove
* wrong `.ref` byte → `FAIL HELLO — guest output marker … != Linux-leg 'WRONG-BYTE'`, exit 1 (`03`)
* member removed from the wave → `FAIL SH — not in the intel wave's … set`, exit 1 (`05`)
