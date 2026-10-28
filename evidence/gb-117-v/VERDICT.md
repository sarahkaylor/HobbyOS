# GB-117-V — verdict: RED (the reported clock is not the DoD's clock; the 42 reproduces only under the unstated rule)

Subject: PR **#228**, head **`86c9dce`** (`tools/board/claim_age.py`, `tools/board/claim_age_test.py`),
graded in a fresh detached worktree of that head. Host-side only: no QEMU, no CI consulted.
Verifier: DEEP Verifier `wgames-verify-gb117v`. I authored neither GB-117 nor the tool.

## Clause matrix

| # | Clause | Result |
|---|---|---|
| 1 | Per open card, the **newest comment** age in the five buckets + oldest N with ids | **RED** — the clock is "newest comment whose body does not contain the substring `Coordinator`", not the newest comment. Measured effect: 5 stale/fresh verdicts flip, 3 cards change bucket, `over24h` 39→42 |
| 2 | `make host_tests` runs `claim_age_test.py` and exits 0 | **PASS** — `PASS tools/board/claim_age_test.py`, 482 checks / 0 failed, `TEST EXIT: 0` (see note: the card's literal token `HOST_TESTS_EXIT=0` does not exist in this tree) |
| 3 | decision/`blocked` cards printed separately | **PASS** — 9 live cards in their own section |
| 4 | Live run reproduces the 42/28 split and the stale/fresh split | **PASS on the number, and that is the problem** — `over24h=42` exactly, but only under the unstated clock; under the DoD's own wording it is **39** |
| 5 | The sweep falsifier operates and is quoted | **PASS** — fixture b1 (fresh `updated_at` 10 h + 48 h comment → STALE) and the live counter `cards STALE despite fresh updated_at (<24h): 45` |

`updated_at` is genuinely excluded from the clock everywhere but the informational column
(`claim_age.py:69`, `:128`) — the central claim of the card holds, and the fixture proves it.

## 1 — the clock measured, not argued

`newest_comment_age()` filters on **body text**:

```
human = [c for c in comments if "Coordinator" not in (c.get("body") or "")]
```

That is not authorship — it is a substring match on the comment body. Two consequences, both measured
(`04-exclusion-probe.txt`, `05-two-clocks.txt`, GETs only against the live board, 60 open cards):

**(a) It flips verdicts.** 19 cards have a newest comment containing the word; **5 of them change
STALE/FRESH**:

```
FLIP #179 newest=15.8h -> excluded-clock=37.4h
FLIP #178 newest=10.9h -> excluded-clock=42.6h
FLIP #174 newest=1.6h  -> excluded-clock=38.8h
FLIP #77  newest=1.6h  -> excluded-clock=139.7h
FLIP #33  newest=6.7h  -> excluded-clock=63.6h
```

**(b) It also drops *worker* comments that merely mention the word.** #142's newest comment is an
`EVIDENCE — GB-089 null-entry control built + verified both arches …` post; it is excluded because the
word appears somewhere in its body (clock 90.9 h → 91.4 h, bucket unchanged here, but the mechanism is
wrong: a worker quoting a coordinator instruction loses their own comment).

**(c) The DoD's own number depends on the rule.** Same board, two clocks:

```
literal  buckets: under6h=10 6-12h=6 12-24h=4 over24h=39 nocomments=1  -> split=40/20
shipped  buckets: under6h=8  6-12h=3 12-24h=4 over24h=42 nocomments=3  -> split=45/15
deviation moves over24h by +3 cards and stale by +5
```

So clause 4's `over24h=42` is reproduced **by the tool as shipped** and **not** by the definition the
DoD states. A verifier who quoted "42" as clause-1 evidence would be certifying a number the card's own
words do not produce. That is the RED.

## 2 — the gate ran, inside `make host_tests`

```
  PASS  tools/board/claim_age_test.py
=== Test Results ===
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
RC=0
```
Discovery is real: `tools/t5/run_t5_tests.py:40` globs `tools/*/*_test.py`, so the file is in the gate
with no Makefile edit. **Disclosure:** my first `make host_tests` in the fresh worktree failed to *build*
(`fatal error: 'typeinfo' file not found`, `RC=2`, `02-host-tests.log`) because this worktree lacked the
vendored libc++ tree; after symlinking `third_party/libcxx-21.1.8/src` from the primary checkout the run
above is `02b-host-tests.log`. The failure was my environment, not the PR — but it is in the evidence
rather than deleted.
**Also:** the clause's named token `HOST_TESTS_EXIT=0` does not exist anywhere in this tree
(`grep -rn HOST_TESTS_EXIT` → one stale markdown report). The suite's own line is `TEST EXIT: 0`; I
graded the substance (test executed, gate exited 0) and am flagging the token so the next card does not
inherit it.

## 3, 5 — sections and the falsifier

```
decision/blocked cards (9):
  #91 Backlog comment 3.3h fresh ...
  #105 Done comment 92.1h STALE ...
cards STALE despite fresh updated_at (<24h): 45 (a bulk sweep cannot re-arm them)
```
Self-test:
```
  PASS: b1 sweep-proof stale  b2 bucket map  b3 decision/blocked sep -> split=4/1
CLAIM_AGE_TEST: ALL PASSED
```
Fixture b1 (`claim_age.py:207-210`): card #1, `updated_at` 10 h old, single comment 48 h old, asserted
STALE — the seeded-sweep falsifier the card asked for, and it fails if `updated_at` ever enters the clock.

## What would make this GREEN — either is acceptable, the choice is the Coordinator's

1. **Amend the definition** (one line, no code change): GB-117's DoD says "the newest **non-coordinator**
   comment", and §4.2 records that a coordinator post does not re-arm the stall clock. The tool already
   prints that rule in its own header; the DoD is what lags. Then `42` is correct by definition and I
   re-grade against the amended text immediately.
2. **Fix the filter** to be authorship-based rather than body-substring (`c["user"]["login"]` or an
   explicit "Coordinator" tag), keep the same DoD sentence, and show the numbers again.

What must not happen is GREEN under the present text: the report would be certified as "newest comment"
while five of its cells are decided by a rule the card does not state — the exact defect class this
program keeps paying for.

## Reproduce

```
git worktree add /tmp/w117 --detach 86c9dce && cd /tmp/w117
ln -s $PRIMARY/third_party/libcxx-21.1.8/src third_party/libcxx-21.1.8/src   # host_tests C++ headers
python3 tools/board/claim_age.py --self-test      # CLAIM_AGE_TEST: ALL PASSED
make host_tests                                   # PASS tools/board/claim_age_test.py, TEST EXIT: 0
python3 tools/board/claim_age.py                  # over24h=42 split=45/15 (shipped clock)
python3 evidence/gb-117-v/probe_two_clocks.py     # 39 vs 42 on the same board
```