# L81 — the menu is passable: the front-end drives to the planet, and the wall
# was the address-space region table

Lane `l81-ingame`, worktree `~/hobbyos-rw-lanes/l81-ingame` (branch
`rw-l81-ingame`, seeded at the merged tip `d374e6ca`).  The question this lane
was given: the game reaches its main menu at 533-549 s — can anything drive it
*past* the menu with input, and what is the next wall?

Yes.  A QMP click driver took the game from the main menu through
**New colony → Crashlanded → Cassandra Classic / Strive to survive / Reload
anytime mode → Next → Create world → Generate**, and the world generated: the
game reached **Select starting site**, the planet view, with **0 exceptions in
Player.log**.  The wall that stopped the first attempt was a kernel one — the
per-address-space region table (2 048 entries) — and fixing it is what let the
world generate at all.  The second stop was the driver's own: the planet view
has no footer button, so a script that only clicks buttons cannot choose a
tile.  That is now implemented (a land pixel is clicked, then the confirm
button), but the confirming run is not part of this record: two other attempts
died in the def load at the ladder's known SIGSEGV (§5).

---

## 0. Summary

| question | answer |
|---|---|
| can input drive past the menu? | **yes.** Nine input events — the Steam modal's Ignore (auto) plus eight game clicks: New colony `(1526,441)`, Crashlanded `(908,286)` (already selected), Next `(1378,918)`, Cassandra `(721,389)`, Strive to survive `(893,599)`, Reload anytime mode `(893,723)`, Next `(1378,918)`, Generate `(1379,916)`. Each is verified by an OCR class change and a pixel diff (§2) |
| does world generation complete? | **yes.** Generate at flow t=829 s; a loading tip at t=839 s; **Select starting site** at t=861 s. Player.log ends with normal asset unloading and **0 exception lines** |
| how fast is it? | **0.66-0.95 fps** on every front-end screen (guest `[XSW]` windows: menu 0.858, scenario 0.724, storyteller 0.694, worldopts 0.666, site 0.695). The host was running two other lanes' VMs; L80 measured 9-20 fps for the same tree on a quiet host, so this is contention, not a menu cliff |
| what stopped run 1? | **the kernel's region table.** `[THRS] regs=2003` (cap 2 048), then world generation's Mono code-manager guard page (`len=0x3000`, MAP_FIXED PROT_NONE at `0x114CA35000`) was refused: `[MR] FAIL … blk=none`, `[LMMAP] fail … rc=-12`; Boehm printed `mmap(PROT_NONE) failed` and the game took SIGABRT |
| the fix | `VM_REGION_MAX_CAP` 2 048 → **16 384**, allocated on the first growth (process start), `[MR] FAIL` now carries `nregs=`/`cap=` |
| does it hold? | **yes.** Run 3: `regs=3321` during world generation, the planet on screen, 0 exceptions, 0 GPU refusals |
| what stopped run 3? | the **driver**: `Select starting site` is a full-window globe with no footer button; `[flow] no_advance site`, then 92 s static → wall. Fixed after the run (land-pixel click + confirm) |
| what stops runs 2 and 4? | a pre-existing **SIGSEGV during the def load** at `addr=0x3A0` (JIT code, `regs≈342`, `res_mib=893`). L80 recorded the same window as "run D stalled at t=466 s"; it is not the region table and not the menu |

---

## 1. The driver

`tools/rw/rw_flow.py`, enabled by `RW_FLOW=1` through
`tools/rw/rw_desktop_drive.py`.  It takes over after the launcher starts:

* **waits** for the main menu by OCR (`Tutorial` + `New colony` + `Quit to
  OS`), dismissing the Steam modal only when the OCR says it is the modal
  (L79 called the brown-button finder on every poll; the menu's own buttons
  have the same fill);
* **clicks by evidence**, not by label alone.  An advancing click is the
  **rightmost flat-brown footer button** — the affirmative on every RimWorld
  dialog.  This is the L79 bug that had to die: L79's OCR matched "Generate"
  to a word box at `x=544`, the Back/Reset-all column, so its run left the
  screen it meant to advance.  Card and radio labels are still OCR-located;
  radios are found by the bright ring on the label's own row;
* **verifies every click**: before/after screenshots diffed (pixels, bbox,
  clock-region share), OCR re-classified, `[XSW]` present counter and CRC
  re-read.  A click that changes nothing is retried (the same button, up to 3
  times, polling up to 24 s — a 1 fps transition can take that long); it is
  never replaced by a different button;
* **measures a stage** with a 15 s window: fps from the guest's own `[XSW]`
  `n=`/`t=` rows, plus a screenshot diff against the stage-entry frame;
* **declares a wall** when a stage shows no pixel change outside the panel
  clock for 90 s and the present counter is also static, and then dumps the
  last `[XSW]`/`[THRS]`/`[BLKD]`/`[FUTR]` rows plus the GPU refusal count
  (`wall-<stage>.json`);
* **survives the launcher's log replay**: when the child dies, rwlaunch dumps
  Player.log to the console, and Player.log contains a copy of the `[XSW]`
  rows.  Parsing the whole serial file then reads OLD present counters
  (measured: the flow read n=110/t=128 s while the live counter was
  n=320/t=422 s).  `serial()` truncates at the `--- Player.log (` marker, and
  `rwlaunch: waitpid=<pid> status=<n>` is a first-class wall ("game-exited").

`flow.jsonl` is append-only (every record carries a run id) and
`flow-state.json` is rewritten on every stage entry, so a run that dies leaves
its stage ladder on disk.  `tools/rw/l81-flow-summary.py` prints the stage,
click, poll and XSW tables and applies the three-part in-game rule.

---

## 2. Run 3 — menu to planet, click by click

`obj/l81/run3/` (serial, Player.log, flow.jsonl, 100+ PNGs).  Env:
`RW_FLOW=1 RW_FLOW_STAGE_SECS=15 RW_QEMU_GL=1 RW_GLMODE=virgl RW_VIEWPORT=1`,
`--run-timeout 2400`.

| flow t | stage | click | OCR class after | diff px | bbox | clock share |
|---:|---|---|---|---:|---|---:|
| 680.9 | menu | — | menu | — | — | — |
| 697.9 | menu | New colony `(1526,441)` | **scenario** | 909 087 | (10,44)-(1912,1068) | 0.0 |
| 727.2 | scenario | Crashlanded `(908,286)` | scenario | 225 | (908,286)-(1894,1069) | 0.20 |
| 738.6 | scenario | footer / Next `(1378,918)` | **storyteller** | 557 944 | (470,42)-(1913,1069) | 0.0001 |
| 767.7 | storyteller | Cassandra Classic `(721,389)` | storyteller | 5 367 | (721,389)-(1886,1069) | 0.002 |
| 778.0 | storyteller | Strive to survive `(893,599)` | storyteller | 43 313 | (616,389)-(1886,1069) | 0.0006 |
| 788.5 | storyteller | Reload anytime mode `(893,723)` | storyteller | 78 775 | (616,589)-(1894,1069) | 0.0006 |
| 799.9 | storyteller | footer / Next `(1378,918)` | **worldopts** | 348 040 | (470,216)-(1894,1069) | 0.0002 |
| 829.3 | worldopts | footer / Generate `(1379,916)` | **unknown** (world-gen tip) | 685 181 | (447,42)-(1913,1069) | 0.0 |
| 860.6 | — | — | **site** | 1 821 284 | (automatic, poll caught it) | — |

Stage rows (fps is the guest `[XSW]` window, not the host's):

| stage | fps | span | presents | CRC | changed | 15 s diff px | bbox | clock |
|---|---:|---:|---:|---|---:|---:|---|---:|
| menu | 0.858 | 23.3 s | 240 | `f88d9ec9e1a32c7f` | 0 | 66 | (1857,1063)-(1894,1069) | 1.0 |
| scenario | 0.724 | 27.6 s | 260 | `9e0f2f8b1dd860a3` | 0 | 34 | clock only | 1.0 |
| storyteller | 0.694 | 28.8 s | 280 | `8e5fc2df4e79a6e7` | 1 | 40 | clock only | 1.0 |
| worldopts | 0.666 | 15.0 s | 330 | `2ecc740ae7e17aeb` | 0 | 28 | clock only | 1.0 |
| **site** | 0.695 | 28.8 s | 340 | `b7e314f74a8eb29a` | 1 | **20 110** | **(489,114)-(1894,1069)** | **0.0024** |

The site row is the one that is not the clock: 20 110 pixels changed across
the planet (bbox 489,114-1894,1069, 99.76 % of the change outside the panel
clock) while the front-end screens' only 15 s change was the clock itself.

`[XSW]` CRC ladder (first guest t): `f88d9ec9e1a32c7f` menu 604 s →
`9e0f2f8b1dd860a3` scenario 641 s → `8e5fc2df4e79a6e7` storyteller 684 s →
`8121d54a0e6adb4a` Strive selected 714 s → `a28b034580f8c5c7` worldopts 730 s
→ `2ecc740ae7e17aeb` worldopts (seed) 743 s → **`b7e314f74a8eb29a` 772 s**
→ `9a34775b8241a612` 874 s (site frame).  **14 distinct CRCs**, every click
landing one.

Player.log: 0 lines matching `Exception`/`NullReference`/`StackTrace`; the log
ends with `Unloading 0 unused Assets …` and the last `[XSW] n=350` — a healthy
idle engine at the site screen.

### 2.1 What the furthest screen is

OCR: `Select starting site Learning helper + World view camera movement`.
Described from the frame (`f097_site_poll.png`): a full-window planet with
brown/green landmasses and blue oceans, geographic labels (`Letobor Ocean`,
`Banastra Sheet`, `Mount Toxin`, `Braira Groves`), and faction site icons
(skulls, settlements, camps) scattered across it.  This is the world map, one
step before character creation — the game generated a planet from the seed on
the Create-world screen.

The 92 s wall after this is the driver's: `[flow] skip site phrase="Select
random site" note="not on screen"`, `[flow] no_advance site`, then polls that
only see the clock.  The planet does not repaint unless it is rotated or a
tile is clicked.  Fixed after the run: `land_point()` finds a 9x9 all-land
window nearest the globe centre (the centre pixel is `(104,99,52)`), clicks
it, and then confirms with the rightmost footer button (`OK`/`Next` are in
the OCR candidates for this stage).

---

## 3. The wall that was real: the address-space region table

### 3.1 Measurement (run 1, cap 2 048)

`obj/l81/run1/` — same driver, same clicks, Generate at flow t=816 s.  The
screen went to the desktop with only the taskbar (`OCR "Apps 22:53:16"`), the
present counter stopped at `n=360`, and the serial carried:

```text
[THRS] pid=4 live=27 … regs=2003 anon=1912 anon_mib=6508 big_mib=255 stk=3 res_mib=1403 /NFS/RimWorldLinux
[MR] FAIL pid=110 len=0x3000 base=0x114CA35000 blk=none
[LMMAP] fail pid=110 len=0x3000 prot=0 flags=0x32 hint=0x114CA35000 rc=-12
[SIGX] … sig=6 …                      (abort)
```

and Player.log's last line: **`mmap(PROT_NONE) failed`** — Boehm's
`GC_unix_mmap_get_mem` abort.  The mapping is 3 pages of `MAP_FIXED`
`PROT_NONE` (`flags=0x32`) — a Mono code-manager guard page, not a heap
extension.  `regs=2003` against `VM_REGION_MAX_CAP 2048`: the first seconds of
planet generation (which JITs a lot of new managed code) crossed the cap,
`vm_region_insert_locked` answered -ENOMEM, and the GC killed the process.

### 3.2 Fix

* `VM_REGION_MAX_CAP` 2 048 → 16 384 (16 384 × 40 B = 640 KiB);
* `region_grow()` allocates the full table on the **first** growth (the AS has
  16 regions then, i.e. process start) instead of asking for its largest
  contiguous run late in the run; if that request is refused it falls back to
  the old 64-entry step, so nothing is worse than L55's behaviour;
* `[MR] FAIL` now prints `nregs=`/`cap=` so the cap and a genuine overlap are
  distinguishable in one row;
* `[LMMAP] fail`/`[LMMAP] big` rows for refused and ≥32 MiB anonymous mmaps.

### 3.3 Confirmation (run 3)

`regs=3321` during world generation (> the old cap by 1 273), the planet on
screen, `GPU res_create refusals: 0`, Player.log 0 exceptions.  The cost is
one 640 KiB contiguous allocation per address space, taken at process start.

---

## 4. Regression

| gate | result | log |
|---|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | _filled at the end of the lane_ | `obj/l81/units.log` |
| `m3_window` | _filled_ | `obj/l81/gates/` |
| `m4_gl` | _filled_ | |
| `rwnfs` | _filled_ | |

---

## 5. The other wall: an early SIGSEGV in the def load

Runs 2 and 4 never reached the front-end.  Both died at the same point in the
load, with the same shape and a different JIT address:

```text
[THRS] pid=4 … regs=342 anon=251 anon_mib=5203 res_mib=893
[SIGX] pid=4 tid=4 sig=11 code=1 addr=0x0x3A0 ipc=0x10B8DC0C5F (run 4)
[SIGX] pid=4 tid=4 sig=11 code=1 addr=0x0x3A0 ipc=0x10B8DC6B3F (run 2)
[RW] rwlaunch: waitpid=4 status=11
```

`addr=0x3A0` is a null object plus a field offset, read from JIT code, at
`res_mib=893` — long before the region table matters (`regs=342`).  L80's run
D recorded the same window as "stalled at t=466 s with no presents for 3+
minutes"; it is a pre-existing instability of the load, not a consequence of
the region table and not the menu.  The driver now reports it as
`[flow] wall stage=game-exited … waitpid=4 status=11` instead of polling a
dead window for the rest of the budget (run 2 wasted ~600 s doing that).

---

## 6. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l81-ingame

RW_FLOW=1 RW_FLOW_STAGE_SECS=15 RW_FLOW_WALL_SECS=90 \
RW_QEMU_GL=1 RW_GLMODE=virgl RW_VIEWPORT=1 RW_WAIT_SECS=2900 \
  tools/rw/run-rimworld-desktop.sh --timeout 600 --run-timeout 2400

python3 tools/rw/l81-flow-summary.py obj/rw-desktop/flow.jsonl \
        --serial obj/rw-desktop/serial-desktop.log --wall-dir obj/rw-desktop
```

`RW_FLOW_TOTAL` (default `run-timeout - 120`) is the flow's own budget;
`RW_FLOW_GEN_MAX` caps the world-generation wait; `RW_FLOW_SKIP_FPS=1` drops
the 15 s stage windows when only the next screen matters.
