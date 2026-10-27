# GB-111-V (#215) — VERDICT: **GREEN**

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-27 00:5xZ.
Verifies GB-111 (#214) / ship **PR #213 @ `d245ead`**. Host-side T1 as the card specifies (leg B is not
re-run here); no CI consulted. I did not author GB-111 or PR #213.

**Tree that would merge: `d245eadf140b0a1f88c57354e40b815e8ee21275`** (PR #213 head at gate time).

## One line

Both blocks are genuinely guarded, and the guard was proved on a **real failure path** I forced myself
(`ulimit -n 5` → socketpair and pipe2 both fail EMFILE): 14 `SKIP reason=…` rows, no write/poll/select on
an uninitialised fd, process exit 0. On success the rows are unchanged (41 rows, 0 SKIP, byte-identical
across runs). Removing the guard — in a variant that still compiles — makes their check **FAIL (2)** and
makes the probe emit garbage "ready" rows on uninitialised fds, i.e. the defect the card is about.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | guard test PASS, incl. socketpair `if (spc == 0)`, pipe `if (prc == 0)`, SKIP+reason, guard-removal detectable | **PASS** |
| 2 | forced failure → SKIP rows with a reason, never fd use | **PASS** — my own `ulimit` run, 14 SKIP rows |
| 3 | success byte-identical; leg A full row count (41) | **PASS** |
| 4 | removing the guard makes the check fail | **PASS** — `T4_PROBE_GUARD_TEST: FAIL (2)`, rc=1 |

## Clause 1/3 — the suite and the success path (`01-gate.log`, `05-break-clean.log`)

```
  PASS: socketpair block guarded on success (if (spc == 0))
  PASS: pipe block guarded on success (if (prc == 0))
  PASS: socketpair failure -> SKIP with reason
  PASS: pipe failure -> SKIP with reason
  PASS: write(sv[1]) occurs inside the socketpair guard
  PASS: write(pfd[1]) occurs inside the pipe guard
  PASS: host leg A: socketpair succeeds (real rows, byte-identical)
  PASS: host leg A: no SKIP rows on success
  PASS: guard-removed variant is structurally detectable
T4_PROBE_GUARD_TEST: PASS                                             rc=0

$ cc -Wall src/user/t4_probe.c && ./probe
row=pipe2 rc=0 errno=0
row=sockpair rc=0 errno=0
rows=41  skips=0  (identical to the earlier healthy run)
```

## Clause 2 — the real failure path, forced rather than simulated (`03-forced-failure.log`)

```
$ ( ulimit -n 5; ./t4_probe ) > rows.txt      # one free fd: socketpair and pipe2 both EMFILE
run rc=0
row=pipe2 rc=-1 errno=9
row=pipe_write rc=-1 errno=9 SKIP reason=no-pipe
row=sockpair rc=-1 errno=9
row=sock_write rc=-1 errno=9 SKIP reason=no-socketpair
row=sockpoll_ready rc=-1 errno=9 SKIP reason=no-socketpair
row=sockpoll_revents rc=-1 errno=9 SKIP reason=no-socketpair
row=sockselect_ready rc=-1 errno=9 SKIP reason=no-socketpair
row=sockselect_fdset rc=-1 errno=9 SKIP reason=no-socketpair
row=sockpselect_ready rc=-1 errno=9 SKIP reason=no-socketpair
row=sockpselect_fdset rc=-1 errno=9 SKIP reason=no-socketpair
… 14 SKIP rows in total
```
No `EBADF`, no `write`/`poll`/`select` on the uninitialised `sv[]`, and the probe still exits 0 — the block
is reported, not silently omitted. This is the DoD's substance measured on a real failure, not on a mock.

## Clause 4 — the can-fail, cleanly (`05-break-clean.log`)

Break: `if (spc == 0) {` → `if (spc == 0 || 1) {` (compiles; the guard no longer guards):
```
  FAIL: socketpair block guarded on success (if (spc == 0))
  FAIL: write(sv[1]) occurs inside the socketpair guard
T4_PROBE_GUARD_TEST: FAIL (2)          rc=1
```
and the broken probe, on the same forced-failure path, emits fake readiness:
```
row=sockpair rc=-1 errno=9
row=sockpoll_ready rc=1 errno=0        ← "ready" on an uninitialised fd
row=sockselect_ready rc=1 errno=0
row=sockpselect_ready rc=1 errno=0
```
Restored: `T4_PROBE_GUARD_TEST: PASS`, `git status --short` empty, rows byte-identical to the earlier run.
(A first attempt of mine removed the `if` outright and hit a compile error from the orphaned `else`;
I redid it as the compiling variant above so the detection is not just "the file stopped building".)

## Observation (not a verdict cell)

`t4_probe_guard_test.py` is a **static** check — it matches the source text (`if (spc == 0)`,
`write(sv[1])` inside the guard). It caught both of my breaks because both altered that text, but a guard
that reads correctly and still uses the fd (say, a write placed before the check) would pass the static
match. The behavioural proof is the `ulimit` run above; if you want it permanent, a small case that
compiles and runs the probe under `RLIMIT_NOFILE=5` and asserts the SKIP rows would make the suite carry
both halves.

## Reproduce

```
git worktree add /tmp/gb111v d245ead && cd /tmp/gb111v
python3 tools/t3/t4_probe_guard_test.py                       # PASS
cc -Wall src/user/t4_probe.c -o /tmp/p && /tmp/p | grep -c '^row='     # 41, 0 SKIP
( ulimit -n 5; /tmp/p ) | grep -c 'SKIP reason='              # 14 — the real failure path
# can-fail: "if (spc == 0 || 1)" -> T4_PROBE_GUARD_TEST: FAIL (2)
```