# cp/p8 — Cyberpunk 2077 in HobbyOS on lavapipe: the launch recipe and the first boots (lane C8-game, wave 5)

Plan: `docs/cyberpunk.md` §6 P8, §7.2e (C8-game row), §0.2 items 1, 2, 13, 14
and 18.  Template: the P5 harness (`tools/cp/p5/`, `docs/cp-vk.md` §8.5).
Every number below was measured on **strix** (Ryzen AI MAX+ 395, Zen 5,
AVX-512, KVM, `-cpu host -smp 8 -m 12288M`, one QEMU slot, 1–3 other lanes'
guests on the box) on tree `cyberpunk` @ 949ba008 plus this lane's harness
commits.  Evidence: `docs/cp-game-evidence/*.txt`; screendumps (git-ignored)
under `evidence/cp-game/`.

**Result.** The recipe launches the game in the guest's X session with every
P8 setting in place, and the game runs for up to 42 s (guest clock; the
tightest bracket is run 3's 30–33 s).  In that time the process tree logs 96
loaddll rows, including vkd3d-proton's `d3d12.dll`/`d3d12core.dll` and DXVK's
`dxgi.dll` from the prefix as native overrides, the game's own `sl.interposer`,
PhysX, Bink, Oodle, ICU and the Steam API shim (which fails and carries on, as
in P0).  It then stops, in every game boot that got past the warm-up (runs 1–4 and
6), at the **same kernel ceiling**:
the **wineserver's fd table is full (`MAX_OPEN_FDS` 256)**.  The wineserver's
`recvmsg` fails with EMFILE.  Wine kills the game, and then the wineserver
itself crashes on HobbyOS's non-Linux `recvmsg` semantics.  No game window
appeared, so the Gate P8 main menu was not reached.  The ranked blockers are
in §6.

## 1. The recipe (`tools/cp/p8/`)

| file | what |
|---|---|
| `run-p8.sh` | the host driver: builds, stages, exports, boots, checks the image afterwards |
| `p8launch.c` | the in-guest native launcher (XSTART), modelled on `winep5.c` |
| `boot_p8.py` | one QEMU slot for the boot only, timeline, screendumps, OCR, stop rules |
| `seed-prefix.sh` | seeds the prefix image on the host with debugfs: DLLs, profile, cache dirs |
| `rows.sh` | the rows that matter in a P8 serial log |

`P8_PHASE=probe` runs the probes of §4 alone.

```
# once: the prefix image (wineboot -i in the guest, then the host seeds it)
P8_PHASE=pfx   PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 bash tools/cp/p8/run-p8.sh
# each game boot, on a copy of obj/cp-p8/pfx-seeded.img
               PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 bash tools/cp/p8/run-p8.sh
# the kernel probes only (§4), no Wine, no game export
P8_PHASE=probe PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 bash tools/cp/p8/run-p8.sh
```

Defaults: Wine `~/cp/wine-build-x` (X11 + Vulkan), X stage `~/cp/x11-build-x2`
with C4-x2's one-screen `XHOBBY.BIN` (sha256 `935c760f…4c97`, checked) and
`HB_SECOND_SCREEN=0`, lavapipe + loader `~/cp/vk2-build/stage-x11`, vkd3d-proton
3.1 + DXVK 3.1.1 from `~/cp/vk2-pe`.

- **The game copy.** It is exported only with `tools/cp/exports.sh add
  /srv/nfs/cp-game` and removed in the EXIT trap.  The guest mounts it at
  `/GAME`, next to the run's own staging export `/srv/nfs/cp-game-p8` at `/CP`
  (WINE, X11, VK and the two PE probes).  The harness never writes, chmods,
  copies or deletes anything under `/srv/nfs/cp-game`.  Before and after
  every boot it records the file count, bytes and newest mtime: 156 files,
  66,820,176,214 bytes, newest mtime 1794714264, the same in every run.  It
  refuses to start if the copy is already exported.
- **The game's CWD** is `/GAME/bin/x64`, the read-only NFS export (`chdir`
  in the launcher; the compat process inherits it, so Wine's cwd is
  `Z:\GAME\bin\x64`).  The command is `wine Z:\GAME\bin\x64\Cyberpunk2077.exe
  --launcher-skip`.  There is no Steam client: `steam_api64.dll` loads and
  fails, as in P0.
- **The prefix** is `WINEPREFIX=/EXT/pfx` on the ext2 data disk (NVMe ns 2).
  It is built once: a fresh `mkcpdata.sh` image (16 GiB sparse), one boot
  that runs `wineboot -i` (status 0 in 42–46 s), then `reg add
  HKCU\Software\Wine\WineDbg ShowCrashDialog=0` (a crash prints a backtrace
  instead of opening a dialog nobody clicks), then `sync`.  The host then
  runs `e2fsck -fn` (rc 0) and `seed-prefix.sh`, and `e2fsck -fn` again
  (rc 0).  Every game boot runs on a sparse copy of that image.
- **The native overrides.** `seed-prefix.sh` writes vkd3d-proton's
  `d3d12.dll` + `d3d12core.dll` and DXVK's `dxgi.dll` + `d3d11.dll` over
  Wine's placeholders in `system32`.  That is Proton's layout: the game
  directory is read-only and cannot hold them.  The launcher sets
  `WINEDLLOVERRIDES=d3d12,d3d12core,dxgi,d3d11=n;mscoree,mshtml=;winemenubuilder.exe=d`.
  Proof: loaddll rows `C:\windows\system32\dxgi.dll … native` in the game,
  and the caps probe's `GetModuleFileName` rows (§5).
- **The profile.** It is the user's own P0 first-run state from
  `~/cp/game-profile`, written byte-identical (sha256 read back from the
  image): `users/root/Saved Games/CD Projekt Red/Cyberpunk 2077/{user.gls,
  EnabledMods.json,HDRUserSettings.json}` and `users/root/AppData/Local/CD
  Projekt Red/Cyberpunk 2077/UserSettings.json`.  The prefix user is `root`.
  **No key was changed.**  `UserSettings.json` (version 152) already matches
  all 35 keys of `tools/cp/p0host/settings-low-native.keys` plus
  `/video/display/Resolution = "1280x720"`: QuickPresets "Custom" (= Low with
  scaling off), RayTracing false, ResolutionScaling "Off", TextureQuality
  "Low", WindowMode "BorderlessWindowed", EnableTelemetry false,
  EnableMarketing false.  `P8_SETTINGS_FIX=1` would apply exactly the
  differing P0 keys and log them.
- **The caches** are on ext2: `VKD3D_SHADER_CACHE_PATH=C:\vc`
  (= `/EXT/pfx/drive_c/vc`) and `MESA_SHADER_CACHE_DIR=/EXT/mc`.  DXVK 3.x has
  no state cache; `DXVK_LOG_PATH=none`.  Nothing reached them yet: both were
  empty after every boot, because the game never created a D3D12 device.
- **App id.** `SteamAppId=1091500` selects vkd3d-proton's per-game profile,
  as Proton does.
- **lavapipe feature level.** `VKD3D_FEATURE_LEVEL=12_0` is on by default
  (`P8_VKD3D_FL`).  Without it, vkd3d-proton on lavapipe offers FL 11_1 only
  (§5).
- **First-run screens are never answered.**  When OCR matches the user
  agreement, the CDPR account offer or the telemetry consent, `boot_p8.py`
  stops and keeps the screendump.  The only key it ever sends is one Space at
  the game's own title prompt "PRESS … TO CONTINUE" (`P8_PRESS_CONTINUE`,
  default on).  That prompt shows on every launch; it is not a first-run
  screen, and P0's walker pressed Space there too.  It stops at the main menu
  once three OCR passes find two of NEW GAME / LOAD GAME / CONTINUE /
  SETTINGS / CREDITS / QUIT GAME.  None of this triggered, because no game
  screen was ever drawn.
- **Warm-up.** `winep5.c`'s keepalive `vkcube.exe --idle` plus a `--winprobe`
  retry works around win32u's 10 s explorer wait.  The keepalive now gets a
  15 s head start (`P8_KEEPALIVE_HEAD_MS`), so only one wineserver starts on
  the ext2 prefix (stop 6).  The first winprobe got the X driver in 5 of 6
  game boots; in run 2 it took two tries (stop 5).

## 2. Boots, in order

| run | what changed | furthest point | stop |
|---|---|---|---|
| pfx-try1 | first prefix boot, the full P8 env (21 entries, 626 bytes) | `wine` itself, 1.5 s | stop 1 (env cap): `wine` SIGSEGV in libc `strlen(NULL)` (`__strlen_avx2`, libc+0x188915) before ntdll loads; wineboot status 11 |
| pfx-try2 | env cut to 13 entries / 398 bytes | prefix built, wineboot 42 s | harness bug: debugfs `mkdir` of an existing dir (`AppData/Local`) left 2 unconnected inodes (e2fsck pass 3); fixed in `seed-prefix.sh` |
| pfx | same | prefix built + seeded, e2fsck rc 0 twice | — |
| run1 | first game boot | game +~40 s: 96 modules | stop 2: `NtCreateFile Too many open files`, `Protocol error: process 015c: recvmsg: Too many open files`, wineserver NULL read (`grab_object`, object.c:540), game dead, `REDEngineErrorReporter.exe` spawned |
| run2 | + `p8fdcount` headroom census | game +34–45 s, status 5 | stop 2, plus `get_desktop_window failed to start explorer c000011f` (STATUS_TOO_MANY_OPENED_FILES), `DXVK: v3.1.1` starts, `Vulkan: vkGetInstanceProcAddr not found`, game jumps to NULL (PC 0); stop 5 during the warm-up |
| run3 | headroom every 3 s, cap 100 | game +30–33 s | stop 2; the game hits int3 (`Vector: 3 RIP: 0x1422d7b7b`, inside the exe); the wineserver crashes the same way |
| run4 | + the D3D12 caps probe before the game | game +26–36 s | stop 2 (the wineserver's recvmsg, then the crash); caps rows §5 |
| caps-fl120 | caps probe only, `VKD3D_FEATURE_LEVEL=12_0` | — | FL 12_0 device OK (§5) |
| run5 | the committed recipe, all defaults | warm-up, +10 s | **stop 6**: the guest wedged (`[YLOCK-WEDGE] site=ext2_lock`), the serial log silent for 300 s, the stall rule stopped it |
| run6 | + the keepalive starts 15 s before the first winprobe (stop 6 mitigation) | game +12–42 s | stop 2 again, with the final recipe; at the launcher's 300 s game deadline, `kill(game, SIGKILL)` + `waitpid` never returned (**stop 7**) and the run was stopped by hand |
| probe1, probe2 | `P8_PHASE=probe` (probe2 adds `p8killprobe`; its evidence supersedes probe1's) | — | §4 |

Wall times (guest clock, which tracked host wall time within a second here):
boot to game launch 24–39 s, launch to first game window **never**, launch to
the furthest point (the first EMFILE row) at most 42 s; each run brackets it
between two census rows: run 3 30–33 s, run 4 26–36 s, run 2 34–45 s, run 6
12–42 s, run 1 before 42 s.  The host timelines are in
the evidence files.

NFS was clean in every run: 0 no-reply rows, 0 hard retries, 0 GIVE UP, 0
NFLK rc=-1.  `e2fsck -fn` of the run image was rc 0 after every boot.

## 3. The stops, with evidence

### Stop 1 — a native spawner hands a compat process at most 512 bytes / 24 entries of env (fixed in the harness; kernel item for the owner)

- **Symptom:** pfx-try1.  `[KERNEL] pid=18 (/CP/WINE/bin/wine) memory fault
  VA=0x0 PC=0x1080190915 … sys=257`.  The PC is libc-2.31 + 0x188915
  (`__strlen_avx2`, `vpcmpeqb (%rdi)` with rdi = 0) in the `wine` loader
  before ntdll.so: 60 regions, no ntdll mapping.  `wineboot -i` status 11
  after 1.5 s.
- **Cause:** `HO_ENV_LEN` 512 (`process.h:238`, the per-process env blob that
  `spawn_ex` marshals) and `COMPAT_MAX_ENV` 24 (`program_loader.c:632`).  The
  first P8 env was 21 entries and 626 bytes; the entries past 512 bytes were
  dropped silently.
- **Probe:** `src/user/cp/p8envprobe.c` (run by `P8_PHASE=probe` and by the
  prefix boot).  Rows on base 949ba008: `E1 execve env: 40/40 entries intact
  -> PASS` (compat `execve` carries 1,560 bytes), but `E3 spawn_ex env: 13/40
  entries intact -> FAIL` (`E3 spawned40 sees envc=15 bytes=535`: 512 bytes
  plus the kernel's own `LD_LIBRARY_PATH=/LIB64`).  The Linux host passes
  E1–E4.
- **Fix (harness):** the launcher env is down to 13–14 entries and 398–423
  bytes.  WINESERVER and WINEDLLPATH are gone (Wine derives both from its
  bin dir), and so are LANG, DXVK_LOG_LEVEL and VKD3D_DEBUG; the cache paths
  are short.  The launcher prints `[P8] env entries=… bytes=…` every boot.
  The ~90 bytes left are thin: the next lane to add a variable should check
  that row.
- **Owner:** none this wave (process/exec).  The proposed diff
  (`p8-proposed-kernel.diff.txt`) sets `HO_ENV_LEN` to 2048 (one copy lives
  on the kernel stack at `trap.c:2466`) and `COMPAT_MAX_ENV` to 64.

### Stop 2 — the wineserver's fd table (256) fills during the game's start (C1-k5)

- **Rows (run1, the same in runs 2–5):**
  ```
  [X] 0144:err:winediag:NtCreateFile Too many open files, ulimit -n probably needs to be increased
  [X] Protocol error: process 015c: recvmsg: Too many open files
  [KERNEL] pid=21 (wineserver) memory fault VA=0x0000000000000000 PC=0x0000001000043D20 … sys=47
  [X] wineserver crashed, please enable coredumps (ulimit -c unlimited) and restart.
  ```
  `Protocol error: … recvmsg` is printed by the **wineserver**
  (`server/request.c:448`, `receive_fd`) when `recvmsg` on its own end of a
  client's socket fails.  The kernel answers EMFILE there only when the
  receiver's table cannot take the fd (`unix.c:576` stream / `:717` dgram:
  `grp->num_open_fds + want > MAX_OPEN_FDS`).  So the wineserver's own
  per-process table is what was full.  The wineserver opens every file the
  clients open (NtCreateFile → `open()` in the server), and it holds the
  server end of 3 pipes per client thread.  P0 measured 466–474 fds in the
  game process alone at the menu, with 90 threads.
- **Not the system-wide table:** `src/user/cp/p8fdcount.c` opens /dev/null
  until it fails, then closes them all.
  - With only the X session up, a fresh process gets 253 opens, which is
    its own 256 limit.
  - With the Wine session up, before the game, it gets 120–150 global slots,
    so the session already holds about 360–390 of the 512.
  - During the game it still got 100 of 100 at t=30 s, right before the
    EMFILE row at t=30–33 s (run3).
  So `MAX_GLOBAL_FILES` 512 is close but was not the first to run out.
  (HobbyOS answers EMFILE for both limits; `fs.c:630` should say ENFILE, as
  in the proposed diff.)
- **Owner: C1-k5.** It is the known ceiling, and C1-k5 is raising it to
  ≥ 4096 per process and ≥ 16384 system-wide.  After that lands, the next
  ceilings on the same path are poll nfds and epoll (the wineserver's main
  loop) and MAX_PROCESSES with 90+ game threads.

### Stop 2b — `recvmsg` into a full fd table fails instead of truncating, and that crashes the wineserver (C1-k5)

- Linux (`scm_detach_fds`) delivers the data, drops the fds that do not fit
  and sets `MSG_CTRUNC`.  Wine then sees `ret == sizeof(data)` with fd −1 and
  fails only that one request.  HobbyOS returns −1/EMFILE and leaves the
  message queued (the documented P4 IPC design, `p4-ipc-design.md` §4.2).
  Wine treats that as a protocol error and `kill_process`es the client.  The
  next datagram on that socket carries `tid 0` for a process with no threads
  left, and `grab_object(get_process_first_thread())` reads NULL.  PC
  0x1000043D20 = wineserver+0x43D20 = `grab_object` (`object.c:540`), called
  from `receive_fd`; that is the wineserver crash of runs 1, 3, 4 and 5.
- **Probe:** `src/user/cp/p8scmprobe.c` (the `P8_PHASE=probe` boot).  Linux
  host: `S1 recvmsg ret=4 -> PASS`, `S2 MSG_CTRUNC=1 fd_delivered=-1 ->
  PASS`, `S3 second recvmsg ret=-1 errno=11 -> PASS`.  HobbyOS 949ba008:
  `S1 recvmsg ret=-1 errno=24 -> FAIL`, `S2 MSG_CTRUNC=0 -> FAIL`, `S3 second
  recvmsg ret=4 -> FAIL` (the message was not consumed).
- **Proposed diff:** `p8-proposed-kernel.diff.txt`, the `unix.c` hunks.
  Deliver what fits, close the rest, set `ctrunc`; also set `ctrunc` when an
  install fails in the stream path.  The design note's "retryable EMFILE"
  rule would then hold only for native callers, if C1-k5 wants to keep it
  there.  The unit test `unix_test.c:452` tests `fs_msg_install_gfd` directly
  and is unaffected.

### Stop 3 (consequences of stop 2, not separate blockers)

- `get_desktop_window failed to start explorer c000011f` and
  `nodrv_CreateWindow` (runs 2 and 3).
- DXVK's dxgi could not reach `vulkan-1.dll` (`Vulkan: vkGetInstanceProcAddr
  not found`), then the game called NULL (run 2).
- The game's own int3 (run 3), and `REDEngineErrorReporter.exe` (run 1).

Each follows an EMFILE row, so none can be judged until stop 2 is gone.

### Stop 4 — lavapipe offers D3D12 feature level 11_1 (fixed in the harness for lavapipe)

See §5.  `D3D12CreateDevice(…, D3D_FEATURE_LEVEL_12_0)` fails with
`E_INVALIDARG` (TiledResourcesTier 1).  `VKD3D_FEATURE_LEVEL=12_0` makes
vkd3d-proton advertise 12_0, and the device then creates.  The recipe sets it
for lavapipe; Venus needs none of this (P0: FL 12_1 on the RX 9070 XT).
**Unverified:** whether the game itself asks for 12_0, and whether it uses
tiled resources that lavapipe cannot back.  SM 6.0 is the ceiling on lavapipe
(asked 6_7, got 0x60), and the wave lane count is 8.

### Stop 5 — intermittent: a Wine client loads `msvcrt.dll` with an empty import name (unowned; 1 of 9 Wine boots)

Run 2, the first warm-up `winprobe01`:
`err:module:import_dll Library  (which is needed by
L"C:\\windows\\system32\\msvcrt.dll") not found`, then `#GP` at
0x6ffffff990c0, status 5; the keepalive `vkcube.exe` faulted at the same
moment.  The second winprobe was fine.  An import-name string that reads as
empty means a page of an image read back as zeros.  NFS showed nothing (0
no-reply / retry rows).  This is the shape of the old NFS zero-fill (fixed
in C3-nfs2) but on a clean NFS log, so it points to the page cache or
mapping path.  It was not seen in the other 8 boots.  Ask the integrator for
an owner (vm / NFS page cache).  The same boot also logged
`trace:o-	:DllMain stack overflow 1792 bytes addr 0x6ffffffa665a`, with a
garbage debug-channel name, and `Unhandled page fault on read access to
FFFFFFFFFFFFFFFF at address 00006FFFFFF990C0`.  Something in that process
read corrupted memory.

### Stop 6 — intermittent: kernel wedge on `ext2_lock` while wineservers race on the ext2 prefix (C3-ext2w; mitigated in the harness)

Run 5, about 10 s after the keepalive and the first winprobe were spawned
together, three wineservers started (pids 19, 20, 22; two lost the race and
exited normally).  The guest then stopped printing anything, kernel dumps
included:
```
[YLOCK-STALL] site=ext2_lock lock=ext2_lock addr=0x7023957C pc=0x7 extra=0x100000 cpu=0
[YLOCK-WEDGE] site=ext2_lock lock=ext2_lock addr=0x7023957C pc=0x7 extra=0x1 cpu=0
[YLOCK-OWNER] ext2_lock pid=21 cpu=7 pcb_live=1 ost=3 osys=0
  [YLOCK] f->lock held=1 owner_cpu=0 nwait=0 waiters=-
  [YLOCK] f->lock held=1 owner_cpu=7 nwait=0 waiters=-
```
CPU 0 spins on `ext2_lock`, which pid 21 (the surviving wineserver, on CPU 7)
holds.  Each CPU also holds a file `f->lock`.  That looks like an ABBA
deadlock between `ext2_lock` and a per-file lock, reached when two or more
processes start on the same ext2 prefix at once.  Gate P3 (`pfx_gate`) only
ever starts one Wine at a time, which is why it never saw this.  Seen once
in 6 game boots and 0 of 3 prefix boots.  **Owner: C3-ext2w** (ext2 and its
locking this wave): the `[YLOCK]` rows name the lock sites.  A probe should
run two compat processes that hammer `open`/`fstat`/`fcntl(F_SETLK)`/`chdir`
on one ext2 directory at once; this lane did not write it (it needs the
lock-order reading from the ext2 owner).  Mitigation in `p8launch.c`: the
keepalive gets a 15 s head start, so the first wineserver comes up alone.
Run 6 passed the warm-up with it.

### Stop 7 — SIGKILL to a thread group whose leader thread has exited does nothing (unowned: process/signals)

After stop 2 the game's main thread is gone (`[IDLESTUCK] slot=123 pid=123
st=9` = `PROC_STATE_THREAD_DONE`), while its other threads stay BLOCKED on
the dead wineserver's pipes.  The game never exits.  In run 6 the launcher's
`kill(pid, 9)` followed by `waitpid(pid, …, 0)` hung for over 5 minutes;
runs 1–4 show the same st=9 leader for as long as they ran.  On Linux the
leader is a zombie, `kill(tgid, SIGKILL)` takes down every thread and the
parent reaps the group.
- **Probe:** `src/user/cp/p8killprobe.c`.  The child's main thread
  `pthread_exit`s while a second thread blocks in `read`; the parent sends
  SIGKILL and polls `waitpid(WNOHANG)` for 10 s.  Linux host: `kill rc=0; K1
  reaped=1 after 100 ms -> PASS`, `K2 … sig=9 -> PASS`.  HobbyOS 949ba008:
  `kill rc=-1; K1 reaped=0 after 10000 ms -> FAIL`, `K2 -> FAIL`, and the
  child's thread (pid 26) is still alive at the end of the boot.
- **Harness:** the launcher now SIGKILLs and polls for 30 s
  (`kill_reap`), says `[P8] pid N not reaped 30 s after SIGKILL (stop 7)`,
  and goes on to `wineserver -k`, `sync` and `[P8] done`.  The stall rule
  counts only launcher/Wine rows at line start; spliced kernel rows used to
  keep it from firing.
- **Owner:** none this wave (`process_signal` for a group whose leader is
  THREAD_DONE); integrator to assign.  It will matter for any Wine program
  that `ExitThread`s its main thread.

### Not blockers (yet)

- `ENOSYS nr=221` (`fadvise64`) from the wineserver, once per boot; Linux
  returns 0.
- `secur32: no schannel support` / `no Kerberos support`: the game's
  networking DLLs; harmless offline.
- `ZwLoadDriver … winebth c00000e5`; `winemenubuilder … (126)` (disabled on
  purpose).
- RAM: not reached.  The game died at about 13 threads.  §0.2 item 6 (the
  5.25 GiB x64 frame pool, whatever `-m` says) is still ahead.  `sysinfo(2)`
  counts legacy 32 MiB blocks and does not see v2-AS memory (it read
  11520/11520 MiB free with the game up), so the launcher no longer prints it.

## 4. Probes (lane-owned: `src/user/cp/p8*`)

| probe | what | Linux host | HobbyOS 949ba008 |
|---|---|---|---|
| `p8envprobe.c` E1/E2 | compat `execve` with 40 entries / 1,560 bytes | PASS | PASS |
| `p8envprobe.c` E3/E4 | native `spawn_ex` → compat with the same env | — (no native ABI) | **E3 FAIL** 13/40 (535 bytes) |
| `p8scmprobe.c` S1–S3 | SCM_RIGHTS into a full fd table | PASS ×3 | **FAIL ×3** |
| `p8fdcount.c` | headroom (not pass/fail) | — | 253 with X only; 120–150 with the Wine session |
| `p8killprobe.c` K1/K2 | SIGKILL a group whose leader exited, then reap it | PASS ×2 | **FAIL ×2** (kill rc −1, never reaped) |
| `p8d3d12caps.c` | the D3D12 stack's caps under Wine | — | §5 |

All of them are built by `run-p8.sh` (musl static-pie, or the P5 PE recipe)
and staged on the copy of `disk.img`; none is in the Makefile.

## 5. What the D3D12 stack offers the game (`p8d3d12caps.exe`, run4 and caps-fl120)

| | default | `VKD3D_FEATURE_LEVEL=12_0` |
|---|---|---|
| modules | `C:\windows\system32\{d3d12,d3d12core,dxgi}.dll` (the seeded vkd3d-proton 3.1 / DXVK 3.1.1) | same |
| adapter | "llvmpipe (LLVM 21.1.8, 256 bits)", vendor 0x10005, 11520 MiB | same |
| CreateDevice FL 12_0 | **0x80070057** | 0x00000000 |
| max feature level | 0xb100 (11_1) | 0xc000 (12_0) |
| shader model | 6.0 | 6.0 |
| binding / tiled / heap tier | 3 / 1 / 2 | 3 / 2 / 2 |
| wave ops, lanes | yes, 8..8 | same |
| raytracing / mesh / sampler feedback | 1.1 / 0 / 0.9 | same |

CreateDevice takes 273–525 ms.

## 6. Ranked blockers (C8-game's gate list)

| # | blocker | evidence | owner |
|---|---|---|---|
| 1 | `MAX_OPEN_FDS` 256: the wineserver's table fills in the game's first ≤ 42 s | stop 2; every game boot past the warm-up (runs 1–4, 6); `p8fdcount` headroom | **C1-k5** (in progress) |
| 2 | `recvmsg` SCM_RIGHTS into a full table → EMFILE instead of MSG_CTRUNC; Wine kills the client and crashes in `grab_object` | stop 2b; `p8scmprobe` S1–S3 FAIL (Linux PASS); proposed `unix.c` diff | **C1-k5** (fd install path) |
| 3 | `MAX_GLOBAL_FILES` 512: the Wine session alone holds ~360–390 before the game; a 466-fd game will not fit | stop 2 headroom rows; P0 fd counts | **C1-k5** (≥ 16384) |
| 4 | native `spawn_ex` env cap (512 bytes / 24 entries), dropped silently | stop 1; `p8envprobe` E3 FAIL; proposed diff | unowned: integrator to assign (process/exec) |
| 5 | intermittent kernel wedge: `ext2_lock` vs a file lock with concurrent wineserver starts on the ext2 prefix | stop 6; 1/6 game boots; harness mitigation in place | **C3-ext2w** |
| 6 | intermittent corrupted read in a Wine client (`msvcrt.dll` empty import name, garbage debug channel) | stop 5; 1/9 Wine boots | unowned: integrator (vm / page cache) |
| 7 | SIGKILL of a group whose leader thread exited: kill −1, never reaped | stop 7; `p8killprobe` K1/K2 FAIL (Linux PASS) | unowned: integrator (process/signals) |
| 8 | lavapipe FL 11_1, SM 6.0 | §5; harness override in place | C8 (done for lavapipe); Venus (C6) removes it |
| 9 | `file_alloc` failure answers EMFILE, not ENFILE | `fs.c:630`; proposed diff | C1-k5 |
| 10 | `fadvise64` (221) ENOSYS | one row per boot | trivial, any kernel lane |

Furthest screendump: the X desktop with no game window
(`evidence/cp-game/run2-final.png`, taken after the game had died: IceWM,
xeyes and the clock).  Every shot after launch was identical to the
pre-launch baseline; OCR finds only `xeyes`.

## 7. Next run, once C1-k5 lands

`P8_PHASE=probe` first: `p8fdcount` should report ≫ 253 and S1–S3 should
pass if the `unix.c` change went in.  Then a game boot with the same command.
The stop rules (first-run screens, menu OCR, stall) are already armed for
the first game window.  Raise `P8_GAME_S`/`TIMEOUT_S` once the game gets
past 60 s: shader compilation on lavapipe will be slow and silent (the stall
rule counts only launcher and Wine rows).
