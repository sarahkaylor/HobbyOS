# cp-hunt — the crash and glitch hunt (wave 8: lane H1-hunt; wave 9 round 3: lane H6-hunt3)

Plan: `docs/cyberpunk.md` §6 P8/P9, wave 8.  Branch `cp-hunt` from integ15 =
a23bfbeb (no kernel or product code changed by this lane).  Evidence:
`docs/cp-hunt-evidence/*.txt` (short excerpts only; the screendumps and WAVs
are game content and stayed on strix, then were deleted).

Every number is from **strix** (Ryzen AI MAX+ 395, Zen 5, AVX-512, KVM
`-cpu host -smp 8`, RX 9070 XT on renderD129 through `tools/cp/host-venus.sh`),
tree `~/cp/lanes/hunt` = integ15, Wine `~/cp/wine-cp` (X11 + ntsync +
winealsa), Venus ICD a copy of `~/cp/integ-venus` (`~/cp/lanes/hunt-venus`),
seeded prefix a copy of integ's `obj/cp-p8/pfx-seeded.img` (sha256
`b56d5231…`).  P8 boots: 12 GiB; P9 boots: 32 GiB under
`/tmp/cp-bigguest.lock`.  Other lanes' guests ran beside every boot (H2-early,
H3-winefix, H4-kglitch, the integ16 gate batch); the host sampler lists them.

## Round 3 (integ21 = 5f8c4b76, then integ22 = 010e9fd2; lane H6-hunt3)

Branch `cp-hunt3` (from `cyberpunk` = integ21; no kernel, Wine, Mesa or
grader code changed); strix tree `~/cp/lanes/hunt3`, raw evidence under its
`obj/hunt-logs/` (run logs, serial copies, classifier output, drive logs);
short excerpts in `docs/cp-hunt-evidence/round3/`.  Wine `~/cp/wine-cp2`
(read-only), Venus ICD a copy of H1's `hunt-venus` (`~/cp/lanes/hunt3-venus`;
`tools/mesa/venus` has no diff since integ15), seeded prefix from this tree's
own `obj/cp-p8/pfx-seeded.img` (made with wine-cp2, 07:55; no wineboot
update on any boot).  Every boot: `tools/cp/qemu-slot.sh` (one slot, one game
boot of this lane at a time), `-smp 8`, Venus on renderD129 through
`tools/cp/host-venus.sh`, the game copy only through run-p8.sh's refcounted
export, saves only from `~/cp/game-profile/` (ManualSave-86, AutoSave-16).
**Memory:** the cold boots, the load-churn and the settings boots ran at
`P9_MEM=12288` (12 GiB, no big-guest lock: H7-fault held
`/tmp/cp-bigguest.lock` with its own 32 GiB P9 boots for most of the
afternoon; the game peaks at 4.6 GiB resident and the guest's frame pool
never fell below 6.8 GiB free); the two soaks ran at 32 GiB under the lock.
Other lanes' guests ran beside most boots (`fault`, `fault-base`, `fault-dev`;
the host sampler lists them).

**Boots:** 44 game boots, 2026-11-18 14:55 – 2026-11-19 01:34.  On integ21:
20 scripted cold boots (h3-c01..c20, 12 GiB), 2 one-hour soaks (h3-soakA/B,
32 GiB), 9 interactive boots driven from the host by `tools/cp/hunt/drive.py`
(load churn h3-churn1..4, settings h3-set1..5, 12 GiB) and 3 hand-driven
exploration boots (h3-ctl1..3) that found the menus, the in-world load, the
exit to the main menu and the Settings layout.  On **integ22** (010e9fd2,
H7-fault's fault/NFS round, merged into `cp-hunt3` at 8878b04a and rebuilt; the
integrator asked for the clean-exit and every-save counts there): 10 more
cold boots h3-d01..d10 (5 quitting from the pause menu, 5 from the main menu,
5 per save).  Every count below names its kernel.

**Headline.**
* **One crash, a new one:** H6-07, a Venus device loss in h3-c06 (1 of
  44 game boots).  The host stalled for ~5 s, the Venus fences stopped retiring,
  and the kernel's submit2 gave up after its 5 s bound; vkd3d-proton got
  `VK_ERROR_DEVICE_LOST` and the game exited with code 3.  The kernel fix is
  H7-fault's.  Device-lost rate: integ21 1/34 boots, integ22 0/10 (not
  enough boots to call a change; the trigger is the host, see H6-09's stall).
* **Cold boots (item 1):** 20 of 20 at 12 GiB reached the world from a save
  (10 × ManualSave-86, 10 × AutoSave-16), and 10 of 10 more on integ22.  0 early exits, 0 hangs, no kernel
  fault row except h3-c06's crash path and one recovered `[TLBTIMEOUT]` on
  integ22 (h3-d05, H6-09, for H7-fault), 0 grader-misclassified boots among
  them; QEMU start → main menu median 121 s, LOAD GAME → PRESS … TO CONTINUE
  median 82 s (71–98).  None of the 44 boots hit the integ23 `away_clock_set`
  #DE (H6-08).
* **Long sessions (item 2):** two ≥ 60-minute soaks, 62.4 and 67.3 minutes in the world, both quit cleanly.  No thread
  growth, no fd or EMFILE trouble, the guest's free memory flat after the
  first half hour.  The kernel's `[THRS]` census stops at 41 minutes in every
  boot that long (H6-02).
* **Load churn (item 3):** one 72.8-minute session with **12 loads of 10
  different saves** through the pause menu, then **5 × EXIT TO MAIN MENU →
  LOAD GAME**.  Every one of the 27 pause-menu loads in this lane's boots and
  every menu load reached the world.  In-world loads take 66–91 s (median 73).
* **Settings churn (item 4):** **10 applied resolution / window-mode changes**
  in one session (720p ↔ 1080p twice, Borderless ↔ Fullscreen twice, Borderless
  ↔ Windowed once).  No crash, hang, device-lost row, black or torn frame.  The
  Quick Preset was not changed (the driver could not reach its row; H1 round 2
  has 1/1).
* **Clean exit (item 5):** QUIT GAME from the pause menu 23/23 (18 on integ21, 5 on integ22), from the main menu
  11/11 (6 on integ21, 5 on integ22; 3 more never got to it: H6-04).  Every one: the game's status 0 within 2–4 s of the Enter, `wineserver
  -k` in 144–297 ms, no process left alive.  But p8launch's own keepalive
  stays an unreaped zombie and holds every boot's teardown for 30 s (H6-03;
  it also trips the console watchdog in 12 of 30 cold boots).
* **Every save loads (item 6):** ManualSave-86 33/33 loads reached the world (28 on integ21, 5 on integ22), AutoSave-16 21/21 (16 on integ21, 5 on integ22) (the
  profile holds only these two).
* **Harness / grader:** H6-01 (the first-run guard fires on the in-game
  Settings GAMEPLAY tab), H6-04 (the main menu after EXIT TO MAIN MENU hides
  its keyboard focus: 2/2 after a pointer wiggle, 1/7 without), H6-05
  (`script-done` while the game still runs), all with diffs or workarounds.

### Coverage (the brief's items, in order)

| item | asked | ran | result |
|---|---|---|---|
| 1. rare-failure rate | ≥ 20 cold boots to an in-world load from a save, mostly Venus | 30 cold boots h3-c01..c20 (Venus, 12 GiB; `r3/cold-*.p9`), plus 13 interactive boots that also loaded a save cold (h3-ctl1..3, h3-churn1..4, h3-set1..5) | **30/30 reached the world** (integ21 20/20, integ22 10/10; 15 × ManualSave-86, 15 × AutoSave-16).  Early exits 0/30, hangs 0/30, **crash 1/30** (h3-c06, integ21: H6-07, a Venus device loss); kernel rows: h3-c06's crash path, one recovered `[TLBTIMEOUT]` in h3-d05 (integ22, H6-09); grader-misclassified 0/30 (the 3 `seek-failed:menu-quit` boots are harness failures, graded as such: H6-04); visual: no corrupt frame (shotcheck flagged only loading-screen fades, the desktop after the quit, and one `garbage?` candidate in 6216 shots: h3-churn3 s270, a bright frame between the prologue's load screen and its tutorial popup, not seen at the other 20 AutoSave-16 loads).  QEMU start → main menu median 121 s (105–176), LOAD GAME → PRESS … TO CONTINUE median 82 s (71–98), world fps median 29–43 (12 GiB guests) |
| 2. long sessions | 2 × ≥ 60 min in the world (P9, ≤ 32 GiB) | h3-soakA (62.4 min in the world, audio) and h3-soakB (67.3 min, the map once per phase), both P9 32 GiB, integ21 | 2/2 ran their hour and quit cleanly; 0 kernel fault, EMFILE, `grab_object` or device-lost rows; game threads flat (48 / 47); guest free memory −332 / −272 MiB, flat over the last half; no monotonic fps drift (phase medians 22–44 fps, a trough in phases 7–10 of both: the walk's route), 1 + 4 hitches, all host-side (a 2 s present flip; two 2–3 s Venus fence stalls during host I/O pressure); audio: 5 WAV dropouts, all in the load and the first 10 s of the world, none in the following 62 min; `[THRS]` gone after 41 min (H6-02) |
| 3. load churn | ≥ 10 different saves back to back through pause → LOAD GAME → world; ≥ 5 × quit-to-main-menu → load | h3-churn4: **12 pause-menu loads, 10 different saves** (QuickSave-3, -2, ManualSave-86, QuickSave-1, -0, AutoSave-16, QuickSave-4, ManualSave-87, -88, -89; QuickSave-1 and ManualSave-87 twice) in one 72.8-minute session, then **5/5** EXIT TO MAIN MENU → title → LOAD GAME → world (ManualSave-86 / AutoSave-16 alternately, then ManualSave-92), then QUIT GAME: clean.  Before it, h3-churn1..3 (5 + 6 + 4 pause-menu loads) ended on driver bugs, never on the game | 27/27 pause-menu loads and 5/5 menu loads reached the world; in-world load (confirm → HUD) 66–90 s (median 73 s); EXIT TO MAIN MENU → title 25–88 s; 0 kernel fault rows, 0 device-lost rows; game threads flat at 47 (peaks 60–62 inside loads), regions 1387 → 1422 over 12 loads, resident 4.5–4.6 GiB, the guest's free memory back to 10.9 GiB at the end (from 10.3 at the start); the `[THRS]` census stopped at 41 min (H6-02) |
| 4. settings churn | 720p ↔ 1080p, fullscreen ↔ windowed, quality preset, in the world | h3-set2 (X screen 1920x1080 so the game offers 1920x1080): **10 applied changes** in one session: Resolution 1280x720 → 1920x1080 → 1280x720 twice, Windowed Mode Borderless → Fullscreen → Borderless twice and Borderless → Windowed → Borderless once (the game's own "Are you sure you want to apply these changes?" CONFIRMed each time), ~10 s in the world after each; then QUIT GAME, clean | 10/10 applied without a crash, hang, device-lost row or kernel row; 0 of 639 screendumps flagged (no black, torn or garbage frame); the presentation path followed each change (zero-copy scanout at 1080p borderless/fullscreen, put_image for the 720p window on the 1080p screen).  **Quick Preset: not changed** (h3-set3..5, 9 tries): Enter right after the tab switch does nothing, one Down lands on the section title whose Enter asks "restore default settings for this section?" (cancelled), and the driver could not place the row after the list scrolls; H1's round-2 h2-ctl1 has the only preset change (1/1, Custom → Ray Tracing Overdrive) |
| 5. clean exit | QUIT GAME from the main menu ≥ 5 and from the in-world pause menu ≥ 5; the game exits, wineserver ends, nothing left | pause menu: 10 cold (h3-c01, c04, c05, c08, c09, c12, c13, c16, c17, c20), 6 drive boots, 2 soaks on integ21; 5 cold on integ22 (h3-d01, d03, d05, d07, d09).  Main menu (after EXIT TO MAIN MENU → title → main menu): 9 cold on integ21 (6 got to QUIT GAME), 5 on integ22 (h3-d02, d04, d06, d08, d10) | **pause menu 23/23, main menu 11/11 clean**: `[P8] end game status=0` 2.0–4.3 s after the Enter (main menu: no confirmation dialog, 1.8–3.0 s), `wineserver -k` status 0 in 144–297 ms, quiesce: no live process left; sync and `[P8] done` every time.  The one leftover is p8launch's own keepalive, exited but never reaped (H6-03: 34/34 boots that reached the teardown), which costs every boot 30 s and trips the console watchdog in 12 of the 30 cold boots |
| 6. every save loads | each save in `~/cp/game-profile/` at least once (k/n) | ManualSave-86 and AutoSave-16 (the only two) | **ManualSave-86 33/33** loads reached the world (integ21 28: 10 cold, 9 drive-boot starts, 2 soaks, 7 from the pause or main menu in sessions; integ22 5); **AutoSave-16 21/21** (integ21 16, integ22 5).  Every AutoSave-16 load opens the game's tutorial popup ("WELCOME TO CYBERPUNK 2077! … [E] continue"); Esc closes it |

### The 60-minute soaks (item 2)

P9, 32 GiB under `/tmp/cp-bigguest.lock`, ManualSave-86, `r3/soak60.p9`
(h3-soakA, with audio to the `wav` backend) and `r3/soak60-map.p9`
(h3-soakB, the map opened and closed once per phase, no audio); both with
`-DP8_FD_CAP=4096` so the launcher's 30 s fd probe tries 4096 opens.
World = phases `soak01..soak14` (~4.45 min each), then QUIT GAME from the pause
menu.

| | h3-soakA | h3-soakB |
|---|---|---|
| time in the world | 62.4 min (14 phases) | 67.3 min (14 phases; soak11's loops ran in the hub menu: `M` opened the hub and one Esc cleared the map words, so its 85 fps median is a menu's) |
| verdict / exit | script-done; QUIT GAME → `end game status=0` after 66.3 min of game process; `wineserver -k` 236 ms; clean | script-done; QUIT GAME → `end game status=0` after 71.0 min of game process; `wineserver -k` 218 ms; clean |
| world fps median per phase | 38.0, 43.2, 34.6, 33.6, 41.9, 43.5, 31.9, 27.2, 23.6, 23.4, 26.9, 34.6, 38.9, 41.0 | 42.6, 43.0, 34.9, 36.4, 36.9, 43.0, 27.4, 28.2, 22.8, 22.4, (85.0 hub), 24.3, 30.9, 38.3 |
| world p10 per phase (min) | 19.2–39.9 (6.2) | 18.2–37.5 (0.3) |
| hitches (guest-time fps-row gap > 2.5 s) | 1 (soak09, 3.07 s: one present `flip` of 2.05 s, `[HBP] … flip=148827/2045504`; no kernel row) | 4, all in soak13 (gaps up to 4.94 s): two `[VENUS] stall` rows (ring-1 fences unretired 2.0 s and 3.0 s) while the host sampler skipped 3–4 s at a time and the host's PSI io "full" was 51–58 %: H6-07's trigger, below its 5 s bound |
| 9070 XT busy, world | median 37 %, per phase 33–40 % | median 37 % |
| `[THRS]` (game) | threads 15 → 48 at load, **48 flat**; regions 1382 → 1474 (+2.2/min) and resident 4564 → 4668 MiB in the 41 min it lasted; **stopped at serial L27196** (H6-02) | threads 18 → 47, **47 flat**; regions 1374 → 1466, resident 4577 → 4678 MiB; **stopped at serial L27257** (H6-02) |
| guest free memory (launcher census, 30 s) | 27734 MiB at soak01 → 27402 at soak14 (−332 MiB, flat from soak08 on: 27411 → 27402) | 27683 MiB at soak01 → 27411 at soak14 (−272 MiB, flat from soak06 on) |
| fd probe (4096 opens / 30 s) | 1021 opened, errno 24, in **134/134** censuses: the probe's own per-process limit, constant; the system table never ran short | 1021 in **143/143** |
| processes (census) | 12 throughout → 9 after the game | 12 throughout → 9 after the game |
| EMFILE / `grab_object` / device-lost rows | 0 / 0 / 0 | 0 / 0 / 0 |
| kernel rows | 0 fault; 5 `[NFSPIPE] stall` (151–172 ms) | 0 fault; 8 `[NFSPIPE] stall`; 2 `[VENUS] stall` (above) |
| audio (WAV, `wavgaps.py`) | 3966.6 s of audio, **5 dropouts (500 ms in all), every one in the save's loading or the first 10 s of the world (game s 155–241), 0 in the following 62 min**; 0 gaps ≥ 500 ms, 0 edge clicks; WAV / wall 0.988 | (no audio) |
| `[SND]` / `[AWAY]` | 6 `[SND] xrun` rows (5 within 0.3 s of the stream's start, 1 at 43 min with a 742 ms write gap: the soak09 flip); 6 `[SND] zeros`; 27 `[AWAY-Z]`; `[AWAY]` capped at 400 | (no audio) `[AWAY]` capped at 400 |

**W3 / W2 watch.**  No thread growth (48 game threads for the 41 minutes the
census lasted, and the process count flat at 12 for the whole hour) and no
EMFILE or `grab_object` row in either soak or in any of this lane's
44 game boots.  The last ~20 minutes of each soak have no
thread count (H6-02).

### Round 3 findings

| ID | symptom | rate | first-seen evidence | signature | layer | severity | status |
|---|---|---|---|---|---|---|---|
| **H6-07** | **crash**: in the prologue world (AutoSave-16), the Venus fences stopped retiring for ~5 s while the host stalled; the kernel's submit2 found all 16 fence slots in flight for 5 s and returned -EIO, the Venus driver reported `VK_ERROR_DEVICE_LOST` to vkd3d-proton, and the game went down its crash path (CrashReporter.exe, then its own int3) and exited with status 768 (exit code 3) | 1/44 game boots of this lane (h3-c06, integ21; integ22 0/10); 0 in H1's 40 (no `submit2: no fence slot` row in hunt-logs / hunt2-logs); near misses: h3-soakB's two 2–3 s fence stalls in the same host conditions (PSI io full 51–58 %, sampler gaps), recovered | `cp-hunt-evidence/round3/h6-07-venus-device-lost.txt` | `[VENUS] submit2: no fence slot for 5 s` → `err:vkd3d-proton:d3d12_command_queue_execute: Failed to submit queue(s), vr -4.` → `wine: Unhandled exception 0x80000003 … at address 0000000142FAA013` → `[P8] end game status=768` | host stall (QEMU's main loop not running: sampler gap, PSI io full 24.6 %) × kernel policy (`src/kernel/virtio_gpu_3d.c` submit2's 5 s fence-slot bound) | **crash** | open: H7-fault owns the kernel fix (integrator, wave 9) |
| **H6-01** | boot_p9's first-run guard stops a boot on the game's own Settings → GAMEPLAY tab: its row "Share game telemetry" matches `boot_p8.FIRST_RUN`'s `telemetry` pattern.  No first-run screen was shown or answered; the stop is the safe direction, but no P9 script can visit that tab | 1/1 boots that showed the tab (h3-ctl2) | `cp-hunt-evidence/round3/h6-01-first-run-guard-settings.txt` | `FIRST-RUN SCREEN 'telemetry' at s114-t000619-tb2: stopping without answering it` | harness (P9 grader, `tools/cp/p9/boot_p9.py` `shoot()` + `boot_p8.FIRST_RUN`) | harness | open: worked around (drive.py never enters GAMEPLAY: SOUND → 5 × left, GRAPHICS → right); optional diff below for the integrator |
| **H6-02** | the kernel's `[THRS]` census (threads, regions, anon / resident MiB per Linux-ABI group, every 5 s) stops for good after 4000 rows per boot: with the game's 7–8 Wine groups that is ~41 minutes, so a 60-minute soak has no thread / memory sample for its last ~20 minutes (the W3 TEB-leak watch goes blind) | every boot longer than ~41 min: 2/2 drive sessions over 41 min (h3-churn1 at serial L32250, h3-churn4 at L40883); both soaks (h3-soakA at L27196, h3-soakB at L27257): 4/4 | `cp-hunt-evidence/round3/h6-02-thrs-cap.txt` | `-- [THRS] trend: 4000 rows = the kernel cap (4000): the census STOPPED at serial L32250` (classify.py) | kernel instrument (`src/kernel/process.c` `rw_thr_sample()`, `rows < 4000`) | cosmetic (blinds a leak detector) | open: H7-fault owns the kernel fix |
| **H6-03** | p8launch never reaps its keepalive (`wine vkcube.exe --idle`): after `wineserver -k` every game boot's quiesce lists `vkcube.exe/17:s4` (PROC_STATE_EXITED, waiting for its parent's waitpid) and waits out its whole 30 s bound before the sync | every game boot that reached the launcher's teardown: 34/34 (integ21 24, integ22 10); the 30 s silent wait also trips the console watchdog (`[WATCHDOG] console silent for more than 20s` between `end kill` and `quiesce after kill`) in 12/30 cold boots (integ21 8/20, integ22 4/10) | `cp-hunt-evidence/round3/h6-03-keepalive-unreaped.txt` | `[P8] quiesce after kill: extra=1 left: vkcube.exe/17:s4` | harness (`tools/cp/p8/p8launch.c` `quiesce()`; only `sweep()` reaps `keep_pid`) | perf (30 s per boot) + a false "process left behind" | open: diff below; classify.py counts `:s4` as exited-unreaped, not left behind |
| **H6-04** | after EXIT TO MAIN MENU the main menu shows no keyboard focus for 8 Down taps when a relative-mouse move (H1-12's workaround) came just before the taps; with no pointer move the first tap shows it | with the wiggle 2/2 (h3-c02, h3-c03, integ21); without it 1/12 (h3-c18; found in 4 taps in h3-c07, c10, c11, c14, c15, c19 on integ21 and 5/5 on integ22); drive.py, no pointer move: 0/5 (h3-churn4) | h3-c02 / h3-c03 run logs (`focus s0NN-…-menu-quitK: no highlighted row` × 9) vs h3-churn4 drive log (`seek LOAD GAME: tap 1 focus SETTINGS`) | `SEEK FAILED menu-quit: main menu row 'QUIT GAME' not focused after 8 x down` | game UI (a pointer move switches the menu to pointer mode) × harness | harness | open (H1-12's class): the wiggle is gone from `r3/cold-*-mq.p9` (from h3-c06 on); `seek` has no recovery when the focus stays hidden |
| **H6-05** | a P9 boot whose interactive script ends with `done` (or a play script that ends while the game still runs) gets the pass verdict `script-done`, and QEMU is then stopped under the running game: the verdict does not say whether the game exited.  3 of this lane's drive boots ended that way after a driver failure; the classifier's "clean exit" column caught them | 3/3 such boots (h3-set1, h3-churn2, h3-churn3) | r3table.py (`verdict script-done`, `clean no`, `agree NO`) | `verdict: script-done; QEMU stopped, slot released` with no `[P8] end game` row | harness (P9 grader, `boot_p9.py` end of phase 2) | harness (grader) | open: diff below |
| **H6-06** | one 2.6 s frame (a Venus ring-1 fence waiting 2 s) while EXIT TO MAIN MENU unloads the world | 1 of 20 exit-to-main transitions (h3-churn4 #5); 0 in-world | h3-churn4 `serial.txt` L60525 | `[VENUS] stall pid=93 tid=122 waits_ms=2000 ctx=11 ring=1` + `[HBP] … n=1 fps=0.4 … sub=2639618` | game / Venus (H1-15's class, host side not sampled) | perf (a menu transition) | open, observation |
| **H6-08** | the integrator's integ23 #DE (`Vector: 0`) in `away_clock_set` (the `[AWAY]` instrument; two CPUs set one ntsync event, the window divisor wraps to 0) | **0/44** boots of this lane (no `Vector: 0` row in any serial; integ21 34, integ22 10) | integ23 gates (the integrator's) | `[KERNEL] … Vector: 0` with rip in `away_clock_set` | kernel (instrument) | crash | **fixed in cac5d3bd** (integ23) |
| **H6-09** | a TLB shootdown went unacked while the CPU it waited for held the console for 2 s (`[CONS] takeover … holder=3 … watched_us=2000003`) during a host I/O stall; one game anon fault took 6.4 s; the boot recovered and passed | 1/10 integ22 boots (h3-d05, during the game's launch, before the menu); 0/34 on integ21 | `cp-hunt-evidence/round3/h6-09-tlbtimeout-host-stall.txt` | `[TLBTIMEOUT] shootdown root=0x00000002154A2000 unacked cpus=0x0000000000000008` + `[PFX] F tid=92 … us=6368643 kind=anon` | kernel paging (TLB shootdown × console) under a host stall (sampler gaps of 7 s, PSI io full 54–64 %) | hang (recovers) | open → H7-fault (kernel paging) |


### Notes on the round-3 rows

**H6-07 (the crash).**  In h3-c06, ~70 s into the prologue world (AutoSave-16),
the game's ring-1/ring-3 Venus fences stopped retiring.  Two seconds later
the kernel's stall instrument printed (`[VENUS] stall … waits_ms=2001`), and
three seconds after that `virtio_gpu_3d_submit2()` found all 16 fence slots
still in flight and gave up (`[VENUS] submit2: no fence slot for 5 s`,
-EIO).  The guest Venus driver turned that into `VK_ERROR_DEVICE_LOST`
(vkd3d-proton: `Failed to submit queue(s), vr -4`), the game's crash path
started CrashReporter.exe (which cannot run without mscoree, as in every
earlier crash census), hit its own `int3` (`Unhandled exception 0x80000003 …
at 0000000142FAA013`), and the game exited with status 768 (exit code 3) 10 s
later.  The kernel had nothing to do with the fault itself: the only kernel
row after it is the dying process's jump into an unmapped hole.  The host was
the trigger: run-hunt.sh's 1 Hz sampler missed four seconds (20:32:59–20:33:02)
and the host's PSI io "full" avg10 rose from 3.4 % to 24.6 % at that moment,
with H7-fault's 32 GiB Venus guest and a 12 GiB guest beside this one; the
guest's console waited 2 s on its UART at the same time (`[CONS] takeover …
watched_us=2000005`), i.e. QEMU's main loop, which retires the fences and
drains the serial port, was not running.  This is H1-15's class (round 1: a
7.7 s fence stall that recovered); this time the kernel's 5 s bound made it
fatal.  Linux's virtgpu EXECBUFFER has no such bound; the kernel fix (a longer bound in
`src/kernel/virtio_gpu_3d.c` submit2) is H7-fault's (integrator, wave 9).
A check for whoever takes it: stall QEMU's main loop for 6 s during a Venus
world phase (H4's console test injects host stalls the same way); before the
diff the game should lose its device, after it the frame should recover.  Not
run here (it means stopping a game boot's QEMU from the host, outside this
lane's brief).

**Host stalls (H6-07, H6-09, the soaks' hitches).**  Every severe event in
this round came with strix itself stalling: run-hunt.sh's 1 Hz sampler
missing 3–7 s at a time while `/proc/pressure/io` "full" avg10 stood at
25–64 % (h3-c06 20:33, h3-soakB 00:19, h3-d05 00:55).  At 00:19 and 00:55 the
sampler saw no other lane's guest, so the pressure is not only other lanes'
boots.  What the guest sees is QEMU's main loop not running: Venus fences
unretired (H6-07 fatal at 5 s, soakB 2–3 s recovered), the console waiting on
its UART (`[CONS] takeover … watched_us≈2000000`), and in h3-d05 a TLB
shootdown that the CPU stuck in that console wait could not ack in time
(H6-09, recovered; for H7-fault, kernel paging).  The host side is not this
lane's to change; the sampler's `io_full10` column is the thing to watch.

**H6-04 (the main-menu focus after EXIT TO MAIN MENU).**  cold-*-mq.p9 first
did what H1 recommended for H1-12, a relative-mouse wiggle (`look 60 0`, `look
-60 0`) before the main-menu seek; both boots that did (h3-c02 one second
after the menu appeared, h3-c03 after a 10 s settle) saw no highlighted row
for 8 Down taps.  drive.py's seeks after the same exit never moved the
pointer and found the focus after one tap, 5/5 (h3-churn4).  With the wiggle
removed from the scripts (from h3-c06 on): 11 of 12 found it in 4 taps (h3-c07, c10, c11, c14, c15, c19 on integ21; h3-d02, d04, d06, d08, d10 on integ22), and h3-c18 again saw no highlighted row for 8 taps.  So in this state the
pointer move is what hides the keyboard focus (the menu switches to pointer
mode with the cursor off every row), the opposite of what H1 saw after the
Settings sub-menu.  `seek` has no fallback once the focus is hidden.

**H6-05 (script-done with the game still running).**  `boot_p9` grades the
play script, not the game: a script that ends (`done`, `stop`) leaves the game
running and the boot stops QEMU under it.  That is by design for fpsprobe /
soak (`stop`), but the verdict text is the same whether the game quit
cleanly or was cut off.  h3-set1, h3-churn2 and h3-churn3 were cut off after a
driver failure and still read `script-done`.  r3table.py flags them
(`clean no`, `agree NO`).  Proposed (`tools/cp/p9/boot_p9.py`, after `mark(f"play
script ended: {verdict}")`):

```diff
+            if verdict in ("script-done", "script-stop") and "[P8] end game " not in seen:
+                # cp-hunt H6-05: the script ended with the game still up; QEMU is stopped under it
+                why = (why + "; " if why else "") + "the game was still running (no QUIT GAME)"
+                mark(f"verdict detail: {why}")
```

**H6-01 (the first-run guard on the in-game Settings).**  boot_p9 checks
every screendump's OCR against `boot_p8.FIRST_RUN` and stops the boot on a
match.  The game's Settings → GAMEPLAY tab lists "Share game telemetry", so a
boot that shows that tab stops (h3-ctl2).  The stop is the safe direction and
nothing was answered; it only means no script can visit that tab.  drive.py
steers around it (Settings reopen on the tab they were closed on: from SOUND
five times left, from GRAPHICS once right).  If the integrator wants scripts
to pass that tab, one way (`tools/cp/p9/boot_p9.py` `shoot()`), with the tab
bar as the proof of an in-game menu:

```diff
             flat = " ".join(text.split())
             log(f"shot {name}: OCR {flat[:200]!r}")
+            # cp-hunt H6-01: the game's own Settings (its tab bar) list "Share game telemetry"
+            in_settings = re.search(r"SOUND\s+CONTROLS\s+GAMEPLAY\s+GRAPHICS", flat, re.I)
             for fr_name, rx in P8.FIRST_RUN:
-                if rx.search(flat):
+                if rx.search(flat) and not (fr_name == "telemetry" and in_settings):
                     raise FirstRun(fr_name, name)
```

**H6-02 (`[THRS]` stops at 4000 rows).**  `rw_thr_sample()` keeps one
boot-wide `rows < 4000` budget; with the game's 7–8 Wine groups printing a row
each every 5 s it runs out ~41 minutes into a game boot, and every soak loses
its thread and region census for the rest.  The kernel change is H7-fault's
(integrator, wave 9); classify.py reports where the census stopped.

**H6-03 (the unreaped keepalive).**  Proposed (`tools/cp/p8/p8launch.c`
`quiesce()`); every game boot's shutdown gets 30 s shorter and the leftover
list empty:

```diff
 static __attribute__((unused)) void quiesce(const char *tag, int secs) {
   int extra = 0;
   for (int t = 0; t < secs * 4; t++) {
+    /* cp-hunt H6-03: reap this launcher's own exited children first (the
+       keepalive from session_up() is one); an exited, unreaped child counts
+       as "extra" (vkcube.exe/N:s4) and holds the quiesce for its whole bound */
+    int rs;
+    while (waitpid(-1, &rs, 1 /* WNOHANG */) > 0) { }
     extra = extra_procs(0);
```

### Round 3 boots (r3table.py)

`menu` = QEMU start → main menu (s); `load` = LOAD GAME → PRESS … TO CONTINUE (s); `exit2t` = EXIT TO MAIN MENU's confirm → the title (s); `quit` = the quit's Enter → `[P8] end game` (s); `clean` = game status 0, `-k` 0, nothing alive after, sync, done; `shots` = shotcheck flags / shots; `agree` = the verdict agrees with the boot (NO = H6-05).  h3-c*/ctl/churn/set/soak: integ21; h3-d*: integ22.  The interactive boots' loads are in their drive logs (`load` and `world` columns cover scripted `expect`s only).

| run | verdict | menu | load | world | exit2t | quit | clean | kill_ms | left | faults | stalls | hitch | wmed | shots | devlost | agree |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| h3-ctl1 | expect-failed:loaded2 | 105.7 | 72.9 | no | - | - | no | - | - | 0 | 4 | 0 | - | blackx1/165 | 0 | yes |
| h3-ctl2 | first-run:telemetry | 137.0 | - | no | - | - | no | - | - | 0 | 0 | 0 | 225.1 | 0/115 | 0 | yes |
| h3-ctl3 | seek-failed:sto | 121.5 | - | no | - | - | no | - | - | 0 | 0 | 0 | 129.9 | 0/95 | 0 | yes |
| h3-c01 | script-done | 153.9 | 82.8 | yes | - | 4.2 | yes | 238 | 0 | 0 | 0 | 0 | 31.9 | 0/63 | 0 | yes |
| h3-c02 | seek-failed:menu-quit | 152.5 | 76.0 | yes | 21.2 | - | no | - | - | 0 | 0 | 0 | 33.6 | 0/74 | 0 | yes |
| h3-c03 | seek-failed:menu-quit | 152.4 | 97.7 | yes | 21.0 | - | no | - | - | 0 | 0 | 0 | 31.2 | 0/79 | 0 | yes |
| h3-c04 | script-done | 136.9 | 85.8 | yes | - | 3.5 | yes | 239 | 0 | 0 | 2 | 0 | 37.1 | blackx1/60 | 0 | yes |
| h3-c05 | script-done | 107.5 | 76.7 | yes | - | 3.3 | yes | 228 | 0 | 0 | 0 | 0 | 33.7 | 0/55 | 0 | yes |
| h3-c06 | game-exited | 106.7 | 89.5 | yes | - | - | no | 188 | 0 | 8 | 6 | 0 | 32.6 | 0/54 | 2 | yes |
| h3-c07 | script-done | 122.1 | 87.6 | yes | 21.4 | 3.0 | yes | 230 | 0 | 0 | 0 | 0 | 33.8 | 0/75 | 0 | yes |
| h3-c08 | script-done | 106.6 | 97.4 | yes | - | 3.8 | yes | 219 | 0 | 0 | 2 | 0 | 37.5 | 0/61 | 0 | yes |
| h3-c09 | script-done | 106.4 | 77.7 | yes | - | 3.5 | yes | 225 | 0 | 0 | 1 | 0 | 34.8 | 0/56 | 0 | yes |
| h3-c10 | script-done | 152.9 | 97.7 | yes | 20.9 | 2.3 | yes | 219 | 0 | 0 | 1 | 0 | 33.2 | 0/79 | 0 | yes |
| h3-c11 | script-done | 166.4 | 88.4 | yes | 20.5 | 2.5 | yes | 228 | 0 | 0 | 0 | 0 | 33.5 | 0/77 | 0 | yes |
| h3-c12 | script-done | 168.0 | 96.2 | yes | - | 3.2 | yes | 144 | 0 | 0 | 0 | 0 | 37.6 | 0/65 | 0 | yes |
| h3-c13 | script-done | 121.5 | 77.1 | yes | - | 4.0 | yes | 222 | 0 | 0 | 1 | 0 | 34.6 | 0/56 | 0 | yes |
| h3-c14 | script-done | 106.1 | 80.7 | yes | 20.5 | 2.5 | yes | 220 | 0 | 0 | 1 | 0 | 43.2 | 0/74 | 0 | yes |
| h3-c15 | script-done | 105.7 | 82.3 | yes | 20.8 | 2.0 | yes | 224 | 0 | 0 | 0 | 0 | 37.1 | 0/73 | 0 | yes |
| h3-c16 | script-done | 121.4 | 70.9 | yes | - | 3.7 | yes | 230 | 0 | 0 | 0 | 0 | 34.2 | 0/59 | 0 | yes |
| h3-c17 | script-done | 121.4 | 81.7 | yes | - | 3.7 | yes | 238 | 0 | 0 | 0 | 0 | 38.0 | 0/60 | 0 | yes |
| h3-c18 | seek-failed:menu-quit | 107.0 | 85.8 | yes | 20.8 | - | no | - | - | 0 | 0 | 0 | 39.1 | 0/75 | 0 | yes |
| h3-c19 | script-done | 121.2 | 71.4 | yes | 20.7 | 1.8 | yes | 226 | 0 | 0 | 0 | 0 | 40.5 | 0/71 | 0 | yes |
| h3-c20 | script-done | 105.4 | 75.7 | yes | - | 3.5 | yes | 221 | 0 | 0 | 0 | 0 | 41.3 | 0/56 | 0 | yes |
| h3-churn1 | script-done | 121.9 | - | no | - | - | yes | 297 | 0 | 0 | 0 | 0 | - | 0/637 | 0 | yes |
| h3-churn2 | script-done | 106.4 | - | no | - | - | no | - | - | 0 | 0 | 0 | - | blackx1/436 | 0 | NO |
| h3-churn3 | script-done | 167.9 | - | no | - | - | no | - | - | 0 | 8 | 0 | - | blackx1,garbage?x1/322 | 0 | NO |
| h3-churn4 | script-done | 122.3 | - | no | - | - | yes | 275 | 0 | 0 | 10 | 0 | - | blackx1/999 | 0 | yes |
| h3-set1 | script-done | 136.3 | - | no | - | - | no | - | - | 0 | 0 | 0 | - | 0/57 | 0 | NO |
| h3-set2 | script-done | 151.3 | - | no | - | - | yes | 228 | 0 | 0 | 0 | 0 | - | 0/639 | 0 | yes |
| h3-set3 | script-done | 119.9 | - | no | - | - | yes | 226 | 0 | 0 | 0 | 0 | - | 0/112 | 0 | yes |
| h3-set4 | script-done | 122.9 | - | no | - | - | yes | 229 | 0 | 0 | 1 | 0 | - | 0/112 | 0 | yes |
| h3-set5 | script-done | 150.1 | - | no | - | - | yes | 219 | 0 | 0 | 0 | 0 | - | 0/107 | 0 | yes |
| h3-soakA | script-done | 121.9 | 87.3 | yes | - | 4.0 | yes | 236 | 0 | 0 | 5 | 0 | - | 0/189 | 0 | yes |
| h3-soakB | script-done | 120.8 | 76.5 | yes | - | 3.9 | yes | 218 | 0 | 0 | 14 | 0 | - | 0/252 | 0 | yes |
| h3-d01 | script-done | 136.6 | 83.2 | yes | - | 4.0 | yes | 264 | 0 | 0 | 2 | 0 | 31.5 | 0/58 | 0 | yes |
| h3-d02 | script-done | 151.7 | 75.7 | yes | 21.0 | 2.5 | yes | 225 | 0 | 0 | 0 | 0 | 36.5 | blackx1,partial-blackx1,same-as-prevx1/76 | 0 | yes |
| h3-d03 | script-done | 106.9 | 77.2 | yes | - | 2.8 | yes | 224 | 0 | 0 | 1 | 0 | 29.8 | 0/56 | 0 | yes |
| h3-d04 | script-done | 105.7 | 82.1 | yes | 20.8 | 2.0 | yes | 239 | 0 | 0 | 0 | 0 | 37.7 | 0/73 | 0 | yes |
| h3-d05 | script-done | 175.9 | 72.1 | yes | - | 3.5 | yes | 224 | 0 | 1 | 7 | 0 | 36.8 | 0/57 | 0 | yes |
| h3-d06 | script-done | 106.1 | 71.1 | yes | 21.1 | 2.5 | yes | 251 | 0 | 0 | 1 | 0 | 35.3 | 0/75 | 0 | yes |
| h3-d07 | script-done | 166.9 | 81.4 | yes | - | 3.5 | yes | 230 | 0 | 0 | 0 | 0 | 38.2 | 0/61 | 0 | yes |
| h3-d08 | script-done | 106.6 | 82.5 | yes | 21.4 | 2.5 | yes | 219 | 0 | 0 | 0 | 0 | 29.1 | 0/71 | 0 | yes |
| h3-d09 | script-done | 108.2 | 72.4 | yes | - | 4.3 | yes | 241 | 0 | 0 | 3 | 0 | 30.4 | 0/54 | 0 | yes |
| h3-d10 | script-done | 122.4 | 87.6 | yes | 21.1 | 2.2 | yes | 224 | 0 | 0 | 0 | 0 | 33.5 | 0/76 | 0 | yes |

world reached: 32/44 (h3-c01 h3-c02 h3-c03 h3-c04 h3-c05 h3-c06 h3-c07 h3-c08 h3-c09 h3-c10 h3-c11 h3-c12 h3-c13 h3-c14 h3-c15 h3-c16 h3-c17 h3-c18 h3-c19 h3-c20 h3-soakA h3-soakB h3-d01 h3-d02 h3-d03 h3-d04 h3-d05 h3-d06 h3-d07 h3-d08 h3-d09 h3-d10)
clean exit: 34/44 (h3-c01 h3-c04 h3-c05 h3-c07 h3-c08 h3-c09 h3-c10 h3-c11 h3-c12 h3-c13 h3-c14 h3-c15 h3-c16 h3-c17 h3-c19 h3-c20 h3-churn1 h3-churn4 h3-set2 h3-set3 h3-set4 h3-set5 h3-soakA h3-soakB h3-d01 h3-d02 h3-d03 h3-d04 h3-d05 h3-d06 h3-d07 h3-d08 h3-d09 h3-d10)
early exits: 0/44
kernel fault rows: 2/44 (h3-c06 h3-d05)
device-lost rows: 1/44 (h3-c06); EMFILE/grab_object rows: 0/44
hitches in the world: 0/44
verdict disagrees with the boot: 3/44 (h3-churn2 h3-churn3 h3-set1)
menu: median 121.9 (n=44, 105.4..175.9)
load: median 81.7 (n=33, 70.9..97.7)
exit2t: median 21.0 (n=14, 20.5..21.4)
quit: median 3.5 (n=28, 1.8..4.3)
kill_ms: median 226 (n=35, 144..297)

### Round 3 harness (`tools/cp/hunt/`, lane H6-hunt3)

| file | what |
|---|---|
| `r3/cold-{m86,a16}-{pq,mq}.p9` | one cold boot: main menu → LOAD GAME → ManualSave-86 (list row 509) or AutoSave-16 (row 380) → loading → ~90 s in the world → QUIT GAME from the pause menu (`pq`) or EXIT TO MAIN MENU → title → QUIT GAME from the main menu (`mq`).  AutoSave-16 opens the game's tutorial popup ("WELCOME TO CYBERPUNK 2077! … [E] continue"): Esc closes it (E does not), and `unwind` presses Esc only while it shows |
| `r3/soak60.p9`, `r3/soak60-map.p9` | ≥ 60 minutes in the world on ManualSave-86: 14 phases `soak01..soak14` of soak.p9's loop (the second with the map opened and closed once per phase), then QUIT GAME |
| `drive.py` | the host-side driver for interactive boots (`run-hunt.sh drive`, `HUNT_DRIVE=churn,…` / `settings,…`): sends only boot_p9 input / wait / shot lines through `ctl.txt` and reads each result itself (boot_p9's `seek` / `expect` end the boot when they fail).  Readings: boot_p9's own `focus_in()` for the main / pause menus while the game picture is where its tables expect it, else the yellow pixels next to each label tesseract found; the save list's cyan frame and the save name OCR'd inside it; the Settings rows' focus tint (the row band's g − r against thin strips above and below it, placed from the OCR'd labels with a fitted scale and offset); waits = OCR until a regex matches, never fatal.  `churn` = F5 quick saves, then N loads of different saves through pause → LOAD GAME, then M × (EXIT TO MAIN MENU → title → LOAD GAME); `settings` = VIDEO tab Resolution 720p ↔ 1080p, Windowed Mode cycled, GRAPHICS Quick Preset; `menuloads`, `quit` |
| `r3table.py` | the per-boot round-3 table: the boot's verdict next to this lane's own reading (load time, world, exit-to-title time, quit time, clean exit, `wineserver -k` ms, kernel rows, hitches, shotcheck flags) and the k/n lines; "agree" = verdict and reading agree |
| `r3post.sh` | per boot: classify.py again, shotcheck.py `--json`, then r3table.py + `classify.py --aggregate` over the set; `--purge` deletes the screendumps after |
| `classify.py` (extended) | the session record: the launcher's 30 s census (procs, fd probe, free MiB), the game's end and the teardown (`wineserver -k` status / ms, quiesce leftovers, sync, `[P8] done` → "clean exit"; an exited-unreaped `:s4` leftover is the keepalive, H6-03), `[THRS]` trends per process (threads / regions / anon / res at 0-25-50-75-100 %, and whether the 4000-row kernel cap stopped the census, H6-02), device-lost and EMFILE / `grab_object` rows, `[AWAY]` / `[AWAY-Z]` / `[SND] zeros` counts, and the script's load / world counts from the run log |
| `ctl.sh` (extended) | `CTL_UNTIL=REGEX`: shots 4 s apart until the OCR matches (a wait that cannot end the boot); the save-list frame reading |
| `run-hunt.sh` (extended) | evidence under the tree (`obj/hunt-logs`), WAVs under `~/cp/lanes/<tree>-wav`, export `/srv/nfs/cp-<tree>-p8/-p9`; KIND `drive` |


## Round 2 (integ17 = bdf39e33)

Branch `cp-hunt2`; strix tree `~/cp/lanes/hunt2`, logs `~/cp/lanes/hunt2-logs`.
Wine `~/cp/wine-cp2` (the patched stage).  The seeded prefix was rebuilt by
this lane with that stage (`P8_PHASE=pfx`, 07:55; stamp 1794996799 = wine-cp2's
wine.inf, so no per-boot wineboot update).  The Venus ICD is unchanged
(`tools/mesa/venus` has no diff integ15 → integ17).  At most one boot of
this lane ran at a time, so it used one slot at a time; P9 boots ran under
the big-guest lock.

**Boots:** 18 game boots on integ17 (6 P8 to the menu: Venus 4, lavapipe 2; 12 P9: 2 play, 3 reloads, 3 fpsprobe 720p, 2 fpsprobe 1080p, 1 soak with audio, 1 hand-driven Settings boot), 07:55–11:25, plus one fpsprobe 720p audio boot (h2i18-a1, after merging integ18 = 0a985aa2: world 40.3, p10 27.0; 7 WAV dropouts, 0 alsa-lib underruns, 0 kernel xruns, 8 `[SND] zeros` rows; `r2-integ18-audio.txt`) on integ18.

* **The fixes hold.**
  - Early exits 0 of 18 launches.
  - 0 kernel-fault rows.
  - QEMU start → launcher: median **5.3 s** (integ15: 61.1 s; 11/22 boots over 60 s → 0/18), H2's `cache=unsafe`.
  - `[BADELR]` and `[VENUS] fence_wait2 timeout` rows: 0 (integ15: 22/22 and 18/18 boots).
  - The `[WATCHDOG]` row is down to 2 of 18 boots. Each one now comes with a ≥ 19 s silence that the host saw too (the boot's end).
  - The H1-04 wineboot reinstall is gone: 0 `InstallHinfSection` rows. However, H2's new warning about it fires falsely on every boot (H1-16).
* **H1-12 (the reload) is solved by the harness:**
  - play → save → reload → world passed 2/2: h2-play1 → h2-rl1, h2-play2 → h2-rl2b.
  - In both, the F5 right after loading wrote the same `playerPosition` as the play boot's quick save.
  - The root cause is a 104 px tall CONTINUE highlight. boot_p9's seek mislabels it and overshoots. There is a one-line `focus_in` diff for H2.
  - The "no focus after Settings" variant is cured by a mouse wiggle (1/1).
* **H1-08 on a real boot:** at 1920×1080, `--game-res` fixes the main menu; the UI does scale 1.5×. The save list then fails (H1-17, bicubic blur of a 1–2 px frame). With NEAREST scaling, the whole stock fpsprobe passed (h2-fq2nn, world 34.5).
* **fps (fpsprobe 720p Low):** world medians 33.7, 28.1, 30.3. The median of medians is 30.3 (integ15: 30.0). p10 is 25.5–27.6 (integ15: 20.6–26.4); the low tail is up.
* **Graphics preset:** the change now takes. Enter on Quick Preset, not Right, moved it from Custom to "Ray Tracing Overdrive". The save played on it at 15.5–18.8 fps with no device loss.
* **Soak:** 28 minutes, world median 37.6 (p10 29.7; integ15 35.9 / 23.9), **0 hitches** (integ15: 12, including the 8 s H1-15 freeze), no `[VENUS] stall` row, resident +6.3 MiB/min and flat over the last three phases (no leak), 0 flagged shots (108). Audio on the integ17 kernel: 8 WAV dropouts (integ15 22), 3 alsa-lib underrun rows (49), 37 `[SND] xrun` rows.
* **New:** H1-16 (a false prefix-stamp warning), H1-17 (the 1080p list seek), H1-18 (this lane's own lock collision, fixed).

### integ15 vs integ17 (this lane's boots)

| | integ15 | integ17 |
|---|---|---|
| game boots | 22 | 18 |
| verdicts | main-menu=11, script-done=1, script-stop=6, seek-failed:menu-continue=2, seek-failed:menu-load=2 | listseek-failed:loadlist-focus=1, main-menu=6, script-done=5, script-stop=5, seek-failed:menu-continue=1 |
| early exits | 0/22 | 0/18 |
| kernel fault rows | 0 | 0 |
| QEMU start → launcher (NFS mount) s, median | 61.1 (n=22, 6.4–142.2) | 5.3 (n=18, 2.3–6.3) |
| boots with that > 60 s | 11/22 | 0/18 |
| P8 Venus launch → menu s | 140.6 (n=7, 109.9–170.0) | 132.9 (n=4, 93.2–140.6) |
| P8 lavapipe launch → menu s | 242.9 (n=4, 202.4–278.5) | 255.2 (n=2, 232.7–277.6) |
| fpsprobe 720p world medians | 29.0, 33.4, 30.0 | 33.7, 28.1, 30.3 |
| fpsprobe 720p p10 | 24.7, 20.6, 26.4 | 25.9, 25.5, 27.6 |
| fpsprobe 1080p world medians | 29.8, 33.0 | 34.5 |
| soak world median (p10) | 35.9 (23.9) | 37.6 (29.7) |

| class | signature | integ15 boots (rows) | integ17 boots (rows) |
|---|---|---|---|
| audio | `alsa-lib snd_pcm_recover: underrun occurred` | 2/22 (56) | 1/18 (3) |
| audio | `kernel [SND] xrun row` | 0/22 (0) | 1/18 (37) |
| exitpc | `wine code=0xC0000135 DLL_NOT_FOUND` | 22/22 (42) | 18/18 (18) |
| kdiag | `badelr` | 22/22 (439) | 0/18 (0) |
| kdiag | `watchdog` | 15/22 (43) | 2/18 (2) |
| kstall | `nfspipe` | 21/22 (70) | 14/18 (33) |
| kstall | `venus` | 18/22 (294) | 0/18 (0) |
| wine_err | `system:find_gpu_from_path Failed to find gpu with path "PCI\\VEN_1002&DEV_7550&SUBSYS_0000` | 2/22 (8) | 0/18 (0) |
| wine_err | `system:lock_display_devices Failed to read display config.` | 2/22 (4) | 0/18 (0) |

The 18 integ17 boots include the two harness failures (h2-fq1, h2-rl2) and
the hand-driven h2-ctl1.  "watchdog" in integ17: both rows came with a ≥ 19 s
silence that the host saw as well (h2-play1 and h2-rl1, each at the game's
exit), i.e. true rows, not the false ones H1-14 was about.

#### Round 2 P8 boots

| boot | kind | verdict | launch s | launch→menu s | kernel faults | kernel stalls (rows) | EXITPC≠0 | new Wine err sigs | new DXVK/vkd3d sigs | REDEngineErrorReporter | 9070 XT busy med/p90 | other guests seen |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| h2-p8l1 | p8l | main-menu | 21.3 | 277.6 | 0 | 0 | 1 | 0 | 0 | yes | 87/97 | 2 |
| h2-p8l2 | p8l | main-menu | 21.3 | 232.7 | 0 | nfspipe×1 | 1 | 0 | 0 | yes | 28/70 | 2 |
| h2-p8v1 | p8v | main-menu | 24.3 | 140.6 | 0 | 0 | 1 | 0 | 0 | yes | 69/76 | 2 |
| h2-p8v2 | p8v | main-menu | 24.3 | 93.2 | 0 | nfspipe×2 | 1 | 0 | 0 | yes | 66/74 | 2 |
| h2-p8v3 | p8v | main-menu | 24.3 | 125.2 | 0 | 0 | 1 | 0 | 0 | yes | 94/98 | 1 |
| h2-p8v4 | p8v | main-menu | 24.3 | 140.6 | 0 | nfspipe×1 | 1 | 0 | 0 | yes | 88/98 | 1 |

#### Round 2 P9 boots (world = `world` rows; for the soak, all `soakN` rows)

| boot | kind | verdict | world median | world p10 | world min row | rows | hitches (>1 s frame) | menu median | 9070 XT busy (world) med/p90 | game res MiB first→max | kernel stalls | other guests seen | script |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| h2-ctl1 | settings | script-done | 15.5 | 13.8 | 12.57 | 16 | 0 | 219.7 | 48/54 | 549→4540 | nfspipe×1 | 3 |  |
| h2-fp1 | fp720 | script-stop | 33.7 | 25.9 | 17.25 | 62 | 0 | 216.7 | 34/39 | 553→4614 | nfspipe×3 | 3 |  |
| h2-fp2 | fp720 | script-stop | 28.1 | 25.5 | 15.84 | 60 | 0 | 216.2 | 35/45 | 579→4591 | nfspipe×2 | 3 |  |
| h2-fp3 | fp720 | script-stop | 30.3 | 27.6 | 21.91 | 60 | 0 | 84.9 | 45/98 | 488→4496 | 0 | 2 |  |
| h2-fq1 | fp1080 | listseek-failed:loadlist-focus | None | None | - | 0 | 0 | 109.3 | -/- | 444→2751 | nfspipe×1 | 1 | FAILED loadlist-focus: no entry focused at y=509.0 after 8 t |
| h2-fq2nn | fp1080 | script-stop | 34.5 | 25.3 | 10.51 | 64 | 0 | 111.6 | 54/61 | 510→4413 | nfspipe×4 | 3 |  |
| h2-play1 | play | script-done | 32.6 | 20.8 | 11.12 | 376 | 0 | 214.9 | 35/40 | 528→4654 | nfspipe×6 | 3 |  |
| h2-play2 | play | script-done | 31.5 | 23.5 | 0.6 | 379 | 0 | 215.8 | 36/43 | 484→4515 | nfspipe×5 | 3 |  |
| h2-rl1 | reload2 | script-done | 28.8 | 24.9 | 13.14 | 58 | 0 | 197.6 | 35/41 | 379→4133 | nfspipe×2 | 2 |  |
| h2-rl2 | reload2 | seek-failed:menu-continue | None | None | - | 0 | 0 | 207.6 | -/- | 569→2668 | nfspipe×2 | 2 | FAILED menu-continue: main menu row 'CONTINUE' not focused a |
| h2-rl2b | reload2 | script-done | 29.8 | 27.4 | 25.07 | 48 | 0 | 122.5 | 36/86 | 503→4133 | nfspipe×2 | 3 |  |
| h2-soak1 | soak | script-stop | 37.6 | 29.7 | 20.37 | 1059 | 0 | 213.6 | 37/47 | 552→4513 | nfspipe×1 | 2 |  |

h2-ctl1's world is the Ray Tracing Overdrive preset (above).  Soak per phase:
`cp-hunt-evidence/r2-soak.txt`.

### Round 2 findings and status changes

| ID | symptom | rate | first-seen evidence | signature | layer | severity | status |
|---|---|---|---|---|---|---|---|
| **H1-17** | at a 1920×1080 game resolution with H2's `--game-res`, the main-menu seek works (the menu does scale 1.5×), but the save-list seek fails: `game_view()` scales with PIL's default bicubic, which blends the 1–2 px cyan focus frame below `list_focus()`'s colour test (the target entry reads as a 1-row sliver) | 1/1 stock boot (h2-fq1); 0/1 with NEAREST (h2-fq2nn passes the whole fpsprobe, world 34.5) | `cp-hunt-evidence/r2-1080p-listseek.txt` | `LISTSEEK FAILED loadlist-focus: no entry focused at y=509.0 after 8 taps` | harness (`boot_p9.game_view`) | harness | open: one-word diff below (H2-early) |
| **H1-16** | `run-p8.sh`'s new prefix-stamp warning (209a5ce4) fires when the stamp matches: `.update-timestamp` ends in CR LF, so `"$PSTAMP" != "$WSTAMP"` though both print the same number | 18/18 boots (every integ17 game boot) | `cp-hunt-evidence/r2-prefix-stamp-cr.txt` | `== WARNING: prefix stamp 1794996799 != …/wine.inf mtime 1794996799` | harness (`tools/cp/p8/run-p8.sh`) | cosmetic (a false alarm on every boot) | open: diff below (H2-early) |
| **H1-18** | `run-hunt.sh`'s one-boot lock was `/tmp/cp-hunt-lane.lock` for every tree; another lane's copy (audio2-base) held it, and this lane's boot waited ~10 min behind theirs | 1 wait (h2-rl1) | `h2-rl1` start 08:17 → its log starts 08:27 | `fuser /tmp/cp-hunt-lane.lock` → audio2-base's run-hunt | this lane's harness | harness | **fixed in e11b0700** (lock per tree) |

**Console host stalls (observation, H1-03's class).**  The stall-proof
console (cp-kglitch) counts the time the guest's console waited on the host:
`[CONS] … stalls=57 stolen_ms=8401` in the 5-minute h2i18-a1 boot and
`stalls=35 stolen_ms=3299` in the 28-minute h2-soak1 (gamecheck.sh's last
column).  No boot stalled before the launcher any more (QEMU start →
launcher ≤ 6.3 s in 18/18), so this is no longer a visible hang, but the
host still steals seconds per boot.

**H1-12, the two ways around it.**
* `tools/cp/hunt/reload2.p9`: the reload takes CONTINUE from the **fresh**
  main menu, where the focus always behaves, and skips reload.p9's detour
  through the save list (the evidence that the save is boot A's is the
  position: an immediate F5 after loading writes a quick save whose
  `playerPosition` equals boot A's).  Result: play → save → reload → world **2/2** (h2-play1 → h2-rl1; h2-play2 → h2-rl2b after h2-rl2 failed on the seek), the F5 after loading at the same `playerPosition` as the play boot's quick save both times (`r2-reload.txt`).
* Why the reload's seek fails (h2-rl2, and round 1's h-rl1b): with some
  save sets the focused CONTINUE row is drawn **104 px tall** (rows
  451..555, its save line under it; still there 1.6 s later), and
  `focus_in()` labels a frame by its centre (503 → NEW GAME).  `seek` then
  taps Up once more and overshoots onto the frameless button above.
  `reload2.p9` now takes CONTINUE with two settled Ups (every fresh menu
  answered Up → NEW GAME, Up → CONTINUE) and lets the loading screen's
  `expect` check it.  Proposed for `focus_in()` (H2-early; checked offline on
  the h2-rl1 / h2-rl2 / h2-rl2b shots: only the tall frame's label changes,
  to CONTINUE):

  ```diff
       y0, y1 = max(cl, key=lambda c: c[1] - c[0])
       yc = (y0 + y1) / 2
  +    if y1 - y0 > 50:
  +        # cp-hunt H1-12: a focused row can be drawn taller (CONTINUE with its save
  +        # line under it, rows 451..555): its frame starts at the row, so label by the top
  +        yc = y0 + 18
  ```
* By hand in h2-ctl1: after Esc from Settings the menu showed no focus for
  two Up taps; a relative mouse move of +60 then −60 (QMP `rel` events) and
  the next Up gave NEW GAME, Down gave LOAD GAME (`r2-ctl-preset-focus.txt`).
  In the same boot the save list → Esc left the focus visible, so H1-12 is
  intermittent, not 3/3.  Proposed for `boot_p9.seek()` (H2-early, untested
  in an unattended boot):

  ```diff
                       d = direction if label is None else ("down" if want > names.index(label) else "up")
  +                    if label is None and i >= 1:
  +                        # cp-hunt H1-12: back from a sub-menu the main menu can show no
  +                        # keyboard focus until the pointer moves; a net-zero wiggle brings it back
  +                        send([{"type": "rel", "data": {"axis": "x", "value": 60}}]); tick(0.1)
  +                        send([{"type": "rel", "data": {"axis": "x", "value": -60}}]); tick(0.5)
  ```

**H1-17 diff** (`tools/cp/p9/boot_p9.py` `game_view`, H2-early), verified by
h2-fq2nn through `tools/cp/hunt/boot_p9_nn.py`, which is boot_p9 with only
this replaced:

```diff
-        return g.resize((1280, 720))
+        return g.resize((1280, 720), Image.NEAREST)   # keep the 1-2 px focus frames (cp-hunt H1-17)
```

**H1-16 diff** (`tools/cp/p8/run-p8.sh` ~307, H2-early):

```diff
-  PSTAMP="$(debugfs -R 'cat /pfx/.update-timestamp' "$CPD" 2>/dev/null | head -1 | cut -d' ' -f1)"
+  PSTAMP="$(debugfs -R 'cat /pfx/.update-timestamp' "$CPD" 2>/dev/null | head -1 | tr -d '\r' | cut -d' ' -f1)"
```

**The graphics preset (round 1's open item).**  On the GRAPHICS tab Right on
Quick Preset moves the focus to RUN BENCHMARK instead of changing it; **Enter
changes it**.  One Enter took "Custom" to "Ray Tracing Overdrive" (path
tracing; vkd3d-proton reports DXR 1.1 on the 9070 XT through Venus), saved by
the game on leaving Settings (Texture High, Crowd High, FSR2).  The save then
loaded and played on it at 15.5–18.8 fps world median with no `[VENUS]`
stall, fence or submit error and no device-lost row (1/1, h2-ctl1).


## Round 1 result (integ15)

**22 game boots** on strix, 2026-11-18 02:21–06:42, integ15 code: 11 P8 to the main menu (6 Venus, 4 lavapipe, 1 Windowed under IceWM; 11/11 reached it) and 11 P9 on Venus (3 fpsprobe at 1280×720 Low, 3 at 1920×1080 Low of which one hit a harness gap, 1 ten-minute play + save, 2 reloads, 1 28-minute soak, 1 hand-driven Settings boot), plus a census of 128 older game boots.  Headline:

* **No kernel fault in any boot of this lane** (0 `User process fault`,
  `FATAL`, `#PF in user space`, `[LCOMPAT] fault`, panic, `[TLBTIMEOUT]`,
  `[STALLBREAK]` rows).  Every such class in the census of 128 older game
  boots was last seen on 2026-11-17 by 12:36 (wave 5 / early wave 6 trees).
* **No early exit (H1-02) in this lane's boots** (0 of 22 launches); the old run
  logs have 6, in two populations (2.5–2.9 s and 20–24 s after launch).
* **Audio: underruns are real** (H1-01): 2 of 2 audio boots, 10 and 22
  dropouts (exact-zero 10 ms periods inside loud audio) and 7 / 49
  `snd_pcm_recover) underrun` rows from alsa-lib in the guest, densest in
  the save's loading, ~1 per 2 minutes in the world; WAV length / wall time 0.962–0.967 (the stream starts
  after the launch; no long gaps).
* **One ~8 s freeze in 28 minutes of play** (H1-15): Venus fences unretired
  7.7 s while the host's own sampler stalled 8 s; host side.
* **A 1–2.5 minute pre-launch stall in 11 of 22 boots** (H1-03): QEMU start →
  launcher 64–142 s instead of 6–58 s.  The three with the stage on record
  sat in the firmware (twice) or behind a console that did not reach the
  host (once), each while the host's I/O pressure was 50 % "full"; a
  `cache=unsafe` A/B is proposed.
* **Instruments that mislead**: the stall watchdog's "console silent >
  20 s" (15 of 22 boots, 101 of 128 old ones) is a false positive every time
  it was checked (H1-14, root cause: an unsigned wrap; one-line diff), `[BADELR]` fires on the game's PE image (H1-06),
  the Venus fence-timeout rows are spent in the first 6 s (H1-07).
* **P9 fps verdict (≥ 30 fps world median at 1280×720 Low):** **met at the median, not with margin**: the three 1280×720
  fpsprobe boots give world medians 29.0, 33.4 and 30.0 (median of medians
  **30.0**, p10 20.6–26.4); the two 1920×1080 boots 29.8 and 33.0 (p10 17.4,
  26.3) — the same as 720p, so the frame is not GPU-fill bound; the 10-minute
  play boot 30.5 (p10 21.8) and the 28-minute soak 35.9 (p10 23.9).  The 9070
  XT was 33–54 % busy in the world phases.  That is ~3–5 fps above cp-k7's
  integ11 numbers (25.5–27.9), on integ15 (C1-k7 and C2-ntsync merged; not an A/B, other lanes' guests differ).
* **Soak (28 min in the world):** no fps decay (phase medians 34.5 → 43.0,
  scene-dependent), the game's resident memory 4309 → 4473 MiB (+5.9
  MiB/min, flat over the last two phases), virtual anon +33 MiB, threads 48
  throughout: no leak.
* **Visual review** (737 screendumps, numbers only): no black, torn,
  garbage or stuck frame after the menu; Venus and lavapipe main menus have
  the same mean colour (63.2 / 59.8 / 81.8 both).  **Input**: across 88 P9
  run logs, 274 menu taps moved the focus exactly one row, 0 were dropped;
  but after Esc back from a sub-menu (save list, Settings) the main menu's
  focus could not be followed in 3 of 3 boots (H1-12), which blocked both
  reloads.
* **Harness gaps found and worked around**: P9 menus at 1920×1080 (H1-08,
  `fpsprobe-blind.p9`), a stale seeded prefix that makes every boot re-run
  wineboot's update (H1-04).

## The catalog (sorted by severity × rate)

Format per the wave-8 overrides: ID, symptom, rate (k/n boots, boot IDs),
first-seen evidence, signature, suspected layer, severity, status.  "This
lane's boots" are the `h-*` boots below (integ15 code); "old boots" are the
census of 128 earlier game boots (§ Old logs), quoted only to show whether
something recurs.

| ID | symptom | rate | first-seen evidence | signature | layer | severity | status |
|---|---|---|---|---|---|---|---|
| **H1-01** | audio dropouts: exact-zero runs of 10–290 ms (whole 10 ms periods) inside loud audio; in h-fp2 all in or near the save's loading, in the soak 7 during loading and 15 over 28 minutes in the world (~1 per 2 min); alsa-lib in the guest reports the underruns | 2/2 audio boots (h-fp2, h-soak1) | `cp-hunt-evidence/audio-dropouts-fp2.txt` | `ALSA lib …/pcm.c:8772:(snd_pcm_recover) underrun occurred` (h-fp2 7 rows / 10 dropouts; h-soak1 49 / 22) | game audio thread starved under guest load + winealsa's 40 ms buffer (Wine / kernel scheduling) | wrong output (audible glitch) | open — H5-audio2 owns it (round 2: h2-soak1 8 dropouts / 3 alsa-lib underruns / 37 kernel xrun rows on integ17; h2i18-a1 on integ18 7 dropouts / 0 / 0, 8 `[SND] zeros`) |
| **H1-15** | ~8 s freeze in the world: Venus ring-1 fences unretired for 7.7 s, fps rows of 1.7 and 4.3, audio dropouts; the host's own 1 Hz sampler stalled 8 s twice at the same moment | 1 in 28 min of soak (h-soak1); none in the other P9 world phases (≤ 1 hitch each) | `cp-hunt-evidence/soak-venus-freeze.txt` | `[VENUS] stall pid=110 tid=185 waits_ms=7754 ctx=12 ring=1` | host (amdgpu / virglrenderer under host load), not the guest | hang (recovers) | open (host side); round 2: 0 freezes in the 28-minute soak and 0 hitches in every world phase |
| **H1-02** | the game exits 2–3 s (or ~20 s) after launch, right after REDEngineErrorReporter starts; graded rc=0 "launcher-done" | 0/22 here; old run logs: 6 (integ10-venus, k6-venus, console-lvp1, integ13-lvp1, k7l-1, integ15-lvp2) | `cp-hunt-evidence/mine-census.txt` (run-log section) | `[P8] end game status=0 ms=2886` + REDEngineErrorR exits twice + services.exe/explorer.exe `code=0x1` | H2-early's (Wine session teardown) | crash | **fixed in 4f0da178** (NFS directory-listing cache lock, H2-early) + grader 351b0d49 (`early-exit`, rc 3); round 2: 0/18 |
| **H1-03** | 1–2.5 minute stall before the launcher starts: twice in the firmware stage (serial stuck at the firmware's 113 bytes for 78 / 85 s), once after the X session (console bytes not reaching the host for ~145 s, vCPUs waiting in the UART TX path) | 11/22: QEMU start → launcher NFS mount 64–142 s (fast boots 6–58 s); stage identified in h-soak1 (console), h-p8v5 and h-p8v6 (firmware, host I/O) | `cp-hunt-evidence/pregame-console-stall.txt` | `no launcher/Wine row for 120 s before the game: sampling the vCPUs (prof-prestall)`; host `/proc/pressure/io` full avg10 50–55 % during the stall | host I/O pressure (QEMU's flushes / file writes wait behind other lanes' dirty data) | hang (recovers) | **mitigated in 5784e671** (`P8_DRIVE_CACHE=unsafe` default for game boots); round 2: QEMU start → launcher median 5.3 s, 0/18 over 60 s (integ15: 61.1 s, 11/22) |
| **H1-12** | after Esc back to the main menu from a sub-menu (the save list, Settings), Up/Down no longer show a focus frame: LOAD GAME → NEW GAME then nothing (h-rl1), a taller frame back at LOAD GAME then nothing (h-rl1b), nothing at all after Settings (h-set1) | 3/3 boots that returned from a sub-menu (h-rl1, h-rl1b, h-set1); lanes/play's reload boots (wave 6, wine-build-x) 3/3 fine | `cp-hunt-evidence/reload-seek-continue.txt`, `settings-menu.txt` | `SEEK FAILED menu-continue: main menu row 'CONTINUE' not focused after 6 x up` | game UI × input (where the keyboard focus goes after the sub-menu closes); harness can't follow it | harness (blocks the reload check) | **worked around** (round 2): `reload2.p9` takes CONTINUE from the fresh menu (2/2 reloads, one after a retry: h2-rl1, h2-rl2b; h2-rl2 failed on the seek, which led to the fix); a relative-mouse wiggle restores the focus after a sub-menu (1/1) |
| **H1-08** | P9 `seek` never finds the main-menu focus at a 1920×1080 game resolution | 1/1 seek-based 1080p boot (h-fq1); 0/2 with `fpsprobe-blind.p9` (h-fq1b, h-fq2) | `cp-hunt-evidence/p9-seek-1080.txt` | `SEEK FAILED menu-load: main menu row 'LOAD GAME' not focused after 6 x down` | harness (`boot_p9.canon`) | harness | **main menu fixed in 209a5ce4** (`--game-res`); the save list still fails at 1080p (H1-17) |
| **H1-09** | the ≥ 30 fps world target is met only at the median: fpsprobe 720p medians 29.0 / 33.4 / 30.0, 1080p 29.8 / 33.0; p10 17–26; single ~1.5 s rows down to 6–13 fps | 2/5 fpsprobe boots below 30 (h-fp1, h-fq1b) | P9 table below | `world median 29.0 (p10 24.7)` (h-fp1) | game user code + Venus fence/present path (cp-k7 §6); 1080p ≈ 720p, so not GPU fill | perf | open; round 2: fpsprobe 720p 33.7 / 28.1 / 30.3 (p10 25.5–27.6), 1080p 34.5, soak 37.6 |
| **H1-04** | every game boot re-runs wineboot's prefix update (~9.5 s, writes the prefix) | 22/22 | `cp-hunt-evidence/prefix-stale-stamp.txt` | `wine: failed to start L"C:\\windows\\syswow64\\rundll32.exe": c0000135` (its last step) | harness (seeded prefix from another Wine stage) | perf | **fixed for this lane** (prefix rebuilt with wine-cp2); 209a5ce4 adds the warning, which fires falsely (H1-16) |
| **H1-10** | ~1.5 s freeze (one `approx 0.6fps` row) at the same point of play.p9's second walking loop, game s 753–759; **not** the map close: round 1 compared host seconds with game seconds — the second map close is ~110 s earlier | 2/3 play boots (h-play1 game s 752.9, h2-play2 758.7; not h2-play1) | h-play1 / h2-play2 `fps.csv` | one `approx 0.61fps` / `0.60fps` world row | game (a streaming or scripted point on that walk) / Venus | perf | open; round 2: h2-play2 again at game s 758.7 (h2-play1 not) |
| **H1-13** | 150–260 ms NFS reply loss + resend pauses on the game copy | 21/22 boots, 70 rows (1–8 per boot) | `classify.py` kstall `nfspipe` | `[NFSPIPE] stall inflight=1 issued=518 replies=517 … us=258904` | kernel NFS / slirp | perf | open; round 2: 14/18 boots, 33 rows (integ15 21/22, 70) |
| **H1-05** | win32u cannot find the GPU while the per-boot prefix update rebuilds devices | 2/22 (h-fq1b, h-fq2) | `cp-hunt-evidence/win32u-find-gpu-race.txt` | `err:system:find_gpu_from_path Failed to find gpu with path "PCI\\VEN_1002&DEV_7550…"` | Wine (race inside H1-04's update) | cosmetic | not seen in round 2 (no prefix update): 0/18 |
| **H1-06** | `[BADELR]` reports the game's own healthy resumes; the 20-row cap is spent at once | 22/22 (439 rows, capped at 20 per boot); old boots 127/128 | `cp-hunt-evidence/badelr-pe-image.txt` | `[BADELR] picking a user slot with an implausible frame.elr=0x00000001406A5043 … Cyberpunk2077.e` | kernel instrument (`process.c`) | cosmetic (blinds a hang detector) | **fixed in 991491d1** (H4); round 2: 0 rows in 18 boots |
| **H1-07** | `[VENUS] fence_wait2 timeout` rows: 16 short-timeout polls in the game's first ~6 s exhaust the lifetime cap | 18/18 Venus boots reach the cap | `cp-hunt-evidence/venus-fence-timeout-cap.txt` | `[VENUS] fence_wait2 timeout pid=109 ctx=12 ring=1 seqno=0x0000000100000011 … done=0x0000000000000000` | kernel instrument (`gpu_abi.c`) | cosmetic (blinds a hang detector) | **fixed in af295366** (H4); round 2: 0 rows in 18 boots |
| **H1-14** | trap.c's stall watchdog prints `console silent for more than 20s` (+ a CPU dump) about 1 s after the console printed | 15/22 boots (43 rows); old boots 101/128 | `cp-hunt-evidence/watchdog-false-silence.txt` | `[WATCHDOG] console silent for more than 20s` right after an `[X] 1240.402:…trace:fps` row | kernel instrument (`trap.c` watchdog / `uart_last_activity_ms`) | cosmetic (false hang alarms) | **fixed in d7060448** (H4, `wd_console_silent()`); round 2: 2/18 boots, both true silences of ≥ 19 s at the game exit |
| **H1-11** | the kernel's xrun count is printed only at PCM close, which a game boot never reaches (alsa-lib's own row confirms H1-01 instead) | 2/2 audio boots | `cp-hunt-evidence/audio-dropouts-fp2.txt` | `[SND] pcm closed: chunks=… xruns=…` absent for the game's stream | kernel instrument (`linux_snd.c`) | cosmetic | **fixed in cf7a0865** (`[SND] xrun` rows); round 2: 37 `[SND] xrun` rows in h2-soak1 |
| — | benign, every boot (tagged by `classify.py`): `winediag:getaddrinfo`, winemenubuilder 126, `winebth`, `wineusb.inf` copy error, no schannel / Kerberos, `mfc140u.dll` for the Razer Chroma plugin, DXVK EDID / HDR metadata / unknown adapter interface / ignored flags, `wine code=0xC0000135` (winemenubuilder; + syswow64 rundll32 under H1-04) | 22/22 | `*.class.txt` | — | Wine / DXVK | none | not ours |

### Notes on the rows

**H1-01 (audio dropouts).**  Each dropout is a run of exact digital zero
on both channels, 10–200 ms long, always a whole number of winealsa's 10 ms
periods, with audio at −20…−36 dBFS on both sides — music does not cut to
true zero and back for one period; a starved device does.  They are densest
in the save's loading (game s 147–158 in h-fp2, 7 of the soak's 22: the
phase where the 8 vCPUs are busiest) and keep coming at ~1 per 2 minutes in
the world (15 in the soak's 28 minutes).  The ALSA buffer winealsa asks for is 4 ×
10 ms = 40 ms (`[SND] hw_params … period=480 periods=4`, docs/cp-audio.md
§7), so any 40 ms stall of the game's audio thread (or of the compat
`WRITEI` path behind it) is audible.  The guest confirms it: alsa-lib, in
the game process, prints `snd_pcm_recover) underrun occurred` (7 rows in
h-fp2, 49 in the 28-minute soak; wavgaps counts only the zero runs ≥ 5 ms
between loud audio, 10 and 22).  The kernel's own xrun counter is printed
only at PCM close (H1-11).  Next steps (not done here): correlate the
underrun rows with the guest's CPU load (THRW), then decide between a longer device buffer
on the winealsa side (H3-winefix: the 40 ms comes from the WASAPI period the
game asks for, mapped by winealsa) and the scheduling of the game's audio
thread under load (H4-kglitch).

**H1-11 (xrun count).**  Proposed (`src/kernel/linux_snd.c`, the audio
driver's owner), so H1-01 can be confirmed or ruled out in the guest:

```diff
 static void check_xrun_locked(void) {
   if (S.state == ST_RUNNING && S.stop_thr < S.boundary && avail_locked() >= S.stop_thr) {
     S.state = ST_XRUN;
     S.xruns++;
     S.trigger_us = timer_get_us();
+    /* cp-hunt H1-11: a game never closes its PCM before the boot ends, so
+       the close row never shows them: one row per xrun (first 32, then every 64th) */
+    if (S.xruns <= 32 || (S.xruns & 63) == 0) {
+      struct kline kl;
+      kl_init(&kl);
+      kl_s(&kl, "[SND] xrun n=");
+      kl_i(&kl, (long)S.xruns);
+      kl_s(&kl, " frames_done=");
+      kl_i(&kl, (long)S.frames_done);
+      kl_s(&kl, " chunks=");
+      kl_i(&kl, (long)S.chunks);
+      kl_c(&kl, '\n');
+      kl_emit(&kl);
+    }
   }
 }
```

**H1-14 (watchdog false silence): root cause.**  `watchdog_tick()` reads
`now` at the top, then prints the 30-second `[NFSIO]` row through the normal
console (`wd_puts` → `uart_putc_dbg` → `cons_kputc` → `cons_hw_tx`), and
`cons_hw_tx` stamps `uart_last_activity_ms = timer_get_ms()` — a later
millisecond than `now` once that print (or any other CPU's) crosses one.  The
silence test is unsigned, `now - uart_last_activity_ms < WATCHDOG_SILENCE_MS`,
so `now < last` wraps to ~2⁶⁴ and the watchdog declares 20 s of silence and
dumps the CPUs.  24 of this lane's first 32 rows follow the tick's own
`[NFSIO]` row, the rest follow other console output within 1–8 lines; 101 of
the 128 old game boots carry it.  Proposed (owner of `src/kernel/arch/x64/trap.c`'s
watchdog; H4-kglitch for the console side), with a host-test or probe row
that sets `uart_last_activity_ms = now + 1` and expects no report:

```diff
-  if (now - uart_last_activity_ms < WATCHDOG_SILENCE_MS) {
+  /* cp-hunt H1-14: the [NFSIO] print above (or another CPU) can stamp the
+     console a millisecond AFTER `now`; unsigned now - last then wraps */
+  uint64_t last = uart_last_activity_ms;
+  if (last >= now || now - last < WATCHDOG_SILENCE_MS) {
     return;
   }
```
(x86_64 only: the ARM `trap.c` has no such watchdog compare.)

**H1-03 (pre-game stall).**  In h-soak1 the serial file stopped growing for
~145 s right after IceWM started and the launcher's first row came only at
+150 s (a normal P9 boot launches the game at +25…90 s).  The 20 s vCPU
sample boot_p9 took in the middle shows the guest mostly idle and, in the
kernel, in `cons_hw_tx` / `uart_puts`: it was printing, and the bytes were
not reaching `serial.log`.  The image loader's own timings in that window are
normal, and the Wine `+timestamp` clock agrees with the host clock after the
stall, so the guest itself lost the time waiting on its console.  Two other
boots on strix show the same 120 s trigger and a +150 s launch
(lanes/kglitch-s2 kg-stack1, lanes/play r2-fps-scan5).  Candidates: the
UART TX path waiting on a 16550 that QEMU's main loop is not draining (its
`-serial file:` write blocked; the host was at loadavg 36 with ~79 GB page
cache), or a console lock held across the wait.  The same delay, without
the 120 s trigger, shows in the time from the QEMU start to the launcher's
first NFS mount in this lane's boots: 6–45 s in 8 of the first 13 boots,
71, 87, 90, 127 and 142 s in the other 5 (h-p8v2, h-p8l2, h-p8v3, h-play1,
h-soak1), with identical serial content and no correlation with host load
(max loadavg in the first minute 14–36 either way).  `run-hunt.sh`'s sampler now
records the serial file size and the host's Dirty/Writeback each second, so
the next occurrence shows which (boots from h-p8v4 on).

**H1-03, continued.**  With the sampler's I/O column (h-p8v5, h-p8v6) the
firmware-stage form lines up with the host: the guest sits at the
firmware's 113 bytes while `/proc/pressure/io` "full" climbs to 50–55 % and
Dirty is 2–3.7 GB, and gets through as the pressure drops.  QEMU honours
guest flushes with the default `cache=writeback`, so OVMF's variable writes
to the per-run `ovmf.fd` and the guest's NVMe flushes become host `fsync()`s
that wait behind everyone's dirty data.  Every image these boots use is a
per-run copy, so the flush is not worth a minute.  Proposed (H2-early owns
`tools/cp/p8/boot_p8.py`; the integrator / C6 owns `tools/cp/host-venus.sh`),
then an A/B of boot→launcher time on a busy host:

```diff
-           "-drive", f"if=pflash,format=raw,file={fw}",
+           "-drive", f"if=pflash,format=raw,file={fw},cache=unsafe",   # per-run copies: no host fsync per guest flush (cp-hunt H1-03)
@@
-           "-drive", f"file={disk},format=raw,id=disk0,if=none",
+           "-drive", f"file={disk},format=raw,id=disk0,if=none,cache=unsafe",
@@
-           "-drive", f"file={cpdata},format=raw,id=cpdata,if=none",
+           "-drive", f"file={cpdata},format=raw,id=cpdata,if=none,cache=unsafe",
```
(and the same in `host-venus.sh` lines 112 and 118, and the Venus cpdata drive in `boot_p8.py:111`'s `extra`).
`cache=unsafe` still writes everything back when QEMU exits normally, so a
P9 play boot's data disk keeps its save for the reload; only a host crash
mid-boot loses it, and these copies are throwaway.

**H1-04 (stale prefix stamp).**  Not a game bug: every game boot of this
lane ran a ~9.5 s `wineboot` prefix update (PreInstall, DefaultInstall,
Wow64Install) because the seeded image (made by integ15 with `wine-ntsync`)
carries the old Wine's `.update-timestamp` while the wave-8 default stage is
`wine-cp`.  It costs boot time, writes the prefix on every boot, ends with
the second `0xC0000135` `[EXITPC]` row (32-bit `rundll32`, no WoW64 Wine) and
opens the window for H1-05.  integ16's own `p8_pfx` will rebuild the image
with `wine-cp`; the harness should refuse or warn on a mismatch.  Proposed
(H2-early owns `tools/cp/p8/`), in `run-p8.sh` before the boot:

```diff
+  # the seeded prefix must have been made by this Wine stage, or every game
+  # boot re-runs wineboot's update (~10 s, cp-hunt H1-04)
+  st="$(/usr/sbin/debugfs -R 'cat /pfx/.update-timestamp' "$CPD" 2>/dev/null | head -1)"
+  wi="$(stat -c %Y "$WINE/share/wine/wine.inf" 2>/dev/null)"
+  [ "$PHASE" = game ] && [ -n "$wi" ] && [ "$st" != "$wi" ] &&
+    echo "== WARNING: prefix stamp $st != $WINE/share/wine/wine.inf mtime $wi: the prefix was made by another Wine stage (re-run P8_PHASE=pfx)"
```

**H1-06 / H1-07 (instruments that go blind).**  Both rows have a
lifetime budget that the game spends in its first seconds, the exact
failure mode `process.c`'s own comment warns about ("a noisy instrument with
a budget is worse than no instrument"):

* `[BADELR]` flags `elr` in `[0x70000000, USER_VA_BASE)`.  Wine maps every
  `.exe` at 0x140000000, inside that range, so the 20 rows go on the game's
  own healthy resumes in **every** game boot (this lane's and 127/128 old
  ones).  A real `elr=0x7` after that would not print.  Proposed (H4-kglitch,
  `src/kernel/process.c` ~2697):

  ```diff
  -        int implausible = (elr < 0x1000u) ||
  -                          (elr >= 0x70000000u && elr < USER_VA_BASE);
  +        /* a compat (Linux-ABI) process maps Windows PE images below
  +           USER_VA_BASE (Wine: 0x140000000 for every .exe), so there only
  +           the null page is implausible (cp-hunt H1-06) */
  +        int compat = (proc_table[idx].abi_flags & 1u) != 0;   /* PROC_FLAG_LINUX_ABI */
  +        int implausible = (elr < 0x1000u) ||
  +                          (!compat && elr >= 0x70000000u && elr < USER_VA_BASE);
  ```
  (`abi_flags` is group-anchor state; if a thread slot does not carry it,
  read the anchor's.)
* `[VENUS] fence_wait2 timeout` prints the first 16 expired waits of the
  boot.  All 16 come from short-timeout polls in the game's first ~6 s of
  Vulkan (the fence then retires: `done` catches up), so a real GPU hang
  later in a boot would not print.  Proposed (H4-kglitch / C6 owner,
  `src/kernel/gpu_abi.c` ~1409): count every timeout, print only waits whose
  caller asked for ≥ 1 s (`if (fw.timeout_ns >= 1000000000ull && to_rows++ < 16)`),
  and print the running count in an existing periodic row.

**H1-08 / H1-12 (P9 harness).**  `boot_p9.py`'s `seek` / `listseek` read the
highlighted row's screen position against tables measured on the 1280×720
layout.  At a 1920×1080 game resolution the menu is 1.5× larger and `canon()`
passes the 1920×1080 shot through unchanged, so no row is ever found
(h-fq1).  This lane's `tools/cp/hunt/fpsprobe-blind.p9` replaces the two
seeks with the tap counts every 720p boot needed (inputcheck: 10 of 10), and
the OCR `expect`s still guard each step; h-fq1b onwards use it.  Proposed
(H2-early owns `tools/cp/p9/`), untested:

```diff
@@ def canon(name):
-                if im.size == (1920, 1080) and args.xscreen == "1920x1080":
+                if im.size == (1920, 1080) and args.xscreen == "1920x1080" and args.game_res != "1920x1080":
                     return im
-                g = im.crop((0, 0, 1280, 720)) if im.size != (1280, 720) else im
+                if args.game_res == "1920x1080" and im.size == (1920, 1080):
+                    g = im.resize((1280, 720))     # the menu scales with the game's resolution
+                else:
+                    g = im.crop((0, 0, 1280, 720)) if im.size != (1280, 720) else im
@@ def main():
+    ap.add_argument("--game-res", default="1280x720", help="the game's own Resolution setting")
```
(plus `P9_GAME_RES` → `--game-res` in `run-p9.sh`).  H1-12: reload.p9's
CONTINUE seek lost the focus in both reload boots on h-play1's data disk,
in two different ways (evidence file), so it is the main menu's state after
the save list with that save set, not a dropped or doubled key (inputcheck
over 88 P9 logs: 0 dropped).  A seek that loses a visible focus should
re-read after a second, tap once back the other way, or fall back to taking
the newest save from the LOAD GAME list it already opened.

**The P8/P9 grader (known, H2-early).**  The overrides already list the P8
lavapipe exit 2–3 s after launch that the grader records as rc=0
"launcher-done".  `classify.py` flags it (`early_exit`: the game's `[P8] end
game … ms=` below 60 s and no menu OCR; golden-tested).  In the old run logs
it splits into two populations, 2.5–2.9 s (integ13-lvp1, integ15-lvp2,
k7l-1) and 20–24 s (integ10-venus, k6-venus, console-lvp1); in the four of
them whose serial survives, REDEngineErrorReporter exits twice and
services.exe / explorer.exe / plugplay.exe exit with code 1 (the whole Wine
session going down), which a successful boot never shows.  Proposed grader
diff (H2-early, `tools/cp/p8/boot_p8.py` ~287):

```diff
             if "[P8] done" in ser or "System halt" in ser:
-                verdict = "launcher-done"
+                # the game's own process ended and no main menu was seen:
+                # the early exit (cp-hunt), not a pass
+                verdict = ("game-exited-early" if args.phase == "game" and "[P8] end game " in seen
+                           else "launcher-done")
```
(`return 0 if verdict in ("launcher-done", "main-menu")` then makes it rc 1.)

## P9 fps (strix, Venus, 32 GiB, the P9 verdict for the ≥ 30 fps world target)

World = fpsprobe.p9's scripted 90 s walk (play / soak: every `world` /
`soakN` row); median and p10 of winevulkan's `trace:fps` "approx" rows (each
averages ~1.5 s of guest time); "world min row" is the lowest such row;
"hitches" counts consecutive fps rows more than 2.5 s apart in **guest**
time (`WINEDEBUG=+timestamp`), i.e. a frame that took ≥ 1 s.  (fps.csv's own
`game_s` is the host's read time, which follows boot_p9's screendump cadence,
so its gaps are not the game's.)  9070 XT busy = `gpu_busy_percent` of
renderD129, 1 Hz, over the world phases; it includes the other lanes'
Venus guests listed by the sampler.  The `prof-*` phases (boot_p9's QMP PC
sampler running) are left out: the sampler itself costs frames.

| boot | kind | verdict | world median | world p10 | world min row | rows | hitches (>1 s frame) | menu median | 9070 XT busy (world) med/p90 | game res MiB first→max | kernel stalls | other guests seen | script |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| h-fp1 | fp720 | script-stop | 29.0 | 24.7 | 12.89 | 62 | 0 | 145.7 | 35/40 | 509→4543 | venus×16, nfspipe×1 | 3 |  |
| h-fp2 | fp720 | script-stop | 33.4 | 20.6 | 11.34 | 66 | 0 | 207.7 | 33/37 | 432→4559 | nfspipe×8, venus×16 | 6 |  |
| h-fp3 | fp720 | script-stop | 30.0 | 26.4 | 22.46 | 64 | 0 | 215.6 | 35/40 | 592→4544 | venus×16, nfspipe×3 | 5 |  |
| h-fq1 | fp1080 | seek-failed:menu-load | None | None | - | 0 | 0 | 107.2 | -/- | 812→2752 | venus×16, nfspipe×5 | 5 | FAILED menu-load: main menu row 'LOAD GAME' not focused afte |
| h-fq1b | fp1080 | script-stop | 29.8 | 17.4 | 6.64 | 68 | 1 | 106.9 | 52/58 | 509→4547 | venus×16, nfspipe×2 | 5 |  |
| h-fq2 | fp1080 | script-stop | 33.0 | 26.3 | 13.29 | 64 | 0 | 111.4 | 54/61 | 430→4369 | venus×16, nfspipe×3 | 7 |  |
| h-play1 | play | script-done | 30.5 | 21.8 | 0.61 | 386 | 1 | 217.7 | 35/41 | 390→4704 | nfspipe×8, venus×15 | 8 |  |
| h-rl1 | reload | seek-failed:menu-continue | None | None | - | 0 | 0 | 207.7 | -/- | 400→2716 | nfspipe×3, venus×16 | 8 | FAILED menu-continue: main menu row 'CONTINUE' not focused a |
| h-rl1b | reload | seek-failed:menu-continue | None | None | - | 0 | 0 | 211.3 | -/- | 382→2700 | venus×16, nfspipe×1 | 3 | FAILED menu-continue: main menu row 'CONTINUE' not focused a |
| h-set1 | settings | seek-failed:menu-load | None | None | - | 0 | 0 | 221.0 | -/- | 515→2738 | nfspipe×2, venus×16 | 4 | FAILED menu-load: main menu row 'LOAD GAME' not focused afte |
| h-soak1 | soak | script-stop | 35.9 | 23.9 | 1.72 | 1080 | 12 | 213.6 | 37/41 | 459→4490 | nfspipe×7, venus×24 | 7 |  |

### Soak h-soak1: per phase

| phase | game s | fps rows | median | p10 | min | longest fps-row gap s (host read time) | game res MiB (last) | anon MiB (last) | threads (last) |
|---|---|---|---|---|---|---|---|---|---|
| soak1 | 281-558 | 182 | 34.5 | 21.3 | 3.4 | 5.3 | 4403 | 141064 | 48 |
| soak2 | 562-837 | 181 | 33.8 | 25.2 | 5.2 | 4.1 | 4408 | 141081 | 48 |
| soak3 | 840-1129 | 178 | 35.6 | 20.6 | 1.7 | 18.0 | 4449 | 141081 | 48 |
| soak4 | 1135-1410 | 181 | 36.0 | 25.9 | 8.3 | 4.2 | 4446 | 141097 | 48 |
| soak5 | 1412-1685 | 178 | 38.2 | 25.0 | 16.2 | 3.0 | 4473 | 141097 | 48 |
| soak6 | 1687-1964 | 180 | 43.0 | 29.8 | 4.8 | 4.2 | 4473 | 141097 | 48 |

resident growth over the soak phases: 4309 -> 4473 MiB in 28.0 min = 5.9 MiB/min

## The matrix: what ran (strix, one boot of this lane at a time)

| item | asked | ran | notes |
|---|---|---|---|
| 1. P8 to the menu | Venus ×6, lavapipe ×4 | Venus 6 (h-p8v1…6), lavapipe 4 (h-p8l1…4): 10/10 at the main menu (OCR); launch→menu Venus 110–170 s, lavapipe 202–279 s | P8 table below |
| 2. P9 fpsprobe | ≥3 × 1280×720 Low, 2 × 1920×1080 Low | h-fp1, h-fp2, h-fp3; h-fq1 (harness fail, H1-08), h-fq1b, h-fq2 | 1080p via `prep-settings.sh Resolution=1920x1080` + `P9_XSCREEN=1920x1080` + `fpsprobe-blind.p9` |
| 3. play / save / reload | ×2 | h-play1 (pass: 11.5 min, saved); reloads h-rl1, h-rl1b on its disk both stopped at the CONTINUE seek (H1-12); the second play/reload pair was dropped | play1: 11.5 min, F5 quick save, QUIT GAME, `QuickSave-0` on the data disk (playTime 4699.4, pos −1408.62, 1223.81, 23.07) |
| 4. long soak | 1 × 25–30 min | h-soak1 (28.0 min in six phases, + audio) | soak table below |
| 5. audio | 2 boots with `CP_AUDIO_WAV` | h-fp2, h-soak1 | WAVs measured, then deleted |
| 6. window modes / settings | 1 windowed, 1 settings change | h-win1: `WindowMode=Windowed` (`prep-settings.sh`), main menu by OCR, put_image path (`[P7] scanout refused` ×8, 72 `[HBP] path=putimage` rows), 0 faults, 0 flagged shots.  h-set1: the Settings menu driven by hand (`settings-menu.txt`): tabs with 1/3, the Field of View slider moved and moved back live, the Quick Preset right-arrow did not take, the game stayed up with no device-lost row; the return to the main menu hit H1-12, so the world was not re-entered with a changed setting | |
| 7. visual review | all screendumps | `shotcheck.py` over every boot | numbers only; shots stay on strix |
| 8. input | menus | `inputcheck.py` over this lane's and 88 older P9 run logs | |

Slots: from ~05:00 three of the four QEMU slots were held back to back by
other lanes' probe batches, and a boot of this lane waited up to 17 minutes
for one; the second play/reload pair (item 3) was dropped; everything else ran.

### P8 boots

| boot | kind | verdict | launch s | launch→menu s | kernel faults | kernel stalls (rows) | EXITPC≠0 | new Wine err sigs | new DXVK/vkd3d sigs | REDEngineErrorReporter | 9070 XT busy med/p90 | other guests seen |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| h-p8l1 | p8l | main-menu | 41.8 | 202.4 | 0 | nfspipe×3 | 2 | 0 | 0 | yes | 0/0 | 4 |
| h-p8l2 | p8l | main-menu | 106.5 | 278.5 | 0 | nfspipe×3 | 2 | 0 | 0 | yes | 0/0 | 7 |
| h-p8l3 | p8l | main-menu | 60.0 | 219.7 | 0 | nfspipe×6 | 2 | 0 | 0 | yes | 0/0 | 4 |
| h-p8l4 | p8l | main-menu | 78.3 | 266.1 | 0 | nfspipe×2 | 2 | 0 | 0 | yes | 0/0 | 4 |
| h-p8v1 | p8v | main-menu | 38.6 | 156.0 | 0 | nfspipe×1, venus×16 | 2 | 0 | 0 | yes | 63/73 | 4 |
| h-p8v2 | p8v | main-menu | 89.6 | 170.0 | 0 | venus×16 | 2 | 0 | 0 | yes | 4/72 | 4 |
| h-p8v3 | p8v | main-menu | 145.3 | 111.4 | 0 | nfspipe×2, venus×16 | 2 | 0 | 0 | yes | 0/66 | 5 |
| h-p8v4 | p8v | main-menu | 40.0 | 140.6 | 0 | nfspipe×3, venus×16 | 2 | 0 | 0 | yes | 0/73 | 5 |
| h-p8v5 | p8v | main-menu | 102.4 | 141.4 | 0 | nfspipe×2, venus×16 | 2 | 0 | 0 | yes | 0/73 | 3 |
| h-p8v6 | p8v | main-menu | 112.3 | 109.9 | 0 | venus×15, nfspipe×1 | 2 | 0 | 0 | yes | 73/93 | 4 |
| h-win1 | win | main-menu | 116.5 | 125.5 | 0 | nfspipe×4, venus×16 | 2 | 0 | 0 | yes | 1/73 | 3 |


## Old logs on strix: the census (`classify.py --mine`, `mine_report.py`)

Every `serial.log` / `serial.txt` / `*.serial` under `~/cp/integ1*/`,
`~/cp/lanes/*-logs/`, `~/cp/integ/obj/cp-p8*|cp-p9*` and every
`~/cp/**/obj/cp-p8*|cp-p9*` (595 paths) was read once.  Copies of one boot
(the integ batches copy `serial.log` to `~/cp/integ1N/p8-*.serial`;
`serial.txt` is `serial.log` without CRs) are counted once: the key is the
sha256 of the CR-stripped bytes.  That leaves **147 distinct boots, 128 of
them game boots** (146 duplicates dropped).  Full output:
`docs/cp-hunt-evidence/mine-census.txt`.  The older boots (wave 5 / early
wave 6, before C0-console) interleave rows from two CPUs on the serial
console, so the classifier cuts every kernel row at the next row tag and keys
it on its first six normalised words.

Verdicts of the 128 (from each run dir's `verdict.txt` where it survives):
script-stop 38, main-menu 28, launcher-done 26 (mostly wave-5 boots that died
at the fd ceiling), unknown 14, script-done 8, qemu-exited 6, harness
failures 6, first-run 1, game-exited-before-menu 1.

| signature (classify.py key) | old game boots | last seen | in this lane's 22 |
|---|---|---|---|
| `[LCOMPAT] fault sig=11` | 24/128 | 11-17 04:46 | 0 |
| `[TLBTIMEOUT] shootdown` | 10/128 (+1) | 11-17 12:36 | 0 |
| `[STALLBREAK]` | 15/128 | 11-17 12:36 | 0 |
| `[LCOMPAT] rt_sigreturn: bad rip/rsp`, `fault-in-handler sig=11` | 8/128 each | 11-17 04:46 | 0 |
| `[KERNEL] User process fault! Vector: 3` / `13` | 4/128, 1/128 | 11-17 04:20 / 00:03 | 0 |
| `KERNEL PANIC: stack smashing detected (SEC-…)` | 3/128 | 11-17 04:53 | 0 |
| `[KERNEL] FATAL`, `[BADVEC]`, `fork: AS clone failed` | 1/128 each | 11-17 02:35–03:37 | 0 |
| `Cyberpunk2077.e code=0xC0000005` (ACCESS_VIOLATION) | 11/128 | 11-17 04:40 | 0 |
| `CrashReporter … mscoree.dll not found` | 18/128 | 11-17 04:46 | 0 |
| `winediag:NtCreateFile Too many open files` | 4/128 | 11-17 00:40 | 0 |
| `services.exe` / `explorer.exe code=0x1` (Wine session down) | 34 / 33 /128 | 11-17 18:33 | 0 |
| `[VENUS] fence_wait2 timeout` (H1-07) | 95/128 | 11-18 01:59 | 18/18 Venus |
| `[NFSPIPE] stall` (H1-13) | 64/128 | 11-18 01:07 | 21/22 |
| `[BADELR] … Cyberpunk2077.e` (H1-06) | 127/128 | 11-18 01:59 | 22/22 |
| `[WATCHDOG] console silent` (H1-14) | 101/128 | 11-18 01:57 | 15/22 |
| `[VENUS] stall` (H1-15 class) | 2/128 (k7b-2, p7 ns-s-off1) | 11-17 21:19 | 1/22 |

Restricted to the 68 boots whose serial is from 2026-11-17 12:00 on (integ10
onwards) the only kernel-fault row is one `[TLBTIMEOUT]` at 12:36; the
crash classes above are all wave 5 / early wave 6 and gone on today's code.


## Harness (`tools/cp/hunt/`, new)

| file | what |
|---|---|
| `run-hunt.sh KIND RUN` | one matrix boot through the unchanged P8/P9 harnesses (`p8v`, `p8l`, `win`, `fp720`, `fp1080`, `play`, `reload`, `soak`, `settings`, `script`), plus a 1 Hz host sampler (9070 XT `gpu_busy_percent`, loadavg, and from h-p8v4 on the boot's serial.log size, host Dirty/Writeback and `/proc/pressure/io`; the other QEMU guests every 60 s: C1-k7's `k7-p9.sh` sampler, for the whole boot), the classifier, the audio checks (`HUNT_AUDIO=1`: `CP_AUDIO_WAV`, then the WAV is deleted), and the evidence copies into `~/cp/lanes/hunt-logs/` |
| `batch.sh QUEUE` | the matrix runner: one boot at a time from a queue file that is re-read before every boot (editable while it runs), `QUEUE.done` per boot |
| `classify.py` | per-boot signatures: kernel faults, kernel stalls, kernel instrument rows, `[EXITPC]` codes (NTSTATUS names), process exits, Wine `err:` lines (each tagged benign-with-reason or `new`), DXVK / vkd3d-proton `err`/`warn`, REDEngineErrorReporter, alsa-lib underruns, the `[WATCHDOG]` / `[BADELR]` instrument rows, the early-exit flag, guest-time fps hitches, `[THRS]` memory per process, GPU busy, the longest silent-serial stretch; `--aggregate` = k/n per signature over boots; `--mine` = every old serial log, deduplicated by content hash |
| `mine_report.py` | the old-log census by signature with last-seen times |
| `wavgaps.py` | dropouts (exact-zero runs of 5–500 ms between loud audio), gaps (≥ 500 ms silent), edge clicks, WAV length vs wall time |
| `shotcheck.py` | numbers-only screen of the screendumps: black / window-black / partial-black bands, full-width seams (tearing) off static edges, high-frequency "garbage", identical consecutive play shots, mean RGB; `--compare` Venus vs lavapipe menus |
| `inputcheck.py` | menu taps from `seek` / `listseek`: did each tap move the focus exactly one row |
| `prep-settings.sh` | a copy of a seeded data image with UserSettings.json options changed by debugfs (generalises `strix:~/cp/shot/prep.sh`) |
| `soak.p9` | the 28-minute in-world soak (six phases `soak1..soak6`) |
| `fpsprobe-blind.p9` | fpsprobe.p9 with the two menu seeks replaced by fixed tap counts, for game resolutions whose layout boot_p9's row tables do not know (H1-08) |
| `settings.p9` | main menu, then interactive (`ctl`): the hand-driven Settings / reload boot |
| `tables.py` | the P8 / P9 tables below from the per-boot files |
| `reload2.p9` (round 2) | the P9 reload without the H1-12 traps: CONTINUE from the fresh main menu with two settled Ups, F5 without moving, 2 minutes in the world, QUIT GAME |
| `focusprobe.py` (round 2) | boot_p9's own `focus_in()` on a shot without a tap (a failing `seek` ends the boot); `--list` the save-list frame with a choice of 1080p scaling; `--diff` where two shots differ |
| `ctl.sh` (round 2) | drive an interactive P9 boot (`ctl`) from ssh: append boot_p9 lines, wait for the shot, print its OCR and focus readings |
| `boot_p9_nn.py` (round 2) | boot_p9.py with only `game_view()` swapped for NEAREST scaling (the H1-17 experiment); `run-hunt.sh HUNT_BOOT=` runs a P9 boot with it |
| `compare.py` (round 2) | two log directories side by side (the integ15 vs integ17 table) |
| `test_hunt.py` + `golden/` | golden tests: classifier output, early-exit flag, aggregation, wavgaps on a synthetic WAV with a planted 20 ms dropout and 1 s gap |
