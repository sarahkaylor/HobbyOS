**GREEN — GB-113-V, verified on this VM (DEEP verifier `wgfx-verify`), code PR #222 @ `d21759c`. Local tiers only; no CI is cited, used, or gated on.**

Evidence commit: **`87336db`** on branch **`verify/gb113v`** (`evidence/gb-113-v/`: full-wave log.gz + tier txt, both focused logs + tier txts, the exact revert patch, the parser, and the focused-wave driver). Trees: `wt-gb113v` at `d21759c` (code as submitted) and `wt-gb113v-revert` at `d21759c` + `control_revert.patch` (only the drain/reap change reverted; raw-word printing untouched).

Instruments run: (a) the full intel T3 wave `tools/t3_tier.sh intel` on `d21759c`; (b) a focused one-program intel wave — `obj/intel/tsort_test.bin` staged as `::TSORT_T.BIN` + a one-line `::WAVE.CFG`, driven through the same `tools/t3_tier.sh` — on the fixed tree and on the control tree. The focused driver asserts halted=1, program loaded, wait word printed, no kernel fault; the tier's one-program completeness-floor FAIL is expected and is not read as a result.

## 1. The raw word is printed on both paths — quoted

FAIL path, fresh intel wave, **control** build (the fixed build emits no exit-status FAILs, so this is the only place the path can be exercised — the instrumentation is identical, only the drain order is reverted; see clause 5):

```
[TSORTTEST] FAIL chain from FILE (exit status -1 != 0, raw=0x0000000d)
[TSORTTEST] FAIL chain from stdin (exit status -1 != 0, raw=0x0000000d)
[TSORTTEST] FAIL chain from '-' operand (exit status -1 != 0, raw=0x0000000d)
[TSORTTEST] FAIL diamond from FILE (exit status -1 != 0, raw=0x0000000d)
[TSORTTEST] FAIL diamond from stdin (exit status -1 != 0, raw=0x0000000d)
[TSORTTEST] FAIL cycle from FILE (stderr + rc 1) (exit status -1 != 1, raw=0x0000000d)
```

(12 such lines in that run; full list in `tsort_revert_intel.log`.)

PASS path, fixed build — the word prints on every passing check, including the full wave:

```
[TSORTTEST] PASS chain from FILE
 [raw=0x00000000]
[TSORTTEST] PASS chain from stdin
 [raw=0x00000000]
[TSORTTEST] PASS diamond from FILE
 [raw=0x00000000]
```

Counts: fixed full wave **45 wait words, 45 × `0x00000000`, 0 signal words**; fixed focused wave **12 × `0x00000000`**; control wave **14 words = 2 × `0x00000000` + 12 × `0x0000000d`**. The word lands on its own console line (the console was already splitting `" ["` from the word before this card — GB-082 family), which is why it is quoted with its preceding check line.

## 2. The value selects the mechanism, and the rule is quoted

`src/user/tsort_test.c:285`:
```c
run_rc[slot] = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
```
`src/user/tsort_test.c:316-325` (the failure branch that prints the word):
```c
  if (run_rc[slot] != want_rc) {
    print_console("[TSORTTEST] FAIL ");
    ...
    print_console(", ");
    con_raw(run_raw[slot]);
    print_console(")\n");
```
So `-1` is `WIFEXITED` false, and the word decides which: `0x0000000d & 0x7f = 13 = SIGPIPE` ⇒ *"why do these children die"*; a clean code in the high byte with `rc == -1` would have selected the `PROCESS_STATUS_SIGNAL` mask instead. Observed: the former, on every one of the 12.

## 3. A clean exit is observed as 0 in the same wave as the signal cases

Control wave, one run, one log:
```
385:[TSORTTEST] FAIL chain from FILE (exit status -1 != 0, raw=0x0000000d)
...
[TSORTTEST] PASS empty input
 [raw=0x00000000]
[TSORTTEST] PASS --help prints the 2.1 usage
 [raw=0x00000000]
```
Same serial carries `raw=0x0000000d` (12) and `raw=0x00000000` (2). This is the clause the reviewer bounced last round; it is now a measurement, not an inference.

## 4. The SIGPIPE deaths are gone — counted, same scope

| build | scope | TSORTTEST exit-status FAILs | `FAILURES:` |
|---|---|---|---|
| control (drain reverted) | focused intel wave | **12** | 14 |
| `d21759c` (fixed) | focused intel wave, identical scope | **0** | 4 |
| `d21759c` (fixed) | full intel wave, halted=1, 41168 lines | **0** | 4 (×3 waves) |

The worker's own before/after was 7 → 0 on a full wave; my counts are 12 → 0 at the smaller identical scope and 0 on a full wave. Both are stated as measured, from the runs named.

## 5. The fix is falsifiable — revert only the drain, the deaths return

`control_revert.patch` restores exactly the pre-fix order and nothing else:
```c
close(err_p[1]);
while (en + 1 < errsize) { ... }        /* stderr to EOF */
close(err_p[0]);                        /* read end gone */
close(out_p[1]);
while (n + 1 < outsize) { ... }         /* stdout to EOF */
close(out_p[0]);                        /* read end gone */
/* waitpid LAST */
```
Result: 12 exit-status FAILs, all `raw=0x0000000d`, 14 failures. Re-applying `d21759c`'s select-based concurrent drain: 0. A fix that cannot be un-fixed has not been located; this one can be and was.

## 6. No passing case changed

Diff of the TSORTTEST check lines, control (minus its exit-status lines) vs fixed:
```
1a2,6
> [TSORTTEST] PASS chain from FILE
...
9c19
< [TSORTTEST] FAILURES: 14
---
> [TSORTTEST] FAILURES: 4
```
The only differences are the 12 checks that the control kills and the fix recovers, and the failure count. Every other line is byte-identical, including all four stderr-content failures (`stderr got: ""` in both runs) and the two checks that pass in both waves. The fix does not alter any surviving check's bytes; it stops killing children.

## 7. `GB-095`'s clause 1 is measurable without `GB-100` — demonstrated

Full intel wave on `d21759c`: `halted=1`, 41168 console lines, `[TSORTTEST] FAILURES: 4` in each of the three waves, **0** exit-status failures, **4** stderr-content failures. The count that GB-095 needs ("TSORTTEST reports 0 failures on intel") is therefore readable today, with no clean halt from GB-100 and no wait-encoding wall: it currently reads **false, 4** — for a stderr-content reason, not a status reason.

## 8. The 4 remaining failures — no card exists, stated plainly

`cycle from FILE`, `cycle from stdin names '-'`, `missing file`, `two operands rejected` — all `(stderr)`, all `stderr got: ""` against TSORT 2.1's expected diagnostic text. **No card names this mechanism.** The only card whose DoD touches it is GB-095 (#152, "0 failures on intel"), and GB-095 does not name stderr content; its `Blocked: waiting on GB-100` preamble is now stale (clause 7). I am not folding these into the GREEN: they are a separate, uncarded stderr-content family, and GB-095's clause 1 stays false until they are fixed or carded.

## Scope of this verdict

GB-113's clauses 1-8 are met, so: **GREEN for GB-113**. The intel tier as a whole is **not** green — the same full wave carries 192 FAIL lines from other families (SEDTEST/CMPTEST/SIGTEST/FPU_T, GB-102's clusters) and 30 user memory-fault records. This verdict says the wait word is honest and the SIGPIPE race is fixed; it says nothing about those.

Lanes: GB-113 (#226) → **Merge Gate**; GB-113-V (#227) → **Done**.