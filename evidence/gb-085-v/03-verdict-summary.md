# GB-085-V — gate run at cda3a4d

Worktree `hermes-9642df51`, branch `verify-gb085v` at **cda3a4d** (= `origin/ticket/GB-085-tmp-scope`,
the PR head; not an ancestor of `origin/main`), swarm-farm-2.

## Clause 1 — two simultaneous `make host_tests` runs: FAIL

```
$ python3 tools/host/host_concurrency_gate.py
[gb085] building host_tests once (serial)...
[gb085] serial build green.
[gb085] starting two make host_tests simultaneously...
FAIL: concurrent runs not both green (A rc=2 ok=False ; B rc=0 ok=True)
---- A tail ----
  PASS  tools/t5/selfreg_test.py
[host-tests] done: 20/21 passed
  FAIL  tools/t5/replay_test.py  (FileNotFoundError: [Errno 2] No such file or directory:
        '…/tools/t5/replays/_bad_tmp.jsonl')
make: *** [Makefile:3623: host_tests] Error 1
EXIT=1
```

Leg B is green, leg A is not. The culprit is a **shared path inside the source tree**, which the fix did
not scope — `tools/t5/replay_test.py:35` writes its negative-control fixture next to the replays and
unlinks it at line 45:

```
bad = os.path.join(rep_dir, "_bad_tmp.jsonl")   # rep_dir = tools/t5/replays/
open(bad, "w").write(...)
…
os.remove(bad)
```

Two concurrent runs therefore race: one `os.remove`s the file while the other is still parsing it, and the
loser reports `FileNotFoundError`. That is the same defect class the card exists to close — a fixed path
shared between runs — just in the repo tree instead of `/tmp`.

## Clause 2 — no fixed `/tmp/[A-Z]` path in `src/host/*_test.c`: PASS

```
$ grep -rn '"/tmp/[A-Z]' src/host/*_test.c
(no matches, exit 1)
```

The C-side fixtures really were scoped to per-process dirs. The survivor is on the Python side.

## Clause 3 — negative control: not reached

`host_concurrency_gate.py` aborts at its first assertion, so the `HO_TEST_SHARED_DIR` negative was never
exercised in this run. (The script's own header says it asserts both; I could not observe the second
because the first failed.)

## Verdict

**RED.** The positive control fails on the shipped PR head with a reproducible cause one file away from
the fix — `tools/t5/replay_test.py:35` — so "two concurrent runs both green" is not true yet. Clause 2
passes, which localises the remaining work to the Python tests.

The fix is small: give `replay_test.py` a per-pid fixture name (or a `tempfile.mkdtemp()` dir) the way
`ho_scratch.h` does for the C tests.
