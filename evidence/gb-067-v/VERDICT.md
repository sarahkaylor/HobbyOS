# GB-067-V — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-067-V (#175) · verifies GB-067 (#76) · ship = PR #182, branch `ticket/067-stage4k`
Gate tree `fcdf8ae` = `origin/main` `069cb9f` × PR #182 (`ort` merge, no conflicts)
Run window 2026-10-25T17:13Z – 2026-10-26T03:2xZ · worktree `hermes-2251c302` (swarm-farm-1)

Tier demanded: intel T3 wave. T4/T5 not named by GB-067, not run.

## VERDICT: RED

The feature works and is measured byte-exact. **The gate's own pass condition is not met:** the card
requires the evidence "on a wave that reaches `System halt`", and on this tree **every staged run
wedges** — 3 of 3 — while every unstaged run halts (2 of 2). The tier exits 1 with
`FAIL — the console has no 'System halt' line`.

| Clause | Result |
|---|---|
| 1 in-guest byte-exact read-back of the host-staged 4 KiB file, on a halting wave | **feature PASS, gate condition FAIL** — all four `[STAGE4K]` PASS lines present over the full 4096 bytes; the wave never reaches `System halt` |
| 2 honest ABSENT accounting | **0 ABSENT in 3 staged runs** (the reader opened the file every time); the earlier 1-of-4 was not reproduced here |
| 3 `attr != 0` on created entries (already on main) | **PASS** — `fat16.c:1273 new_entry.attr = 0x20` |

### What passed — the byte-exact read-back (run 1 and run 2)

    [WAVE-TRY] STAGE4K_T.BIN
    Loading program for scheduler (v2 AS): STAGE4K_T.BIN
    /STAGE4K_T.BIN
    [ARRIVED] STAGE4K_T.BIN rc>0
    /STAGE4K.BIN
    [STAGE4K] PASS size == 4096
    [STAGE4K] PASS read full 4096 bytes
    [STAGE4K] PASS byte-exact content (0 mismatches)
    [STAGE4K] ALL PASSED

Fixture: 4096 bytes, `byte[i] = i % 251`, sha256 `d67c656e01756650d77717b0839985a056ec28ffe174601d690fc407a2ceffca`,
staged post-build exactly as the card says:
`T3_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/stage4k.bin ::STAGE4K.BIN'`. On the image:
`STAGE4K  BIN      4096` (mdir). The compare is a full-content loop over all 4096 bytes with an
unsigned cast (`src/user/stage4k_test.c:83-90`) — not a header or first-N check.

### The failing condition (quoted, run 1)

    T3_intel: 14934 console lines, 92 FAIL lines, 6 user memory fault records across 5 processes, 80 programs exited, halted=0
    T3_intel: FAIL — the console has no 'System halt' line (idle/hung wave, or the 2700s deadline hit; a hang is a defect, not a flake)
    T3_intel: FAIL — 1 FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, not because the wave completed
    T3_intel EXIT=1

The console wedges after a 20 s watchdog dump and then repeats `[IDLESTUCK]` for the rest of the run
(3,630 records in run 1, 550 in run 2, 1,512 in run 5), with `lock_owner=7` visible in the claims line —
the `proc_lock`/lock-stall family that GB-060 and GB-096 already own.

### Why this is attributed to staging, not to GB-067's reader

Five intel T3 runs on this worktree, same harness, same 8-CPU guest:

| run | tree | staged file | halt | IDLESTUCK | STAGE4K |
|---|---|---|---|---|---|
| R1 | PR `fcdf8ae` | `/STAGE4K.BIN` (4 KiB) | **no** | 3630 | all PASS |
| R2 | PR `fcdf8ae` | `/STAGE4K.BIN` (4 KiB) | **no** | 550 | all PASS (3 dispatches) |
| R4 | PR `fcdf8ae` | *none* | **yes** | 0 | `FAIL open /STAGE4K.BIN` (expected — nothing staged) |
| R5 | PR `fcdf8ae` | `::OTHER4K.BIN` (unrelated) | **no** | 1512 | `FAIL open /STAGE4K.BIN` |
| R3 | `origin/main` `069cb9f` | *none* | **yes** | 0 | entry absent on main |

Staging **any** file correlates with the wedge (3/3), staging **none** halts (2/2) — including a
staged run in which the reader never reads anything (R5). So the wedge is not the 4 KiB reader and not
PR #182's wave entry; it is the post-build-staged-image path on this tree. R3's halt also shows it is
not "main never halts": main halts when nothing is staged.

To be explicit about what this verdict does **not** say: it does not accuse GB-067's code of the hang.
It says the card's stated evidence condition — a halting wave — could not be produced in three attempts
with the required fixture, so the gate cannot be called GREEN, and a reader quoting this gate would get
`T3_intel EXIT=1`.

### Clause 2 — the ABSENT half, honestly

The titled defect (host-staged file read as ABSENT, 1 of 4 runs under FATBIG/BLKTEST churn) did **not**
reproduce in my staged runs: 0 of 3 runs hit `FAIL open /STAGE4K.BIN` when the file was staged, and the
reader printed all four PASS lines in the two runs where it got that far (R5 staged a different file, so
its FAIL open is by construction). That is a null result on 3 samples against the worker's 1-in-4, not
a refutation: with a denominator that small, 0/3 and 1/4 are compatible.

### To reach GREEN (pick one, Coordinator)

1. **Fix the wedge** (the staging-path hang on this tree — it belongs to the lock-stall family already on
   the board) and re-run this gate unchanged; the byte-exact clause is already measured and will pass.
2. **Amend the card** to accept a non-halting wave when the `[STAGE4K]` block is self-contained and the
   hang is a named, separately-owned defect — that is a `decision`, and it would convert this RED into a
   GREEN on the existing evidence without re-running the 45-minute wave.