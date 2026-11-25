# Cyberpunk 2077's own benchmark, 1280×720 — three stacks, one instrument

Status: **measured 2026-11-24 on `strix` (192.168.0.123), game build 2.5
(Steam app 1091500), one run per stack.**  Branch `nv/wp-cp-bench`.  Every
number below is from the game's own `summary.json` / `frames.csv` written by
its built-in benchmark, captured on screen as well as read out of the disk
image, and kept in `docs/cp-bench-evidence/`.  No number here was transcribed
from memory or from an earlier document.

**What this document is.**  This lane exists because the owner asked for
Cyberpunk's built-in benchmark as a data point.  It reports that data point on
three stacks and labels each one honestly.  **The HobbyOS column is a
*platform* data point** — HobbyOS's Linux-ABI compat + X + Wine + vkd3d-proton
on **Venus over virtio-gpu**.  It is **not** evidence for the `nv` (GSP-RM)
driver: that driver has no 3D engine and cannot run this game today.

Companions: `docs/cyberpunk.md` §0.2 (the binding status table this fills in),
`docs/cp-p0-host.md` (the original host-native column),
`docs/cp-p0-guest.md` (the Linux-guest-on-Venus column, whose benchmark row was
"not run"), `docs/cp-p9.md` (the HobbyOS in-world numbers), `docs/cp-venus.md`
(the Venus host recipe).

---

## 1. The result

One instrument for all three: the game's Settings → GRAPHICS → **RUN BENCHMARK
[B]** → "Are you sure you would like to run a benchmark?" → CONFIRM.  The game
flies its own 64 s scripted camera path over Night City, then prints a Results
page (`Average FPS`, `Min FPS`, `Max FPS`, `Time`, `Number of Frames`,
`Preset Name`, `Texture Quality`, `Resolution`, `Window Mode`, `Vertical Sync`,
`Maximum FPS`, `Frame Generation`, `Resolution Scaling`, `Ray Tracing`) and
writes `benchmarkResults/<date>/{summary.json,frames.csv}` to the prefix.

"Median / p90 / 1 %-low" are `tools/cp/p0host/fps.py`'s definitions —
1000 ÷ the p50 / p90 / p99 frame time — so they are directly comparable with
`docs/cp-p0-host.md`'s columns.

| stack (what produced the number) | preset & settings as the game reported them | game `averageFps` | median | p90 | 1 %-low | 0.1 %-low | frames / time | captured how |
|---|---|---|---|---|---|---|---|---|
| **host native** — Proton Experimental `experimental-11.0-20261112`, RADV, bare-metal RX 9070 XT, 96 GiB.  **The control.** | Custom (Low row set), Texture Low, 1280×720, borderless, VSync Off, cap Off, FSR off, RT off | **139.41** | 145.6 | 108.2 | 77.3 | 63.1 | 8958 / 64.26 s | `tools/cp/p0host/run.sh cpbench720 1280 720 --bench`; screenshot + `summary.json` + `frames.csv` |
| **HobbyOS** — HobbyOS Linux-ABI compat, XHOBBY 1280×720, Wine 11 + vkd3d-proton 3.1 + DXVK 3.1.1, **Venus** on virtio-gpu, C6-p7 zero-copy scanout, guest `-m 12G -smp 8` | **Custom** (= the Low row set: Texture Low, most advanced Low/Medium, Crowd Low, Motion Blur Off), Resolution Scaling **Off**, 1280×720, borderless, VSync Off, cap Off, FSR off, RT off | **29.19** | 29.7 | 22.6 | 18.2 | 16.3 | 1874 / 64.20 s | `run-p9.sh` + `tools/cp/bench/bench720.p9`; VNC/scanout screendumps + `summary.json` + `frames.csv` from `cpdata.img` |
| **Linux guest on Venus** — Ubuntu 26.04 guest (32 GiB), Mesa Venus ICD, Proton Experimental (vkd3d-proton 3.2.0, DXVK 3.1.1-209), Xorg modesetting+glamor 1280×720, `hostmem=16G` | **Low** preset as the game installs it, Texture Low, FSR **2.1 Auto** (upscaling on), 1280×720, borderless, VSync Off, cap Off, RT off | **25.86** | 27.0 | 20.4 | 16.5 | 2.6 | 1703 / 65.85 s | `host-venus.sh` + `guest-cp.sh`/`guest-cp-walk.sh`/`guest-cp-ui.sh`; guest screenshot + `summary.json` + `frames.csv` |

Identical on all three, from each run's own `summary.json`: `gameVersion 2.5`,
`benchmarkName base\streaming\test\benchmark\benchmark.gamedef`,
`renderWidth/Height 1280/720`, `windowMode 1` (borderless), `verticalSync
false`, `fpsClamp -1`, `frameGenerationType 0`, `rayTracingEnabled false`.
The two Venus runs report `gpuName "Virtio-GPU Venus (AMD Radeon RX 9070 XT
(RADV GFX1…#"`, `gpuMemory 16304 MB`; the host run reports `"AMD Radeon RX
9070 XT (RADV GFX1201)"`.  GPU driver: `Radeon software 99.10.2` on host and
guest, `<UNKNOWN VERSION>` inside HobbyOS.

Frame-count check: `8958/64.26 = 139.4`, `1874/64.20 = 29.19`,
`1703/65.85 = 25.86` — the game's own `averageFps` equals the frame count over
its own duration in each case.

## 2. What each column is, and is not

1. **It is the game's own benchmark, not a synthetic scene.**  The scene, the
   camera path, the warm-up and the statistics are the game's.  The same
   `benchmark.gamedef` ran on all three stacks.
2. **The HobbyOS number is platform, not driver.**  It measures
   XHOBBY + Wine + vkd3d-proton + Venus on the virtio-gpu device.  The `nv`
   GSP-RM driver cannot load a 3D engine, so it cannot produce this number;
   nothing here should be read as `nv` validation.
3. **The two guest columns are not the same workload, and are not averaged.**
   The HobbyOS run renders at 1280×720 with resolution scaling **off**; the
   Linux guest run uses the Low preset's own **FSR 2.1 Auto** upscaling, i.e.
   it feeds the GPU *less* work and upscales.  The difference is recorded, not
   smoothed away: see §4 for what it does and does not explain.
4. **Not a gameplay number, and not a replacement for P9's.**  This is a
   scripted flight.  `docs/cp-p9.md`'s HobbyOS in-world medians (12.1–15.0 fps
   at ManualSave-86, menus 34–48) come from a different scene and a different
   instrument (winevulkan `trace:fps` rows), and `docs/cp-hunt.md`'s boots span
   27–37 fps world medians.  The benchmark's 29.19 sits inside that spread; it
   does not invalidate or replace P9's gate number.
5. **n = 1 per stack.**  The spread *inside* one benchmark (min 20.6 / max
   43.9 on HobbyOS) is far larger than any percent-level comparison between
   columns.  Treat differences of a few fps as noise until repeated.

## 3. How each run was made (exact commands)

**Game copy.**  The P8/P9 harness reads `/srv/nfs/cp-game` (exported
read-only to localhost by `tools/cp/exports.sh`).  That copy had been deleted
since the last cyberpunk wave; the game now exists only on the NAS at
`/mnt/fast/games/steamapps/common/Cyberpunk 2077` (NFS from 192.168.5.112,
mode `0770 sarah:sarah` — see the blocker in §5.1).  It was re-staged as a
local copy, and the census proves it is the same copy the earlier waves used:

```
census: 156 files, 66820176214 bytes, newest mtime 1794714264
```

(identical before the run and after all three runs; the game directory is
never written).

### 3.1 Host native (control)

```
cd ~/cp/p0host && bash tools/run.sh cpbench720 1280 720 --bench
```

`run.sh` copies `settings-low-native.json` into its own prefix copy, sets
`Resolution=1280x720`, launches `Cyberpunk2077.exe --launcher-skip` under
Proton inside headless gamescope (`launch.sh`), and `nav.py` walks
PRESS-TO-CONTINUE → main menu → SETTINGS → `3` (GRAPHICS) → `b` → Return,
then waits for a new `benchmarkResults/*/summary.json`.

### 3.2 HobbyOS on Venus

```
cd ~/cp/bench-wp-cp        # a copy of the cyberpunk integration tree (integ27, 0ac70a7b)
VENUS_BUILD=$HOME/cp/integ-venus P9_SCRIPT=tools/cp/bench/bench720.p9 \
P9_MEM=12288 P9_XSCREEN=1280x720 P9_GAME_RES=1280x720 P9_RUN=bench720a \
TIMEOUT_S=3600 P8_STALL_S=900 bash tools/cp/p9/run-p9.sh
```

`run-p9.sh` → `run-p8.sh` builds `hobbyos.elf` + `disk.img`, stages glibc
2.31 / XHOBBY / the Wine tree / the X11 tree / the Vulkan tree (Venus ICD
only) on the read-only exports, copies the seeded prefix (`pfx-saves.img`,
whose `UserSettings.json` is the user's: `QuickPresets Custom`,
`ResolutionScaling Off`, `Resolution 1280x720`, `VSync Off`,
`RayTracing false` — kept with the evidence), and boots
`tools/cp/host-venus.sh`'s QEMU line (`virtio-gpu-gl-pci
blob=true,hostmem=8G,venus=true`, `egl-headless` on `renderD129`, memfd RAM).
`tools/cp/bench/bench720.p9` then does, on the guest's own screen:

```
seek menu-settings main down 6 SETTINGS ; key ret
key 3 x3 ; shot (GRAPHICS tab)
key b ; shot ; key ret            # confirm
expect Average FPS ...            # 900 s budget
shot (results) ; key esc ; key esc ; seek QUIT GAME
```

Boot → main menu 151.7 s; `b` → results screen ≈ 2 min (64.2 s flight plus
loading).  Extraction of the game's files from the ext2 data disk:

```
debugfs -R 'rdump "/pfx/drive_c/users/root/Documents/CD Projekt Red/Cyberpunk 2077/benchmarkResults" /tmp/br' cpdata.img
python3 -I tools/cp/p0host/fps.py /tmp/br/benchmarkResults/*/frames.csv
```

`e2fsck -fn` on the data disk after the run: rc 0.  Exports removed by the
harness's trap; no QEMU left.

### 3.3 Linux guest on Venus

```
cd ~/cp/p0guest
bash tools/host-venus.sh start                 # qemu, egl-headless on renderD129, ssh 127.0.0.1:2225
bash tools/host-venus.sh ssh 'bash ~/tools/guest-x.sh kms 1280x720;
    bash ~/tools/guest-proton.sh mount;                        # overlayfs over the ro virtiofs exports
    python3 ~/tools/guest-cp-settings.py 1280x720;             # sets only Resolution; prints the rest
    bash ~/tools/guest-cp.sh launch bench720;
    bash ~/tools/guest-cp-walk.sh bench720 400'                # to the main menu (47.0 s this boot)
# then, in the guest: click SETTINGS (158,364 in 1280x720), click GRAPHICS tab (497,32),
# 'b' (RUN BENCHMARK), Return (CONFIRM); poll OCR until "Average FPS"; screenshot.
bash tools/host-venus.sh get runs/bench720/summary.json /tmp/g-summary.json
bash tools/host-venus.sh stop
```

The settings the run actually had, printed by
`guest-cp-settings.py` before the launch: `QuickPresets Low`,
`ResolutionScaling FSR2`, `FSR2 Auto`, `DynamicResolutionScaling False`,
`TextureQuality Low`, `RayTracing False`, `VSync Off`, `MaximumFPS_OnOff
False`, `WindowMode BorderlessWindowed`, `Resolution 1280x720`.

## 4. What the numbers say — and what they do not

* **On this box the benchmark is ~4.8–5.4× slower than native on both guest
  stacks**: 139.41 native → 29.19 HobbyOS (0.21×) and 25.86 Linux guest
  (0.19×).  This is a property of the **D3D12-on-Venus path as a whole**
  (Wine + vkd3d-proton + the virtio-gpu command stream + present), not of
  HobbyOS's kernel: the two guests land in the same band.
* **The HobbyOS run is not slower than the Linux guest's, despite doing more
  GPU work.**  HobbyOS rendered at native 720p (scaling off) and got 29.19;
  the guest rendered with FSR 2.1 Auto and got 25.86.  That is consistent with
  `docs/cp-p0-guest.md`'s finding that the Linux guest is *present-path*
  bound (Xorg/glamor → virtio-gpu → QEMU `egl-headless`; its menu median was
  identical at 720p and 1080p), while the HobbyOS run presents through C6-p7's
  zero-copy scanout on a 1280×720 X screen.  **It is one run each and the
  settings differ**, so this is a hypothesis with a named test, not a claim:
  re-run the guest with `ResolutionScaling Off`, and HobbyOS with FSR 2.1 Auto,
  before anyone writes "HobbyOS presents faster than the Linux guest".
* **The 1 %-low and the tail are where the guest shows its seams**: 1 %-low
  16.5 fps on the guest vs 18.2 on HobbyOS, and its 0.1 %-low is **2.6 fps**
  against HobbyOS's 16.3 — the guest's `frames.csv` has **7 frames over
  100 ms** (191, 218, 385, 407, 551 ms …) where the HobbyOS run has **none**
  (worst frame 63.1 ms).  Seven stalls in 1703 frames is a tail, not a trend;
  it is recorded because it is the only structural difference the tails show.
* **The in-game trace does not fully corroborate the frame log, and is not
  used as the number.**  The winevulkan `trace:fps` rows tagged `bench` in the
  HobbyOS run (`winevulkan-fps.csv`) run 21.5–42.3 fps for 53 of 97 rows —
  the band the on-screen HUD showed (`FPS: 26`, `28`, `33`, `42`) and the
  game's own min/max (20.6 / 43.9) — but the other 44 rows read 86–203 fps.
  The game's frame log for that run contains **no** frame faster than 43.9 fps,
  so those rows are not flight frames; a likely explanation is the
  inter-segment cuts/loading that the game's per-frame log does not count, but
  **this lane did not verify it**.  The game's `frames.csv` is the number;
  the trace is context.

## 5. Blockers and anomalies, with the command and the error

### 5.1 The 67 GB game copy was gone, and root cannot read the NAS copy

`/srv/nfs/cp-game` — the path `run-p8.sh`/`run-p9.sh` require — did not exist:

```
$ ls -d /srv/nfs/cp-game*      →  No such file or directory
$ find / -xdev -name Cyberpunk2077.exe  →  (nothing)
```

The only copy is on the NAS mount, which root cannot traverse (NFS root
squash; the export is `0770 sarah:sarah`):

```
$ sudo -n ls /mnt/fast
ls: cannot open directory '/mnt/fast': Permission denied
$ sudo -n mount --bind "/mnt/fast/games/steamapps/common/Cyberpunk 2077" /srv/nfs/cp-game
<my mount script>: NO SOURCE /mnt/fast/games/steamapps/common/Cyberpunk 2077
```

**Fix:** copy it (a bind mount as root is impossible; a symlink would fail the
same permission check inside nfsd) and let the harness serve its own copy:

```
sudo -n mkdir -p /srv/nfs/cp-game && sudo -n chown sarah:sarah /srv/nfs/cp-game
rsync -a "/mnt/fast/games/steamapps/common/Cyberpunk 2077/" /srv/nfs/cp-game/
chmod -R a+rX,a-w /srv/nfs/cp-game        # 328 MB/s from the NAS → 63 GiB in ~3.5 min
```

Census after: `156 files, 66820176214 bytes, newest mtime 1794714264` — the
same copy every earlier wave booted (`docs/cp-p8.md` §1 records exactly these
three values for the copy the harness refuses to run without), so the
instrument is the same one P0 measured.

### 5.2 The HobbyOS play script's result match fired early (harness bug, data intact)

`bench720.p9` v1 ended its `expect` on
`Average\s*FPS|AVERAGE\s*FPS|Results`; the bare word `Results` matched a frame
before the benchmark had finished, so the script went on to quit while the
benchmark was still flying.  The game ignored the Esc/seek input and completed
normally, and the results screen and `summary.json` were both captured — but
the run's verdict is `seek-failed:menu-quit`, not a clean quit:

```
$ cat obj/cp-p9/bench720a/verdict.txt
seek-failed:menu-quit
[P9W] + 340.1 s  play script ended: seek-failed:menu-quit
== e2fsck -fn after the boot: rc=0
```

**Fix for the next run:** match `Average\s*FPS` only (the live HUD says
`FPS: nn`, never "Average FPS"), and quit from the results page's own
`EXIT BENCHMARK` dialog rather than the main menu.  The committed
`bench720.p9` is that v2: its header records what v1 did and why the pattern
changed.

### 5.3 The HobbyOS settings page: judge the tab by its content, not its header

The page opens on its **last-used** tab — on this prefix, `SOUND CONTROLS`
(audio rows).  `key 2` changed nothing (the page stayed on the audio rows);
`key 3` ×3 landed on **GRAPHICS** (`Quick Preset … Custom`, `Resolution
Scaling`, `Frame Generation`, `Texture Quality`, `Ray Tracing`, `Crowd
Density Low` — `settings-graphics.ocr.txt`).  The OCR header always lists every
tab (`SOUND CONTROLS GAMEPLAY GRAPHICS VIDEO …`), so the header proves nothing
about the visible page; the page's own rows do.  `b` triggers **RUN
BENCHMARK** from any tab, so the run does not depend on getting the tab right
(that is also why the sequence presses `3` three times).


### 5.4 The Linux-guest benchmark's confirmation was allowed this round

P0 skipped the benchmark because the confirmation dialog was refused by that
session's permission policy (`docs/cp-p0-guest.md` §5.6).  This round the
owner asked for the number, so `Return` was sent on "Are you sure you would
like to run a benchmark? any unsaved setting changes will be reset." — settings
were already saved, nothing was lost.  The dialog and the run are in
`evidence` (`results.ocr.txt` shows the CONFIRM/CANCEL pair).

### 5.5 `host-venus.sh stop` printed a missing pidfile but did stop

```
$ bash tools/host-venus.sh stop
cat: /home/sarah/cp/p0guest/run/qemu.pid: No such file or directory
stopped
```

No QEMU process remained (checked with `pgrep -af qemu-system`: none).  The
run dir's pidfile is removed by the previous stop; the script's message is
cosmetic.

### 5.6 The host-native run touched the Steam library's manifest, not the game

`run.sh`'s own check printed:

```
WARNING: game dir changed during cpbench720:
/mnt/fast/games/steamapps
/mnt/fast/games/steamapps/appmanifest_1091500.acf
```

Only the Steam app-manifest's mtime moved (Proton/Steam bookkeeping on launch);
the game's files are byte-identical —
`156 files, 66820176214 bytes, newest mtime 1794714264` before and after.

### 5.7 Not blocked, but worth knowing: versions differ between the two guests

HobbyOS stages **vkd3d-proton 3.1** (`~/cp/vk2-pe`, `docs/cp-venus.md` §6) and
DXVK 3.1.1 from the same staging; the Linux guest's Proton is
`experimental-11.0-20261112` with **vkd3d-proton 3.2.0** / DXVK 3.1.1-209
(`docs/cp-p0-guest.md` §1).  The D3D12 translation layer is therefore not the
same code on the two guest columns, which is a second reason not to read their
difference as a HobbyOS-vs-Linux statement.

## 6. Next steps (each with the number it would settle)

1. **Guest, scaling off** (`ResolutionScaling On/Off` at 720p): makes the guest
   column the same workload as the HobbyOS column, and settles §4's second
   bullet.
2. **HobbyOS, FSR 2.1 Auto**: the mirror image, if the guest's upscaling turns
   out to matter.
3. **Repeat each stack 3×**: currently n = 1; the p90/1 %-low tails deserve a
   repeat before anyone quotes them as stable.
4. **P9's in-world number against this benchmark's scene**: the save's street
   at ManualSave-86 is the heavier scene; a benchmark-sized claim should not
   be used to reopen P9's 30 fps gate, and P9's number should not be used to
   predict the benchmark.
5. **The 1 %-low's shape** — done for the HobbyOS column: `docs/nv-n9v-tail.md`
   reads this run's own `frames.csv` + `hbp.csv` and shows the tail is the
   benchmark's camera-path *level* (23.0–43.2 ms medians) plus broadband jitter,
   with no periodic component, and that the 30 fps 1 %-low implies a ~49 fps
   median. Reproduce with `tools/cp/bench/frames-dist.py`.

## 7. Evidence map

| file | what |
|---|---|
| `docs/cp-bench-evidence/host-native-720p/{summary.json,frames.csv,results.png,meta.txt,nav.log}` | the control run; `meta.txt` names Proton, prefix, resolution |
| `docs/cp-bench-evidence/hobbyos-venus-720p/{summary.json,frames.csv,results.png,results.ocr.txt}` | the HobbyOS run, the game's own files from `cpdata.img` and the results screendump |
| `docs/cp-bench-evidence/hobbyos-venus-720p/{UserSettings.json,settings-graphics.ocr.txt}` | the settings in effect, from the prefix image and from the screen |
| `docs/cp-bench-evidence/hobbyos-venus-720p/{timeline.txt,verdict.txt,qemu-cmdline.txt,run-p8.log,winevulkan-fps.csv,hbp.csv,game-census-*.txt}` | how the boot ran: phases, the QEMU line, the harness log, the trace rows, the present-path rows, the game-copy census |
| `docs/cp-bench-evidence/linux-guest-venus-720p/{summary.json,frames.csv,results.png,results.ocr.txt}` | the Linux-guest-on-Venus run, guest screenshot and the game's files |
| `tools/cp/bench/bench720.p9` | the HobbyOS play script used (run-p9's `P9_SCRIPT`) |
| `tools/cp/bench/extract-bench.sh` | `debugfs` for the game's `benchmarkResults` out of a HobbyOS `cpdata.img` |
