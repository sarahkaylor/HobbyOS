# cp/p9 — Cyberpunk 2077 played in HobbyOS on Venus (lane C9-play, wave 6)

Plan: `docs/cyberpunk.md` §6 P9, §7.2f (C9-play row; the user's decisions of
2026-11-17).  Built on the P8 recipe (`docs/cp-p8.md`) and C6's Venus host
recipe (`docs/cp-venus.md`).  Every number below was measured on **strix**
(Ryzen AI MAX+ 395, Zen 5, AVX-512, KVM; RX 9070 XT through Venus on
renderD129), guest `-cpu host -smp 8`, tree `cyberpunk` @ 058fd3d5 plus this
lane's harness commits (no kernel change).  Evidence: `docs/cp-play-evidence/*.txt`;
screendumps (git-ignored) under `evidence/cp-play/`.

**Round 2 (integ9: C1-k6, C6-p7, cp-fork; §10).**  The user's unchanged
1280×720 window now covers a 1280×720 XHOBBY screen (`HB_SCREEN_SIZE`), and
every frame goes through C6-p7's zero-copy scanout.  Load, 10.5 minutes of
play, save, reboot and reload to the identical position pass again.  World
fps is **19–26 median (p10 12–16)**, up from 12–15.  Menus run at 90–110 fps.
The 30 fps target is **still not met** in the world.  The guest is no longer
kernel-bound (44–55 % idle), and in the world scanout and put_image are level
(19.0 vs 19.5).  P9-1 is gone: the reporter's CreateProcess takes 0.1 s, with
0 early exits in 11 boots.  New: stop P9-8, an X session wedge on `fat_lock`
before the game (2 of 13 boots).

**Round 1 result.**  **Gate P9's functional half passes on Venus; its frame-rate target
does not.**  Boot A (gateA5): main menu → LOAD GAME → the user's ManualSave-86
(The Ripperdoc, Little China, Watson) → **11.1 minutes in the world** driven by
scripted QMP keyboard and relative-mouse input (walk, sprint, strafe,
mouselook, the map twice) → F5 → QUIT GAME (status 0).  Boot B (gateB6), a
fresh boot on boot A's disk: CONTINUE → the quick save → **the same place**
(screendumps 0.1 % apart; a new quick save has exactly the same
playerPosition) → QUIT GAME.  No crash, no first-run screen, `e2fsck -fn`
rc 0, game copy unchanged.  **In-world fps: median 12.1–15.0 (p10 7–10)** at
1280×720 against a 30 fps target; menus and loading 34–48 fps.  The guest
spends 55–65 % of its vCPU time in its own kernel, about half of it spinning
on `proc_lock` (§5, owner C1-k6).  The "clean early exit" of integ7 is the
game quitting when its CreateProcess of the crash reporter takes ~17 s,
because HobbyOS's fork walks every page of a 135 GiB reservation (§4, a
kernel fix; probe `p9forkprobe`).  The guest needs ~4 GiB in the world:
12 GiB is enough (§7).

## 1. The harness (`tools/cp/p9/`)

| file | what |
|---|---|
| `run-p9.sh` | host driver: makes `obj/cp-p9/pfx-saves.img` once, then runs `tools/cp/p8/run-p8.sh` with the P9 hooks; afterwards the summary and the save list |
| `seed-saves.sh` | copies the user's saves into a copy of P8's seeded prefix (debugfs, sha256 read back) |
| `boot_p9.py` | boot + play: big guest, play-script interpreter, session record (shots, fps/mem/thread rows with wall times, phases), guest PC sampler |
| `play.p9` | Gate P9 boot A: LOAD GAME → ManualSave-86, ~10 min of play, F5, QUIT GAME |
| `reload.p9` | Gate P9 boot B on boot A's data disk: the save list, CONTINUE, same place?, F5 again, QUIT GAME |
| `ctl.p9` | interactive boot: commands appended to `OUT/ctl.txt` over ssh |
| `summarize.py`, `saves.sh`, `prof.py` | fps per phase, saves on the image (name, time, play time, playerPosition), guest PC profile |

**Reuse, not a copy.**  `run-p8.sh` gained hooks and keeps doing the build,
the staging export, the **reference-counted** game-copy export
(`/tmp/cp-game-export.lock`; P9 never calls `exports.sh` on `/srv/nfs/cp-game`),
the data disk, `e2fsck -fn` and the game-copy census:
`P8_BASE`, `P8_SEEDED`, `P8_CPDATA_FROM` (the reload boot starts from the
previous boot's `cpdata.img`), `P8_BOOT` + `P8_BOOT_ARGS` (the boot script),
and `P8_MEM` up to 32768 — refused unless the boot script is
`boot_p9.py`.  `boot_p9.py` imports `boot_p8.py`'s helpers unchanged.

**The big guest.**  The user approved up to `-m 32G` for P9 game boots, one at
a time.  `boot_p9.py --big-guest` takes `flock /tmp/cp-bigguest.lock` *inside*
its QEMU slot.  `tools/cp/host-venus.sh` (C6's) refuses `MEM` above 12 GiB,
which is its slot rule, so `boot_p9.py` asks it for 12288M and rewrites the
two memory arguments (`-m`, the memfd `size=`); `cmdline.txt` records it.
The guest kernel uses it: `[FRAME] frames=8192000 total_mib=32000`.

**The saves.**  `seed-saves.sh` writes `ManualSave-86` and `AutoSave-16`
from `~/cp/game-profile` into
`pfx/drive_c/users/root/Saved Games/CD Projekt Red/Cyberpunk 2077/`, each
file sha256-checked, `e2fsck -fn` rc 0 before and after.  Their metadata:

| save | quest (tracked) | where | play time | saved |
|---|---|---|---|---|
| AutoSave-16 | The Streetkid (prologue, `q000_street_kid`) | El Coyote Cojo, Heywood (a bar, prologue) | 1 min 38 s | 2025-08-01 |
| ManualSave-86 | The Ripperdoc (`q001_01_victor`, "Talk to Jackie") | Megabuilding H10 atrium, Little China, Watson — open world | 1 h 7 min | 2025-08-08 |

ManualSave-86 is the open-world one, so the gate uses it.

**No first-run screen is ever answered.**  `boot_p9.py` OCRs every screendump
against `boot_p8.FIRST_RUN` (agreement, account offer, telemetry) at any point
of the boot and stops with `first-run:<name>` if one appears.  None did: the
user's preseeded profile carried their choices.  The only input before the
main menu is boot_p8's Space at "PRESS … TO CONTINUE".  After it, every input
is ordinary gameplay: menus, loading, walking, the map, saving, quitting.

## 2. How the game is driven (from the interactive boot `ctl1`)

`ctl1` ran `ctl.p9`: from the main menu on, every command came over ssh
(`OUT/ctl.txt`) and every result was a screendump.  What it established, and
what `play.p9`/`reload.p9` now encode:

- **The keyboard drives every menu; QMP `input-send-event` key down/up.**
  Taps closer than ~0.5 s are dropped by REDengine's UI (9 "up" taps 0.27 s
  apart moved the focus 2–3 rows; 0.8 s apart, every tap counts), so `key`
  holds 200 ms and waits 800 ms.  The focus is hidden until the first key.
  In the main menu "up" past CONTINUE lands on the top-right patch-notes
  button (gateA1 opened "CHANGES IN PATCH 2.5"), so no tap count is trusted:
  `seek` finds the yellow-framed row in the screendump and maps its screen
  row to the menu item (main and pause menus measured on `ctl1`), and
  `listseek` does the same for the save list's cyan frame.
- **The relative mouse works for mouselook.**  QMP `rel` events (PS/2) reach
  the DDX's relative slave, XI2 raw motion and Wine's `WM_INPUT`
  (`docs/cp-x2.md` §5): `look 400 0` turned V about 60° to the right.
  P0 found the *absolute* pointer useless; the relative path is what the game
  reads.
- **Walking, sprinting, strafing:** `hold w`, `down shift` + `hold w`,
  `hold a`/`d`/`s` — all move V (minimap and view change).
- **The map:** `M` opens it, `M` closes it.  Esc on the map goes on to the hub
  menu (gateA2 lost its loops there); `play.p9` now checks the HUD after.
- **Saving:** F5 quick-saves in the world and in the pause menu
  (QuickSave-N on the ext2 prefix).  SAVE GAME → NEW SAVE also works
  (ManualSave-87 in `ctl1`); a keyboard Return on an *existing* entry asks to
  overwrite it, so the scripts use F5.
- **Quitting:** Esc → pause menu → QUIT GAME → "Are you sure you want to
  quit?" → Return.  The game exits with status 0 and the launcher's
  `wineserver -k` / sync follow; `e2fsck -fn` rc 0.
- **LOAD GAME groups saves by playthrough;** `0` expands every group.  With
  the two seeded saves the list is: the Streetkid group (AutoSave-16), then
  the second Streetkid group whose single entry "The Ripperdoc"
  (ManualSave-86, Little China) is centred at screen row 509.
- **A vehicle:** V calls one; nothing came (this V owns no car yet at this
  point of the story; Jackie drives in The Ripperdoc), so there is no
  driving in the gate.  A later save would be needed for it.

## 3. Gate P9 boots

| boot | script | result |
|---|---|---|
| ctl1 | `ctl.p9` (interactive) | menu at launch + 187 s; ManualSave-86 loaded (100 s); ~6 min in the world; F5 ×2, ManualSave-87; QUIT GAME; status 0, e2fsck rc 0 |
| gateA1 | `play.p9` v1 | stopped: `shlex` ate the regex backslashes (`/LOADs*GAME/`), and blind tap counts opened the patch notes — fixed (`seek`) |
| gateA2 | `play.p9` v2 | stopped by hand: Esc from the map → hub menu, loops ran in the hub — fixed (`M` closes the map, HUD check). Its SIGTERM left QEMU running without a slot for ~1 min (killed by pid); `boot_p9.py` now turns SIGTERM into a clean exit |
| gateA3 | `play.p9` v3 | **stop P9-1**: the game exited by itself, status 0 at launch + 20.4 s (§4) |
| **gateA4** | `play.p9` v3 | **boot A PASS**: LOAD GAME → ManualSave-86; 8.8 min in the world (walk, sprint, strafe, look, map ×2); F5 → QuickSave-0; QUIT GAME; status 0; e2fsck rc 0; game copy unchanged |
| gateB4 | `reload.p9` v1 | stopped on a too-strict check: full-screen OCR does not read the small save names ("QuickSave-0") — fixed |
| **gateB5** | `reload.p9` on gateA4's disk | **boot B PASS**: the list shows the new saves; CONTINUE loads QuickSave-0; same place (OCR "THE RIPPERDOC / Talk to Jackie", screendumps); F5 → QuickSave-1 within 1.5 m of QuickSave-0; 2 min more play; QUIT GAME; status 0; e2fsck rc 0 |
| fps-smp4 | `fpsprobe.p9`, `-smp 4 -m 12G` | experiment: world fps median 9.2 (vs 13.8 at `-smp 8`) |
| **gateA5** | `play.p9` final (longer loops) | **Gate P9 boot A PASS**: LOAD GAME → ManualSave-86; **11.1 min in the world** (walk, sprint, strafe, mouselook, map ×2, three guest profiles); F5 → QuickSave-0; QUIT GAME; status 0; e2fsck rc 0; game copy unchanged |
| **gateB6** | `reload.p9` on gateA5's disk | **Gate P9 boot B PASS**: the save list shows the new saves; CONTINUE → QuickSave-0; **the same place**: the screendump right after the reload differs from boot A's right after F5 in 0.1 % of the scene's pixels, and the F5 quick save made after the reload with no movement has **exactly** QuickSave-0's playerPosition (-1443.71, 1308.92, 27.07); 2.1 min more play; QUIT GAME; status 0; e2fsck rc 0 |

**Same place, by the game's own numbers** (`tools/cp/p9/saves.sh`, the saves'
`metadata.*.json` `playerPosition`).  Final pair gateA5/gateB6: QuickSave-0
(boot A) and QuickSave-1 (boot B, F5 after the reload with no movement input)
are both at (-1443.71, 1308.92, 27.07), and the two screendumps
(`gateA5/s120-t000959-saved-place.png`, `gateB6/s054-t000346-reloaded-place.png`)
differ in 0.1 % of the scene's pixels (`p9-gateB6-strix.txt`).  The earlier
pair gateA4/gateB5: boot A's QuickSave-0
(-1414.84, 1287.93, 27.08), and boot B's QuickSave-1, made after the reload
with no movement input in between, (-1415.08, 1289.36, 27.08): 1.45 m apart
(ManualSave-86, where boot A started, is at (-1429.96, 1255.29, 23.09), 37 m
away).  The screendumps `gateA4/s105-…-saved-place.png` and
`gateB5/s036-…-reloaded-place.png` show the same corridor, door and vending
cart.

### 3.1 Wall times (strix, host clock; final pair gateA5 / gateB6, and gateA4)

| point | boot A (gateA5) | boot B (gateB6) | gateA4 |
|---|---|---|---|
| boot → game launch | 36 s | 48 s (4 other guests on strix) | 34 s |
| launch → first window | 61 s | 46 s | 45 s |
| launch → main menu (2nd OCR hit) | 158 s | 208 s | 142 s |
| main menu → loading starts | 18 s (seek LOAD GAME, expand, listseek) | 20 s (save list, then CONTINUE) | 16 s |
| loading screen → in the world | 131 s | 118 s | 132 s |
| in the world | **11.1 min** | 2.1 min | 8.8 min |
| QUIT GAME → game process ended | 16 s | 25 s | 15 s |

Launch → menu on Venus: 142–208 s with the user's profile and saves (integ7:
161–177 s; P0's Linux guest on Venus: 41 s).  The 16.7 s CreateProcess of
§4 is inside every one of them.

### 3.2 Frame rate (winevulkan `trace:fps` "approx" rows, each the mean since the previous row, ~1.5 s)

1280×720, the user's settings (Low-equivalent "Custom", RT off, scaling off),
borderless windowed on the 1920×1080 desktop.  Rows taken while the PC
sampler ran are their own phase (`prof-*`) and are left out.

| phase | gateA5 (boot A) | gateB6 (boot B) | gateA4 | gateB5 |
|---|---|---|---|---|
| intro + title (launch) | 35.9 (22.4, 35.0), 60 | 37.3 (24.2, 35.4), 94 | 40.8 (28.1, 39.1), 60 | 44.6 (16.6, 40.3), 56 |
| main menu | 42.6 (37.2, 39.3), 11 | 39.6 (30.1, 37.3), 12 | 40.6 (32.9, 38.7), 9 | 48.0 (24.8, 43.5), 29 |
| loading screen | 35.6 (13.0, 34.0), 81 | 33.6 (9.3, 32.2), 73 | 40.5 (16.0, 38.1), 83 | 41.0 (15.0, 39.4), 60 |
| **in the world** | **12.1 (7.0, 11.8), 329** | **12.8 (9.6, 12.4), 44** | **13.8 (7.2, 13.2), 250** | **15.0 (8.6, 14.5), 44** |
| map | 20.7 (8.0, 19.3), 22 | — | 27.2 (12.3, 23.4), 18 | — |
| pause menu → quit | 46.4 (22.5, 40.3), 4 | 46.5 (19.3, 41.2), 13 | 45.8 (23.0, 38.7), 5 | 50.3 (20.6, 43.7), 11 |

(median (p10, mean), number of rows)

**Target (§6 P9):** ≥ 30 fps at 1280×720 Low, or within 70 % of the Linux-guest
column, whichever is lower.  P0's Linux guest on Venus has only a main-menu
number (median 142.8–144.4 fps, `docs/cp-p0-guest.md` §5.5); 70 % of that is
~100 fps, so the target is **30 fps**.  **Not met in the world: median
12.1–15.0, p10 7–10, over four boots.**  The menus reach 40–50 fps (P0 guest: 143; host: 388).
`ctl1` (another walk, the same save) gave 5–15 fps in the world and its
street segment dipped to 5.

## 4. Stop P9-1 — the clean early exit is a 17-second process creation (kernel: fork of a big address space)

**Symptom** (integ7 `integ10-venus`; cp-play gateA3; 2 of 13 Venus game boots: integ7 1 of 3, cp-play 1 of 10):
the game ends by itself with status 0 at launch + ~20 s, before its window,
after loading ~84 modules; `REDEngineErrorReporter.exe` is starting.

**What happens** (`docs/cp-play-evidence/p9-stop1-early-exit-fork-strix.txt`,
WINEDEBUG `+timestamp,+process`):
- at launch + ~3.7 s the game's main thread (0140) calls
  `CreateProcess("REDEngineErrorReporter.exe <its own pid>")`;
- Wine's `NtCreateUserProcess` inside it takes **16.7 s** (gateA4:
  37.358 → 54.052 s; gateB5: 16.5 s; gateA5: 39.08 → 56.69 s, 17.6 s); the reporter's first
  instruction runs at 54.093 s; the game's thread queries its own process
  (`NtQueryInformationProcess` class 0, twice) at 54.166 s and goes on
  (steam_api64, the window, the menu);
- in the bad boots the same sequence ends differently: right after those two
  queries, ~16.7 s after the call, the game's threads exit with status 0
  (no exception row, no fault, no crash dump).  The game evidently measures
  how long that stretch of its start-up took and quits when it is too long; a
  16.7 s process creation sits right at its limit, so a slightly slower fork
  tips it over (2 of 13 Venus boots).

**Why process creation takes 16.7 s:** Wine's `NtCreateUserProcess` forks the
game process on the unix side (twice: Wine double-forks to detach) and execs
the preloader.  HobbyOS's fork cost grows with the parent's *reserved* address
space.  `src/user/cp/p9forkprobe.c` (`P8_PHASE=probe`, rows `[P9FORK]`):

| parent | HobbyOS guest | Linux host (strix) |
|---|---|---|
| F1 small | 75 ms | 0.1 ms |
| F2a + 64 GiB PROT_NONE reservation only | **3,937 ms** | 0.1 ms |
| F2b + 1,300 touched 256 KiB mappings only (325 MiB) | 1,329 ms | 13.9 ms |
| F2 both | 5,736 ms (FAIL) | 6.7 ms |
| F3 both + 40 blocked threads | 5,854 ms (FAIL) | 11.4 ms |
| F4 = F3 with vfork + exec | 13 ms | 0.1 ms |

(median of 5 fork+exec → child's first instruction.)  The game at that point
has 668 regions and 135 GiB of anonymous VA (`[THRS] anon_mib=135451`, almost
all of it PROT_NONE reservations, P0 §5) and 508 MiB resident: by F2a's
slope (~61 ms per reserved GiB) one fork costs ~8 s, and Wine forks twice.

**Cause in the kernel:** `vm_as_clone_into()` (`src/kernel/vm.c:5304`) walks
every 4 KiB page of every region — PROT_NONE reservations included — taking
`vm_lock` (irqsave) and doing a page-table walk per page, and it copies every
resident private page eagerly (the "D10 resident-copy fork").  Proposed fix:
walk the source page tables (skip non-present PML4/PDPT/PD entries, so the
cost is O(present pages), not O(VA)), take `vm_lock` once per region or per
table, and skip regions with `prot == PROT_NONE` and no present leaf.  COW
instead of copying is the larger follow-up.  Gate: `[P9FORK] F2/F3 PASS`
(median < 1 s; Linux: 7–11 ms).

**Owner:** the kernel; it is not in any wave-6 row's file list (fork/`vm.c`).
Proposed: C1-k6 (process lifecycle) or the integrator assigns.  Nothing in
`tools/cp/p8|p9` can shorten it: the game, not the harness, creates the
reporter.

## 5. Stop P9-2 — in-world fps is bounded by the guest kernel: `proc_lock`

**Profile** (`boot_p9.py prof`: QMP `info registers -a` at 10 Hz on all 8
vCPUs for 20 s, symbolized with `nm hobbyos.elf` by `tools/cp/p9/prof.py`;
`docs/cp-play-evidence/p9-stop2-kernel-bound-fps-strix.txt`), gateA4, in the
world:

| sample | kernel | idle (HLT) | user | top kernel symbols |
|---|---|---|---|---|
| world-start | 64.8 % | 27.0 % | 8.2 % | `spin_acquire_raw` 31.5 %, `get_cpuid` 7.3 %, `uart_puts` 2.8 %, `schedule` 2.2 %, `sched_wake_idle_cpu` 2.1 % |
| world-mid | 56.1 % | 33.2 % | 10.7 % | `spin_acquire_raw` 28.1 %, `get_cpuid` 6.2 % |
| world-end | 57.6 % | 33.0 % | 9.3 % | `spin_acquire_raw` 29.9 %, `get_cpuid` 5.7 %, `uart_puts` 2.8 % |
| gateB5 reload-world | 52.6 % | 37.0 % | 10.3 % | (same order) |
| fps-smp4 (4 vCPUs) | 58.8 % | 24.4 % | 16.8 % | `spin_acquire_raw` 17.0 %, `get_cpuid` 11.3 %, `uart_puts` 4.1 % |

- The game's own code (`Cyberpunk2077.exe` image) is 5–7 % of the samples;
  all user code together 8–11 %.  The 9070 XT is far from busy.
- vCPU 0 never runs user code (75 % idle); vCPUs 1–7 spend 55–88 % in the
  kernel.
- The spun-on lock is **`proc_lock`**: every `[LOCKDIAG]` row of gateA4 names
  `lock=0x74D4E948` (89 rows), which `nm` resolves to `proc_lock`.  The
  stack-word sampler (`prof … stack`, gateB5) puts these callers under
  `spin_acquire_raw` (rough — stale words also appear): `schedule`,
  `file_select_words` (select/poll), `l_proc_rt_sigprocmask`,
  `process_others_alive` (from the ext2 `flusher_task`) and the watchdog's
  `wd_dump_proc_table`.
- `get_cpuid` (5–11 %) is called by the lock-audit bookkeeping on every lock
  operation (`ls_cpu()` in `lockscale.c`) and does an `rdmsr` of GS_BASE each
  time.
- `-smp 4` lowers the spin share (17 %) but also the parallelism: world fps
  median 9.2 vs 13.8.

**Sleep(1)** costs 7.6–24 ms in these boots (p8caps: 50× Sleep(1) =
380–1,210 ms).  A world frame takes ~70 ms (14 fps), and the vCPUs are
mostly busy in the kernel, not idle, so fps here tracks the kernel's lock
contention, not the sleep granularity; Sleep(1) cannot be isolated as a cause
until `proc_lock` contention is gone.

**Owner: C1-k6** (scheduler sleep/wake paths, signals in `process.c`): take
`proc_lock` out of `schedule()`'s fast path, `rt_sigprocmask`, select/poll
wakeups and the ext2 flusher's liveness check, or split it.  The lock-audit's
`get_cpuid()` (GS_BASE `rdmsr` per call) is a cheap second win (a per-CPU
`%gs` read).  Gate: this lane's `play.p9` world fps, or `fpsprobe.p9`
(3 min of walking + profile) for a quicker A/B.

## 6. Other stops and observations, in order

| # | what | evidence | owner |
|---|---|---|---|
| P9-1 | clean early exit at launch + 20 s (2/13 Venus boots) = 16.7 s CreateProcess of the crash reporter = fork cost ∝ reserved VA | §4, `p9-stop1-early-exit-fork-strix.txt`, `[P9FORK]` | kernel fork/`vm.c` (C1-k6 or integrator) |
| P9-2 | world fps 13.8–15.0 median: 55–65 % of vCPU time in the kernel, ~30 % spinning on `proc_lock` | §5, `p9-stop2-kernel-bound-fps-strix.txt` | C1-k6 |
| P9-3 | the title/loading stall (C6 v19) | **not seen**: 11 of 11 boots that passed the 20 s mark reached the menu, and every save load finished (91–125 s) | — |
| P9-4 | the LOAD GAME list shows "00/00/0000" for most save dates (sometimes the seeded save's real date) | screendumps `ctl1/s015`, `gateB4/s020` | cosmetic; probably the game's file-time/locale query; not chased |
| P9-5 | no audio device (`mmdevapi: No driver from pulse,alsa,oss,coreaudio`) — the game runs silent, as in P0 | serial | AD-9 (virtio-sound), later |
| P9-6 | no vehicle at this point of the save's story | §2 | needs a later save |
| P9-7 | Wine game trace: `Implementation supports neither present_wait1 or present_wait2. Latency will increase.` (vkd3d-proton) | serial | C6-p7 (present-wait comes back with scanout present) |

## 7. Memory

`[P8MEM]` (free frames of the guest's frame pool, launcher rows every 30 s):

| guest | pool | free before the game | free in the world | in use |
|---|---|---|---|---|
| `-m 32G` (gateA4) | 32,000 MiB | 31,652 | 27,572–27,628 | ~4.0 GiB |
| `-m 12G` (fps-smp4) | 11,520 MiB | 11,172 | 7,240–7,354 | ~3.9 GiB |

Venus keeps the game's "VRAM" on the host, so the guest holds only the game,
Wine and the X session: about 4 GiB in the world, the same at 12 and 32 GiB.
The game process: 45–55 threads, ~141 GiB anonymous VA (reservations),
**4.3–4.5 GiB resident** (`[THRS]` `res_mib` 4,337–4,495).  So **12 GiB is
enough for P9 on Venus**; the 32 GiB approval was not needed (but fork's
eager copy of resident pages, §4, would double resident memory for an instant
on every CreateProcess from the game).

## 8. Commands

```
# once: the P8 prefix image (tools/cp/p8, wineboot -i + seeding)
P8_PHASE=pfx TIMEOUT_S=2400 P8_EXPORT=/srv/nfs/cp-play-p8 PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 bash tools/cp/p8/run-p8.sh
# Gate P9 boot A (makes obj/cp-p9/pfx-saves.img on first use)
P9_SCRIPT=tools/cp/p9/play.p9 P9_RUN=gateA PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 bash tools/cp/p9/run-p9.sh
# Gate P9 boot B: reboot on boot A's data disk, reload, same place?
P9_SCRIPT=tools/cp/p9/reload.p9 P9_FROM=gateA P9_RUN=gateB PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 bash tools/cp/p9/run-p9.sh
# a 3-minute fps A/B (e.g. -smp 4, 12 GiB)
P8_SMP=4 P9_MEM=12288 P9_SCRIPT=tools/cp/p9/fpsprobe.p9 P9_RUN=fps-x ... bash tools/cp/p9/run-p9.sh
# the fork probe (no game; one 12 GiB boot, ~1 min)
P8_PHASE=probe P8_EXPORT=/srv/nfs/cp-play-probe PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 bash tools/cp/p8/run-p8.sh
# an interactive boot: P9_SCRIPT=tools/cp/p9/ctl.p9, then  echo "key m" >> obj/cp-p9/<run>/ctl.txt
python3 tools/cp/p9/prof.py obj/cp-p9/<run>/prof-<tag>.txt hobbyos.elf
```

## 9. Screendumps (git-ignored, `evidence/cp-play/` on the Mac worktree; all of every run under `strix:~/cp/lanes/play/obj/cp-p9/<run>/shots/`)

- `gateA5/s016-…-loadlist-ripperdoc.png` — LOAD GAME with both playthrough groups expanded.
- `gateA5/s055-…-inworld.png`, `s058-…-w1.png`, `s060-…-w2.png`, `s065/s076/s089-…-loop.png` — the world after the load, walking/sprinting.
- `gateA5/s061-…-map.png` — the map (Watson, Little China).
- `gateA5/s120-t000959-saved-place.png` and `gateB6/s054-t000346-reloaded-place.png` — **the same place** before the quit and after the reboot + reload.
- `gateB6/s021-…-loadlist-newsaves.png` — the save list on the reloaded disk (QuickSave-0, AutoSave-0, ManualSave-86).
- `gateA5/s127-…-quit-confirm.png` — the quit dialog.
- `ctl1/s031-…-walk1.png`, `s032-…-look1.png` — walking into Jackie's dialogue, then the relative-mouse turn; `s035-…-map1.png`, `s037-…-sprint1.png`, `s060-…-newsave2.png` ("GAME SAVED", ManualSave-87).

## 10. Round 2 on `integ9` (C0-console + C6-p7 + C1-k6 + cp-fork): the game on the zero-copy scanout

`cp-play` merged `integ9` @ adb81ee4.  It brings C1-k6 (`Sleep(1)` ≈ 1 ms,
`proc_lock` off the hot paths), C6-p7 (`GPU_IOC_SCANOUT_BLOB` zero-copy
fullscreen present, `VK_KHR_present_wait`, async present) and cp-fork (fork
walks only what is mapped, private pages COW).  The guest Venus Mesa is this
tree's `tools/mesa/venus/build.sh` (C6-p7's WSI), built into
`VENUS_WORK=~/cp/lanes/play-venus`.

### 10.1 The game covers the screen without touching the user's settings

C6-p7's flip is granted only to the one X window that covers the whole
screen.  The user's profile (unchanged, `~/cp/game-profile`) opens a
1280×720 borderless window, which on the 1920×1080 XHOBBY screen stays on
put_image.  So the X screen now starts at 1280×720:

- **XHOBBY** (`third_party/x11/ddx/hobbyos/hbscreen.c`): `HB_SCREEN_SIZE=WxH`
  in the environment sets screen 0's startup size: the env twin of the
  existing `-screen WxH`, read from `/X11/XSESSION.ENV` like `HB_XDEPTH`.
  Unset keeps the framebuffer's 1920×1080, so nothing else changes.  The
  picture sits top-left on the glass like a RandR mode; RandR can still go to
  1920×1080.
- **`run-p8.sh`**: `P8_XSCREEN=WxH` appends it to the run's `XSESSION.ENV`.
- **`run-p9.sh`**: `P9_XSCREEN` (default **1280x720**), `P9_SCANOUT=0` (puts
  `HB_SCANOUT=0` into the game's environment through `P8_EXTRA_ENV`: the
  put_image A/B on the same screen), `VENUS_BUILD`, and `VENUS_VNC=1`.  A
  blob on the scanout is only visible over QEMU's VNC socket
  (`docs/cp-p7.md` §4); `pmemsave` shows the X framebuffer.

Result: `[P7] scanout taken` at the first window, and **every present of the
game is a blob flip** (`[HBP] path=scanout`, 0 put_image, 0 refused) from the
intro to the quit.

### 10.2 Harness changes for the 1280×720 glass

- VNC shots are 1280×720 while the game owns the scanout and 1920×1080 (X top-left) otherwise.
  The menu and save-list row tables are kept in the old 1920×1080 layout, and
  each shot is mapped into it (`canon()`).
- OCR on the new shots was erratic (the menu items, the red "PRESS … TO
  CONTINUE", the red LOAD GAME title).  `shoot()` now OCRs the shot both bare
  and in the old layout.  Two red-ink-mask OCRs back it up: one on the title
  prompt's band (`prompt_ocr`, also used by `expect … CONTINUE`) and one on
  the main menu's item column (`menu_ocr`).  The save-list check also accepts
  its footer ("Collapse/Expand groups", "Cross Progression").
- The focused row's yellow is dimmer on the glass (~170,125,40 against
  ~230,190,60 on pmemsave shots).  `seek` takes the dimmer yellow and stops the
  main-menu scan above the orange Phantom Liberty banner.
- `[HBP]` rows go to `hbp.csv` (per phase: frames by path, flip/sub/wait µs in
  `summary.txt`), and `[P7]` transitions go to the timeline.
- When no launcher/Wine row arrives for 120 s before the game starts,
  `boot_p9.py` samples the vCPUs once with kernel stack words
  (`prof-prestall.txt`), see §10.5.
- `boot_p8.py` turns SIGTERM into a clean shutdown, as `boot_p9.py` does
  (C6-p7 orphaned a QEMU for 44 min).

### 10.3 Boots on integ9 (strix, `-smp 8 -m 32G`, Venus, X screen 1280×720)

| run | script, path | result |
|---|---|---|
| r2-fps-scan1, -scan2, -scan4 | fpsprobe, scanout | harness bring-up: the title prompt, the LOAD GAME title and the menu items were not OCR'd on the new shots → §10.2 fixes; stopped by SIGTERM (clean) |
| r2-fps-scan3, -scan5 | fpsprobe, scanout | **stop P9-8**: the X session wedges on `fat_lock` before the game (§10.5) |
| **r2-fps-scan6** | fpsprobe, **scanout** | 3 min of walking; menu at launch + 144 s |
| r2-fps-put1 | fpsprobe, put_image | menu-item OCR missed (→ `menu_ocr`) |
| **r2-fps-put2** | fpsprobe, **put_image** (`HB_SCANOUT=0`) | the A/B of r2-fps-scan6 |
| **r2-play-scan1** | play.p9 (Gate boot A), **scanout** | LOAD GAME → ManualSave-86, **10.5 min in the world**, map ×2, F5 → QuickSave-0; the pause-menu `seek` then missed the dim VNC yellow (fixed), so QEMU was stopped by the harness (e2fsck rc 0, the save on disk) |
| **r2-reload-scan1** | reload.p9 (Gate boot B) on r2-play-scan1's disk, scanout | **PASS**: save list with the new saves, CONTINUE → QuickSave-0, **the same place** (screendumps 1.1 % apart; the F5 after the reload has the identical playerPosition (-1451.06, 1213.55, 23.06)); QUIT GAME, status 0, `[P7] scanout back (X revoked)` at the exit; e2fsck rc 0 |
| r2-play-put1 | play.p9, put_image | `M` opened the hub instead of the map, and the strict map check stopped it after 2 min (→ `unwind`) |
| r2-play-put2 | play.p9, put_image | `M` opened the hub and its OCR read no hub word (→ wider `unwind` pattern); stopped |
| **r2-play-put3** | play.p9, **put_image** | 10 min in the world; the second `M` again landed in the hub unread, so the rows after it are hub frames: the A/B below uses the segment up to the second map (6.1 min); stopped by SIGTERM after |

No early exit in any integ9 boot that started the game (11 of 11; P9-1): the
reporter's `NtCreateUserProcess` takes **0.09–0.13 s** (was 16.5–17.6 s).
`Sleep(1)` ×50 = 51.2–51.7 ms (was 380–1,210 ms).  Launch → first window
15–31 s (was 45–61 s); launch → main menu 99–160 s, including the title
Space timing; save loaded → in the world 72–95 s (was 118–132 s).

### 10.4 Frame rate, present path, kernel split (median (p10) fps of winevulkan `trace:fps` rows; `prof-*` phases excluded)

| | fpsprobe scanout (r2-fps-scan6) | fpsprobe put_image (r2-fps-put2) | play.p9 scanout (r2-play-scan1) | play.p9 put_image (r2-play-put3) | reload scanout (r2-reload-scan1) | wave 6 round 1 (gateA5, integ7 kernel, put_image at 1280×720 in 1920×1080) |
|---|---|---|---|---|---|---|
| **in the world** | **25.7 (13.8)**, 59 rows | **18.9 (12.7)**, 70 rows | **19.0 (11.7)**, 366 rows, 10.5 min | **19.5 (15.0)**, 201 rows (world segment to the 2nd map; scanout run's same segment: 19.0 (11.4), 206 rows) | 19.6 (15.8), 51 rows | 12.1 (7.0) |
| standing in the world (prof phase) | 35.1 (31.7) | 22.1 (20.1) | 11.1–20.2 | 11.8–22.9 | 19.9–22.9 | — |
| main menu | 108.8 (98.0) | 89.0 (83.6) | 95.9 (88.6) | 91.2 (82.5) | 89.9 (72.9) | 42.6 (37.2) |
| loading screen | 165.3 (28.6) | 71.0 (22.2) | 107.9 (23.2) | 73.8 (24.5) | 152.3 (27.7) | 35.6 (13.0) |
| map | — | — | 31.9 (21.4) | 38.0 (22.6) | — | 20.7 (8.0) |
| `[HBP]` path | 100 % scanout (26,870 flips, 0 put_image, 0 refused) | 100 % put_image | 100 % scanout (33,563 flips) | 100 % put_image | 100 % scanout | put_image |
| `[HBP]` flip / sub, world (µs avg) | 2,973 / 4,316 | 2,214 / 6,572 | 3,554 / 6,012 | 2,060 / 3,758 | — | — |
| vCPU kernel / idle / user in the world | 30 / 55 / 15 % | 32 / 51 / 17 % | 35–42 / 44–49 / 14–16 % | 33–45 / 43–52 / 12–15 % | — | 53–65 / 27–37 / 8–11 % |
| top kernel symbol | `spin_acquire_raw` 7.4 % | 9.5 % | 11.8 % (+ `sched_wake_idle_cpu` 4.3 %) | — | — | 28–32 % |
| frame pool in use, world | ~4.1 GiB | ~4.1 GiB | ~4.1 GiB (27,563–27,702 MiB free of 31,652) | — | — | ~4.2 GiB |

**Verdict against §6 P9's target (≥ 30 fps at 1280×720 Low): still not met
in the world.**  With integ9 + the zero-copy scanout, the world median is
19–26 fps (p10 12–16) depending on where V walks.  That is 1.6–2.1× round 1.
Standing still, the scanout reaches 35 fps.  The menus are 90–110 fps.
- **Scanout vs put_image:** about +36 % on the short probe (25.7 vs 18.9)
  and +60 % standing (35.1 vs 22.1).  But over the same 6-minute world
  segment of the long sessions the two paths are level (19.0 vs 19.5 fps median): **the world frame is not present-bound**, and the short-probe gap is within run-to-run and host-load variance (2–3 other guests on strix in every run).
- **What's left:** the guest is no longer kernel-bound.  It is 44–55 % idle
  with 15 % user and 30–40 % kernel.  `spin_acquire_raw` fell from 30 % to
  7–12 %.  The frame now waits rather than spins: the present's `sub`
  (Venus' present thread: blit + fence) is 4–10 ms, and the flip is 3–3.6 ms
  (QEMU's egl-headless readback, `docs/cp-p7.md` §8).  The next lever is the
  CPU→GPU submission and wait path per frame (C6), and the remaining
  `proc_lock` share (C1-k6).

### 10.5 Stop P9-8 — the X session wedges on `fat_lock` before the game (new; 2 of 13 integ9 boots)

In r2-fps-scan3 and r2-fps-scan5 the Wine warm-up never finished.  Wine's own
rows stop ~14 s after boot, right after `explorer.exe /desktop` loads
`winex11.drv`/`uxtheme.dll`, and `winprobe01` times out after 300 s.  The X
server keeps drawing (its clock ticks in a VNC shot).

- **scan5 kernel rows:**
  `[YLOCK-WEDGE] site=fat_lock … [YLOCK-OWNER] fat_lock pid=10 cpu=4 pcb_live=1 ost=3 osys=23`.
  The owner is IceWM (pid 10), in `SYS_RENAME` (23), holding the FAT16
  `fat_lock` while it is not running (state 3).
- **The sampler** (`prof-prestall`, kernel stack words): vCPU 0 and vCPU 3
  spin in `ylock_acquire`, one under `fat16_read_direct_locked`, the other
  under `find_entry_in_root` ← `vfs_open_routed`.  The other six vCPUs are
  idle.
- **scan3** printed no YLOCK row, but one vCPU was at 100 % host CPU for
  8 min with the rest idle: the same picture.

Both boots ran with `HB_SCREEN_SIZE=1280x720`, as did the 11 that got past the warm-up.
The rename is IceWM's own preferences/clock-state write on the FAT boot disk,
which this lane does not touch.
**Owner:** the kernel's FAT16/VFS locking (`fat16.c`, `vfs*.c`; not in any wave-6 row) — the integrator
assigns.  Evidence: `docs/cp-play-evidence/p9r2-stop8-fat-lock-wedge-strix.txt`.

### 10.6 Gates and commands (round 2)

- Gate P4 (`tools/cp/p4/run-p4.sh`, lavapipe, the rebuilt XHOBBY with
  `HB_SCREEN_SIZE` unset): **G1/G2/G3 PASS**, the same as integ7.  The DDX
  change is inert unless the variable is set.
- Every game boot: `e2fsck -fn` rc 0, game copy census unchanged, no
  first-run screen, the game export through `run-p8.sh`'s refcount.

```
VENUS_WORK=$HOME/cp/lanes/play-venus bash tools/mesa/venus/build.sh all     # the guest Venus ICD with C6-p7's WSI
P9_SCRIPT=tools/cp/p9/fpsprobe.p9 P9_RUN=r2-fps-scan6 PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 bash tools/cp/p9/run-p9.sh
P9_SCANOUT=0 P9_SCRIPT=tools/cp/p9/fpsprobe.p9 P9_RUN=r2-fps-put2 (same env) bash tools/cp/p9/run-p9.sh
P9_SCRIPT=tools/cp/p9/play.p9 P9_RUN=r2-play-scan1 (same env) bash tools/cp/p9/run-p9.sh
P9_SCRIPT=tools/cp/p9/reload.p9 P9_FROM=r2-play-scan1 P9_RUN=r2-reload-scan1 (same env) bash tools/cp/p9/run-p9.sh
P9_SCANOUT=0 P9_SCRIPT=tools/cp/p9/play.p9 P9_RUN=r2-play-put3 (same env) bash tools/cp/p9/run-p9.sh
# defaults: P9_XSCREEN=1280x720, VENUS_BUILD=~/cp/lanes/play-venus, VENUS_VNC=1
```

Screendumps (git-ignored `evidence/cp-play/r2/`): `r2-play-scan1/s103-…-saved-place.png`
and `r2-reload-scan1/s045-…-reloaded-place.png` (the same place, the
1280×720 glass); `r2-fps-scan6` world and menu shots.

Open: the hub-instead-of-map OCR gap (`unwind` stops one Esc early when the
hub's OCR reads no hub word; a pixel test for the hub would be firmer); the
long put_image session needs a clean rerun past its second map.
