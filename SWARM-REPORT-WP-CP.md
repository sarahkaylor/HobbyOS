# SWARM-REPORT-WP-CP — Cyberpunk 2077's own benchmark, three stacks labelled

Branch `nv/wp-cp-bench`, worktree `wp-cp`, cut from `nv/integ-swarm` @
b3edb2a2.  **Docs + evidence + two harness scripts; no kernel or driver code.**
Owner's ask: Cyberpunk's built-in benchmark at 720p with the preset/settings
named, and an honest label for which stack produced it.

## 1. The data point

The game's own Settings → GRAPHICS → **RUN BENCHMARK** → CONFIRM, 1280×720,
game build 2.5.  Numbers are the game's `summary.json` / `frames.csv` (median /
p90 / 1 %-low = 1000 ÷ p50 / p90 / p99 frame time, `tools/cp/p0host/fps.py`).
Screenshots and the game's own files are in `docs/cp-bench-evidence/`.

| stack | what it is | avg / median / p90 / 1 %-low / 0.1 %-low fps | frames / time | how captured |
|---|---|---|---|---|
| **host native** (Proton Exp. 11.0-20261112, RADV, RX 9070 XT) | the **control** — the same instrument on the same box, no VM | **139.41 / 145.6 / 108.2 / 77.3 / 63.1** | 8958 / 64.26 s | `tools/cp/p0host/run.sh cpbench720 1280 720 --bench`; results screenshot + prefix `summary.json`/`frames.csv` |
| **HobbyOS on Venus** (Linux-ABI + XHOBBY + Wine 11 + vkd3d-proton 3.1, virtio-gpu Venus, C6-p7 scanout, `-m 12G -smp 8`) | **platform** data point — *not* the `nv` driver (no GSP-RM / no 3D engine, cannot run this game) | **29.19 / 29.7 / 22.6 / 18.2 / 16.3** | 1874 / 64.20 s | `run-p9.sh` + `tools/cp/bench/bench720.p9`; screen captured over VNC/scanout, files pulled from `cpdata.img` with `debugfs` |
| **Linux guest on Venus** (Ubuntu 26.04, Mesa Venus, Proton Exp., vkd3d-proton 3.2.0) | the row `docs/cyberpunk.md` §0.2 left as "not run (user: skip)" | **25.86 / 27.0 / 20.4 / 16.5 / 2.6** | 1703 / 65.85 s | `host-venus.sh` + guest `guest-cp-*` tools; guest screenshot + prefix files |

All three, from each run's `summary.json`: `gameVersion 2.5`, 1280×720,
borderless, VSync off, no frame cap, frame generation off, RT off.  Settings
differ in one measured way: **HobbyOS ran `Custom` = the Low row set with
resolution scaling Off; the Linux guest ran `Low` with FSR 2.1 Auto**.  They
are therefore not the same workload and are not averaged or ranked.

## 2. What is verified, and what is not

**Verified (measured, this session, 2026-11-24):**
- The benchmark runs to completion on HobbyOS and the game writes its own
  `benchmarkResults/*/{summary.json,frames.csv}` on the ext2 prefix
  (`e2fsck -fn` rc 0 after the boot; results page OCR + screenshot).
- The **host control reproduces P0's number** (this run 139.41 avg / 145.6
  median / 77.3 1 %-low vs P0's 141.6–142.9 median / 74.1–76.1 1 %-low) on the
  same game copy: census `156 files, 66820176214 bytes, newest mtime
  1794714264`, identical before and after all three runs.  The instrument is
  the one P0 used.
- The Linux-guest-on-Venus benchmark row now exists, captured on screen and
  from the prefix.
- Frame-count checks: `8958/64.26 = 139.4`, `1874/64.20 = 29.19`,
  `1703/65.85 = 25.86` — the game's `averageFps` equals frames ÷ its own time.

**NOT verified / explicitly not claimed:**
- **n = 1 per stack.**  Nothing here is a stable estimate of the p90/1 %-low
  tails; the in-run spread (min 20.6 / max 43.9 on HobbyOS) dwarfs the gaps.
- **Not a driver result.**  HobbyOS's number is the Venus/virtio-gpu platform
  under the Linux-ABI compat layer.  It says nothing about the `nv` driver.
- **Not a gameplay number.**  It is a scripted 64 s camera flight; P9's
  in-world medians (12–15 fps at ManualSave-86) come from a different scene and
  instrument and are untouched by this.
- **No HobbyOS-vs-Linux speed claim.**  The two guest runs differ in upscaling
  and in vkd3d-proton version (3.1 vs 3.2.0).  §4 of `docs/cp-bench.md` records
  the observation (HobbyOS 29.19 at native 720p vs guest 25.86 with FSR 2.1
  Auto) as a hypothesis with the test that would settle it.
- **No kernel/driver build gate applies:** this branch changes no C.  The
  harness did build `hobbyos.elf` + `disk.img` from the run tree
  (`~/cp/bench-wp-cp`, integ27 @ 0ac70a7b) with `TOOLS_SKIP=1`, green, as part
  of `run-p8.sh` — that is the tree that produced the number, not this branch.

## 3. Blockers (exact command → error), all in `docs/cp-bench.md` §5

1. **`/srv/nfs/cp-game` was gone and root cannot read the NAS copy.**
   `ls -d /srv/nfs/cp-game` → `No such file or directory`;
   `sudo -n ls /mnt/fast` → `Permission denied` (NFS root squash, `0770
   sarah:sarah`); `sudo -n mount --bind ".../Cyberpunk 2077" /srv/nfs/cp-game`
   → `NO SOURCE`.  Fixed by `rsync -a` the 63 GiB into `/srv/nfs/cp-game`
   (328 MB/s) + `chmod -R a+rX,a-w`; census identical to P0's copy.
2. **HobbyOS play script matched its results regex early** (v1 included the
   bare word `Results`) → run verdict `seek-failed:menu-quit`; the benchmark
   completed anyway and both the screen and `summary.json` were captured.  The
   committed `bench720.p9` matches `Average\s*FPS` only and says so in its
   header.
3. **The HobbyOS settings page opens on its last-used tab** (`SOUND CONTROLS`
   here) and its OCR header lists every tab; `key 3` did reach **GRAPHICS**
   (proven by the page's own rows in `settings-graphics.ocr.txt`), and `b`
   triggers RUN BENCHMARK from any tab, so the run does not depend on the tab.
   Judge tabs by page content, never by the header.
4. **The guest benchmark's "Are you sure…" dialog was answered** (`Return`) —
   allowed this round; P0 had skipped it under that session's permission
   policy.  Settings were already saved.
5. **`host-venus.sh stop`** prints `cat: …/run/qemu.pid: No such file or
   directory` then `stopped`; no QEMU remained (`pgrep -af qemu-system`: none).
6. **Host run touched Steam's `appmanifest_1091500.acf` mtime only**; game files
   unchanged (census above).

**Housekeeping:** no QEMU left running on `strix` (both the HobbyOS boot and the
Linux guest were stopped and the harness's export trap ran: `exports.sh: remove
/srv/nfs/cp-game ok` / `… /srv/nfs/cp-play-p9 ok`).  r1's VM 300 was not
touched.  `/srv/nfs/cp-game` (63 GiB, read-only) and `~/cp/bench-wp-cp` (tree
copy) were left in place so the next lane can re-run without the 4-minute copy;
neither is another lane's.

## 4. Files

- `docs/cp-bench.md` — the report: table, what each column is/is not, method,
  numbers, blockers, next steps, evidence map.
- `docs/cp-bench-evidence/{host-native-720p,hobbyos-venus-720p,linux-guest-venus-720p}/`
  — `summary.json`, `frames.csv`, `results.png` (+ OCR) per stack; the HobbyOS
  dir also carries `UserSettings.json`, `settings-graphics.ocr.txt`,
  `timeline.txt`, `verdict.txt`, `qemu-cmdline.txt`, `run-p8.log`,
  `winevulkan-fps.csv`, `hbp.csv`, `game-census-*.txt`.
- `docs/cyberpunk.md` §0.2 — the benchmark row filled in for all three stacks,
  with the platform-vs-driver and workload caveats inline.
- `tools/cp/bench/bench720.p9`, `tools/cp/bench/extract-bench.sh` — the HobbyOS
  play script and the `debugfs` extractor.

## 5. Next step

Run the two A/Bs that make the guest columns comparable — **Linux guest with
`ResolutionScaling Off`** and **HobbyOS with FSR 2.1 Auto** — then repeat each
stack 3× before quoting a p90/1 %-low.  That settles the only open question
this data raises: whether the HobbyOS number is ahead because of C6-p7's
scanout or because it simply rendered more pixels.
