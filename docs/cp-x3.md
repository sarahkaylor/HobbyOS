# cp/x3 — MIT-SHM, the tree-built one-screen server, Wine fullscreen (lane C4-x3, wave 5)

Plan: `docs/cyberpunk.md` §7.2e (C4-x3 row), §6 P4/P7.  Groundwork:
`docs/cp-x11.md` §3.1 items 2–4, `docs/cp-x2.md` §3 (one screen, RandR) and
§6 (the MIT-SHM plan this lane carried out).  Every number below is from
**strix** (Ryzen AI MAX+ 395, Zen 5, KVM, up to four guests at once).
Evidence: `docs/cp-x3-evidence/`; screendumps in `evidence/cp-x3/`
(git-ignored).

| What | Where |
|---|---|
| SysV shm (compat + native) | `src/kernel/linux_compat_shm.c` |
| dispatch hunks | `src/kernel/linux_compat.c` (2 lines), `src/include/syscall.h` (`SYS_SHM` 106), `src/kernel/arch/{x64,arm}/trap.c` (one arm each) |
| probe | `src/user/linux_probe_shm.c` (M1–M10) |
| server MIT-SHM | `third_party/x11/build-server.sh` (`-Dmitshm=true`), `patches/xserver/0004-shm-hobbyos-access.patch`, `ddx/include/sys/{ipc,shm}.h`, `ddx/compat/hbxcompat.c` (wrappers), `ddx/hobbyos/hbscreen.c` (`ShmRegisterFbFuncs`) |
| the tree builds its own server | `Makefile` `x11-staged` (`X11_SERVER_INPUTS`) |
| X probe (no Wine) | `tools/cp/p4/xprobe.c` X5 + `xprobe.py` |
| Wine fullscreen probe | `src/user/cp/fullscr.c`, built by `tools/cp/p5/build-pe.sh`, run by `winep5` tag `fullscr`, graded by `boot_wine_p5.py` |

## 1. The one-screen server is the tree's own

A fresh `make ARCH=intel MODE=desktop DESKTOP=icewm x11-staged` of `cyberpunk`
@ 949ba008 on strix (empty `third_party/x11/build/intel`, tarballs from
`~/cp/lanes/k4/third_party/x11/dl` + `~/cp/x11-dl-native`, each checked
against `third_party/x11/sums`) produces **exactly**
`935c760fd450eb9a3326e75691a9b8feaacf155f19033e807f98a4bf78424c97`, C4-x2's
one-screen `XHOBBY.BIN`.  The DDX source was already in the tree (91d37d65,
46ebf634); what was missing was the rebuild.  `x11-staged` accepted any build
dir that already had `bin/XHOBBY.BIN`, so a dir first built before the DDX
change kept its old server for ever (strix: `lanes/vk2` still holds
`4a2edb23…`, the two-screen server; `lanes/k4`/`k4b` happened to be built
after it and hold `935c760f…`, which is why P4 passed with no override).

The Makefile fix (its own commit): in the default build dir, if any of
`X11_SERVER_INPUTS` (`third_party/x11/ddx`, `patches/xserver`,
`build-server.sh`, `build.sh`, `cross/`, `pins/server.txt`,
`obj/<arch>/libc.a`) is newer than `build/<arch>/.server-inputs-checked`, the
recipe re-runs `build.sh --stage server`, whose content key (DDX hash,
patches, meson args, runtime) makes it a ~4 s no-op when nothing changed.  An
explicit `X11_BUILD_DIR` is still used as given.  `run-p4.sh` and
`run-wine-p5.sh` now print the sha256 of the `::/XHOBBY.BIN` they boot;
`P5_XHOBBY` stays as an optional override but no gate needs it.

With this lane's MIT-SHM the tree builds
`9b161eded42374e06415d4033a35763aff8f6a061d8f34b5a0eebff95cbafa07`.

## 2. System V shared memory (`linux_compat_shm.c`)

One kernel segment table, two ABIs:

| ABI | Numbers |
|---|---|
| compat (x86_64 Linux) | shmget 29, shmat 30, shmctl 31, shmdt 67 — `l_tab_shm`, two lines in `linux_compat.c` |
| native | `SYS_SHM` 106 `(op, a1, a2, a3)`: 0 get(key, size, flg), 1 at(id, addr, flg), 2 dt(addr), 3 ctl(id, cmd, buf) |

- A segment is a `VM_OBJ_MEMFD` object (`vm_object_create`), the memfd page
  cache object; an attach is a `VMK_SHARED`/`VM_MAP_SHARED` region over it
  placed with `vm_region_alloc_hole` in the mmap window — exactly the region
  `mmap(MAP_SHARED)` of a memfd installs.  `shmdt` is `vm_munmap_range` over
  the attach.  Only `vm.c`'s public API is used; `vm.c` is untouched.
- `shm_nattch` = object references − 1 (the table's).  Region records hold
  their own reference, so fork, exit and munmap keep it right with no hook.
- `IPC_RMID` clears the key at once (no `shmget` finds it) and the segment is
  reclaimed when nattch reaches 0; until then `shmat` on the id still works,
  as on Linux.  Reclaim runs at every shm call and at RMID/shmdt.
- `struct shmid64_ds` is written byte-wise in x86_64 Linux's 112-byte layout
  (also the native layout, `ddx/include/sys/shm.h`).
- One identity (uid 0): no permission refusals, like root.
- Not modelled: `SHM_REMAP` over a live mapping (EINVAL), `IPC_INFO`,
  `SHM_INFO`, `SHM_STAT` (EINVAL), `SHM_LOCK`/`UNLOCK` (accepted no-ops).
- Table: 256 segments; ids `(seq << 8) | slot`, a stale id is EINVAL.

`linux_probe_shm` (musl static-PIE; same ELF on the strix host):

| Row | Checks |
|---|---|
| M1 | `shmget(IPC_PRIVATE, 1 MiB, IPC_CREAT\|0600)` |
| M2 | `shmat`, 1 MiB pattern reads back |
| M3 | `IPC_STAT`: segsz, mode, nattch 1, cpid |
| M4 | a forked child attaches the same id, both see each other's bytes |
| M5 | nattch 3 → 2 → 1 across the child's attach, detach and exit |
| M6 | `IPC_RMID` while attached (winex11's order), re-attach works, gone after the last detach |
| M7 | keyed: EXCL/EEXIST, lookup, EINVAL for a bigger size, ENOENT, `IPC_SET` |
| M8 | EINVAL for `shmdt` of a non-shm address and `shmat` of a bad id |
| M9 | `SHM_RDONLY` write → SIGSEGV |
| M10 | 40 × 16 MiB create/attach/touch/RMID/detach: free memory unchanged |

base5: `FAIL M1 ... errno=38` (`[LCOMPAT] ENOSYS nr=29`), rc=10.  Lane: 10/10
in the guest; 10/10 on the strix host kernel.

**Kernel fact (for C1-k5):** after `waitpid` returns, a child's address
space can still be alive for ~50 ms in the guest (M5 measured 50 ms; Linux
0): `process.c` never frees an AS another CPU still translates through, and
retries later.  The probe allows 5 s and prints the time.

## 3. MIT-SHM in XHOBBY.BIN

- `-Dmitshm=true`.  The server is a native process, so `<sys/ipc.h>` and
  `<sys/shm.h>` come from the server-stage overlay `ddx/include/sys/`, and
  `shmget/shmat/shmdt/shmctl` are weak wrappers in `ddx/compat/hbxcompat.c`
  over `SYS_SHM` (the repo libc is untouched).
- `patches/xserver/0004`: `shm_access()` returns 0 on HobbyOS.  There are no
  peer credentials on local sockets (`NO_LOCAL_CLIENT_CRED`), so the stock
  code falls through to the "other" mode bits and refuses winex11's
  `IPC_CREAT|0700` segments with BadAccess.  Every client is uid 0 here.
- The DDX calls `ShmRegisterFbFuncs`, so `sharedPixmaps=1` and
  `XShmCreatePixmap` works.
- **Version 1.1, no fd passing.**  The server is built without
  `XTRANS_SEND_FDS` (patch 0001: xtrans's 528-byte `SCM_RIGHTS` buffer is
  larger than the kernel's `K_IPC_CTRL_MAX` 512), and
  `protocol-versions.h` then reports 1.1.  That is what keeps Mesa's X11 WSI
  (lavapipe, P5) on its PutImage path: Mesa only uses shm at ≥ 1.2, through
  `xcb_shm_attach_fd`.  SHM 1.2 needs (a) `K_IPC_CTRL_MAX` ≥ 528 or Linux's
  `MSG_CTRUNC` truncation in `fs.c`, (b) `SCM_RIGHTS` from a compat process
  to a native one, (c) mmap of a received memfd in XHOBBY.  Not done here
  (kernel files outside this lane).

xprobe X5 (in-guest, `docs/cp-x3-evidence/xprobe-shm.txt`): the extension
list (xdpyinfo's view) has MIT-SHM; version 1.1, sharedPixmaps=1; winex11's
`create_shm_image` sequence; XShmPutImage → XShmGetImage and XGetImage of a
pixmap, XShmCreatePixmap readback, a mapped-window readback, and a
screendump of the 256x192 pattern: every pixel matches.  The strix host
Xvfb gives the same rows at version 1.2.

## 4. RandR and Wine fullscreen

C4-x2's RandR (12 modes at 1920x1080, real `RRSetScreenSize` +
`RRSetCrtcConfig`, the mode shown 1:1 top-left with black around) already
covers 1280x720 and 1920x1080.  Wine 11's winex11 drives it through its
**XRandR 1.4** display-settings handler (`xrandr14_set_current_mode`); it
does not use XF86VidMode (Wine is built without libXxf86vm).  The restore on
exit is Wine's own: `ChangeDisplaySettingsEx(..., CDS_FULLSCREEN)` registers
the process with explorer's display-settings restorer
(`programs/explorer/desktop.c:711`), which resets the registry mode when it
exits.

`fullscr.exe run`: CDS_FULLSCREEN 1280x720 → a WS_POPUP TOPMOST window with
corner markers and colour bars → 1920x1080 → 1280x720 → exit without
restoring; `fullscr.exe check` then waits for the desktop mode.  Every GDI
paint of that window goes through winex11's window surface, i.e. XShmPutImage
(kernel rows: `fullscr.exe` shmget 0x3C0000 = 1280x720x4 and 0x870000 =
1920x1080x4, `XHOBBY.BIN` shmat of each id).

**IceWM fix (patches/icewm/0005).**  Run 1–3: M3 and M4 were exact, but in
M5 (shrinking while already fullscreen) winex11 asked for 1280x720 and IceWM
answered ConfigureNotify 1920x1080 (+event,+x11drv trace,
`fullscr-fs4-PASS.txt`).  IceWM re-read its screen list only on
RRScreenChangeNotify, which the size request raises while Wine's CRTC is
still off, and it never refitted existing fullscreen frames.  The patch
re-reads the screens on `RRNotify` CrtcChange too and calls `updateLayout()`
on every fullscreen frame after a screen change.  fs4: M3/M4/M5 exact (rect
settled in ~100 ms), screendumps right, desktop back 1.9 s after exit.  A game
that changes resolution from its own settings while fullscreen is exactly M5.

## 5. Results (strix KVM; lane = cp-x3, base = cyberpunk 949ba008 / C0-sync 15ad4a35)

| Row | Lane | Base | Evidence |
|---|---|---|---|
| fresh tree build of the server = C4-x2's one-screen server | `935c760f…` byte-identical (before MIT-SHM); with MIT-SHM `9b161ede…` | (base5 tree: never builds it; a stale dir keeps its old server) | §1 |
| `linux_probe_shm` | 10/10 (guest), 10/10 (strix host) | FAIL M1 ENOSYS | `probe-shm.txt` |
| xprobe X5: MIT-SHM listed, Put/GetImage round trip, ShmCreatePixmap, window readback, screendump | PASS (0/1456 sampled pixels differ) | MIT-SHM absent | `xprobe-shm.txt` |
| winex11 uses XShm | every Wine GUI process: wineboot, rawinput, winecfg, notepad, vkcube, d3d11tri, d3d12tri, fullscr (`[SHM] get` by the Wine process + `XHOBBY.BIN` shmat of the same id) | — | `p4-run1-PASS.txt`, `p5-run1.txt` |
| Gate P4, tree-built server, no override | PASS G1–G3 (run 1 and run 2) | PASS (k4b) | `p4-run*-PASS.txt` |
| Gate P5 rows 2–4, tree-built server, no override | PASS vkcube/d3d11/d3d12 (run 1) | PASS (run 7) | `p5-run1.txt` |
| Wine fullscreen 1280x720 / 1920x1080 / 1280x720, restore on exit | PASS (fs4, with IceWM 0005) | — | `fullscr-fs4-PASS.txt` |
| full batch (pre-merge) | parity; exitgroup X7 failed once under load (worst 2510 ms), 2/2 PASS on rerun | base5 | `gates-premerge.txt` |

Post-merge runs: see `gates-postmerge.txt`, `p4-postmerge-*.txt`,
`p5-postmerge-*.txt`.

**Wine start-up faults are not this lane's** (`wine-faults-ab.txt`).  Single
faults in a Wine process at start-up (explorer/rundll32/vkcube), all inside
Wine's PE loader (`find_existing_module` loader.c:2789 with a NULL NT header,
`LdrProcessRelocationBlock` loader.c:3614 writing a read-only page,
`import_dll` loader.c:1166), appear in lane and base sessions alike (base
A/B on the base5 kernel + 935c server: 2 of 3 sessions), always before that
process made any shm call.  One of them (P5 run 2) killed explorer and left
the session on the null driver (VOID).  For C1-k5: the loader is seeing image
mappings whose bytes are not the file's.

## 6. Open items

- MIT-SHM rows are 1.1; `[SHM]` kernel rows are capped at 64 per boot.
- After `waitpid`, a child's address space can live ~50 ms more in the guest
  (deferred AS release), so `shm_nattch` lags Linux by that much.

- SHM 1.2 (fd passing) and DRI3: kernel work, see §3.
- The QEMU scanout stays 1920x1080 in a 1280x720 mode (the DDX shows the
  mode top-left, black around).  A real scanout mode change is virtio-gpu
  `SET_SCANOUT` territory (C6/P7, F5).
- `SYS_SHM` is a new native row (106, both arches); native rows are
  integrator-frozen — this needs sign-off.
