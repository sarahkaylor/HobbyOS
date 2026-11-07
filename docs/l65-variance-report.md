## 50. 2026-11-06 — M6/L65: the 47.8 vs 264 MiB spread is a **sampling-time artifact** — every run sits at 47.7 MiB with `Example_Traits.xml` last at t≈280 s, then crosses into the asset burst

§49 left one number unexplained: a soak that read **47.8 MiB** while an
earlier run on the same code read **264 MiB**, with no menu in either.
The open question was whether that is a latent race or scheduling.  This lane
ran three fresh **1800 s** windows with a 30 s sampler
(`tools/rw/l65-soak-sample.py`) and a post-hoc timeline builder
(`tools/rw/l65-timeline.py`), and the answer is neither: **47.8 MiB is what the
loader has read at t≈240-330 s in every run**, and the artifact that carries
the 47.8 figure is a **280 s** run, not a 900 s one.

### 50.1 Method

`RW_KEEP_RUNNING=1 RW_WAIT_SECS=1800` (the launcher's own wait is now a
compile-time override in `run-rimworld-desktop.sh`; it was hard-coded 900, so
the launcher would have exited mid-soak).  `--run-timeout 1800`, per-lane
export `/srv/nfs/rimworld-l65-variance`.  Every 30 s the sampler records, from
the live serial log and a live `mcopy` of `::/RW_HOME/Player.log`:

| column | source |
|---|---|
| NFS MiB | last `[NFSRTT] periodic ... bytes=` (1 MiB cadence) |
| `[XSW]` | presents from the last `[XSW] n=` row |
| live threads / resident MiB | last `[THRS] pid=4 live= res_mib=` (5 s cadence) |
| last file | last `[XO]` open |
| stalls | `wait_max_us>=500000`, `[BLKQ] stall`, `[NFSPIPE] stall`, `[NFSRD] fail`, `[TXT] ZERO`, `KVM internal error` |

Tripwires were the §48/§49 set (block layer, page-in gates, fault branch,
NFS pipeline); no special build flag is needed — the rows are unconditional.

### 50.2 The three curves do not diverge

Cumulative NFS MiB, read from each run's serial log:

| t (s) | run 1 | run 2 | run 3 | slow artifact (l66) |
|---:|---:|---:|---:|---:|
| 120 | 43.7 | 44.7 | 43.7 | 43.7 |
| 180 | 45.7 | 46.7 | 45.7 | 45.7 |
| 210 | 46.7 | 46.7 | 46.7 | 46.7 |
| 240 | 46.7 | 47.7 | 46.7 | **47.7** |
| 270 | 47.7 | 47.7 | 47.7 | 47.7 |
| 300 | 47.7 | 47.7 | 47.7 | 47.7 (run ends 280) |
| 330 | 47.7 | 48.7 | 47.7 | — |
| 360 | 48.7 | 51.3 | 48.7 | — |
| 390 | 54.2 | 67.8 | 52.3 | — |
| 420 | 75.6 | 113.7 | 71.6 | — |
| 450 | 129.0 | 179.8 | 126.2 | — |
| 480 | 189.9 | 262.0 | 192.4 | — |
| 510 | **264.7** | **264.7** | **264.6** | — |
| 540-1800 | 264.7 | 264.7 (died 491) | 264.6 (died 511) | — |

The three fresh runs are the same curve to within ±60 s; run 2 is the fastest
and run 1 the slowest.  The completed totals are deterministic to 0.035 %:
**277 577 677** (run 1), **277 598 238** (run 2), **277 500 762** (run 3).

### 50.3 The 47.8 MiB artifact is a 280 s run, and every long run on record reads 264.7 MiB

`l66-virgl/obj/rw-desktop` (byte-identical in every counted respect to the
l65 copy of the same run) has **34 running shots** at `RW_SHOT_EVERY=8`
(12:13:21 → 12:18:09, 288 s of shooting), a last row
`[RW] rwlaunch: child still running after 280s`, and the harness's
`make: *** [Makefile:2828: run] Killed`.  It is a **300 s** run.

Every run on record that lasted past 400 s read the whole asset set:

| artifact | last `[RW] after` | NFS max | `[XSW]` max |
|---|---:|---:|---:|
| l59-sigsegv | 410 s | 264.7 MiB | 180 |
| l61-jit | 860 s | 264.6 MiB | 270 |
| l63-fpu | 470 s | 264.8 MiB | 250 |
| l64-zeropage | 850 s | 264.7 MiB | 270 |
| l65 run 1 | 1690 s | 264.7 MiB | 260 |
| l65 run 2 | 490 s | 264.7 MiB | 270 |
| **l65/l66 47.8 MiB artifact** | **280 s** | **47.7 MiB** | **230** |

There is no 900 s / 47.8 MiB artifact.  The §49.6 pairing is most plausibly the
300 s verification run's byte count (whose log overwrote the 900 s soak's at
12:13) crossed with the longer run's resident figure — 1 353 MiB resident is
what run 1 reads at t≈860 s, where its NFS was already 264.7 MiB.

At the artifact's final sample it is in the *same state* as the fresh runs at
the same age:

| at t≈281 s | NFS MiB | `[XSW]` | live | res MiB | last `[XO]` |
|---|---:|---:|---:|---:|---|
| slow artifact (final, t=280) | 47.7 | 230 | 30 | 752 | `Core/.../TraitDef/Example_Traits.xml` |
| run 1 | 47.7 | 210 | 34 | 755 | `Core/.../TraitDef/Example_Traits.xml` |
| run 2 | 47.7 | 230 | 31 | 752 | `Core/.../TraitDef/Example_Traits.xml` |
| run 3 | 47.7 | 210 | 40 | 751 | `Core/.../TraitDef/Example_Traits.xml` |

Same file last, same bytes, same resident.  There is nothing at t=280 to
distinguish the "47.8" run from the "264" runs — because the 47.8 run was
killed there.

### 50.4 The branch point: 330-390 s, not 240 s

The first file that a fast run opens *after* the common state is the mod
language set.  The gap is explicit:

| run | last `Core/.../DefInjected` | first `Royalty/.../Keyed` |
|---|---|---|
| slow artifact (ends 280 s) | `Example_Traits.xml` at 240 s | **never** (window ends) |
| run 1 | `Example_Traits.xml` at 260 s | `Royalty/Keyed/Alerts.xml` at 320 s |
| run 2 | `Example_Traits.xml` at 230 s | `Royalty/Keyed/Alerts.xml` at 300 s |
| run 3 | `Example_Traits.xml` at 260 s | `Ideology/Keyed/Dialog_StatsReports.xml` at 330 s |

So the "slow" run's last file is the fast runs' last Core file too; it stopped
in the 60-70 s gap between Core DefInjected and the mod keyed files, not at a
different code path.  What follows the gap is the pipelined asset read — run
2's largest single `[NFSPIPE]` row is `len=2566144 rs=1344 issued=1910
replies=1910 short=0 bad=0 got=2566144 us=575009`, a 2.5 MiB batch of 1 910
RPCs.

The transport rows around the transition are *retries that work*, in all runs:

| run | `[NFSPIPE] stall` at | shape | outcome |
|---|---|---|---|
| slow artifact | 230 s (×2) | `issued=4 replies=2/3`, us≈520-530 ms | retry `got=4096` at ~235 s |
| run 1 | 250, 320, 330, 350 (8 rows) | `issued=2..24 replies=1..23` | all recovered; burst follows |
| run 2 | 230, 300, 310, 320 (7 rows) | same family | all recovered; burst follows |
| run 3 | 250, 330, 340, 350 (7 rows) | same family | all recovered; burst follows |

So a stall row before the burst is the **normal** shape, not the signature of
the slow run: every fresh run has 7-8 of them in the same window and still
finishes.

### 50.5 Verdict

**(b) benign variance that the 1800 s window absorbs.**  Specifically:

1. 47.8 MiB is the loader's position at t≈240-330 s, reached by every run;
   the 1800 s window is what carries it past the def-parse plateau into the
   asset burst (330-390 s) and to the deterministic total (264.7 MiB by
   491-541 s).
2. The only 47.8 MiB artifact is a 280 s run; the "47.8 MiB at 900 s" pairing
   is not supported by any surviving artifact and is most plausibly a
   bookkeeping mix of that run's byte count with the longer run's resident
   figure (the 1 353 MiB resident in §49.6 matches t≈860 s of run 1, whose NFS
   was 264 MiB).
3. It is **not** (c) load-order nondeterminism: the `[XO]` order after the
   first four files differs between every pair of runs (def dirs are read by
   several threads), yet the byte total reproduces to ≤0.01 %.  The loader's
   *work* is deterministic; only its *order* varies.
4. It is **not** (a) a race on this evidence: no run lost a wake at the
   plateau, and no retry row is followed by a stop.  If a 900 s window really
   sat at 47.8 MiB it would be a real stall, but three 1800 s runs did not
   reproduce one, and the artifact that carries the number is 280 s long.
   Two of the three runs died *after* the load (491 s, 511 s) at a
   reproducible block-wedge site (§50.6) — a real bug, but a post-load one
   that does not move the byte count.

### 50.6 What *is* still a real bug in the window: the block wedge

Runs 2 and 3 died at t=491 s and t=511 s, at the **same** fault VA
(`0x10824E1EA0`), after the **same** gate refusals (`[TXT] ZERO off=0x9AD000`
and `IMG SUM pos=0x10000 got=0x76EFDDC5 want=0xDC910CE7`), and it is a clean
replay of §48's tuple with the L63/L49 gates doing their job:

```text
[BLKSLOW] pending cid=43767 slot=2 ms=251 sq_tail=3 cq_head=2 phase=0
[BLKQ] stall cid=43767 ... CSTS=0x0 ... inside=1 n=1
[BLKQ] controller reset #1
[TXT] ZERO off=0x9AD000 reads=3 n0=4096 -> not published
[TXT] IMG SUM pos=0x10000 got=0x76EFDDC5 want=0xDC910CE7 -> not published
[KERNEL] pid=4 ... memory fault VA=0x10824E1EA0 ... in=PROT ... -> killed
[SIGX] pid=4 ... sig=11 ... alt=1 live=37
[KERNEL] pid=4 ... memory fault VA=0x0 PC=0x0 ... inH=1 -> killed
[KERNEL] pid=3 (RWLAUNCH.BIN) memory fault VA=0x1000010EA0 ... -> killed
```

The zero page was **refused**, not executed (`[TXT] ZERO`, `IMG SUM`), so the
L63 verdict stands: the failure is an I/O error, not a `#UD`.  But the block
layer still wedges and the whole process tree still dies.  That is the bug
worth fixing next; it is not the byte-count variance.  Run 1 never hit it: it
parked after the load with 260 presents and grew resident to 1 899 MiB by the
end of its window (launcher t=1690 s).

### 50.7 No menu in any run

Every run presents 260-290 frames at 1920×1080, `box=0,0,1915,1017`,
`crc` alternating `0xcab88ca9/0x2e2650f4/0xbc23863d`, `changed=1`.  The best
frame (shot 40, 132 577 colours) OCRs as RimWorld's loading art with the
`RWLAUNCH` title bar and no menu text.  Run 1's framebuffer drops to 8×1 at
t≈500 s (all later `screendump`s are 8×1), so no visual evidence exists after
that point; run 2's screen returns to the desktop when the game dies at 491 s.
There is no menu, no `[XSW]` bbox other than the full content rect, and no fps
to quote.

### 50.8 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **423 run, 0 failed, UNIT TESTS
PASSED**.  `tools/gate/m3_window_probe.py`: **PASS** (the shim served the whole
XCreateWindow sequence and delivered WM_DELETE_WINDOW).  `tools/gate/m4_gl_probe.py`:
**PASS** (libGL.so.1 loaded, GL 3.3 core triangle rendered and presented, stack
torn down).  Both gates rc=0; logs in `obj/l65-m3.out` / `obj/l65-m4.out`.
