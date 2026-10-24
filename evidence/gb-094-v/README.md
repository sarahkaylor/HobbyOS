# GB-094-V — independent measurement (DEEP Verifier, 2026-10-24)

Card #153 was claimed by two verifier sessions inside the same second (18:17:06Z); the peer posted RED at
19:44:54Z and moved the lanes. **This session moved nothing.** These are my own runs, kept because they
disagree with the peer's clause-1 numbers and because they find a *second*, different failure (clause 4).

Graded: `b5b7b2c` (`GB-094: line-buffer user console output`, #121); baseline A/B: `2d9846b` (its parent).

| # | file | what |
|---|---|---|
| 01 | `01-arm-hist-after.txt` | `b5b7b2c` ARM: 70 glued lines, all kernel tags, **0** user-program tags |
| 02 | `02-arm-hist-baseline.txt` | `2d9846b` ARM: 79 glued lines, **14** user-program-tag-led |
| 03/04 | `03-glued-after.log`, `04-glued-baseline.log` | `t4_glued.sh` on both serials (exit 1 both) |
| 05/06 | `05-tier-arm-after.log`, `06-tier-arm-baseline.log` | the two tier runs |
| 09 | `09-tier-arm-after-run2.log` | a second `b5b7b2c` ARM run (reproducibility of the pipe failure) |
| 07/08 | `07-tier-intel-after.log`, `08-classify-intel.txt` | intel run + `classify_t3_fails.py` |
| 10/11 | `10-pipetest-after.txt`, `11-pipetest-baseline.txt` | the pipe round-trip, before and after |

## Clause 1 — my run: PASS (and it disagrees with the peer's)

```
after      b5b7b2c : T3_arm: 29107 console lines, 1 FAIL, 84 programs exited, halted=1
                     glued=70  [ARRIVED] 24  [DEBUG] 23  [WAVE-TRY] 21  [KERNEL] 2   user-tag 0
baseline   2d9846b : T3_arm: 21979 console lines, 1 FAIL, 84 programs exited, halted=1
                     glued=79  [WAVE-TRY] 22 [ARRIVED] 18 [DEBUG] 14 [SIGTEST] 4 ...      user-tag 14
```
The peer measured 39,023 console lines and 13 FAIL lines and found 3 user-tag-led lines; I measured 29,107
and 1 FAIL and found 0. Same sha, same command — **the ARM wave is not shape-stable between runs**, which
matters for a gate whose clause is a count over serial lines. Both of us agree the count is far above the
card's "40" baseline row (mine 70, theirs presumably higher), so that table is stale either way.

## Clause 4 — FAILS: a redirected fd 1 loses its bytes (the finding this session adds)

```
b5b7b2c  (after)
 806: Parent: writing...
 807: SUCCESSParent: closing write end      <-- the 7 bytes went to the CONSOLE, not the pipe
 819: Child: read returned
 820: Child got no data or error            <-- pipe round-trip empty

2d9846b  (baseline)
 940: Child: read returned
 941: Child got data!
```
Reproduced twice at `b5b7b2c` (runs 05 and 09): `Child got no data or error`. The baseline got the data.
`PIPETEST` takes `pipe()` fds while 0/1 are unset, so `write(local_fds[1], …)` is a `write(1, …)`; GB-094's
`write()` diverts `fd == 1` through `hb_stdout_put()` whenever `hb_stdout_is_console()` says console, and the
bytes are then emitted on the console at the next flush instead of entering the pipe. That is exactly the
hazard the card names: "a redirected fd 1 must still stream byte-for-byte — the buffer must not apply."

Note `PIPETEST` exits 0 either way, so `[ARRIVED] PIPETEST.BIN rc>0` is not evidence of anything; the child's
line is the observable.

## Clause 2 — PASS

`sh.c:3358` flushes the prompt before `read`, and the serial shows the harness reading it first:
```
[read_until] MATCHED: '…user@hobbyos:/$ '
[TEST3] Initial prompt read successfully.
```

## Clause 3 — mechanism present, not separately exercised

`_exit()` flushes (`libc.c:794`) and `exit()` flushes (`libc.c:408`), and no user-tag-led glue appears in my
run — but I did not cite a specific program whose final write has no newline, so this clause is "code path
read + no contrary serial evidence", not a measured pass.

## Clause 5 — artifact count 0, but the run wedged

```
intel b5b7b2c: 10324 console lines, 3 FAIL, 63 programs exited, halted=0, 2 FATAL kernel-mode faults
classify_t3_fails.py: artifacts 0 / real 3     (0 < 35 baseline)
```
Artifacts are 0, but the tier stalled (`halted=0`) with kernel-mode `#PF` dumps, and one line shows mangled
interleaving (`[KERNEL] FATAL: … GErrarpohrics CoVadlieda:t io0xn`). Per the card, a stalled run is not a
verdict on this card — recorded as the shape, not as a pass.
