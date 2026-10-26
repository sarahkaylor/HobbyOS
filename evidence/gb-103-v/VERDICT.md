# GB-103-V — VERDICT: **RED** (clauses 1–2 pass; clause 3's named instrument-failure status cannot fire)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-26 11:49–12:2xZ.
Verifies GB-103 (#189) / ship **PR #194 @ `92e999f`** (`tools/t3_tier.sh` probe_guest isolation +
`tools/t3/t3_monitor_grading_test.sh`). Host-side + ARM T3 on this VM. No CI consulted. I did not
author GB-103 or PR #194.

## One line

The tier no longer aborts inside `probe_guest()` and its grading is untouched — **but the quoted
`INSTRUMENT-FAILURE` line the DoD requires on a mid-sample monitor death cannot be produced**: the
subshell that takes the sample ends with a successful `echo`, so the `st != 0` annotation is
unreachable, and a monitor killed during the sample leaves a silently truncated sample with no named
status. I measured exactly that.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | grading untouched: clean log PASS/0, injected FAIL FAIL/1 | **PASS** |
| 2 | 5/5 monitored ARM runs each produce a verdict, none aborts (esp. not 141) | **PASS** — 5/5, `T3_arm EXIT=1` each |
| 3 | mid-sample monitor close ⇒ the quoted `INSTRUMENT-FAILURE …` line, tier still grades | **FAIL** — the line is unreachable; measured absent with the monitor killed mid-sample |

## Clause 1 — grading untouched (`01-grading-test.log`)

```
== police on CLEAN log ==
T3_intel (police /tmp/t3_clean.log): 1151 lines, 0 FAIL lines, … halted=1, kfault=0, boots=0
T3_intel: PASS (0 FAIL lines, halted)
T3_intel EXIT=0
clean-exit=0
== police on INJECTED-FAIL log ==
T3_intel: FAIL — 1 FAIL line(s); first 5:
1152:SOMETEST: FAIL injected failure
T3_intel EXIT=1
fail-exit=1
```

## Clause 2 — 5/5 monitored ARM runs produce a verdict (`02-five-arm-monitored.log`)

```
run 1 exit=1  T3_arm: 9068 console lines, 1 FAIL lines, 5 user memory fault records across 3 processes, 84 programs exited, halted=1   T3_arm EXIT=1
run 2 exit=1  T3_arm: 9099 console lines, 1 FAIL lines, 5 user memory fault records across 3 processes, 84 programs exited, halted=1   T3_arm EXIT=1
run 3 exit=1  T3_arm: 9394 console lines, 1 FAIL lines, 5 user memory fault records across 4 processes, 84 programs exited, halted=1   T3_arm EXIT=1
run 4 exit=1  T3_arm: 9346 console lines, 4 FAIL lines, 5 user memory fault records across 3 processes, 85 programs exited, halted=1   T3_arm EXIT=1
run 5 exit=1  T3_arm: 9022 console lines, 1 FAIL lines, 5 user memory fault records across 3 processes, 84 programs exited, halted=1   T3_arm EXIT=1
```
Five verdicts, five halts, no exit 141, no death inside `probe_guest()`. (The tiers are red on their own
FAIL lines — expected, and not what this card grades.)

## Clause 3 — the failing cell, measured

Control: an ARM run with `T3_MONITOR=1 T3_TIMEOUT=150`, and QEMU SIGTERM'd **1 s after the sample header
appeared** (killed at t=123.6 s by a driver that polls the log every 200 ms), i.e. the monitor is dead for
essentially the entire sample:

```
SIGTERM -> 138050 at 123.6 s
tier rc = 1  killed: True
sample header present: True
'end guest sample' present: True
INSTRUMENT-FAILURE present: False
verdict line present: True
```
(`04-negative-control2.log`; sample body `05-sample-body.log`, serial `neg2-sample.log`)

So the tier produced its verdict (that half of GB-103 works) and the sample ran to its end **with no named
instrument-failure status**, exactly as the DoD forbids.

**Why, from the code — and this is the defect:** the sample is taken in a subshell whose last command is
an `echo`:

```sh
  (
    set +e
    set +o pipefail
    …
    echo "=== end guest sample ==="
  ) >>"$LOG" 2>&1
  local st=$?
  if [ "$st" != "0" ]; then
    … INSTRUMENT-FAILURE …
```
`st` is the exit status of that final `echo`, which is 0 whenever the subshell runs to completion — and a
dying monitor does not stop it running to completion (every read is `|| true` or grep-swallowed). The only
way to reach the annotation is for the subshell itself to be killed, which is not the condition the DoD
describes. An instrument that cannot fire is not an instrument; this is the same class the card set out to
remove, one layer down.

## One-line fix (not applied)

Capture the status of a command that can fail, e.g. move the sentinel before the capture or record a flag
inside the subshell:

```sh
  (
    set +e
    … reads …
    [ -n "$SAMPLED_OK" ] || exit 3     # or: track socat failures into a variable
    echo "=== end guest sample ==="
  ) …
```
or simply `rc=0; … || rc=1` around each monitor read and `exit $rc` as the subshell's last statement. Then
kill the monitor mid-sample: the quoted line appears and the tier still grades.

## Also found (card hygiene, not a verdict cell)

The gate command the card names does not exist on this tree:
```
$ ls tools/gate/t3_tier.sh
ls: cannot access 'tools/gate/t3_tier.sh': No such file or directory
$ ls tools/t3_tier.sh
tools/t3_tier.sh
```
Running the card's literal command five times gave `bash: tools/gate/t3_tier.sh: No such file or directory`
(exit 127, five times) before I corrected the path to `tools/t3_tier.sh` — which is what the parent DoD's
"5/5 monitored runs" would also have hit. Either the card or the tree should be fixed so a verifier
following the card literally runs the gate.

## Reproduce

```
git worktree add /tmp/gb103v 92e999f && cd /tmp/gb103v && mkdir -p obj
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
bash tools/t3/t3_monitor_grading_test.sh
for i in 1 2 3 4 5; do T3_MONITOR=1 T3_LOG=$PWD/m$i.log T3_TIMEOUT=1200 bash tools/t3_tier.sh arm; echo "run $i exit=$?"; done
# negative control: start a monitored run with T3_TIMEOUT=150, SIGTERM QEMU when the sample header
# appears, then: grep -c INSTRUMENT-FAILURE <log>   -> 0   (the defect)
```