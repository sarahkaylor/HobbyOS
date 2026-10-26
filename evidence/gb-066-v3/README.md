# GB-066-V2 (re-gate 2) — gate evidence (VERDICT: RED, wall 1 fixed, wall 2 measured)

Ticket: GB-066-V2 (#195), re-opened 16:07Z for PR **#200**
PR #200 head **`fbd3cf3`** — `Makefile`, +10/−1: build the probe as `-static-pie -fPIE` and assert `Type: DYN`.
Base: `origin/main` **`b6c34fd`**. Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2).
Local only — no CI cited.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | leg B runs inside HobbyOS-intel, guest rows quoted with a count | **FAIL** — arrives and runs, **17 of 41 rows**; `row count A=41 B=17` |
| 2 | diff reports a count; 0 rows must fail | **PASS** — counts printed, 0-row leg A refused (round 1, unchanged harness) |
| 3 | a wrong probe makes the harness FAIL | **PASS** — `--self-test fail_count=0` (re-run this round) |
| 4 | absent probe reported absent | **PASS** — round 1, unchanged harness |
| 5 | ARM runs, or stated | **STATED** — not run; the loader/dispatch path is shared |
| 6 | nothing else regresses | **PASS** — `make host_tests` exit 0 |

## What PR #200 actually fixed — verified, and it matters

Wall 1 from the 15:43Z RED is gone:
```
$ make ARCH=intel obj/intel/t4_legA.out          # PR #200 Makefile
  Type:                              DYN (Position-Independent Executable file)
  Entry point address:               0x1cc1
41                                               # leg A rows on the host
```
and in the guest the probe is now **executed**, not refused:
```
[WAVE-MANIFEST] T4PROBE.BIN
[ARRIVED] T4PROBE.BIN rc>0
t4-probe
… 17 row= lines …
t4-probe-arrived
probe-exit
```
No more `elf loader: ET_EXEC linked below the user window`. The build rule is correct, the ET_DYN assertion
means a regression to `-static` fails the build loudly, and `leg A` still captures 41 rows. That half of the
re-open is real.

## Clause 1 still FAILS — 17 rows is the failure state, not the fix

The re-open message cites `t4-probe-arrived -> 17 row= lines` as the evidence that the wall is gone. It is
evidence that the probe *runs*; it is also the exact number the gate fails on:
```
$ T4_SERIAL=obj/t4_legB_pr200.log T4_LEGA_OUT=obj/intel/t4_legA.out bash tools/gate/t4_device.sh
t4_device: probe ARRIVED in guest (t4-probe-arrived present).
t4_device: row count A=41 B=17
t4_device: FAIL — row count differs (A=41 B=17)
```
The probe does not crash: it reaches its last emission in source order (`procfd_read`, `t4_probe.c:272`) and
prints `probe-exit` (`t4_probe.c:276`). The 24 missing rows are the **conditional** ones — the probe emits
them only when the call in front of them succeeds, and in-guest those conditions do not hold:

```
dent_have_alpha dent_have_beta dent_have_charlie dent_total
null_open_r null_open_w null_read1 null_write3
pipe2
poll_badfd poll_badfd_revents poll_ready poll_ready_revents
pselect6_ready pselect6_ready_fdset
select_ready select_ready_fdset
sockpair sockpoll_ready sockpoll_revents sockpselect_fdset sockpselect_ready sockselect_fdset sockselect_ready
```
No row appears in B that is absent from A — the guest is a strict subset, so this is not probe drift; it is
24 rows of the ABI surface where the guest's behaviour differs from Linux (open of `/dev/null`, `pipe2`,
`poll`/`select`/`pselect6` and their socket variants, and the `getdents64` name checks).

That is the T4 differential doing its job: the instrument now runs in the guest and it has produced its
first real finding. It is not a GREEN, and the DoD's clause 1 ("leg B runs and the diff passes") is not met
by "it dispatches now".

## Verdict

**RED** — but a materially better RED than 15:43Z. PR #200 closes the build/ELF-class wall, proved by
construction (ET_DYN, entry 0x1cc1, probe executed in-guest, `probe-exit` reached). Clause 1 now fails for
the reason the card exists: 24 of 41 rows do not appear in the guest, and the harness says so with counts
rather than a bare PASS.

Next wall, named for whoever picks this up: the `null_*`, `pipe2`, `poll/select/pselect6` and `sock*` rows
— those are the calls whose in-guest failure makes the conditional rows vanish. Either the guest grows
those syscalls or the probe's expectation set is wrong for M1; the differential cannot tell you which, but
it has now narrowed it to 24 names.