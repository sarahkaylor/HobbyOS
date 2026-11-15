# Cyberpunk 2077 P0 — the host-native column (lane C7-p0host)

How the real game behaves on real Linux, measured 2026-11-14 on the Strix Halo
workstation, so P1–P9 of `docs/cyberpunk.md` are planned on numbers. Everything
below comes from raw files committed under `evidence/cp-p0host/` (paths given per
table) and from scripts under `tools/cp/p0host/`; §15 has every command needed to
regenerate it; §13 lists proposed amendments to `docs/cyberpunk.md`. Where something could not be measured it says so and why.

**Lane C7-p0host, 2026-11-14.** Steam build of Cyberpunk 2077 **2.5** under **Proton
Experimental 11** (wine-11.0, vkd3d-proton 3.2 `2576c0b4a`, DXVK `v3.1.1-209`) on the RX
9070 XT (RADV, Mesa 26.0.8), Low preset with upscaling/frame-generation/RT off, run
headless in gamescope and driven by keyboard + OCR.

| Headline (clean runs) | 1280×720 Low | 1920×1080 Low |
|---|---|---|
| Built-in benchmark, median / mean / p90 / 1 %-low fps | 141.6–142.9 / 136.4–136.8 / 106.7–106.8 / 74.1–76.1 | 140.6–142.0 / 135.5–135.6 / 104.2–108.1 / 73.3–75.7 |
| Main menu, median / mean / p90 / 1 %-low fps | 388.4–398.7 / 339.1–349.9 / 283.9–321.6 / 115.3–123.2 | 393.5 / 344.9–347.3 / 297.4–332.9 / 120.7–124.2 |
| Launch → main menu (warm) | 36.7–47.6 s | 44.2–45.7 s |
| RSS menu / peak · threads menu / peak · fds | 2.2 / 3.9–4.0 GiB · 90 / 93 · 466–474 | 2.3 / 3.5–4.0 GiB · 90 / 92 · 466 |
| Game VRAM menu / peak · CPU cores busy menu / bench | 1.8 / 3.2 GiB · 8.2 / 11.3–11.4 | 2.0 / 3.4 GiB · 8.1 / 11.2–11.4 |

- **CPU-bound**: resolution does not change any number; with Wine's in-server sync
  (what upstream Wine on HobbyOS uses) the benchmark drops 23 % and the menu 38 %.
- **VA**: 141.5 GiB mapped at the menu (137 GiB PROT_NONE, largest 63.3 GiB), top at
  0x7fffffff0000; `[0, 4 GiB)` fully mapped or reserved; exe at its preferred 0x140000000
  (relocatable: DYNAMIC_BASE + relocs); KUSER_SHARED_DATA at 0x7ffe0000 (memfd page).
- **Files**: archives are **read, never mapped**; largest game-file mapping 3.0 MiB;
  1.35 GB read before the menu, single reads up to 171.6 MB. Largest memfd 64.0 MiB,
  largest shared mapping a 192 MiB GPU BO.
- **Syscalls**: 7.1 M / 123 distinct to the menu; TEB via `arch_prctl(ARCH_SET_GS)`;
  per-thread `PR_SET_SYSCALL_USER_DISPATCH`; `userfaultfd`, `eventfd2`, `inotify` used.
- **Vulkan**: vkd3d-proton asks for 1.3, enables 73 device extensions / 202 feature bits.
- **Menu reached** with no audio endpoint, with no reachable Steam client (and no
  network), and distro Wine 10 + Proton's vkd3d-proton/DXVK runs up to the CDPR user
  agreement screen (not accepted).
- **Incident**: the first runs wrote into the Steam library (a stale vkd3d cache in
  `bin/x64`, Steam's shader cache, two `.pyc` files, Steam's own `LastPlayed`); fully
  listed in §2 and now prevented by running every launch with the library read-only.

---

## 1. Machine, software, and how the game was run

| Item | Value | Source |
|---|---|---|
| Host | `strix` (sarahpc), Ubuntu 26.04.1, kernel 7.0.0-43-generic, Ryzen AI MAX+ 395 (32 threads), 93 GiB RAM | `evidence/cp-p0host/misc/versions.txt` |
| GPU used | AMD Radeon RX 9070 XT (RADV GFX1201), PCI 65:00.0, card2 / renderD129, 16 GiB VRAM | game's own `summary.json` `gpuName`; vkd3d-proton log |
| GPU not used | Radeon 8060S iGPU (f5:00.0, renderD128) | `MESA_VK_DEVICE_SELECT=1002:7550`, `VKD3D_FILTER_DEVICE_NAME` |
| Mesa / Vulkan | Mesa 26.0.8-1ubuntu0.3, RADV, Vulkan 1.4.335 | `versions.txt` |
| Game | Steam app 1091500, buildid 26161058, **game version 2.5** (exe FileVersion 3.0.5813403), `Cyberpunk2077.exe` 66,704,848 bytes, sha256 `d36e74c4…` | `misc/pe-Cyberpunk2077.exe.txt`, `summary.json` `gameVersion` |
| Proton | Proton Experimental `experimental-11.0-20261112` (wine-11.0) | `versions.txt` |
| vkd3d-proton (bundled) | `vkd3d-1.1-5742-g2576c0b4a` (reports applicationVersion 3.2.0, build 2576c0b4adfae4a) | Proton `files/lib/wine/vkd3d-proton/version`, Proton log |
| DXVK (bundled) | `v3.1.1-209-g1039f6820` (only `dxgi.dll` matters for D3D12) | `files/lib/wine/dxvk/version` |
| Distro Wine | wine-10.0 (Ubuntu 10.0~repack-12ubuntu1) | `versions.txt` |
| Compositor | gamescope 3.16.20 `--backend headless`, nested size = game resolution, `-r 1000` | `tools/cp/p0host/launch.sh` |
| Frame logger | MangoHud 0.8.2~rc2 (menu), the game's own benchmark `frames.csv` (benchmark) | |

**How the game was launched.** Not through the Steam UI (that would have used the
user's prefix). `tools/cp/p0host/launch.sh` runs Proton's own entry point
`proton waitforexitandrun Cyberpunk2077.exe --launcher-skip` with the variables Steam
sets (`SteamAppId=SteamGameId=STEAM_COMPAT_APP_ID=1091500`,
`STEAM_COMPAT_DATA_PATH=` **our copy** of the prefix, `STEAM_COMPAT_CLIENT_INSTALL_PATH`),
inside a headless gamescope, against the user's running, logged-in Steam client.
Two deliberate differences from a Steam launch:

1. **No Steam Linux Runtime container.** Proton 11 declares `require_tool_appid
   4183110` (SteamLinuxRuntime_4, pressure-vessel); we run Proton's Wine directly on
   the host's glibc 2.43 / Mesa 26. This is the configuration closest to HobbyOS
   (no container there either) and the one where strace sees every process. It
   means no number here contains pressure-vessel; the Steam client itself is not
   in our process tree at all (only Proton's Windows-side `steam.exe` bridge and
   `lsteamclient` are).
2. **Private caches and CWD.** Shader/pipeline caches go to `~/cp/p0host/cache/`
   and the working directory is an empty `~/cp/p0host/cwd` (see §2 for why).

**Settings.** Captured from the game itself, not guessed: in Graphics, Quick Preset
→ **Low**, then Resolution Scaling → **Off** and Frame Generation → **Off** (the game
relabels the preset "Custom" once upscaling is off), ray tracing off, VSync off,
FPS cap off. The resulting key list is `tools/cp/p0host/settings-low-native.keys`
(41 keys, applied with `settings.py`); only `/video/display/Resolution` changes per
run. The file is `drive_c/users/steamuser/AppData/Local/CD Projekt Red/Cyberpunk
2077/UserSettings.json` in the prefix (`version` 152). The game saves it only on a
clean exit. Every benchmark's `summary.json` confirms the state it ran in
(`upscalingType 0`, `FSR3FrameGenEnabled false`, `rayTracingEnabled false`,
`renderWidth/Height`). An early run with Frame Generation still selected doubled
the reported fps (257 vs ~138 at 720p) — kept out of every table.

**Input automation works.** `tools/cp/p0host/nav.py` drives the game with xdotool
key events into gamescope's Xwayland and reads the screen through gamescope's
screenshot atom (`GAMESCOPECTRL_REQUEST_SCREENSHOT`) + tesseract OCR. The mouse
does not work (REDengine reads raw relative input; XTEST absolute moves do not
reach its cursor), the keyboard does: arrows move focus, `A`/`D` change a value,
`1`/`3` switch tabs, `B` starts the benchmark, `Return` confirms. The path to the
built-in benchmark is Main menu → Settings → Graphics → `B` → confirm.
Screendumps: `evidence/cp-p0host/screens/menu-720p.jpg` (the main menu),
`graphics-tab-720p.jpg` (the settings used), `nosteam-assert.png` (§10).

**Lock discipline.** Every game launch — measured or not — after the three
exploratory runs (explore1–3, 21:18–21:40) held `~/cp/measure.lock` for its whole life (`run.sh` re-execs
itself under `flock`). The machine was nevertheless not idle: the other lane's
QEMU guest was resident at times (`sys_vram_before` is 3.9–4.1 GiB in some runs and
0.9 GiB in others) and unrelated processes ran (a root `rm` at 27 % CPU during one
run, the user's ComfyUI). Per-run `host_busy_pct` and `top` snapshots are in the
evidence so contaminated runs can be told apart.

## 2. Incident: early runs wrote into the Steam library (disclosed, contained)

The brief forbids modifying the game files and the Steam library. The first runs
did, through mechanisms I had not anticipated. All were found by this lane's
own `find -newer` audits; every one is now prevented structurally, not by
convention. What changed, exactly (`find /mnt/fast/games/steamapps … -newer
<first-run stamp>`, 2026-11-14 22:2x):

| Path (under `/mnt/fast/games/steamapps/`) | What happened | Writer | When | State now |
|---|---|---|---|---|
| `common/Cyberpunk 2077/bin/x64/vkd3d-proton.cache` | A **pre-existing** (stale) pipeline cache was merged/rewritten — run explore1's log says `Read-only cache is out of date, discarding it` / `Successfully promoted write cache to read cache`. vkd3d-proton resolves `vkd3d-proton.cache` against the CWD, and Proton only redirects it when Steam passes a shader path. | vkd3d-proton in our game process | 21:18–21:44 (runs explore1–3, aborted perf-720p-a) | file still there (1,492,432 bytes, 21:44); original content not recoverable. Not a game file (no Steam manifest entry); the game rebuilds it. |
| `common/Cyberpunk 2077/bin/x64/<exe>-<date>-<pid>.txt`, `.nfs*` | The game drops pid-named 31-byte files into its CWD at start and deletes them at exit; killed runs left NFS silly-renames | the game | same window | gone (directory mtime 22:00:55 is the NFS client reaping them) |
| `shadercache/1091500/{mesa_shader_cache_sf/…/AMD RADV GFX1201/foz_cache{,_idx}.foz, radv_builtin_shaders/part0/mesa_cache.{db,idx}, fozpipelinesv6/steamapprun_pipeline_cache.*/{steamapp_pipeline_cache.foz,bucket.json,TOUCH}}` | Mesa and Steam's fossilize appended shader/pipeline entries. **Cause:** `steamclient.so`, which `lsteamclient` loads into the game, `setenv()`s `MESA_SHADER_CACHE_DIR` and the fossilize path to `<library>/shadercache/1091500` (strings `%s/mesa_shader_cache_sf/%s/%s`, `fozpipelinesv%u`, `MESA_SHADER_CACHE_DIR` in `~/.local/share/Steam/linux64/steamclient.so`), overriding the private cache variables `launch.sh` sets. | Mesa/fossilize in our game process | 21:28–22:20 (every run up to noaudio-os) | appended caches remain; not reversible. Same files a normal Steam launch writes. |
| `common/Proton - Experimental/__pycache__/{filelock,steampipe_fixups}.cpython-314.pyc` | host `python3` (3.14) byte-compiled Proton's modules next to them | our `python3 proton` | 21:18:59 | **deleted** by me (they did not exist before; Steam's own `.cpython-313/-39.pyc` untouched) |
| `appmanifest_1091500.acf` (`"LastPlayed" "1794716577"` = 22:22:57) | the user's Steam client recorded the app as played | Steam client | at each run's exit | left as Steam wrote it (editing appmanifests is forbidden) |

Also verified: Steam AutoCloud ran at every exit of our runs and uploaded nothing
(`~/.local/share/Steam/logs/cloud_log.txt`: "Running AutoCloud on exit … No launch
record found … Skipping un-modified file … should be nothing to download"; zero
upload lines). The user's prefix `compatdata/1091500` has no file newer than the
first run. `workshop/appworkshop_294100.acf` (RimWorld) changed at 21:51:58; nothing
in this lane touches RimWorld.

**Containment (in force for every run from `rotest`, 22:31:37, onward):** `launch.sh` wraps the whole run
in `ns.sh rolib`, a private mount namespace in which `/mnt/fast/games/steamapps` is a
**read-only** bind (a write attempt gets `EROFS`), with this lane's own directories
bound over the two paths Proton/Steam insist on writing: `shadercache/1091500` →
`~/cp/p0host/cache/steam-shadercache` and Proton's `dist.lock` (its file lock opens
`O_RDWR|O_CREAT|O_TRUNC` even when nothing changes) → `~/cp/p0host/run/dist.lock`.
`/mnt/fast` is NFS with root_squash, so the binds are made by `mountas.py` as root
with `setfsuid(1000)`. Plus `PYTHONDONTWRITEBYTECODE=1`, private
`VKD3D_SHADER_CACHE_PATH`/`DXVK_STATE_CACHE_PATH`/`MESA_SHADER_CACHE_DIR`, and an
empty CWD. `stop.sh` re-audits the game dir, Proton, `shadercache/1091500`,
`compatdata/1091500` and the library's top level after every run and writes
`game_dir_changes.txt` (empty for every contained run). Consequence for the
numbers: runs before 22:31 used the user's warm Mesa cache in the library;
runs after it use the private cache (warmed by one run first).

---

## 3. Item 1 — performance (plan §3.3 item 1)

**Method.** Each perf run (`run.sh <tag> W H --bench --menu-log 30`): launch → OCR
sees the main menu → 10 s settle → `/proc` snapshot → MangoHud per-frame log for 30 s
(Shift_L+F2 into the game window; MangoHud 0.8.2 does not write a log with
`no_display`, so a 3-digit fps counter is visible bottom-right) → Settings → Graphics
→ `B` → the game's own benchmark (64.3 s, ~8,700 frames, written by the game to
`Documents/CD Projekt Red/Cyberpunk 2077/benchmarkResults/<date>/{frames.csv,summary.json}`
in our prefix). `sampler.py` records, at 1 Hz, the game process's
VmRSS/VmHWM/threads/fds/utime+stime, its DRM fdinfo VRAM on 65:00.0, the card's
`mem_info_vram_used`, and `/proc/stat`. Definitions (`fps.py`): mean = frames /
seconds; median = 1000 / p50 frame time; p90 = 1000 / p90 frame time (10 % of frames
are slower); 1 %-low = 1000 / p99 frame time. "Launch" = the moment `launch.sh`
starts gamescope; "prompt" = first OCR of "PRESS … TO CONTINUE" (the boot movie,
answered with Space at once); "menu" = first OCR of the main-menu items (screenshot
cadence ≈1.3 s).

**Contamination.** `host busy %` is the whole machine (32 threads) over the window;
the game alone accounts for ~25 % (menu, 8 cores) and ~35 % (benchmark, 11 cores).
Runs at 60–80 % carried load from outside this lane; where a `top` snapshot exists
(runs from perf-1080p-b on, `runs/<tag>/proc_bench_top.txt`) it shows the other lane's
`qemu-system-x86` at 730–1,418 % CPU plus its `virgl` render thread during our
benchmark, although our run held `~/cp/measure.lock` — the other lane's *setup* work
does not take the lock, by design of the rules. They are
kept in the tables, marked ✗, and excluded from the headline.

Raw: `evidence/cp-p0host/runs/<tag>/{fps-menu-mangohud.txt, mangohud-frames.csv.gz,
fps-bench-game.txt, bench-frames.csv.gz, bench-summary.json, summary.txt,
samples.csv.gz, events.log}`.

| run | res | menu fps mean / median / p90 / 1%-low (MangoHud, 30 s) | benchmark fps mean / median / p90 / 1%-low (game `frames.csv`) | game `averageFps` | host busy % menu / bench | clean |
|---|---|---|---|---|---|---|
| perf-720p-a | 1280x720 | 349.9 / 398.7 / 283.9 / 115.3 | 105.0 / 119.9 / 73.6 / 34.0 | 105.0 | 35.7 / 60.4 | menu ✓ bench ✗ |
| perf-720p-b | 1280x720 | 339.1 / 388.4 / 321.6 / 123.2 | 136.4 / 142.9 / 106.7 / 74.1 | 136.4 | 28.3 / 39.8 | ✓ |
| perf-720p-c | 1280x720 | 281.8 / 322.4 / 127.5 / 102.0 | 119.4 / 123.6 / 96.5 / 66.9 | 119.4 | 73.3 / 80.3 | ✗ |
| warm-720p | 1280x720 | — | 136.8 / 141.6 / 106.8 / 76.1 | 136.8 | 33.5 / 37.4 | bench ✓ |
| warm2-720p | 1280x720 | — | 103.7 / 111.1 / 74.2 / 54.5 | 103.7 | 76.1 / 78.2 | ✗ |
| perf-1080p-a | 1920x1080 | 344.9 / 393.5 / 297.4 / 120.7 | 135.6 / 140.6 / 108.1 / 75.7 | 135.6 | 30.6 / 41.2 | ✓ |
| perf-1080p-b | 1920x1080 | 347.3 / 393.5 / 332.9 / 124.2 | 114.1 / 124.5 / 85.0 / 43.7 | 114.1 | 25.3 / 60.4 | menu ✓ bench ✗ |
| perf-1080p-c | 1920x1080 | 303.0 / 338.0 / 207.9 / 109.8 | 119.2 / 125.0 / 94.2 / 67.3 | 119.2 | 71.7 / 78.0 | ✗ |
| warm-1080p | 1920x1080 | — | 135.5 / 142.0 / 104.2 / 73.3 | 135.5 | 32.6 / 36.5 | bench ✓ |
| warm2-1080p | 1920x1080 | — | 104.1 / 112.7 / 73.9 / 52.2 | 104.1 | 75.7 / 78.6 | ✗ |
| perf-720p-srvsync | 1280x720 | 212.6 / 242.1 / 110.8 / 102.7 | 105.3 / 109.1 / 87.1 / 59.9 | 105.3 | 15.8 / 30.2 | ✓ (different config, see below) |

(`warm*` runs were the first benchmark after a shader-cache change and have no menu
log; their benchmark numbers match the clean perf runs, so the pipeline cache was not
limiting the benchmark. `warm*` "menu" columns below include the walk to the
benchmark and are not menu-idle numbers.)

| run | launch→prompt s | launch→menu s | RSS menu / peak MiB | threads menu / peak | fds menu / peak | game VRAM menu / peak MiB | CPU cores menu / bench |
|---|---|---|---|---|---|---|---|
| perf-720p-a | 28.8 | 36.7 | 2228 / 3759 | 92 / 92 | 474 / 475 | 1829 / 3236 | 8.00 / 10.51 |
| perf-720p-b | 30.8 | 47.6 | 2205 / 3922 | 90 / 93 | 466 / 478 | 1781 / 3167 | 8.15 / 11.36 |
| perf-720p-c | 30.2 | 38.4 | 2259 / 4042 | 90 / 93 | 466 / 475 | 1813 / 3195 | 7.55 / 10.62 |
| warm-720p | 28.9 | 79.4 | 3939 / 4005 | 90 / 90 | 470 / 470 | 2988 / 3195 | 10.13 / 11.29 |
| warm2-720p | 29.6 | 108.3 | 2826 / 3569 | 93 / 95 | 465 / 469 | 1781 / 3168 | 8.66 / 10.24 |
| perf-1080p-a | 34.8 | 45.7 | 2257 / 3463 | 90 / 92 | 466 / 475 | 2002 / 3390 | 8.08 / 11.38 |
| perf-1080p-b | 33.3 | 44.2 | 2293 / 3995 | 90 / 92 | 466 / 474 | 1986 / 3354 | 8.07 / 10.83 |
| perf-1080p-c | 33.9 | 49.4 | 2263 / 4007 | 90 / 92 | 466 / 474 | 1970 / 3417 | 7.73 / 10.61 |
| warm-1080p | 31.0 | 45.7 | 3837 / 4030 | 90 / 92 | 465 / 474 | 2516 / 3386 | 9.87 / 11.20 |
| warm2-1080p | 34.5 | 48.3 | 2774 / 4131 | 90 / 93 | 465 / 478 | 1844 / 3374 | 9.05 / 10.35 |
| perf-720p-srvsync | 29.6 | 46.6 | 2237 / 3468 | 90 / 93 | 465 / 473 | 1765 / 3206 | 4.46 / 8.66 |

"game VRAM" is the game process's `drm-memory-vram` from `/proc/<pid>/fdinfo` (65:00.0,
summed over distinct `drm-client-id`). The card total (`mem_info_vram_used`, includes
the desktop's ~0.9 GiB and, when resident, the other lane's guest) in the clean runs:
2.8–3.0 GiB at the menu, 4.1–4.4 GiB in the benchmark, i.e. 3.3–3.5 GiB above the
pre-launch baseline at peak (`summary.txt`: `sys_vram_*`).

**Headline (clean runs only).**

| | 1280×720 Low | 1920×1080 Low |
|---|---|---|
| Benchmark median fps (runs) | 141.6, 142.9 | 142.0, 140.6 |
| Benchmark mean / p90 / 1 %-low | 136.4–136.8 / 106.7–106.8 / 74.1–76.1 | 135.5–135.6 / 104.2–108.1 / 73.3–75.7 |
| Main menu median fps (runs) | 398.7, 388.4 | 393.5, 393.5 |
| Main menu mean / p90 / 1 %-low | 339.1–349.9 / 283.9–321.6 / 115.3–123.2 | 344.9–347.3 / 297.4–332.9 / 120.7–124.2 |
| Launch→menu (warm caches) | 36.7, 47.6 s | 45.7, 44.2 s |

Resolution does not move either number: at Low the game is **CPU-bound** on this
host (8 cores busy at the menu, 10.5–11.4 during the benchmark; the 9070 XT is far
from saturated). Every other metric is resolution-independent too: RSS 2.2–2.3 GiB
at the menu and 3.4–4.0 GiB peak (VmHWM, reached in the benchmark), 90–92 threads
(peak 93), 466–474 fds, 1.8–2.0 GiB game VRAM at the menu and 3.2–3.4 GiB peak.
Counting every process of the run (all processes whose environment carries our
prefix path, `proc_menu_tree.txt`): **15–16 processes and 173–190 threads** at the
menu across all runs (190 in perf-720p-srvsync, 188–189 in the perf runs); the game has
15 `redDispatcher*` worker threads on this 32-thread host (`proc_menu_threads.txt`).

**Launch-to-menu, other conditions** (same scripts):

| condition | run | launch→menu |
|---|---|---|
| warm page cache + warm Mesa cache | perf-* | 36.7–49.4 s |
| this host's page cache for the game dir and Proton evicted first (`evict.py`, 64.4 GiB, POSIX_FADV_DONTNEED; the NFS server's own cache untouched) | cold-720p | 77.7 s |
| empty vkd3d-proton pipeline cache (first run after redirecting it to `~/cp/p0host/cache`) | cwdtest | 117.2 s |
| empty Mesa shader cache (first run inside the read-only namespace, §2) | rotest | 110.2 s |
| under `strace -f` (fsync) | trace | 58 s |
| under `strace -f`, in-server sync | trace-srvsync | 457 s |

Between the first prompt (~29–35 s, the boot movie) and the menu the game shows a
second "PRESS … TO CONTINUE" when loading is done; that gap is the load, and it is
what varies (4–118 s across runs). The two longest (noaudio-os/os2, 103 and 118 s) had
only 25 % host CPU load; their cause was not isolated (cache state, or the other lane's
guest reading the same NFS server, are candidates, not findings).

**In-server synchronisation (AD-8), one run.** Proton runs Wine with fsync
(`futex_waitv`) by default ("fsync: up and running"). With `PROTON_NO_FSYNC=1`
(Proton's log: `Options: {'nofsync', …}`) Wine falls back to wineserver-side
synchronisation — what upstream Wine on HobbyOS uses under AD-8. Same machine, same
settings, a clean window (15.8 % / 30.2 % busy): benchmark **105.3 fps mean / 109.1
median** vs 136.4–136.8 / 141.6–142.9 with fsync (−23 % mean), menu **242 median** vs
388–399 (−38 %); wineserver itself at 60 % of a core during the benchmark
(`proc_bench_top.txt`). One run, so ±5 % noise is not excluded, but the direction and
size are far outside it.

## 4. Item 2 — syscall histogram of one launch-to-menu (plan §3.3 item 3)

**Method.** Separate runs from the perf runs: `WRAP="strace -f -tt -y -s 96 -o
trace.txt"` puts strace in front of `python3 proton`, so every process the launch
creates — the Proton script, both wineservers, every Wine process, the game and its
children — is traced; gamescope and the user's Steam client are outside the tree
(Proton runs outside pressure-vessel here, §1, so there is no pressure-vessel or
Steam-client code to attribute). `strace-summary.py` re-joins unfinished/resumed
calls, follows `clone`/`clone3`/`fork` return values to map threads to processes,
names processes by their last `execve` (Wine processes by their `.exe`), and cuts two
windows: **launch → the moment OCR saw the main menu**, and **the following 30 s idling
at the menu**. Two configurations: Proton's default (fsync, `trace`) and in-server
synchronisation (`PROTON_NO_FSYNC=1`, `trace-srvsync`, the AD-8 configuration). Both
reached the menu under strace (58 s and 457 s). The raw logs (2.0 GB and 12.1 GB) were
deleted after summarising; their sizes are recorded in `strace/<run>/trace-size.txt`.
Raw tables: `evidence/cp-p0host/strace/{trace,trace-srvsync}/{summary-launch,summary-menu30}/
{hist_all,hist_by_proc,ioctl,prctl,procs,files,mmaps}.txt`.

| window | syscalls (all processes) | distinct | top syscalls (count, % of all) |
|---|---|---|---|
| fsync, launch → menu (58.7 s) | 7,085,204 | 123 | futex_waitv 2,968,612 (41.9 %, 1,028,099 returned an error = timeouts/EAGAIN), futex 1,111,269 (15.7 %), getrusage 525,706 (7.4 %), read 343,249 (4.8 %), rt_sigprocmask 315,337 (4.5 %), sched_yield 262,853 (3.7 %), write 239,081 (3.4 %), ioctl 221,280 (3.1 %), poll 151,562, epoll_pwait2 116,300, newfstatat 90,440, close 74,233, openat 71,844, recvmsg 71,042 |
| fsync, 30 s at the menu | 3,730,729 (≈123,000/s) | 33 | futex_waitv 2,083,364 (55.8 %), futex 653,747 (17.5 %), getrusage 289,413 (7.8 %), sched_yield 144,707 (3.9 %), read 94,660, write 92,181, rt_sigprocmask 86,981, poll 73,246, ioctl 48,931 |
| in-server sync, launch → menu (457 s) | 55,847,380 | 123 | read 13,430,151 (24.1 %), getrusage 10,773,202 (19.3 %), write 9,253,990 (16.6 %), rt_sigprocmask 9,169,596 (16.4 %), sched_yield 5,386,600 (9.6 %), writev 2,208,785 (4.0 %), poll 1,222,758, epoll_pwait2 1,072,077, pselect6 787,067, futex 635,633 |
| in-server sync, 30 s at the menu | 3,575,286 (≈118,000/s) | 31 | read 863,074 (24.1 %), getrusage 714,832 (20.0 %), write 599,347 (16.8 %), rt_sigprocmask 586,884 (16.4 %), sched_yield 357,417 (10.0 %), writev 140,003 (3.9 %) |

Per process (fsync, launch → menu): the game 5,705,981 syscalls (94 distinct),
wineserver 460,342 (59), `winedevice.exe` 316,252 (77), `xalia.exe` 268,497 (68),
`explorer.exe` 89,871, `wineboot.exe` 42,228, `tabtip.exe` 40,543, Proton's `steam.exe`
39,579, `conhost.exe` 30,832, `REDEngineErrorReporter.exe` 30,550, `plugplay.exe`
30,319, `services.exe` 19,192, `rpcss.exe` 4,034, `svchost.exe` 3,757, the Proton python
script 2,773. With in-server sync the game makes 37.1 M and **wineserver 14.2 M**
(read 6.6 M + write 6.4 M on its request pipes) — every wait/signal becomes a round
trip to wineserver.

**Who uses what** (`hist_by_proc.txt`; fsync run unless stated):

| syscall / option | count and caller | relevance |
|---|---|---|
| `arch_prctl(ARCH_SET_GS)` | 98 in the game (≈ one per thread), 219 in all; `ARCH_SET_FS` 2 per process (glibc) | open question 3: Wine sets the TEB through `ARCH_SET_GS` at thread start; whether it also uses `wrgsbase` on its syscall path is invisible to strace (the host has FSGSBASE) |
| `prctl(PR_SET_SYSCALL_USER_DISPATCH)` | 97 in the game, ≈ one per thread in every Wine process | new: Wine arms Syscall User Dispatch per thread (to catch Windows code issuing raw `syscall`) |
| `prctl(PR_SET_NAME)` / `PR_SET_PTRACER` | 26 / 1 in the game | thread names; Yama ptracer |
| `futex_waitv` | 2.94 M in the game (fsync only; 1 in the in-server run) | in §3.2's "absent" list; only needed for fsync |
| `getrusage` | 478,873 in the game (10.4 M in the in-server run) | Wine's thread-time queries; must be cheap |
| `sched_yield` | 239,437 in the game | spin-waits |
| `userfaultfd` + `UFFDIO_API` | 1 per Wine process (14) | Wine probes uffd (write-watch support) |
| `memfd_create` | 14 game, 5 wineserver, 4 xalia, 3 explorer | the `wine-mapping` sections (§5) |
| `process_vm_readv` | 936, wineserver only | wineserver reads client memory |
| `eventfd2` | 1 game, 2 winedevice, 1 steam.exe | in §3.2's "absent" list |
| `inotify_init1` / `inotify_add_watch` | 1 / 2, `winedevice.exe` only | in §3.2's "absent" list |
| `timerfd_create` | 1, `winedevice.exe` | present in HobbyOS |
| `signalfd*`, `io_uring_*`, `ptrace`, `seccomp`, `modify_ldt`, `personality`, `membarrier`, `pidfd_*` | 0 | not needed to reach the menu |
| `sched_setaffinity` | 90 game, 173 wineserver | thread affinity masks |
| `clone3` | 123 in the game | threads (glibc 2.43); `clone` 27 |
| `shmget`/`shmat`/`shmctl` | 1 each, the game | X MIT-SHM |
| `copy_file_range` 47, `sendfile` 42 | the Proton python script only | prefix setup (Proton-only) |

ioctls (fsync, launch → menu, all processes): 221,280. Of these, 216,199 are DRM on
`/dev/dri/renderD129` from the game (`AMDGPU_INFO` 121,609, `SYNCOBJ_TIMELINE_WAIT`
47,332, `AMDGPU_CS` 10,767, other `SYNCOBJ_*` ≈ 34,000, `GEM_CREATE` 681, `GEM_MMAP` 539)
— RADV's kernel interface, replaced on HobbyOS by Venus/`/dev/gpu`, so these say
nothing about HobbyOS's ioctl surface except the submit rate (≈ 10,767 CS in 59 s).
The rest: `SYNC_IOC_MERGE` 2,336 (sync_file), `VFAT_IOCTL_READDIR_BOTH` 1,408 (Wine's
directory-read fast path; 879 return an error, i.e. Wine falls back), `FS_IOC_GETFLAGS`
889, `SIOCGIFMTU`/`SIOCGIFFLAGS` 64 each and `SIOCGIFHWADDR`/`SIOCGIWNAME` (winedevice's
nsiproxy), `TCGETS2`, `FIONREAD`, `UFFDIO_API` 14, `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` 9 and
`UDMABUF_CREATE` 9 (gamescope's WSI path — a harness artefact).

## 5. Item 3 — VA layout and the PE image (plan §3.3 item 5, §10 Q1–Q3)

**PE header** (`tools/cp/p0host/pe.py`, cross-checked with `objdump -p`;
`evidence/cp-p0host/misc/pe-Cyberpunk2077.exe.txt`):

| Field | Value |
|---|---|
| Machine / Magic | 0x8664 / PE32+ (0x20b) |
| ImageBase | **0x140000000** (5 GiB) — the MSVC x64 default |
| SizeOfImage | 0x581b000 (88 MiB); `.text` 48 MiB, `.data` 25 MiB virtual / 0.5 MiB raw |
| DllCharacteristics | 0x8160 = **DYNAMIC_BASE** \| HIGH_ENTROPY_VA \| NX_COMPAT \| TERMINAL_SERVER_AWARE |
| Relocations | present (`.reloc` 1.2 MiB, 1,707 blocks) → the image is relocatable |
| Subsystem | 2, Windows GUI, 6.0 |
| Stack reserve / commit | 2 MiB / 4 KiB |
| TLS | yes, with callbacks (`AddressOfCallBacks` 0x143028260) |
| Static imports | 37 DLLs: KERNEL32 (224), USER32 (76), icuuc (96), PhysX3 (42), WS2_32 (31), bink2w64 (21), sl.interposer (20), libcurl, WINHTTP, REDGalaxy64, amd_ags_x64, amd_fidelityfx_loader_dx12, libxess*, HID, SETUPAPI, POWRPROF, … — **no `steam_api64.dll`, no `d3d12.dll`/`dxgi.dll` static import** (both are loaded at run time) |

**At the main menu** (`/proc/<game>/maps` snapshot 10 s after the menu appeared, run
`perf-720p-b`; `evidence/cp-p0host/runs/perf-720p-b/proc_menu_maps.txt`, condensed by
`maps.py` into `maps-menu-summary.txt`; the other runs agree to within a few mappings):

| Fact | Value |
|---|---|
| Mappings / VA in use | 1,981 mappings, **141.5 GiB** of VA, of which **137.3 GiB are PROT_NONE reservations** |
| Lowest / highest user address | 0x10000 (`apisetschema.dll`) / **0x7fffffff0000** (top of the 47-bit space; excl. `[vsyscall]`) |
| `Cyberpunk2077.exe` | loaded **at its preferred base 0x140000000** (0x140000000–0x14581b000). Only the header page and `.pdata` are file mappings; `.text/.rdata/.data` are anonymous memory that Wine's loader filled with `pread64` (three 50,273,792-byte reads in the trace, §6) — consistent with section raw sizes being only 0x200-aligned |
| KUSER_SHARED_DATA | **0x7ffe0000**, one page, `r--s`, backed by `memfd:wine-mapping` |
| [0, 4 GiB) | 4,091 MiB of 4,096 MiB mapped or reserved — Wine fills the whole 32-bit range: NLS files, the PEB/TEB/stacks, 1 GiB PROT_NONE blocks at 0x800f0000 and 0xc00f0000 |
| [4, 8 GiB) | 0x100000000–0x120000000: 1 MiB `rw-p` blocks each behind a 4 KiB `r--p` page (stack-shaped) + game DLL pieces (`icudt.dll` …), the exe at 0x140000000, 602 MiB committed anon at 0x180010000, a 15.4 GiB reservation starting at 0x1a5a10000 |
| Largest single mapping | **64,832 MiB PROT_NONE** at 0x5ac010000; then 32,182 MiB, 15,782 MiB and five ≈4 GiB reservations (owner not identified from `/proc`; the shape is an allocator reserving pools up front) |
| Largest committed private mapping | 704 MiB anon `rw-p` at 0x580010000 (602 MiB at 0x180010000 next) |
| Largest MAP_SHARED mapping | **192 MiB `rw-s` `/dev/dri/renderD129`** (a GPU BO); 3 shared mappings exceed 64 MiB, all GPU BOs |
| memfd | 8 shared `memfd:wine-mapping` mappings totalling 64.0 MiB, largest exactly 64.0 MiB |
| Largest file mapping | 132 MiB `libLLVM.so.21.1` (Ubuntu links it into both `libvulkan_radeon.so` and `libvulkan_lvp.so`; the loader opens both ICDs), 49 MiB `steamclient.so`, 17 MiB `libvulkan_radeon.so`, 14 MiB `libvulkan_lvp.so` |
| Game-dir files mapped | 51 regions over 29 files, total 3.6 MiB; largest 3.0 MiB (`Cyberpunk2077.exe` `.pdata`). **No `.archive` is mapped** |

By backing (same snapshot): anon PROT_NONE 140,594 MiB (173 mappings), anon private
3,252 MiB (562), GPU BO mmaps 614 MiB (211), other file-private 321 MiB (931), shared
memfd 64 MiB (8), other file-shared 30 MiB (39), `[heap]` 10 MiB, game-dir files
3.6 MiB (51). Other runs at the menu: 140.9–141.4 GiB mapped, 1,881–1,974 mappings
(`runs/*/maps-menu-summary.txt`). 20 s into the benchmark (`maps-bench-summary.txt`,
same run): 2,246 mappings, still 141.5 GiB, anon private up to 4,296 MiB with a
**1,532 MiB** largest committed mapping, PROT_NONE 136.3 GiB (largest 62.5 GiB), GPU BO
mmaps 632 MiB (largest still 192 MiB).

**Wine and the TEB.** Open question 3 is answered by the syscall trace in §4
(`arch_prctl`/`prctl` table), not by the maps.

## 6. Item 4 — file access: what is mapped, what is read (plan §3.3 item 6, gap #7)

From the full trace of the fsync launch (§4), cut at the moment the menu was seen
(`strace -f -tt -y`, so every `read`/`mmap` carries its file path; unfinished/resumed
pairs re-joined). Raw: `evidence/cp-p0host/strace/trace/summary-launch/{files.txt,
mmaps.txt}`, `strace/trace/read-sizes.txt`; 30 s at the menu in
`strace/trace/summary-menu30/files.txt`.

| Fact (launch → main menu) | Value |
|---|---|
| Game-dir files touched | 90 |
| **Bytes read from the game dir** | **1,350,000,340** (1.26 GiB) in 5,809 `read`/`pread64` calls — the game 1,222,640,316; Proton's `steam.exe` 127,048,012 (it reads `Cyberpunk2077.exe`); the error reporter 263,680; wineserver 48,332 |
| `.archive` files | 32 opened, 65.2 GB in total; **660,791,208 bytes read** from them; **0 mmaps** — they stay open as fds (`proc_menu_fds.txt` lists them) and are read with `read()` at offsets |
| Other big reads | `engine/shader_final.cache` 176.9 MB (whole file), `Cyberpunk2077.exe` 190.6 MB (Wine's loader reads the image sections: `.text` with 50,273,792-byte `pread64`s), `libxess.dll` 77.7 MB, `r6/cache/tweakdb.bin` 44.1 MB (one `read`), `libxess_fg.dll` 45.5 MB, FidelityFX DLLs 69 MB, `r6/cache/final.redscripts` 19.3 MB |
| **Largest single read** | **171,596,309 bytes** (`engine/shader_final.cache`, one `read()`); then 73,979,392 (`libxess.dll`, `pread64`), 3 × 50,273,792 (`Cyberpunk2077.exe`), 44,143,434 (`tweakdb.bin`) |
| Read sizes | 1,834 calls of ≤ 4 KiB, 2,136 of 4–64 KiB, 1,728 of 64 KiB–1 MiB, 100 of 1–16 MiB, **11 of > 16 MiB carrying 603,308,611 bytes** (44.7 % of all bytes) |
| **Largest mmap of a game file** | **3,166,208 bytes** — `Cyberpunk2077.exe` at file offset 0x397a000 (`.pdata`), `PROT_READ|PROT_WRITE`, `MAP_PRIVATE|MAP_FIXED` (made by both `steam.exe` and the game); every other game-file mapping is ≤ 77,824 bytes (DLL section pieces) |
| At the menu, 30 s idle | 51,474,432 bytes read from the game dir (≈1.7 MB/s of streaming while the menu scene animates) |

So Wine maps only page-aligned pieces of the PE files and reads everything else; the
game reads its archives. **No game file needs a mapping anywhere near 64 MiB**; what
the data path needs is `read`/`pread` of up to ~172 MB per call into user memory,
about 1.3 GB before the menu, and ~470 simultaneously open fds.

## 7. Item 5 — Vulkan: what vkd3d-proton enables (plan §3.3 item 4)

Method: a 193-line pass-through Vulkan layer built for this lane
(`tools/cp/p0host/vkdump/`, compiled on strix against Ubuntu's `libvulkan-dev` +
`vulkan-utility-libraries-dev`) logs every `vkCreateInstance` and `vkCreateDevice`
the process makes: app/engine/apiVersion, extensions, and every `*Features` struct in
the `pNext` chain with its true members (242 structs known to the generated table).
vkd3d-proton's own log cannot do this — the bundled release build compiles its TRACE
feature dumps out (no `robustBufferAccess` string in `d3d12core.dll`). Raw:
`evidence/cp-p0host/vk/vkdump.txt`, condensed `vk/vksum.txt`.

| Fact | Value |
|---|---|
| Vulkan API version requested | **1.3.0** by vkd3d-proton (engine "vkd3d", engineVersion 0xc02000 = 3.2.0) and by DXVK's `dxgi` (engine "DXVK"); the driver offers 1.4.335 |
| Instances / devices in one launch to menu | 9 `vkCreateInstance`: DXVK `dxgi` ×3 (1 in Proton's `xalia.exe`, 2 in the game), vkd3d-proton ×2, 2 in the game with a NULL application name at apiVersion 1.1, 2 without `pApplicationInfo` (other Wine processes); and **2 `vkCreateDevice`**, both vkd3d-proton on the RX 9070 XT, identical |
| Instance extensions (vkd3d) | `VK_KHR_surface`, `VK_KHR_xlib_surface`, `VK_KHR_xcb_surface`, `VK_KHR_wayland_surface`, `VK_KHR_get_surface_capabilities2`, `VK_KHR_surface_maintenance1`, `VK_EXT_surface_maintenance1`, `VK_KHR_get_physical_device_properties2`, `VK_KHR_external_semaphore_capabilities` |
| Queues | family 0 ×1 (graphics), family 1 ×4 (compute), family 4 ×1 (`QUEUE_SPARSE_BINDING_BIT` only, per `vulkaninfo`) |
| Device extensions enabled | **73** (list below) |
| Feature structs in the chain | **57**, with **202** feature bits set (incl. core `VkPhysicalDeviceFeatures`, Vulkan 1.1/1.2/1.3 structs) |

The 73 device extensions vkd3d-proton enabled for this game on RADV GFX1201:
`VK_AMD_buffer_marker VK_AMD_device_coherent_memory VK_AMD_shader_core_properties
VK_AMD_shader_core_properties2 VK_AMD_shader_explicit_vertex_parameter
VK_EXT_conditional_rendering VK_EXT_conservative_rasterization
VK_EXT_custom_border_color VK_EXT_depth_bias_control VK_EXT_depth_clip_enable
VK_EXT_descriptor_buffer VK_EXT_device_generated_commands
VK_EXT_dynamic_rendering_unused_attachments VK_EXT_extended_dynamic_state2
VK_EXT_extended_dynamic_state3 VK_EXT_external_memory_host
VK_EXT_fragment_shader_interlock VK_EXT_graphics_pipeline_library VK_EXT_hdr_metadata
VK_EXT_image_sliced_view_of_3d VK_EXT_image_view_min_lod VK_EXT_line_rasterization
VK_EXT_memory_budget VK_EXT_memory_priority VK_EXT_mesh_shader
VK_EXT_mutable_descriptor_type VK_EXT_pipeline_library_group_handles VK_EXT_robustness2
VK_EXT_shader_atomic_float VK_EXT_shader_float8 VK_EXT_shader_image_atomic_int64
VK_EXT_shader_module_identifier VK_EXT_shader_stencil_export
VK_EXT_swapchain_maintenance1 VK_EXT_transform_feedback VK_EXT_vertex_attribute_divisor
VK_EXT_zero_initialize_device_memory VK_KHR_acceleration_structure
VK_KHR_calibrated_timestamps VK_KHR_compute_shader_derivatives VK_KHR_cooperative_matrix
VK_KHR_deferred_host_operations VK_KHR_fragment_shader_barycentric
VK_KHR_fragment_shading_rate VK_KHR_index_type_uint8 VK_KHR_maintenance5
VK_KHR_maintenance6 VK_KHR_maintenance8 VK_KHR_maintenance9 VK_KHR_maintenance10
VK_KHR_pipeline_library VK_KHR_present_id VK_KHR_present_id2 VK_KHR_present_wait
VK_KHR_present_wait2 VK_KHR_push_descriptor VK_KHR_ray_query
VK_KHR_ray_tracing_maintenance1 VK_KHR_ray_tracing_pipeline
VK_KHR_shader_float_controls2 VK_KHR_shader_maximal_reconvergence
VK_KHR_shader_quad_control VK_KHR_shader_untyped_pointers VK_KHR_swapchain
VK_KHR_swapchain_maintenance1 VK_KHR_unified_image_layouts
VK_MESA_image_alignment_control VK_NV_compute_shader_derivatives
VK_NV_cooperative_matrix2 VK_VALVE_mutable_descriptor_type VK_KHR_external_memory_fd
VK_KHR_external_semaphore_fd VK_KHR_timeline_semaphore`.

What vkd3d-proton's info log adds (`runs/*/proton-log-vk-err.txt`): "DX Ultimate
supported", SM 6.6/6.7/6.8, DXR 1.1, `VK_EXT_descriptor_buffer` used for bindless,
EXT_dgc fast paths for ExecuteIndirect, "Applying resizable BAR budget to memory types
0x218", `DEVICE_LOCAL | HOST_COHERENT` used for UPLOAD heaps, swapchain 4 images with
`KHR_present_wait` latency control, and a per-game workaround ("Detected game
Cyberpunk2077.exe, adding 1 configs").

**Caveat.** This is what vkd3d-proton *enables because RADV offers it*, not the
minimum it requires: it opportunistically turns on everything it knows (ray tracing,
mesh shaders, DGC, cooperative matrices) even though these runs had RT off. The
Venus yes/no column belongs to the guest lane; which of these the menu and the Low
preset actually need has to be shown by a run against Venus's reduced list, not
inferred here.

## 8. Item 6 — audio (plan §3.3 item 7, AD-9)

PipeWire's default sink on this host is `auto_null` (`pactl info`), so the normal runs
already play into a null sink. Three no-audio runs, each to the main menu
(`evidence/cp-p0host/runs/noaudio-*/`):

| run | how audio was made unreachable (game only; host untouched) | Wine's audio state (Proton log) | main menu? |
|---|---|---|---|
| noaudio-wine | `WINEDLLOVERRIDES=winepulse.drv=d;winealsa.drv=d` (Proton log: "Effective WINEDLLOVERRIDES: winepulse.drv=d;winealsa.drv=d") | `err:mmdevapi:init_driver No driver from L"pulse,alsa,oss,coreaudio" could be initialized` (×5); no `audio_client_*` threads | **yes**, 66.4 s |
| noaudio-os | private mount ns with tmpfs over `/dev/snd`, `PULSE_SERVER=unix:/nonexistent/p0host` | **not unreachable**: `winepulse.drv` loaded and was unloaded, then `winealsa.drv` initialised through Ubuntu's ALSA→PipeWire plugin (`audio_client_main` thread exists) | yes (invalid as a no-audio test; kept as the measured trap) |
| noaudio-os2 | as above **plus tmpfs over `/run/user/1000`** (no PipeWire/Pulse/D-Bus sockets) | `winealsa.drv` loaded, no endpoint; no `audio_client_*` threads; `mmdevapi` notification thread only | **yes**, 149.2 s |

The game reaches the main menu with **no audio endpoint at all**, either because Wine
has no audio driver or because the OS has no reachable device. Both no-endpoint runs have 86 game threads at the menu vs 90 in normal runs, with
`audio_client_main`/`audio_client_timer` among the missing (`proc_menu_threads.txt`). Launch times of the two OS-level runs
(137.6 s, 149.2 s) are long, but so is noaudio-os, in which audio *was* reachable,
and noaudio-wine (66.4 s, no audio driver at all) is not; the slow load phase is not
explained by audio (see §3, launch-to-menu). Only the menu was checked; the
benchmark/in-game was not run without audio.

## 9. Item 7 — AVX (plan §3.3 item 8, gap #1, AD-4)

The host CPU has AVX/AVX2/AVX-512 and they cannot be masked for a native process
here; the masked run belongs to the guest lane (`-cpu host,-avx,…`). What the host
side can say:

- **Neither the Proton log nor any game output reports CPU features.** No
  `avx|xsave|cpuid|sse4` line in the Proton log (`+seh,+loaddll,…` channels); the game's
  benchmark `summary.json` records only `cpuName`.
- **Static count over the exe** (`objdump -d` of `.text`, 13,039,850 instructions;
  `evidence/cp-p0host/misc/avx-static.txt`): 3,359 VEX-encoded instructions, of which
  1,641 touch `%ymm` registers (AVX/AVX2) and 339 are FMA; **0 touch `%zmm`** (no AVX-512
  in the game image); 501 BMI1/BMI2 and 104 LZCNT/TZCNT/POPCNT instructions; 25
  `cpuid` and 3 `xgetbv` — the shape of runtime dispatch (check OSXSAVE/XCR0, then pick
  a path), which is why the masked guest test is still needed: static presence does
  not say whether a non-AVX fallback exists.
- **Thread state at the menu** (`/proc/<pid>/task/*/arch_status`, `proc_menu_arch_status.txt`):
  most game threads show a non-`-1` `AVX512_elapsed_ms`, i.e. the kernel saw AVX-512 state
  in their XSAVE area — but `cat /proc/self/arch_status` shows the same for `cat`
  (the host's glibc evidently runs AVX-512 code paths), so this measures the Linux userland
  under Wine, not the game.

## 10. Item 8 — does it run without a reachable Steam client?

Never by stopping the user's client: each variant runs the whole launch in a private
namespace (`tools/cp/p0host/ns.sh`) and additionally bind-mounts `/bin/false` over
every way to *start* a Steam client or URL handler (`steam.sh`, `steamrt64/steam`,
`ubuntu12_32/steam`, `/usr/lib/steam/bin_steam.sh`, `xdg-open`) and clears
`DBUS_SESSION_BUS_ADDRESS`, so a "Steam is not running" path could not start or
message the real client. Evidence that the client was or was not reached: the
client's own `cloud_log.txt` ("Running AutoCloud on exit" for AppID 1091500 at the
run's exit) and `appmanifest_1091500.acf` `LastPlayed`.

| run | hidden from the game | outcome |
|---|---|---|
| nosteam | `~/.steam` (tmpfs) + private `/dev/shm` | **Proton's `steam.exe` aborts before the game starts**: `err:steamclient:steamclient_init unable to load native steamclient library`, then a Wine dialog "Assertion failed! Program: C:\windows\system32\steam.exe File: ../src-lsteamclient/steamclient_main.c Line: 375 Expression: "!status"". Hiding `~/.steam` hides `sdk64/steamclient.so` itself, so this is "the client is not installed", not "not running". |
| nosteam2 | `~/.steam` replaced by a tmpfs holding only its library symlinks (`sdk64`, `sdk32`, `bin*`, `root`, `steam` — no `steam.pid`, `steam.pipe`, `steam.token`, `registry.vdf`) + private `/dev/shm` | Game reached the menu (201 s), **but the client was still reached**: Steam ran "AutoCloud on exit" and set `LastPlayed` at the exact second the run ended (23:23:24). Its IPC does not depend only on those files and `/dev/shm`. |
| nosteam3 | as nosteam2 **plus a private network namespace** (loopback only: no TCP/abstract-socket path to the client, and no internet) | **Main menu reached (40 s).** The log shows the failure: `err:steamclient:steamclient_init_registry Failed to connect to Steam` and the game's own `[S_API FAIL] Tried to access Steam interface SteamUser023 before SteamAPI_Init succeeded.` The client logged nothing for this run; `LastPlayed` stayed at 23:23:24 and `game_dir_changes.txt` is empty. |

**Answer: yes — the Steam build reaches the main menu with no reachable Steam client**
(and no network). `SteamAPI_Init` fails and the game carries on; `steam_api64.dll` is
not a static import of the exe (§5); it and `GameServicesSteam.dll` are loaded at run
time (both appear in the trace's file list and in the game's maps).
What *is* required under Proton is that Proton's own `steam.exe` bridge can load the
native `steamclient.so`; with a different launcher (distro Wine, §11) no bridge exists
and the game still starts. Not checked: whether anything after the menu (loading a
save, achievements, cloud) depends on Steam.

## 11. Item 9 — distro Wine + Proton's vkd3d-proton/DXVK (plan §1.4 trigger)

`tools/cp/p0host/distro-wine-prefix.sh` makes a **fresh** prefix for Ubuntu's
`/usr/bin/wine` (wine-10.0, `wineboot -i`; not a copy of the user's), copies Proton's
`d3d12.dll` + `d3d12core.dll` (vkd3d-proton) and DXVK's `dxgi.dll` into its `system32`,
sets `WINEDLLOVERRIDES=d3d12,d3d12core,dxgi=n,b`, and installs the same Low/native
`UserSettings.json`. Launched like the others (gamescope, read-only library namespace,
`shm,steamhome,nolaunch` — distro Wine has no Steam bridge anyway). Run `distrowine`,
log in `evidence/cp-p0host/runs/distrowine/gamescope-tail.txt`.

**Result: yes, it starts.** The game ran its boot movie, rendered D3D12 through
vkd3d-proton (`info:vkd3d-proton:… Device supports VK_EXT_descriptor_buffer!`,
`pEngineName: vkd3d`, `Loaded L"C:\windows\system32\d3d12.dll" … native`), loaded and
then failed Steam (`[S_API] SteamAPI_Init(): SteamAPI_IsSteamRunning() did not locate a
running instance of Steam.`, `… Could not determine Steam client install directory.`)
and carried on, showed the "PRESS … TO CONTINUE" prompt twice like the Proton runs (at
30 s and 86 s), and then stopped at a screen Proton runs never showed: the **CD
PROJEKT RED USER AGREEMENT** (OCR: "CD PROJEKT RED USER AGREEMENT last updated: 10
AUGUST 2025 …"). A fresh prefix has no record of the user having accepted it. This
lane does not accept agreements, so the run ended there (nav timeout, `rc=2`); the main
menu itself was not reached under distro Wine.

Errors on the way, none fatal: `mfc140u.dll` missing (needed only by
`CChromaEditorLibrary64.dll`, Razer Chroma), `ntlm_auth` not found, DXVK "Failed to
parse display metadata". So the §1.4 Proton trigger ("fails under upstream Wine but
works under Proton") is **not** fired by anything seen up to the agreement screen;
the remaining unknown is everything after it.

---

## 12. What this means for HobbyOS

Each row: finding → the plan item it decides (`docs/cyberpunk.md`).

| # | Finding (section) | Plan item | Consequence |
|---|---|---|---|
| 1 | The game reserves **141.5 GiB of VA** at the menu, single reservations of **63.3 GiB**, 31.4 GiB, 15.4 GiB and five ≈4 GiB; the highest mapping ends at 0x7fffffff0000 (§5) | gap #2, AD-3, F1, `src/include/vm.h` (`USER_WINDOW_SIZE` 32 GiB, MMAP slot 18 GiB) | F1's full 47-bit compat window is not a nicety, it is the minimum: today's 32 GiB window cannot hold even the largest single reservation. PROT_NONE reservations must cost no frames and no eager page tables (137 GiB of the 141.5 GiB is PROT_NONE). |
| 2 | [0, 4 GiB) is 99.9 % mapped or reserved by Wine; the exe sits at its preferred **0x140000000**; a 602 MiB committed block at 0x180010000 and a 15.4 GiB reservation starting at 0x1a5a10000 cross the 8 GiB line (§5) | AD-3 fallback ("carve the first 2 GiB out of the identity map") | The fallback is **insufficient as written**: Wine and the game use the whole 0–8 GiB identity range, not just 0–2 GiB. Either the full direct-map redesign, or a fallback that removes the identity map from **all of 0–8 GiB** for compat processes. |
| 3 | `KUSER_SHARED_DATA` is mapped at **0x7ffe0000** as a shared memfd page (`memfd:wine-mapping`) (§5) | gap #2, F1 ("an ordinary user page that Wine maps") | Confirmed: the kernel only has to allow the address and support a shared memfd mapping there. |
| 4 | `Cyberpunk2077.exe` has DYNAMIC_BASE + 1,707 relocation blocks (§5) | open question 1 | **Relocatable.** Wine still loads it at 0x140000000 when free, so the compat AS should leave that range free rather than rely on relocation. |
| 5 | Wine creates **8 shared memfd mappings, the largest exactly 64.0 MiB**; the largest MAP_SHARED mapping is a **192 MiB GPU BO**, and 3 shared mappings exceed 64 MiB, all GPU BOs (§5) | gap #7 (`VM_OBJ_MAX_BYTES` 64 MiB), F3 `GPU_IOC_RES_MAP_BLOB` | The 64 MiB per-object cap is exactly at Wine's largest memfd — one byte more and it fails; raise it (≥256 MiB) for memfd/MAP_SHARED anon. F3 blob mappings must not inherit the cap: BOs of ≥192 MiB are mapped at the menu. |
| 6 | No `.archive` is ever mapped; the largest game-file mapping is 3.0 MiB (§5, §6) | gap #7, open question 5 | Game data does **not** need >64 MiB file mappings; it needs fast `read`/`pread` over NFS. Reads of up to 171.6 MB per call and ~1.3 GB before the menu are the storage requirement (P3), not mappings. |
| 7 | Benchmark ≈ **140 fps median at both 1280×720 and 1920×1080 Low**; menu ≈ 390 fps at both; the game process uses 8 cores at the menu and 10–11 during the benchmark (§3) | AD-10, P0 go/no-go (15 fps), P9 | Host-native is **CPU-bound** at Low on this machine: resolution does not move the number. The baseline HobbyOS will be compared against is therefore mostly a CPU/Wine/syscall-path baseline, and HobbyOS's `MAX_CPUS 8` (vs 10–11 busy cores here) is a ceiling to keep in mind when reading P9 numbers. |
| 8 | **173–190 threads in 15–16 processes** at the menu (game alone 86–93, 15 `redDispatcher` workers on 32 host threads) (§3) | `MAX_PROCESSES 192` (`src/include/process.h`) | Within 2 of the limit on the host; with 8 vCPUs the worker count may drop (not measured), but the cap should be raised before P8 rather than discovered there. |
| 9 | RSS 2.2 GiB at the menu, 3.4–4.0 GiB peak; game VRAM 1.8–2.0 GiB at the menu, 3.1–3.4 GiB in the benchmark; ~470 fds (§3) | §0 "≥16 GiB guest", AD-5 host-visible heap | The game's own footprint (≤ 4.0 GiB RSS, ≤ 3.4 GiB VRAM at Low) is well inside today's 12 GiB guest and the 16 GiB card; memory size is not the constraint. |
| 10 | Launch-to-menu 37–48 s warm, 78 s with this host's page cache for the game + Proton evicted, 110–117 s with an empty Mesa or vkd3d-proton cache (§3) | P3 (storage), P8 | Launch time moves with data and cache state (×2–3); HobbyOS's NFS read path and a persistent pipeline/shader cache on the ext2 volume (AD-7) will set its launch time. |
| 11 | The Proton process tree is 16 processes: wineserver, services, 2×winedevice, plugplay, svchost, rpcss, explorer, tabtip, conhost, the game, its `REDEngineErrorReporter.exe`, plus Proton-only `steam.exe` and `xalia.exe` (§3) | P2, AD-1 | Upstream Wine's process set minus `steam.exe`/`xalia.exe` is what P2 must bring up; the game spawns a second PE process (`REDEngineErrorReporter.exe`) at start. |
| 12 | vkd3d-proton requests Vulkan **1.3**, enables 73 device extensions / 202 feature bits on RADV, uses a dedicated sparse-binding queue family, and creates 2 devices (§7) | AD-5, P5/P6, open question 4 | The list for the guest lane's Venus yes/no column. |
| 13 | AVX2/FMA/BMI code present in the exe, no AVX-512, `cpuid`/`xgetbv` dispatch present (§9) | gap #1, AD-4 | AD-4 (XSAVE with AVX, AVX-512 masked) matches the binary; whether AVX is *required* stays with the guest lane's masked run. |
| 14 | Audio: the menu is reached with Wine's audio drivers disabled and, separately, with no reachable ALSA/PipeWire device (§8) | gap #10, AD-9, open question 6 (audio half) | AD-9 "null in v1" is safe for the menu: Wine with no audio endpoints is enough. Ship no audio driver at all rather than a fake one. |
| 15 | Steam: with no reachable Steam client (and no network) `SteamAPI_Init` fails and the Steam build still reaches the main menu; Proton's `steam.exe` bridge needs `steamclient.so` to be *loadable*, nothing more (§10) | §1.5 ("use the GOG build"), AD-1 | The Steam build the user owns is usable on HobbyOS without a Steam client, as far as the menu: no `lsteamclient`, no Steam runtime. §1.5's GOG assumption can be relaxed to "either build", pending a check past the menu. |
| 16 | Distro Wine 10.0 + Proton's vkd3d-proton/DXVK `dxgi` runs the game through the boot movie and loading to the CD PROJEKT RED user agreement (fresh prefix; not accepted by this lane) (§11) | §1.4 Proton trigger, AD-1 | No evidence for the Proton trigger: upstream Wine plus the two DLL sets gets as far as Proton does before the agreement page. P2's pin can stay upstream Wine. The agreement must be accepted once per prefix by the user. |
| 17 | Launch→menu is 7.1 M syscalls in 123 distinct calls (fsync). The TEB is set with `arch_prctl(ARCH_SET_GS)` once per thread; every Wine thread arms `PR_SET_SYSCALL_USER_DISPATCH`; every Wine process probes `userfaultfd`; `getrusage` (thread times) is the third most frequent call; of §3.2's absent list `futex_waitv` (fsync only), `eventfd2` (4) and `inotify_*` (3) appear, `signalfd`/`io_uring` do not. **In-server sync costs 23 % of benchmark fps and 38 % of menu fps on the host**, wineserver doing 14.2 M syscalls to the menu (§3, §4) | gap #3, §3.2, AD-8, §1.4 fast-sync trigger | Gap #3 is confirmed as `ARCH_SET_GS`. Add to the P1/P2 syscall list: `prctl(PR_SET_SYSCALL_USER_DISPATCH)`, `userfaultfd` (whether Wine tolerates `EINVAL`/`ENOSYS` for these two is to be verified at P2, not measured here), a cheap `getrusage(RUSAGE_THREAD)`, `eventfd2`, `inotify_init1/add_watch` (winedevice only), `process_vm_readv` (wineserver), `memfd_create`. AD-8's in-server sync is a measured −23 % on the host before any HobbyOS cost: the §1.4 fast-sync trigger should be expected to fire, and wineserver's pipe round-trip latency is a HobbyOS hot path from day one. |
| 18 | `steamclient.so`, loaded into the game by `lsteamclient`, overrides `MESA_SHADER_CACHE_DIR` with the Steam library's shader cache; vkd3d-proton writes its cache relative to the CWD; the game writes pid files into its CWD (§2) | AD-7 | On HobbyOS the game dir is read-only NFS: the CWD and every cache path must point at the writable ext2 volume, or the first launch fails on a write. |

## 13. Proposed amendments to `docs/cyberpunk.md` (for the integrator; not applied here)

Each is a proposal with its evidence; the integrator decides.

1. **§0 table and §3.1** — replace "verify" items with measured facts: game 2.5 (Steam
   buildid 26161058); exe ImageBase 0x140000000, DYNAMIC_BASE, relocatable; RAM peak
   3.4–4.0 GiB RSS; GPU memory 3.2–3.4 GiB at Low; AVX2/FMA/BMI present in the exe,
   no AVX-512 (§5, §3, §9). The "~70 GB" row: the install is 62 GiB (`du -sh`); the 32 `.archive`
   files the game opens to reach the menu total 65.2 GB = 60.8 GiB (sizes in the trace's
   `files.txt`).
2. **Gap #2 / AD-3** — the fallback "carve the first 2 GiB out of the identity map" is
   insufficient: Wine maps or reserves 4,091 of the first 4,096 MiB and the game's own
   image and allocations sit at 5–6.6 GiB and beyond (§5, finding 2). Either drop the
   fallback or widen it to all of 0–8 GiB.
3. **F1** — add: single PROT_NONE reservations up to 64 GiB (63.3 GiB measured), 141.5 GiB
   of VA at the menu, highest mapping ending at 0x7fffffff0000; reservations must be
   free of frames and eager page tables. Today's 32 GiB `USER_WINDOW_SIZE` cannot hold
   the largest single reservation.
4. **Gap #7** — reword: the game reads its archives (0 archive mmaps, largest game-file
   mapping 3.0 MiB); the 64 MiB cap is instead exactly at Wine's largest `memfd` section
   (64.0 MiB) and below the GPU BO mappings (192 MiB). Raise the per-object cap for
   memfd/shared-anon, and keep F3 blob mappings free of it. Add "single `read()` of up to
   ~172 MB; ~1.3 GB read before the menu" to P3's storage requirements.
5. **§3.2 / P1–P2** — `MAX_PROCESSES 192` vs 173–190 threads in 15–16 processes at the
   menu on the host; add `PR_SET_SYSCALL_USER_DISPATCH`, `userfaultfd`, `getrusage(RUSAGE_THREAD)`,
   `process_vm_readv`, `eventfd2`, `inotify_*` to the syscall work list (§4).
6. **§10 open questions** — close Q1 (relocatable, loaded at preferred base), Q3 (TEB via
   `arch_prctl(ARCH_SET_GS)` per thread; `wrgsbase` use not observable by strace), Q5
   (archives are read, not mapped; largest file mapping 3.0 MiB), Q6 audio half (menu
   reached with no audio endpoint). Q2 (preloader's reserved ranges) is answered for
   the Proton build only by the maps in §5; repeat at the upstream-Wine pin.
7. **AD-8 / §1.4** — record the host-native cost of in-server sync: −23 % benchmark,
   −38 % menu fps (one run). Expect the fast-sync trigger; consider an `eventfd`- or
   `futex_waitv`-based sync as a planned item rather than a contingency.
8. **AD-1 / §1.4 / §1.5** — the Steam build reaches the menu with no Steam client; distro
   Wine + Proton's vkd3d-proton/DXVK `dxgi` reaches the agreement screen. The GOG
   requirement in §1.5 can become "either build"; nothing observed fires the Proton
   trigger.
9. **AD-7** — add the game's write behaviour: pid-named files and (via vkd3d-proton) a
   pipeline cache in the CWD, Steam's `steamclient.so` redirecting Mesa's shader cache
   into the Steam library, Proton's `dist.lock` opened read-write (§2). On HobbyOS the
   CWD and every cache variable must point at the ext2 volume.
10. **AD-10** — the host-native baseline is CPU-bound and identical at 720p and 1080p:
    ≈142 fps median benchmark, ≈390 fps median menu (§3). The go/no-go threshold of 15 fps
    is an order of magnitude below it; the guest lane's column decides.
11. **Appendix B** — corrections: `VKD3D_DEBUG` cannot list enabled features (TRACE is
    compiled out of the bundled build) — use a layer (`tools/cp/p0host/vkdump`); MangoHud
    0.8.2 writes no log with `no_display`; `strace -f -c` alone cannot attribute calls to
    processes — trace with `-f -tt -y` and post-process; run Proton with
    `PYTHONDONTWRITEBYTECODE=1` and the Steam library read-only (§2); `pgrep -f
    Cyberpunk2077.exe` (used in the `maps` sketch) matches several processes (the Proton
    script, `steam.exe`, the game) — select the one whose `argv[0]` is the Windows path;
    the game's main-thread `comm` is `GameThread`.
12. **§9 risks** — add "a shared host's other lanes' setup work is not covered by the
    measurement lock" (seen here: 6 of 10 benchmark runs contaminated, §3).

## 14. What could not be measured, and why

| Item | Why not |
|---|---|
| Benchmark/in-game numbers under in-server sync, more than one run | time; one clean run only (§3) |
| AVX-masked run | impossible natively on this host; the guest lane's job (§9) |
| Steam-free play past the menu (saves, achievements) | the brief stops at the menu; untested |
| Distro Wine past the user agreement | accepting the agreement is the user's decision; not done (§11) |
| Pressure-vessel / Steam Linux Runtime share of syscalls | runs used Proton without the container (§1); there is no container code in the traces |
| A cold-cache launch with the NFS server's cache also cold | only this host's page cache could be dropped (`evict.py`); the server (192.168.0.47) was not touched |
| Launch-time variance on a quiet machine | the machine was shared with another lane's guest throughout (§3) |
| Exclusive-fullscreen behaviour (open question 8) | gamescope always composites; the game chose `BorderlessWindowed` there |
| X server surface the game needs (open question 9) | not traced at the X protocol level; only `shmget`/`shmat` (MIT-SHM) visible |
| vkd3d-proton's *minimum* feature set | the layer records what it enabled on RADV, not what it requires (§7) |

## 15. Regenerating everything

All scripts live in `tools/cp/p0host/` and run on strix from `~/cp/p0host/tools/`
(`rsync -a tools/cp/p0host/ strix:cp/p0host/tools/`). Paths are in `env.sh`.

```bash
# 0. one-time setup (on strix)
sudo apt-get install -y mangohud gamescope python3-pefile libvulkan-dev vulkan-utility-libraries-dev
mkdir -p ~/cp/p0host/{pfx-proton,cache/steam-shadercache,cwd,run}
cp -a /mnt/fast/games/steamapps/compatdata/1091500 ~/cp/p0host/pfx-proton/   # 2.3 GB copy; ACL warnings are harmless
S="$HOME/cp/p0host/pfx-proton/1091500/pfx/drive_c/users/steamuser/AppData/Local/CD Projekt Red/Cyberpunk 2077/UserSettings.json"
cp "$S" ~/cp/p0host/settings-low-native.json
python3 ~/cp/p0host/tools/settings.py ~/cp/p0host/settings-low-native.json set \
    $(grep -v '^#' ~/cp/p0host/tools/settings-low-native.keys | grep .)
~/cp/p0host/tools/vkdump/build.sh                    # the Vulkan layer for item 5
~/cp/p0host/tools/distro-wine-prefix.sh              # item 9 prefix

# 1. every game run (each takes ~/cp/measure.lock itself; namespace keeps the library read-only)
cd ~/cp/p0host
tools/batch-main.sh warm perf cold vk audio audio2 steam steam2 steam3 sync distro trace
#   or one run:  MANGO=1 tools/run.sh perf-720p-x 1280 720 --bench --menu-log 30

# 2. per-run numbers
python3 tools/summarise.py runs/<tag>                # launch times, RSS/threads/fds/VRAM/CPU
python3 tools/fps.py runs/<tag>/mangohud/<file>.csv  # menu fps
python3 tools/fps.py "<prefix>/…/benchmarkResults/<dir>/frames.csv"   # benchmark fps
python3 tools/maps.py runs/<tag>/proc_menu_maps.txt  # VA layout
python3 tools/vksum.py runs/vkfeat/vkdump.txt        # Vulkan features
python3 tools/strace-summary.py runs/trace/trace.txt out --until <menu HH:MM:SS.ffffff>
python3 tools/read-sizes.py runs/trace/trace.txt <menu HH:MM:SS.ffffff>
python3 tools/pe.py "$EXE"                           # PE header
objdump -d --no-show-raw-insn -M suffix "$EXE" | grep -c '%ymm'   # static AVX count

# 3. evidence + tables
tools/collect.sh                                     # -> ~/cp/p0host/evidence (text only)
python3 tools/table.py evidence/runs perf-720p-a perf-720p-b …
tools/cp/p0host/check-no-game-files.sh               # in the repo (Mac worktree), before committing
```

Files of the lane: `env.sh` (paths), `launch.sh` (gamescope + Proton/distro Wine, private
caches/CWD, `ns.sh rolib`), `run.sh` (lock, settings, sampler, nav, stop), `nav.py` (OCR
driver, `/proc` snapshots), `shot.sh`/`ocrbox.sh` (screenshot + OCR), `sampler.py`,
`stop.sh` (wineserver -k, gamescope, game-dir audit), `ns.sh` + `mountas.py`
(namespaces), `settings.py` + `settings-low-native.keys`, `fps.py`, `summarise.py`,
`table.py`, `maps.py`, `pe.py`, `vkdump/`, `vksum.py`, `strace-summary.py`,
`read-sizes.py`, `evict.py`, `distro-wine-prefix.sh`, `batch-perf.sh` (first, aborted
batch), `batch-main.sh`, `collect.sh`, `check-no-game-files.sh`.
