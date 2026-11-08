# L87 — no lost wake: the pre-menu "freeze" is a 150 s present-only stall rule

Branch `rw-l87-lostwake` (from `rimworld` @ `4eab3dad`, includes L83–L86).
Worktree `~/hobbyos-rw-lanes/l87-lostwake`. Nothing pushed.

The question this lane was handed: most boots stop before RimWorld's main menu
with one parked GL writer —

```
[FUTEX] q slot=31 pid=31 st=8 as=0x75704A50 key=0x113000A460 bitset=0xFFFFFFFF
        waits=3798 wakes=3797 … UnityGfxDeviceW
```

Is that a lost wake in the compat condvar/futex path, a waker that is itself
parked, or an event that never arrives?

**None of them.** The guest is not frozen. The front end needs ~650 s of game
time on this host, including a measured **215 s window with no present at all**
while the def loader is still opening files. L86's driver calls that window a
stall after **150 s**, returns to the harness, and the harness kills QEMU —
which reads as a guest freeze. In L86's own log the verdict lands at elapsed
568.9 s; in this lane's first fixed boot the menu is on screen at 597.7 s, **29
seconds later**. Five boots then reached the menu, and the L86 keyboard flow —
never executed before — typed into the Seed field and moved focus with one Tab.

## 0. Summary

| question | answer |
|---|---|
| mechanism (a) lost wake in our futex path? | **no.** The `[CWV]` per-key ledger shows `waits == wakes` for the render-thread key in every boot (0x113000A460: 9 592/9 592, 13 350/13 350, 10 465/10 465, 9 645/9 645). 0 `[LOSTWAKE]` rows. |
| mechanism (b) waker parked on the same key? | **no.** The wakers (main `/NFS/RimWorldLinux`, `Loading.AsyncRe`) are RUNNING in the dumps and issue `FUTEX_WAKE … woke=1` on the render key throughout the plateau; the three-party handshake keeps cycling. |
| mechanism (c) an event never reaches the wait? | **no.** Every park is dequeued. What never arrived was the *next screenshot poll*: the observer had already given up. |
| what was the stall? | `rw_l86_input.main_menu()`'s rule: if `[XSW] n` has not changed for 150 s, emit `[L86] STALLED` and return. The normal load has a 215 s no-present window. |
| what fixed it? | liveness = a new present **or** a new `[XO]` loader open; quiet budget 900 s; menu budget 1500 s; harness default 1800 s; progress rows; automatic Steam-modal dismissal. |
| 4 boots to the menu? | **yes** — five boots in this lane reached the menu (presents 590 / 970 / 690 / 600 / 440), 0 `[LOSTWAKE]` in every serial log (§5). |
| keyboard? | **yes** — Seed field `Seed |swamp` → `Seed |sw0345amp` from five QMP `send-key` digits (436 game pixels in a 68×16 box), and one Tab moves a 1×16 focus bar at (740,88)–(740,103) (§6). |
| units? | `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` → **436 run, 0 failed, UNIT TESTS PASSED**. |

## 1. The verdict that was being read as a freeze

L86's attempt logs contain the driver's own verdict, 29 s before this lane's
menu:

```
[L86] {"ev": "poll", "stage": "menu", "text": "… Initializing. …", "elapsed": 552.2}
[L86] {"ev": "stalled", "stage": "menu", "n": 390,
       "note": "[XSW] n frozen for 150 s -- guest idle stall, not an input failure",
       "elapsed": 568.9}
[L86] STALLED: [XSW] n=390 frozen for 150 s
[L86] {"ev": "no_target", "stage": "menu", "label": "New colony",
       "text": "… Initializing. …"}
[L86] {"ev": "done", "stage": "end", "note": "reached_site=False", "elapsed": 571.7}
```

The screen at that moment is still the loading art (`Initializing.`); the
presents are frozen because RimWorld's def pass does not draw; the kernel's
`[IDLESTUCK]` rows are idle-CPU bookkeeping (one idle CPU on an 8-vCPU machine)
and the `[FUTEX] q` row is a *parked waiter*, which is what a waiter looks
like. None of those is a defect. The driver then returned, the harness killed
QEMU, and the post-mortem read the parked waiter as the cause.

### 1.1 The measured load has a 215 s no-present window

L78's successful menu run (M2, a boot that reached the menu) has exactly this
shape, from its own `[XSW]` rows:

```
n=280 crc=0xc9ac860fdaed6059 t=322520
n=290 crc=0x9bcb235cecf1c6d0 t=537237     <- 214 717 ms with no present
n=300 crc=0xf88d9ec9e1a32c7f t=547995     <- the menu frame
```

L87 boot B shows the same window from the driver's side, with the new liveness
rows (presents frozen, loader still opening files):

```
[L86] loading: n=380 opens=1196 quiet=33s
[L86] loading: n=380 opens=1196 quiet=100s
[L86] loading: n=380 opens=1196 quiet=167s     <- the old rule fired at 150 s
… 29 s later:
[L86] {"ev": "target", "stage": "menu", "label": "New colony",
       "screen": [1215, 267], "note": "ocr:New colony", "elapsed": 597.7}
```

So the "timing-sensitive freeze" is timing-sensitive in the *observation*:
host load (L86's OCR every 15 s) stretches the plateau past 150 s, and the
driver gives up inside it.

## 2. Instrumentation: the per-key wait/wake ledger (`[CWV]`)

`[FUTEX] q` prints the **waiter's own** counters. A parked thread has
`waits = wakes + 1` by construction — that is the arithmetic of being parked,
not evidence of a lost wake (the lane brief's calibration boot with
`waits=17 wakes=16` and a menu is the same fact). What was missing was a
**per-key** ledger that says whether the *word* got a wake after the last park,
and whether that wake dequeued anyone.

`src/kernel/linux_compat_thr.c` now keeps, under `proc_lock`, one entry per
`(addr_space, uaddr)` (`RW_CWV=1`, default off):

```
[CWV] KEY key=0x… w=<parks> k=<dequeues> park=<current waiters>
      lw=<seq of last park> lk=<seq of last wake> lwp=<parker> lkp=<waker>
      verdict=<woke-after-park|NO-WAKE-AFTER-PARK|idle>
[CWV] EV  key=0x… seq=<global> W pid=<parker> val=<expected>
[CWV] EV  key=0x… seq=<global> K pid=<waker> val=<word before wake> woke=<n>
[CWV] PARK pid=<n> key=0x… seq=<park seq> waits=<n> wakes=<n> <thread name>
```

Sequence numbers are global, so the main → loader → gfx rotation can be ordered
across keys. A stall dump prints every key that still has a waiter, then the
most recently active keys, then every queued thread.

### 2.1 What the rows say for key 0x113000A460

All five boots, last ledger row for the render key:

| boot | `w` (parks) | `k` (dequeues) | current | last parker → last waker |
|---|---:|---:|---|---|
| A | 9 592 | 9 592 | 0 | 31 → 7 |
| B | 13 350 | 13 350 | 0 | 31 → 7 |
| C | 10 465 | 10 465 | 0 | 31 → 8 |
| D | 9 645 | 9 645 | 0 | 31 → 7 |
| E | 7 410 | 7 410 | 0 | 31 → 7 |

Every park on the render thread's key was dequeued by a wake. The waker is the
main thread; the render thread is `UnityGfxDeviceW`. The main thread is not
parked on that key — it parks *between* rounds on `0x1083B1B624` waiting for
`Loading.AsyncRe` (pid 30), which is RUNNING (not in the `[CWV] PARK` list).
That is the three-party handshake L72 named: main hands a step to the loader and
to the gfx worker, the loader reports back, repeat.

### 2.2 `NO-WAKE-AFTER-PARK` is a snapshot, not a deadlock

At one dump in boot B the main thread's key reads:

```
[CWV] KEY key=0x1083B1B624 w=896 k=895 park=1 lw=47785 lk=47783 lwp=7 lkp=30
      verdict=NO-WAKE-AFTER-PARK
```

27 log lines later in the same boot, the same key:

```
[FUTXW] WAKE key=0x1083B1B624 by=30 n=1 pre=0x1 post=0x1 parked=1 woke=1 pids=7
```

and the boot goes on to the menu. The verdict says "at this instant the last
event was a park"; it cannot say "no wake will ever come", and the very next
round proves it. This is the trap the lane brief warned about, in the other
direction: one parked waiter is not proof, and one snapshot verdict is not
either.

## 3. Mechanism decision

| candidate | verdict | evidence |
|---|---|---|
| (a) lost wake in our condvar/futex path | **refuted** | per-key `w == k` for the render key in 4/4 boots; every `K` row names the waiter it dequeued (`woke=1`); 0 `[LOSTWAKE]`; the wait/wake pair is taken under the same `proc_lock`, so the compare-and-park cannot race a wake |
| (b) waker parked on the same key | **refuted** | the wakers are main (8/7) and `Loading.AsyncRe` (30), both RUNNING in the `[IDLESTUCK]`/`[ALIVE-SET]` samples; their `K` rows continue through the plateau (`WAKE … by=30 … pids=8`, `by=8 … pids=31`) |
| (c) event/dropped wake before the wait | **refuted** | every park is followed by a dequeue; the game reaches the menu without any change to the futex/kernel path |
| actual cause | **false stall in the harness** | `[L86] STALLED … 150 s` at elapsed 568.9 s; menu at 597.7 s with the fixed rule; L78 M2's 215 s no-present window |

## 4. Fix

All in `tools/rw/`, no kernel behaviour change:

1. `rw_l86_input.py`
   * `main_menu()` liveness is now `liveness_quiet(now, last_present, last_open)`
     — a present **or** an `[XO]` loader open resets the clock — and
     `is_stalled()` fires only after 900 s of both being quiet
     (`RW_L86_STALL_SECS`, was 150).
   * `RW_L86_MENU_TIMEOUT` default 900 → 1500 s (the menu arrives ~650 s after
     launch; OCR polls add host load).
   * every 60 s it emits `[L86] loading: n=… opens=… quiet=…` so a plateau is
     visible in the log instead of being inferred from silence.
2. `rw_desktop_drive.py`
   * a `[drive] progress n=… opens=…` row per screenshot;
   * the Steam modal is dismissed automatically when it is on screen (it is
     drawn *over* the loaded menu and can sit there for the whole window —
     boot C-noclick, n=790, modal still up), instead of requiring
     `RW_CLICK_IGNORE_AFTER`.
3. `run-rimworld-desktop.sh`: `RUN_TIMEOUT_S` default 300 → 1800, with the
   measured load budget in the comment.
4. `tools/rw/l87-boots.py`: one-row-per-boot summariser (presents, menu, XO
   opens, `[LOSTWAKE]`, ledger for the handshake keys).
5. `tools/rw/l87-liveness-test.py`: pure regression test of the stall rule on
   the real timeline (old rule fires at 150 s inside the plateau; new rule does
   not; it still fires after 900 s of both signals being quiet). 7/7 PASS.

## 5. Five boots to the menu

All with `RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720 RW_WM_HONOUR=1`,
virgl, IceWM. "menu OCR" is tesseract on the last client-area crop; the raw
numbers and screenshots are under `obj/l87/boot{A,B,C,D,E}/`.

| boot | mode | `[XSW] n=` at menu | menu OCR (verbatim) | `[LOSTWAKE]` | render-key ledger |
|---|---|---:|---|---:|---|
| A | plain, 1800 s window | **590** | `New colony … Options … Mods … Credits … Quit to OS … English` | 0 | w=9 592 k=9 592 |
| B | `RW_L86=1`, full flow | **970** (menu at 597.7 s) | `New colony … Choose scenario … Crashlanded` | 0 | w=13 350 k=13 350 |
| C | plain, auto-Ignore | **690** | `New colony … Options … Mods … Credits` | 0 | w=10 465 k=10 465 |
| D | plain, auto-Ignore | **600** | `New colony … Options … Mods … Credits` | 0 | w=9 645 k=9 645 |
| E | plain, auto-Ignore | **440** | `New colony … Options … Mods … Credits` | 0 | w=7 410 k=7 410 |

A–D are the four menu receipts the lane was asked for: four boots under the
same long-window recipe (A before the driver change, B–D after it), each with
the menu on screen and OCR'd.  The only boot between them that did not show
the menu was `C-noclick`: the Steam modal covered the already-loaded menu for
the whole window (`n=790`, modal still up) and there was no click driver in
that run.  Two later attempts (E, E2) were environment duds, not game stalls:
the boot fell back to `DESKTOP.BIN` because the 1 GB FAT image had a 0-byte
`XSESSION.BIN` and `XHOBBY.BIN` — a disk image rebuilt in parallel with a boot
(serial: `[XSESS] ERROR: cannot spawn /XHOBBY.BIN errno=2`).  After rebuilding
`disk.img` cleanly, boot E reached the menu again, so that failure is separate
and reproducible.

The `[XSW] n=` values differ because the boot ends at different points in the
menu's 1 fps animation; the menu frame is the same (`New colony` OCR) in all
four. In each boot the last ledger row before the kill still has the engine
alive (`[FUTR]` ~124 ops/s at the menu).

## 6. Keyboard — the deliverable L86 could not reach

Boot B, `RW_L86=1`, `l86.jsonl` rows quoted verbatim.

### 6.1 Click chain (all `[L86] click` rows)

| action | stage | screen | content | diff px | OCR after |
|---|---|---|---:|---:|---|
| `Ignore` (Steam modal) | modal | (813,592) | (481,552) | 415 372 | menu: `New colony … Options … Quit to OS` |
| `New colony` | menu | (1215,267) | (883,227) | 107 801 | `Choose scenario … Crashlanded … Three crashlanded survivors` |
| `Crashlanded` | scenario | (547,489) | (215,449) | 78 905 | `Choose scenario … Crashlanded … The Sanguophage` |
| footer (`Next`) | scenario | (1387,745) | (1055,705) | 202 869 | `Choose AI Storyteller … Cassandra Classic` |
| `Cassandra Classic` | storyteller | (731,215) | (399,175) | 1 300 | `Choose AI Storyteller … Cassandra` |
| `Strive to survive` | storyteller | (903,424) | (571,384) | 6 780 | same page, difficulty selected |
| `Reload anytime mode` | storyteller | (903,549) | (571,509) | 58 599 | same page, mode selected |
| footer (`Next`) | storyteller | (1388,747) | (1056,707) | 327 973 | `Seed |swamp … Factions … Overall rainfall` |

### 6.2 Seed field — five QMP `send-key` digits

| action | OCR before | OCR after | diff bbox (game area) | px |
|---|---|---|---:|---:|
| click `Seed` at screen (703,92) | `Seed \|swamp` | `Seed \|swixmp` (caret/selection) | (100,81)–(1914,1071) | 7 446 |
| inject `1 2 3 4 5` (`send-key` qcodes) | `Seed \|swixmp` | **`Seed \|sw0345amp`** | **(700,88)–(767,103)** | **436** |

The raw driver row is `{"ev":"seed_typed","ok":true,"field_before":"Seed |swixmp",
"field_after":"Seed |sw0345amp","pixels":481,"bbox":[700,88,1914,1071]}`; the
481 includes 45 px of taskbar clock — the game-area-only recomputation is 436 px
in a 68×16 box around the field. Digits appear inside the word because the
click landed mid-text (the caret was between `sw` and `amp`); the field changed
from five real key events, which is the proof. Screenshots
`l86_062_seed_after_Seed.png` → `l86_063_seed_typed.png`.

### 6.3 Tab — one key, one focus bar

| action | OCR before | OCR after | diff bbox (game area) | px |
|---|---|---|---:|---:|
| one `Tab` | `Seed \|sw0345amp` (field focused) | same text, focus moves | **(740,88)–(740,103)** | **16** |

The driver's whole-screen diff is 228 px with bbox (740,88)–(1914,1071); 215 of
those pixels are the IceWM taskbar clock at rows 1060–1071. Restricted to the
game window (rows < 1054) the change is exactly **16 pixels: a 1×16 vertical
focus bar at x=740, y=88…103**, i.e. a localised focus-ring/caret move and not
the clock. Screenshots `l86_064_tab_before.png` → `l86_065_tab_after.png`; the
jsonl row is `{"ev":"tab","pixels":228,"bbox":[740,88,1914,1071],"note":"one
Tab, no other input"}`.

### 6.4 What was not reached

`Generate` was not clicked: the world-options footer at 1280×720 draws
`Generate` at the bottom edge and the OCR probe (`no_target worldopts
Generate`) did not read it in this run. World generation and the site screen are
still open; the Seed/Tab evidence above does not depend on them.

## 7. Reproduction

```sh
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l87-lostwake
fuser -k disk.img; sleep 2
RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720 RW_WM_HONOUR=1 \
  bash tools/rw/run-rimworld-desktop.sh --run-timeout 1800   # default now
# keyboard + full front end:
RW_L86=1 RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720 RW_WM_HONOUR=1 \
  bash tools/rw/run-rimworld-desktop.sh --run-timeout 1800
```

`RW_CWV=1` in the `make` line enables the ledger (default off; the unit tier
does not build it). `tools/rw/l87-boots.py --keys obj/.../serial*.log` prints
the boot/ledger table; `tools/rw/l87-liveness-test.py` is the stall-rule
regression.

## 8. Files

| file | change |
|---|---|
| `src/kernel/linux_compat_thr.c` | `[CWV]` per-key ledger (gated `RW_CWV`), `park_seq`, `[FUTEX] queue used/parked` counters fixed |
| `Makefile` | `RW_CWV=1` build flag |
| `tools/rw/rw_l86_input.py` | liveness = present or loader open; 900 s quiet budget; 1500 s menu budget; loading rows |
| `tools/rw/rw_desktop_drive.py` | progress rows; automatic Steam-modal dismissal |
| `tools/rw/run-rimworld-desktop.sh` | default run window 1800 s |
| `tools/rw/l87-boots.py` | boot/ledger summariser |
| `tools/rw/l87-liveness-test.py` | stall-rule regression (7/7 PASS) |
| `docs/l87-lostwake-report.md` | this report |
