# host_tests: why the python suite skips two rows, and why it can now fail

*Written 2026-11-24 for main's red linux-ci (run 45261954209). Supersedes the
"known 38/40, two gaming.md reds" note in older lane docs.*

## The symptom, measured

main @ `9b9d946a`, linux-ci job **arm** (`138000746218`), step "ARM tier 1: host
golden tests (native, no QEMU)":

```
[host-tests] self-registering suite: 36 python tests
  FAIL  tools/board/sync_gaming_md_test.py  (FileNotFoundError: … 'gaming.md')
  FAIL  tools/t5/roadmap_doc_test.py        (FileNotFoundError: … 'gaming.md')
[host-tests] done: 40/42 passed
[host_tests] FAILED: python3 tools/t5/run_t5_tests.py (exit 2)
make: *** [Makefile:4880: host_tests] Error 1
```

`exit 2` is the suite's own exit status: it is the **failure count**, not an
interpreter error. Two python tests failed, the 482 C checks before them passed,
so the ARM job was red on the python scheduler alone. MEASURED (job log fetched
through the Actions API — `gh run view --log-failed` truncates the step).

## The cause

`gaming.md` — the plan doc two tests read — is not in this repo. It was deleted
with the decommissioned swarm:

```
c3f78668 docs: remove stale gaming.md plan (HobbyOS swarm decommissioned)
 gaming.md | 1283 -------------------------------
```

Only the doc was deleted; `tools/board/sync_gaming_md_test.py` and
`tools/t5/roadmap_doc_test.py` kept calling `open("gaming.md")` unguarded, so any
tree without the file died in `open()` before printing a verdict. A workstation
that still holds an untracked `gaming.md` (the Mac does) passes both; every clean
checkout — CI, this VM — fails. That asymmetry is the whole red.

Two aggravating factors, both fixed here:

- the suite printed only the **last** line of a failing script, so the log said
  `FileNotFoundError: …` with no file, line or stack — which is why the red
  survived a day of triage;
- `tools/t3/t3_instrument_failure_test.py` needed `socat`. Where it is absent the
  connector exits non-zero, the gate correctly reads that as INSTRUMENT-FAILURE,
  and the test reported a gate defect that was a missing binary (this VM: 3
  failures locally vs CI's 2).

## The convention now

A host test that cannot run part of itself **says so and exits 77**:

```
SKIP: gaming.md is not in this tree — the live-plan row needs it, and the plan
doc was removed with the decommissioned swarm (c3f78668)
```

`tools/t5/run_t5_tests.py` grades three ways:

| verdict | rule |
|---|---|
| `PASS` | exit 0 **and** the script printed its own verdict |
| `SKIP` | exit 77 **and** a `SKIP: <reason>` line — counted apart from passes, printed with its reason |
| `FAIL` | everything else, including exit 77 with no reason and exit 0 that printed only a `SKIP:` |

So a skip is never a pass (GB-007), never a silent hole, and never reddens a
clean tree. Every `FAIL` now prints the tail of that script's own output, so the
next traceback is in the log. The exit status stays the failure count (0 = green),
capped at 255 so a 256-failure run cannot wrap to green. `--self-test` proves all
of it every run: a `SKIP` is graded SKIP, `exit 77` with no reason is graded FAIL,
`exit 0` with a `SKIP:` marker is graded FAIL, and a script that raises is
graded FAIL with its traceback preserved.

The pure rows of both tests still run everywhere; only the row that needs the
live plan is skipped. On a box that has the plan, drop it at the repo root and
both tests run whole:

```bash
cp /path/to/gaming.md .          # untracked; not part of any commit
python3 tools/t5/run_t5_tests.py # 0 failed, 0 skipped
```

## Reproduce

```bash
git worktree add /tmp/main-9b9d946a 9b9d946a
cd /tmp/main-9b9d946a
make third_party/libcxx-21.1.8/.fetched-ok      # CI's prefetch step
TOOLS_SKIP=1 make host_tests                    # was: exit 2 at the python suite
python3 tools/t5/run_t5_tests.py                 # now: 0 failed, 2 skipped, exit 0
```

Proof that it can still fail: create `tools/t5/zz_probe_test.py` containing
`raise RuntimeError("probe")`, run the suite — `exit 1`, one `FAIL` row, and the
probe's full traceback in the output. Delete it afterwards.

## What this does not fix

linux-ci's **intel** job (`138000745587`) is red at a different step, "x64 kernel
unit tests (KVM)": `test_net_socket_fd_table` dies with `[BADVEC] … fatal
frame=… not-a-frame`. That is the socket table holding a physical address as a
pointer — PR 440's fix. A fully green linux-ci on main needs both; this change
owns the ARM job.
