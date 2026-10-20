# GB-070-V — verification evidence (DEEP Verifier, 2026-10-20)

Subject: GB-070 (#79) "W0-corpus gate names the absent members" — verdict **RED**.
Revision run: `0d381008f3c3626a5cd2d0a335a129ab3b13f78f` (`refs/heads/main` on GitHub, `git ls-remote origin main`), fresh worktree, clean tree.

| # | file | what it is |
|---|---|---|
| 01 | `01-t4_w0_selftest.log` | `bash tools/gate/t4_w0.sh --self-test` → 3 cases proven, `self-test result: fails=0`, exit 0 |
| 02 | `02-t3_intel_run.log` | `T3_QEMU_ARGS=-enable-kvm bash tools/t3_tier.sh intel` — a REAL intel wave on this VM (no borrowed log), 779 console lines |
| 03 | `03-t4_w0_real_intel_wave.log` | `bash tools/gate/t4_w0.sh --serial obj/t3_intel.log --members 'HELLO SH CAT LS'` → exit 1, names all four |
| 04 | `04-t3_intel_summary.txt` | the tier's own verdict line for that wave |
| 05 | `05-wave-facts.txt` | wave counters + the `HELLO.BIN exited` line the gate contradicts |
| 06 | `06-wave-fail-lines.txt` | the wave's 4 FAIL lines (graphics — GB-065 device-tier, unrelated to this gate) |
| 07 | `07-wiring.txt` | `make -n host_tests` wiring + "no producer of `[W0-ARRIVED]`" proof |

Environment prerequisites used (documented §11.5 one-time staging, no tree changes):
`third_party/libcxx-21.1.8` + `third_party/llvm-21.1.8/dist-staging` rsynced from the primary tree;
`/usr/sbin` and `/sbin` added to `PATH` so `mkfs.fat` is found by the `disk.img` recipe (it is installed at
`/sbin/mkfs.fat`, just not on the non-login PATH).

## The two findings

**A. GB-070's DoD is unmet: the W0 corpus does not run.** The real wave shows `63 programs exited`
and **0** `[W0-ARRIVED]` lines; SH, CAT and LS never appear in the exited set at all. So the gate's
RED is correct in substance.

**B. The instrument is wrong about HELLO, and its PASS path is unreachable on a real wave.**
The gate accepts exactly one proof of arrival, `[W0-ARRIVED] <member>` (`tools/gate/t4_w0.sh:113`),
and **nothing in the tree prints that string** — `grep -rln W0-ARRIVED src tests tools` returns only the
gate itself. Meanwhile the wave log at line 331 shows:

```
[KERNEL] Process 31: HELLO.BIN exited unexpectedly or gracefully.
```

so the gate's line `FAIL HELLO — not in the intel wave's exited-programs/arrival set (W0 corpus member never ran)`
is false about HELLO. The `--serial` path never looks at the exited-programs set its message names.

## Wiring (DoD 3)

`make -n host_tests | grep t4_w0` → `bash tools/gate/t4_w0.sh --self-test` only. The real-wave gate
(`t4_w0_gate` → `t4_w0.sh --serial $(T3_LOG)`) is a standalone target, not wired into `host_tests` or
the merge gate, so the suite gains the negative self-test but no wave check.
