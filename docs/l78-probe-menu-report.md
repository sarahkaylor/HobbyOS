# L78 — the block-stress probe survives the full window, and the game reaches
# the main menu

Lane `l78-menu`, worktree `~/hobbyos-rw-lanes/l78-menu` (branch `rw-l78-menu`,
seeded at the merged tip `e6fd3500`). Two questions: (1) can L75's block-stress
probe run for a full 1800 s **alongside** the game instead of stalling at ~70 s,
and (2) what does a menu run on the merged tree actually show?

Both answered: the probe ran 1800 s with **6 430 ops, 803 write-verify cycles,
0 bad, 0 mismatches**, and the game **reached the main menu** — the first time
on this ladder. The menu then presents at **1.00 fps** and is static; that is
the new wall, and it is a different one from `Initializing.`

## 0. Summary

| question | answer |
|---|---|
| does the probe survive a full window? | **yes.** P3: 1800 s, 29 heartbeats t=122…1806 s, ops 209 → 6 430 monotonic, reads 5 627, write-verify 803, `bad=0 mismatch=0 slow=0`, `busy` plateaus at 118 (token held by the loader; a skip, never a spin) |
| does the game survive next to it? | **yes.** Same window: 1 380 presents, the menu frame is the last frame, pid 5 alive with 25 threads and `res_mib=1330` at t=1800 s, **0 exceptions in Player.log** |
| what was L75's stall? | an unbounded `blk_token_acquire()` in the probe thread, a 33-op round with no rate limit, a shared 4 KiB stack buffer, and `[BKS]`/fault rows built from dozens of `uart_puts` (shredded by the game's console traffic). The L78 probe bounds every one of those (§1) |
| does the block layer stay clean? | **yes.** `stalls=0 devoff=0 hole=0` all window; one reset, the lane's own self-check at t≈1501 s, `cache1=cache2=raw1=raw2=0xFC31BFF590C22325 reinit_rc=0 ms=134 match=1` |
| does the game reach the menu? | **yes.** The `[XSW]` CRC sequence ends at `0xf88d9ec9e1a32c7f` with `changed=0` (a static menu), OCR of the frame reads the menu (`Tutorial`, `New colony`, `Quit to OS`, …), and the frame has 318 766 distinct colours |
| what does the menu cost? | **1.00 fps** (120 presents in the last 120 s of M1; 0.90 fps averaged over the menu phase of M1 and P3) |
| is `Initializing.` still the wall? | **no.** The loader completes, the Steam modal clears by itself within ~30 s (M2/P3; no input required), and the main menu is up. The next wall is the menu's own 1 fps / static frame — §4 |

## 1. The probe: what L75 shipped, and the L78 shape

L75's probe (`src/kernel/virtio_blk.c`) issued 24 reads + 3 write-verify cycles
per round, every round, and slept with `safe_wfi()`. It is recorded in
`docs/l75-blkreset-report.md` §2.5 as stopping ~70 s into the game's load, and
the L75 ladder was run **without** it.

The L78 probe keeps the same work (random 512…4096-byte reads over the free
tail of the namespace, plus read/write-back/verify cycles) but bounds every
resource it can touch:

| bound | value | why |
|---|---|---|
| operations in flight | **1** (`stress_inflight`, asserted per op) | the driver is single-inflight; the probe must not queue behind itself |
| rate | one op per **250 ms** (≤ 4 ops/s) | the game's plateau is ~150 ops/s, so the probe is ~3 % of the device's work |
| token wait | **60 ms deadline** (`blk_token_try_acquire`) | a busy device is a counted `busy` skip — never a spin, which is the L75 stall |
| wait between ops | `safe_wfi()` halt loop (250 ms) | not runnable, no scheduler state of its own, ~4 wake-ups/s |
| write-verify target | a cluster whose **FAT entry is 0** (`fat16_lba_scratch_geom` + a raw FAT-sector read under the token) | the probe can never stamp a live FAT/dir/data sector, and it writes back exactly the bytes it read |
| cache coherence | `blk_cache_invalidate(lba,1)` **after** the token is released | holding the device token across fill/bucket locks is the lock order that wedged the census (measured, §1.1) |
| buffers | static, not stack (`stress_buf[4096]`, `stress_back[512]`, `stress_fat[512]`) | the probe is single-inflight, and a 4 KiB local on a kernel-thread stack is a canary waiting to be smashed |
| failure handling | `bad`, `mismatch`, `busy`, `skip`, `slow` are counted rows; a slow op backs off 5 s; nothing retries | a failed verify is data for the report, not a reason to stop or loop |
| rows | one `struct kline` per row, one `uart_puts` | the console is shared with the game and a multi-put record is shredded at every call boundary (the L75 run lost its `[BKS] start` row that way) |

### 1.1 The two shapes that did not survive, and what they taught

Both were launched as 1800 s windows and both died at ~244 s. They are
recorded here because they are the evidence for the final shape, not because
they shipped.

| attempt | shape | outcome |
|---|---|---|
| P1 | 200 ms rate, `blk_cache_invalidate` under the token, 240 s self-check unchanged | census stops at t≈250 s; game restarts its Biotech def pass; `[STALLBREAK] cpu=3 lock=0x702382e8 owner_cpu=4 stale_ms=15390` (the address is inside `wr_locks[]`), `[FATW] CONC path=/RW_HOME/Player.log busy=2` twice; no self-check row at all — the census was parked in `blk_token_acquire()` |
| P2 | 250 ms rate, invalidate outside the token, self-check quiet-gated, **scheduler sleep** (`process_sleep_ms`) between ops | `KERNEL PANIC: stack smashing detected (SEC-4 canary)` at t=244 s, machine reboots |

The working hypothesis for both: the only new ingredients were a kernel
thread that changed its own scheduler state (`process_sleep_ms` →
`arch_yield`) and a 4 KiB stack buffer. The final probe has neither — it halts
between ops and its buffers are static — and the same window then ran clean
(P3). `process_sleep_ms` was removed from the tree again rather than left as
an unused, untested primitive.

The self-check also changed. L75 ran it at a fixed t=240 s in the 5 s census;
with the probe active that put a forced controller re-initialisation in the
middle of the loader's page-in stream, and the census' unbounded token acquire
then parked the census (P1). It now waits for **1.5 s of device quiet**
(forced at 1500 s, skipped with a row after 1750 s) and takes the token with a
deadline, so it can never block the census.

## 2. P3 — the probe window (1800 s, probe active, game running)

`RW_CHURN_SOAK=1` (kernel `RW_BLK_STRESS=1`), virgl, the desktop harness,
`--run-timeout 1800`, `RW_CLICK_IGNORE_AFTER=600`. Log
`obj/l78/serial-P3.log`; the table below is `tools/rw/l78-bks.py`.

| t (s) | ops | reads | write-verify | bad | mismatch | busy | skip | slow |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 122 | 209 | 183 | 26 | 0 | 0 | 5 | 0 | 0 |
| 243 | 575 | 504 | 71 | 0 | 0 | 9 | 0 | 0 |
| 303 | 688 | 602 | 86 | 0 | 0 | 76 | 0 | 0 |
| 423 | 1 030 | 902 | 128 | 0 | 0 | 118 | 0 | 0 |
| 603 | 1 720 | 1 505 | 215 | 0 | 0 | 118 | 0 | 0 |
| 904 | 2 899 | 2 537 | 362 | 0 | 0 | 118 | 0 | 0 |
| 1204 | 4 072 | 3 563 | 509 | 0 | 0 | 118 | 0 | 0 |
| 1505 | 5 247 | 4 592 | 655 | 0 | 0 | 118 | 0 | 0 |
| 1806 | 6 430 | 5 627 | 803 | 0 | 0 | 118 | 0 | 0 |

(29 heartbeats in total, one a minute, none missing; `inflight=0` on every
row.)

Device and game on the same window:

| instrument | value |
|---|---|
| `[BLKD]` `stalls=` / `devoff=` / `hole=` / `mmio=` | `0` / `0` / `0` / `1` on every row |
| resets | **1**, the self-check: `cache1=cache2=raw1=raw2=0xFC31BFF590C22325 reinit_rc=0 ms=134 match=1` |
| presents | **1 380** (`[XSW]`), last CRC `0xf88d9ec9e1a32c7f` at t=1737 s |
| menu | CRC first at game t=549 s; 1 070 presents over 1 188 s = **0.90 fps** |
| end state | `[THRS] pid=5 live=25 gt=25 … res_mib=1330`, `/NFS/RimWorldLinux`; game alive at the menu |
| Player.log exceptions | **0** |

The `busy` plateau (118 by t=423 s, never rising again) is the probe doing
what it was told: whenever the loader held the device token longer than 60 ms,
the probe skipped its op instead of waiting inside `blk_token_acquire()`. The
op count still rises at ~3.5/s for the rest of the window, so "busy" is not a
stall — it is a bounded, counted deferral.

## 3. The menu

### 3.1 Ladder

| run | kernel | probe | window | menu on screen | presents | end state |
|---|---|---|---:|---|---:|---|
| M1 | L78 | no | 1800 s | **yes**, from t=533 s (game clock) | 1 360 | menu at 99_final; `pid=5 live=25 res_mib=1330`; `[XSW]` last t=1721 s |
| P3 | L78 | yes | 1800 s | **yes**, from t=549 s | 1 380 | menu at the end; 0 exceptions |
| M2 | L78 | no | 1800 s | **yes**, from t=548 s (game clock) | 1 390 | menu at 99_final; `pid=5 live=25 res_mib=1326`; 0 exceptions |

The Steam modal: `Could not initialize Steam API` is real and appears during
the load (M1 `09_running.png`, M2 `11_click03.png`, both 263 153 colours; OCR
reads the modal). In M1 the lane's click helper (`RW_CLICK_IGNORE_AFTER`, QMP
`input-send-event`, the same brown-button finder as `tools/rw/click-ignore.py`)
clicked `805,768` while the modal was up, and the menu was on screen 32 s
later. M2 is the cleaner record: its shot 11 still shows the modal and its
shot 12 (30 s later, **no click**) is the menu; P3 never sampled the modal at
all. The modal clears by itself within ~30 s. It is a startup event, not the
wall — no input is required.

### 3.2 Evidence that the frame is the game's menu (M1)

1. **`[XSW]` changing CRCs.** 22 CRC transitions in M1:
   `0x9408…c95b → 0x2e26…5dfa → 0xcab8…3336 → 0xbc23…ad2f → … → 0xbb5a…cd1e
   → 0x9bcb…c6d0 → 0xf88d9ec9e1a32c7f`. The last transition is the menu, and
   the menu frame then stays `changed=0` — a static UI, not a frozen pipeline
   (the presents continue at 1 fps).
2. **Diff bbox not the panel clock.** `tools/rw/l73-frame-diff.py
   09_running.png 12_click03.png` (modal → menu): `DIFF pixels=412337
   (19.885 %)`, `bbox=(10,44)-(1912,1069) size=1903x1026`, clock region 90 px
   (**0.0 %** of the change), `VERDICT: CHANGING (not the clock)`. Two menu
   frames 30 s apart differ only in the clock (28 px, bbox
   `(1881,1063)-(1894,1069)`) — the menu is static.
3. **OCR.** `tesseract 15_click06.png`: "LUDEON studios — RIMWORLD / A story
   generator by Tynan Sylvester / Fiction primer / Tutorial / … / New colony /
   … / Help translate / Credits / Buy soundtrack / Quit to OS / English". That
   is the main menu, not the loading art (whose OCR is "z Fi").
4. **Colour count.** The first menu frame has **318 766 distinct colours**
   (the loading art: 27 895; the modal: 263 153; the desktop: 92).
5. **Rate.** 970 presents over 1 086.75 s = **0.89 fps** averaged over the
   menu phase; the last 120 s of M1 are exactly **120 presents in 120 s =
   1.00 fps**.

### 3.3 M2 — the second window, same evidence

`obj/l78/runM2.log`, `obj/l78/serial-M2.log`, `obj/l78/Player-M2.log`,
`obj/l78/shots-M2/` (72 shots preserved).

| instrument | M1 | M2 |
|---|---|---|
| menu on screen from (game clock) | t=533 s | t=548 s |
| `[XSW]` CRC transitions | 22 | 23 |
| menu CRC | `0xf88d9ec9e1a32c7f` | `0xf88d9ec9e1a32c7f` |
| presents (whole window) | 1 360 | 1 390 |
| presents at the menu | 970 / 1 086.75 s = 0.89 fps | 1 090 / 1 192 s = 0.91 fps |
| last 120 s | 120 presents = 1.00 fps | 110 presents = 0.92 fps |
| first menu frame, distinct colours | 318 766 | 318 766 |
| modal → menu diff | 412 337 px (19.885 %), bbox (10,44)-(1912,1069), clock 0.0 %, CHANGING | 412 265 px (19.882 %), bbox (10,44)-(1912,1069), clock 0.0 %, CHANGING |
| self-check | `… reinit_rc=0 ms=134 match=1` | `cache1=cache2=raw1=raw2=0xFC31BFF590C22325 reinit_rc=0 ms=75 match=1` |
| resets / stalls | 1 (self-check) / 0 | 1 (self-check) / 0 |
| Player.log exceptions | 0 | 0 |
| end state | `pid=5 live=25 res_mib=1330` | `pid=5 live=25 res_mib=1326` |

The last `[XO]` in M2 is `/Data/Biotech/Languages/English/Keyed/
Misc_Gameplay.xml` — the loader's last keyed file, the same one L75 recorded
at the end of its 1800 s windows. The difference is what happens after it:
the game now presents the menu instead of parking on `Initializing.`

## 4. What the menu changes about "the next wall"

`Initializing.` is passed. The loader completes, the game presents a real main
menu, and it stays there for the rest of a 1800 s window with the block layer
clean. The next wall is the menu itself:

* it presents at **0.9-1.0 fps** (one frame a second, static content),
  against the 12-16 fps the loading animation gets on the same device
  (`docs/rimworld-virgl-m3.md` §3.2). The engine is alive (`[FUTR]` 9-11
  rounds/s at the menu, 25 threads), so this is not a hang; it is the menu's
  render loop costing ~1 s a frame.
* the frame does not change on its own (CRC `changed=0`), so the next question
  is input: a click on `New colony` (or a hover) is the only way to tell a
  slow-but-live menu from one that has painted its last frame. The lane's
  click path (QMP `input-send-event`) is in the harness now, so that is a
  one-env-var experiment for the next lane.
* the Steam modal is a startup event, not a wall: it clears by itself within
  ~30 s (M2 shot 11 → shot 12, no input) and the game continues.

The rows that would have been "the stop" if the game had stopped, taken at
the menu instead (M2, t=1800 s):

```text
[XO]    last = /Data/Biotech/Languages/English/Keyed/Misc_Gameplay.xml  (1 208 rows)
[FUTR]  t=1777s ops=192512 d=2048 rate=9/s dt=207997ms
        keys=0x10E089D218:187w187k,0x113000A460:552w567k,0x10895B9A50:1w6k
[THRS]  pid=5 live=25 gt=25 hw=44 slots=103/191 regs=345 anon=254
        anon_mib=6412 big_mib=255 stk=3 res_mib=1326 pk_mib=1830 /NFS/RimWorldLinux
[XSW]   n=1390 crc=0xf88d9ec9e1a32c7f changed=0 w=1920 h=1080 box=0,0,1915,1017
[BLKD]  reqs=267747 stalls=0 devoff=0 cc0=1 resets=1 hole=0
Player.log: 0 exception lines
```

The round rate fell from 264/s during the load (M1 t=551 s) to 9/s at the menu:
the engine is idle, not looping. Nothing in this window names a block-layer or
page-in cause for the 1 fps: the device is idle at the menu (`reqs` barely
moves), `hole=0`, one reset (the self-check), and the probe's 803 write-verify
cycles all read back.

## 5. Regression

| gate | result | log |
|---|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **434 run, 0 failed — UNIT TESTS PASSED** | `obj/l78/units.log` |
| `m3_window` | **PASS** (25 s) | `obj/l75/gates/m3_window.log` |
| `m4_gl` | **PASS** (65 s) | `obj/l75/gates/m4_gl.log` |
| `rwnfs` | **PASS** (14 s) | `obj/l75/gates/rwnfs.log` |

`bash tools/rw/l75-gates.sh m3_window m4_gl rwnfs`, after the desktop harness
had exited; the unit tier and the harness were never concurrent.

Net: **434/0 units + 3/3 gates + 3 x 1800 s windows** (P3 with the probe, M1
and M2 without), with the menu reached in all three and the block layer clean.

## 6. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l78-menu

# the probe window: probe + game + the Steam-modal click
RW_CHURN_SOAK=1 RW_QEMU_GL=1 RW_GLMODE=virgl RW_VIEWPORT=1 \
  RW_CLICK_IGNORE_AFTER=600 RW_WAIT_SECS=1950 \
  tools/rw/run-rimworld-desktop.sh --timeout 600 --run-timeout 1800

# the menu window: no probe
RW_QEMU_GL=1 RW_GLMODE=virgl RW_VIEWPORT=1 RW_CLICK_IGNORE_AFTER=600 \
  RW_WAIT_SECS=1950 \
  tools/rw/run-rimworld-desktop.sh --timeout 600 --run-timeout 1800

# the numbers
tools/rw/l78-bks.py  obj/l78/serial-P3.log        # probe survivability table
tools/rw/l78-wall.py obj/l78/serial-M2.log --player obj/l78/Player-M2.log
```
