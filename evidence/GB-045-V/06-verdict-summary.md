# GB-045-V — gate run manifest

- Verifier: DEEP (DeepSeek v4.1 Flash, dgx1), worktree `hermes-b3ca2037`, host **swarm-farm-2**
- Date: 2026-10-20 (UTC)
- Revision run: **98247dd9eccbf9ec434a8841abb1b3b267dea5f5** — `HEAD` == `origin/main` (fetched), tree clean
- Env: `dist-staging` copied from this VM's main checkout before the builds

## Gate

```
$ make obj/gfxbench.bin
make: *** No rule to make target 'obj/gfxbench.bin'.  Stop.          # OBJ_DIR = obj/$(ARCH)
$ make ARCH=arm   obj/arm/gfxbench.bin     -> exit 0, 63456 bytes
$ make ARCH=intel obj/intel/gfxbench.bin   -> exit 0, 60328 bytes
```

`GFXBENCH.BIN` is loaded by the kernel boot wave (`src/kernel/main.c:246`), so a normal tier boot measures it.

### ARM (numbers emitted)

```
GFXBENCH full min=0 med=1000 mean=800 n=10
  RTTI_T downcastGFXBENCH r256 min=0 med=0 mean=200 n=30
GFXBENCH r64 min=0 med=0 mean=280 n=50
GFXBENCH DONE
```

Two defects visible in those three lines: every `min=` is **0** (the timer never resolves — a metric whose
best case is 0 µs cannot discriminate a regression), and the r256 row is **spliced** with another
process's `RTTI_T downcast` output, the console-interleave hazard this ticket's own README warns about.
The arm wave was stopped at ~10 min while `STRESS.BIN` was still running; the tier's own verdict is not
this ticket's DoD, and the gfxbench rows had already been emitted.

### INTEL (no numbers at all)

```
379:Loading program for scheduler (v2 AS): GFXBENCH.BIN
603:[KERNEL] pid=52 (GFXBENCH.BIN) memory fault VA=0x000000007559DBC8 PC=0x000000100000A098 rw=w  in=PROT region=NONE cr3frame=0x0000000020DB0000 asroot=0x0000000020DB0000 -> killed
604:[KERNEL] Process 52: GFXBENCH.BIN exited unexpectedly or gracefully.

count of 'GFXBENCH <phase>' rows on intel: 0
```

The instrument crashes before printing a single row on x64 (the family-B fault shape: `VA=…DBC8`,
`in=PROT region=NONE`). The intel tier also reports `4 FAIL lines / GRAPHICS TESTS FAILED`, EXIT=1.

## The report and its instruments are not on this sha

```
$ ls perf-baseline-REPORT.md            -> No such file or directory
$ git ls-files | grep -i perf-baseline  -> (nothing)
$ ls tools/t5/boot_time.py tools/t5/perf_report.py
  -> both: No such file or directory
```

GB-045's own evidence cites `tools/t5/boot_time.py` (12.70 s mean over 13.03/12.53/12.53), `tools/t5/perf_report.py`
and `perf-baseline-REPORT.md` — all three live only in **PR #30**, which is still open and **dirty**
(`head 51e033a`-era branch, `mergeable_state=dirty` per the board's 2026-10-19/20 notes). DoD 3 is
explicit: a number that exists only in the report and cannot be re-measured ⇒ RED "instrument absent".

## DoD scorecard

| clause | result |
|---|---|
| 1 builds and boots on arm **and** intel | builds both; **intel emits zero rows** (memory fault, killed) |
| 2 headline numbers reproduced within the stated tolerance | **impossible** — boot-time mean 12.70 s has no on-main instrument; `min=0` on arm makes the flush numbers meaningless; no framerate row exists anywhere (`gfxbench` measures flush µs per phase, not fps) |
| 3 a report-only number ⇒ RED "instrument absent" | **fires** — report + `boot_time.py` + `perf_report.py` absent from `main` |
| 4 name the sha / PR #30 dirty | **98247dd**; PR #30 dirty, so the report content is not on main |

## Verdict

**RED.** GB-045 (#45) → In Progress.
