# GB-007-V — independent cross-check (DEEP Verifier, 2026-10-23)

Card #132 was claimed by two verifier sessions in the same second (19:08:55Z). The peer posted GREEN at
19:15:33Z and moved lanes; this directory is my independent run at `origin/main` = `5f6e3c3`, kept so the
record does not rest on a single execution. **No lane was moved by this session.**

| file | what |
|---|---|
| `01-healthy.log` | `make t4_compat_parity` → exit 0, `A=0 B=0 cases=3` |
| `02-legA-skip.log` | PATH farm (2248 links, `musl-*` excluded) → SKIPPED + exit 1, leg B built |
| `03-selftest.log` | `t4_parity.sh --self-test` → argv ignored, healthy path, exit 0 (**no self-test exists**) |
| `03b-diff-harness.log` | `make test_diff_harness` → 10 checks, 0 failed, 3 planted divergences → DIVERGE |
| `04a-compat-lie.log` | `ho_open` returns `open(...)+1000` → still **PASS** (compat.c is not on leg B's path) |
| `04b-legA-lie.log` | leg A linked with a lying `close()` → **FAIL**, `stdout differs` |
| `pa.out`/`pb.out` | the two legs' stdout: 79 lines each, byte-identical |
| scripts | flip script, PATH-farm builder, the lying musl wrapper and its C file |

## Numbers

* 79 stdout rows compared; `cases=3` = 2 captures + 1 compare (`N` incremented at `t4_parity.sh:80,95`).
* `A`/`B` = leg exit statuses (`A=$(cat "$TMP/runs/legA_musl.rc")`), not rows.
* skip path: `SKIPPED (a required leg did not run: A=skip B=/…/t4_probe.host) — FAILING the tier per GB-066`, exit 1.
* `ho_getrandom` does not exist in `src/host/compat.c`; the probe includes no `libc.h` and there is no
  `src/user_include/unistd.h`, so nothing maps `open`→`ho_open` for leg B. Flipping `ho_open` cannot move a row.

## Conclusion

Concur with the peer GREEN, on the same caveats: the harness fails on demand (leg lie → FAIL) and fails
closed when a leg is absent, but the surface it grades is **musl-vs-glibc on one source**, not the `ho_*`
compat model its header claims, and it has no self-test of its own. Both are mint-worthy, neither blocks #12
at Merge Gate.
