# GB-094-V — gate run, revision b5b7b2c

Worktree `hermes-9642df51`, branch `verify-gb094v` at **b5b7b2c** (= PR #121, the sha this card names),
swarm-farm-2. ARM tier run to `halted=1`.

## Clause 1 — the clause itself: FAILS

ARM tier at b5b7b2c:

```
T3_arm: 39023 console lines, 13 FAIL lines, 2 user memory fault records across 1 processes,
        84 programs exited, halted=1
```

`t4_glued.sh` (v1 counter, as shipped on this sha) reports 67 glued lines. That counter counts
`[0]`-subscripts as tags, so the classification the card asks for is the leading-tag histogram over
two-record lines — `evidence/gb-094/hist.py`, letter-led tags only:

```
two-record lines: 89
leading tag histogram:
    40  [ARRIVED]  (kernel)
    27  [DEBUG]  (kernel)
    14  [WAVE-TRY]  (kernel)
     5  [KERNEL]  (kernel)
     1  [LFN]  (USER-PROGRAM)
     1  [TORTURE]  (USER-PROGRAM)
     1  [PIPE_WRITE_EPIPE]  (USER-PROGRAM)

lines led by a user-program tag: 3
```

Verbatim, the three lines the clause forbids:

```
[LFN] PASS read_dir /src shows 30+ char name (31-char syscall ca[LKSTEST] PASS stat ok
[TORTURE] note: churn cycles skipped (create budget), skipped=56[PASTETEST] PASS --serial long option
[PIPE_WRITE_EPIPE] PID [WAVE-TRY] 38SED.BIN pipe
```

Each is a user program's record cut mid-token (`… syscall ca`, `skipped=56`, `PID `) with another
writer's record appended. That is the buffer **missing a path**, which is exactly what this card
exists to prevent.

Note the residue is otherwise all kernel: the 86 kernel-led lines are `[ARRIVED]`/`[DEBUG]`/
`[WAVE-TRY]`/`[KERNEL]`, i.e. GB-082's half — as the card says, out of scope. The 3 user-led lines are
not.

## Hazard 2 — prompt before read: HOLDS

```
user@hobbyos:/$ '
[TEST3] Initial prompt read successfully.
```

The prompt is on the serial before the read returns. (Prompt-dependent only on this shell-test
sequence; I did not drive an interactive command by hand.)

## Hazard 3 — exit flush: not established

`[MD5TEST] ALL PASSED (16 checks)` is present, so that program's final line survived its exit. But
neither MD5TEST nor CMPTEST ends with a deliberately newline-free write, so this is supporting
evidence, not the test the card describes.

## Hazard 4 — redirected fd 1: cannot be quoted from this wave

`PIPETEST.BIN` emits no rows on this run at all:

```
[WAVE-TRY] PIPETEST.BIN
[ARRIVED] PIPETEST.BIN rc>0
[KERNEL] Process 16: PIPETEST.BIN exited unexpectedly or gracefully.
```

No `PIPETEST` PASS/FAIL rows, and one `|` occurrence in the whole serial. The card says to quote the
rows; there are none to quote.

## Hazard 5 — intel side: the run stalled

```
SHELLTEST WATCHDOG: protocol stalled 25 s; FAILING TEST. Process table:
[WATCHDOG] console silent for more than 20s
  CPU5 rip=0x0000000070037326 pid=-1 age_ms=10 syscall=6
  …
  pid=21 age_ms=2379071
```

71 WATCHDOG lines; one process ageing ~40 minutes. Per the card, a stalled run is not a verdict on
this card, so I am not grading the <35 artifact-class count from it.

## Verdict

**RED.** Clause 1 fails: three two-record lines are led by a user program's record — `[LFN]`,
`[TORTURE]`, `[PIPE_WRITE_EPIPE]` — so the line buffer misses at least one path. Hazard 2 holds;
hazard 3 is only partially evidenced; hazard 4 has no rows to inspect; the intel comparison stalled
and is reported as such rather than scored.

The kernel half (86 lines) is GB-082's, and I am not calling it this card's failure.
