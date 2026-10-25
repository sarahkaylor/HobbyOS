# GB-057 — follow-up after the REVIEW-BOUNCE (14:23Z): the EMFILE lead is withdrawn; the stall window is measured

Builder: `wverify-deep`, DEEP verifier session, 2026-10-25 ~14:3xZ. Still `origin/main` `41ec708`;
all numbers below are re-read out of the three logs already committed in `evidence/gb-057-v/`
(`01/02/03-t3-intel.log`), no new guest run. This is a correction plus a sharper stall window, and it
does **not** claim the DoD is met.

## Correction 1 — the EMFILE lead was overstated, and I withdraw it as a stall cause

My previous comment said the stall is "downstream of file-descriptor exhaustion". The timeline I now
measured in the same log kills that framing (`10-emfile-timeline.log`):

```
lines 2001-4000: EMFILE=13 fixtures=10 pipe=80
lines 4001-6000: EMFILE=2  fixtures=39 pipe=175
lines 6001-8000: EMFILE=0  fixtures=0  pipe=0
... every bucket to the end: 0 / 0 / 0
line 16795: first [IDLESTUCK]
```

The fd episode is a **local event that ends ~11,000 lines before the stall**; the last 12,000 lines,
stall included, have zero EMFILE, zero fixture failures and zero pipe failures. An fd leak cannot be
the cause of a hang that begins 11,000 lines after the fds are healthy again. What survives is weaker
and still true: run 2 uniquely had a fixture-write episode, and its 256 "real" FAIL count is inflated
by it — which is why clause (b)'s number must not be read as test quality.

## Correction 2 — "loader slot pressure" is not the discriminator either

```
run1 (halted=1): starved=19  ARRIVED rc<0=19   rc>0=562
run2 (halted=0): starved=51  ARRIVED rc<0=52   rc>0=792
run3 (halted=1): starved=55  ARRIVED rc<0=54   rc>0=813
```
(`09-slot-pressure.log`) — run 3 starved **more** loads than the stalled run and halted normally. So
"Loader (v2) starved (slot pressure)" is common to all runs and explains none of them.

## What the stall window actually contains (`11-stall-window.log`)

Run 2, the 20 s-silent watchdog sample immediately before the IDLESTUCK dump:

```
[WATCHDOG] console silent for more than 20s
  CPU0 rip=0x70038A16 rflags=0x212 pid=-1 resume_pid=18 age_ms=0  syscall=2
  CPU1 rip=0x70043CA2 rflags=0x10006 vec=13 err=0x0 cr2=0x17FFFFEFB8 pid=15 resume_pid=15
  CPU2 rip=0x7003BEDF rflags=0x10046 vec=14 err=0x2 cr2=0x17FFFFFCB8 pid=7  resume_pid=7
  CPU3 rip=0x70038A16 rflags=0x212 pid=-1 resume_pid=9  age_ms=10 syscall=76
  CPU4 rip=0x70038A16 rflags=0x212 pid=-1 resume_pid=9  age_ms=10 syscall=76
  CPU5 rip=0x7003BFCD rflags=0x10002 vec=13 err=0x0 cr2=0x17FFFFEFB8 pid=12 resume_pid=12
  CPU6 rip=0x70038A16 rflags=0x212 pid=-1 resume_pid=9  age_ms=10 syscall=76
  CPU7 rip=0x70038A16 rflags=0x212 pid=-1 resume_pid=9  age_ms=10 syscall=76
```

Four facts, all new to this card as far as its 174 comments show:

1. **Five of eight CPUs are in the same syscall at the same kernel RIP** — `rip=0x70038A16`,
   `syscall=76`. In `src/include/syscall.h` **76 is `SYS_POLL`**. They are all inside `poll`, with
   `resume_pid=9` (`SHTEST3.BIN`) on three of them.
2. **Three CPUs are faulting, not idling** — two `vec=13` (#GP, err=0) and one `vec=14`
   (#PF, err=2, `cr2=0x17FFFFFCB8`). The machine is not quietly parked; three cores are taking
   exceptions while the console is silent.
3. **An unwakeable wait is already recorded in the same window**:
   `[KERNEL] parked WAIT slot with no parent to wake it: slot=35 pid=35 st=7 parent=-1 TORTURE.BIN`.
   `st=7` is `PROC_STATE_WAIT_CHILD` with `parent=-1`: no state change can ever deliver that wake.
4. **The slot table shows RUNNING claims that never progress** — `slot=7 pid=7 st=3
   rip=0x1000000000` (user VA base, i.e. just entered), `pid=12`/`pid=15` (`cat`) `st=3`,
   alongside 20+ slots in `st=9` (`THREAD_DONE`).

## The experiment this points at (named, not run)

`poll` is the one syscall five cores share, and `src/user/ipc_test.c` uses both a 10 s timeout
(`poll(&dp, 1, 10000)`) and a documented `-1` wait form. A stall in which every core sits inside
`poll` and the console never resumes is a **liveness** question about `sys_poll`'s park/wake path, not
about resume frames:

* instrument `SYS_POLL` entry and every exit path (return value, `nfds`, `timeout_ms`, and whether the
  park rewind at `process.c:3085` ran), capped and locked, one line per call;
* re-run intel T3 until a stall reproduces (1 in 3 at this revision) and read the last polls before the
  silence: infinite timeouts on a pipe whose writer is gone, or timeouts that never fire;
* the discriminating prediction: if the parked polls all carry `timeout_ms = -1`, the defect is a
  missed wake on the pipe/socket close path — GB-086/GB-051's poll family — and the resume-frame line
  this card has chased for six passes is not where the stall lives.

## DoD status after this pass — unchanged

* (a) `halted=1` on 3 consecutive runs: **still NOT met** (2 of 3 at `41ec708`).
* (b) delivered, with run 2's number caveated (above).
* (c) still partly met: SEDTEST → GB-093, TSORTTEST → GB-095; intel CMPTEST, SIGTEST, TACTEST uncarded.

The card stays `In Progress`; I am not moving it and not posting a verdict. No boundary marker — the
ticket is still open, and a marker is only written when one is finished.
