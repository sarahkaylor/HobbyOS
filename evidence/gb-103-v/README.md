# GB-103-V — gate evidence (VERDICT: RED, clause 3 of 3 — reproduced on an unchanged head)

Ticket: GB-103-V (#190) — T3_MONITOR probe_guest must never abort; the INSTRUMENT-FAILURE status must fire
Verifies: GB-103 (#189), ship = PR **#194**, head **`92e999f`** (unchanged — see "why this is a re-gate
and not a re-report" below)
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2), host `swarm-farm-2`.
Local only — no CI/check result is cited.

## Why this card is a re-gate and not a repeat

`GB-103-V` was gated RED at 12:21Z by `wverify-deep` (farm-3) on this same head `92e999f`, then re-opened
to `Ready` "so the gate is re-run after the status is made reachable". I claimed it at 12:31Z and checked
first whether that precondition had been met:

```
$ git fetch origin refs/pull/194/head
$ git log -1 --format='%h %ci %s' pr/194
92e999f 2026-10-26 11:13:56 +0000 GB-103: probe_guest() must never abort the tier (T3_MONITOR=1 exit 141, no verdict)
$ git log --oneline 92e999f..pr/194          -> (empty)
$ git log --all --oneline --since="2026-10-26 12:00" --grep=GB-103  -> (empty)
```

The head had not moved, so nothing had been fixed. Rather than spend 5 ARM waves (~45 min) reproducing a
result measured ten minutes earlier on the identical tree, I re-measured the two clauses that are cheap
and decisive — clause 1 (grading, seconds) and clause 3 (the failing cell, one intel wave) — and I state
plainly that clause 2 is farm-3's measurement on this head, not a fresh one of mine.

## Clause table

| # | clause | result | measured by |
|---|---|---|---|
| 1 | grading untouched: clean PASS/0, injected FAIL/1 | **PASS** | me, this run |
| 2 | 5/5 monitored runs produce a verdict, none aborts (esp. not 141) | **PASS** | farm-3, same head `92e999f` — not re-run by me |
| 3 | mid-sample monitor close ⇒ quoted `INSTRUMENT-FAILURE …` line, tier still grades | **FAIL** | me, this run (and farm-3) |

## Clause 1 PASS — measured on the PR head

`tools/t3/t3_monitor_grading_test.sh` checked out from `pr/194` (it does not exist on `main`):

```
== police on CLEAN log ==
T3_intel: PASS (0 FAIL lines, halted)          T3_intel EXIT=0
== police on INJECTED-FAIL log ==
1152:SOMETEST: FAIL injected failure           T3_intel EXIT=1
GRADING_EXIT=0
```

## Clause 3 FAIL — my own negative control, QEMU alive at the sample

`midsample_probe.sh`: intel tier with `T3_MONITOR=1 T3_TIMEOUT=240`; the script waits for the sample
header to appear in the tier's own serial log, then SIGTERMs the live QEMU while the sample is running.

```
sample header seen in obj/t3_gb103v_sample_intel.log at 12:45:00
SIGTERM -> qemu pid 135742 at 12:45:00
tier rc = 1
qemu was alive at the sample: True
sample header present:      True
'end guest sample' present: True
INSTRUMENT-FAILURE present: False
verdict line present:       True
T3_intel: 10049 console lines, 20 FAIL lines, …, halted=0
```

The tier still grades (that half of the card works), the sample runs to its own `=== end guest sample ===`
even though the monitor died, and **no `INSTRUMENT-FAILURE` line is written**. This is the same cell
farm-3 measured with a different trigger (they killed 1 s after the header on ARM; I killed at the header
on intel with the guest wedged) — two different paths, same silence.

The code reason, quoted from the head:

```sh
      echo "=== end guest sample ==="
  ) >>"$LOG" 2>&1
  local st=$?
  if [ "$st" != "0" ]; then
      { echo ""; echo "=== T3 guest sample: INSTRUMENT-FAILURE — … (rc=$st; $why) …"; } >>"$LOG"
  fi
  return 0   # probe failure must NEVER abort the tier
```

`st` is the status of the subshell's **last command**, an `echo`. Every monitor read is already
`|| true`/grep-swallowed, so the subshell always completes with 0 and the annotation is unreachable
unless the subshell itself is killed — which is not the condition the DoD names. An instrument that
cannot fire is not an instrument.

## Card hygiene (not a verdict cell)

The card's clause-2 command is `bash tools/gate/t3_tier.sh arm`; that path does not exist — the tier is
`tools/t3_tier.sh`. Confirmed again here (`tools/gate/` has no `t3_tier.sh`). Whoever re-mints should fix
the path, and also say explicitly whether clause 2 must be re-run on the new head (it must, once the head
moves).

## Verdict

**RED** — same failing cell as the 12:21Z verdict, reproduced independently on the unchanged head with a
different trigger. Clause 1 and clause 2 stand; clause 3 needs the one-line status capture the previous
verdict already specified (set `rc=0` inside the subshell, `rc=1` on any monitor read failure, and make
`exit $rc` the subshell's last statement).

This card should be **Done** rather than looped back to `Ready`: re-opening it while the head is static
produced exactly one wasted claim (mine). A fresh `-V3` minted after `pr/194` moves past `92e999f` is the
cheaper shape.