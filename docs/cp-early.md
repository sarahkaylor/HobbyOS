# cp-early: the REDEngineErrorReporter early exit (lane H2-early, wave 8)

Branch `cp-early` from integ15 @ a23bfbeb.  Evidence: `docs/cp-early-evidence/`.
Every number here is from strix (Zen 5, `ssh strix`), intel guests, KVM `-cpu host`,
8 vCPUs, 12 GiB.

## 1. The bug

Sometimes `wine Cyberpunk2077.exe --launcher-skip` exited with status 0 about
2.5-3 s after launch (the 20 s variant came from a slower boot), just after the game
started `REDEngineErrorReporter.exe <pid>`.  The menu never appeared.  P8's
grader recorded this as `launcher-done`, rc 0.

Occurrences in the saved serials on strix, de-duplicated:

| boot | gpu | launch -> exit | evidence |
|---|---|---|---|
| integ13 lvp1 | lavapipe | 2579 ms | `~/cp/integ13/p8-lvp.1.serial` |
| integ12 batch lvp2 (dir integ15) | lavapipe | 2886 ms | `~/cp/integ15/p8-lvp.2.serial` (game at line ~1474) |
| integ10-venus | Venus | 20049 ms | `~/cp/integ/obj/cp-p8/integ10-venus/serial.txt` |
| gateA3 (P9) | Venus | 20362 ms | `~/cp/lanes/play/obj/cp-p9/gateA3/serial.txt` |
| console-lvp1 | lavapipe | 23606 ms | `~/cp/lanes/console/obj/cp-p8/console-lvp1/serial.txt` |
| k7l-1 (P9) | Venus | 2467 ms | timeline only (`~/cp/lanes/k7-ev/`) |

In the census of every game-boot serial on strix, 7 of about 150 boots show this
exit.  Each is the boot's first (and only) launch.  None of them loaded steam_api64.

## 2. Root cause: the NFS directory-listing cache was shared without a lock

`src/kernel/nfs.c` keeps one directory listing (`static struct dir_cache dcache`:
path, count, names[4096]) for every lister on every CPU.  Neither
`nfs_list_dir()` nor `nfs_dir_entries()` nor getdents64's loop in `fs.c` took
a lock.  Two listers of different directories therefore interleaved:

- one tested "fresh" (`valid && path == X`) while the other refilled the cache
  with Y, so it read Y's names;
- both found it stale and filled at once, appending into the same `names[]`.

The result was listings of 29, 42, 49 or 62 entries, made of both directories.

The trigger: REDEngineErrorReporter is a console program, so it starts
`conhost.exe`.  conhost's user32/gdi32 initialisation lists Wine's fonts directory,
`/CP/WINE/share/wine/fonts` (56 entries), over NFS.  It does this at the moment the game lists
its own configuration directories (`/GAME/engine/config/base`, `.../platform/pc`,
`/GAME/r6/config`).  The kernel's one `[GDT]` row per listing shows the swap
(`docs/cp-early-evidence/rootcause-gdt-strix.txt`):

```
[GDT] rel=/engine/config/base total=56 path=/GAME/engine/config/base    (right: 7)
[GDT] rel=/r6/config total=56 path=/GAME/r6/config                      (right: 6)
[GDT] rel=/r6/config total=42 path=/GAME/r6/config
[KERNEL] Process 126: Cyberpunk2077.e exited unexpectedly or gracefully.
[P8] end game status=0 ms=2886
```

All three early-exit serials that have `[GDT]` rows show wrong counts for the
game's config directories.  integ10-venus got 58 (the `bin/x64` count) for
`engine/config/base` and `r6/config`.  Every good boot lists base 7,
platform/pc 2, engine/config 4, r6/config 6 and fonts 56.  The game, with no
config files in its directories, ends with status 0.  It does not crash and
writes no report (section 5).

This explains each observed property:
- **Timing dependent:** it needs the font scan and the config scan to overlap.
- **Right after the reporter:** the reporter is what starts conhost.
- **Seen on both lavapipe and Venus:** the bug is in the NFS client, not the GPU path.
- **Only first launches:** the repeat launches below never overlapped (0/56).
- **No `err:` line and no kernel fault:** nothing faults; a getdents call returns the wrong names.

## 3. The fix

- `src/kernel/nfs.c`: `dcache_lock` (ylock, registered "nfs_dir_lock") guards
  the cache.  `nfs_list_dir()` and `nfs_dir_entries()` take it themselves.
  New `nfs_list_dir_locked()` and `nfs_dir_entries_locked()` serve a caller that
  already holds `nfs_dir_lock()`.  `dir_cache_invalidate_all()` (umount) takes it.
- `src/kernel/fs.c` getdents64, NFS branch: holds `nfs_dir_lock()` from the count to
  the last packed entry, so the total and the names come from one fill.  It is
  released on each of the three exits.
- `src/include/nfs.h`: the prototypes and the locking rule.

**Lock ordering.**
- `nfs_dir_lock` is the outermost lock on this path.
- getdents64's NFS branch holds no other fs lock: `f->lock` is taken only by the
  tmpfs branch, and the global file table is only read.
- The trap entry (`sys_getdents64`) and `vfs_read_dir()` hold nothing.
- Under the lock the fill takes, briefly and in turn: the NFS attribute-cache
  spinlock (`nfs_ac_lock`) in `nfs_lookup`; `net_lock`, the mount's socket pool and
  the RPC path; and the console lock in `kl_emit`.  The writes of dirents into the
  user buffer may page-fault (`vm_lock`).  Each of these is acquired and released
  inside the critical section.  None of those paths takes `nfs_dir_lock`, so no
  cycle is possible.
- Nothing re-enters the cache under the lock: `nfs_lookup` and the RPC code never list a
  directory.  The lock is not recursive, and no path needs it to be.

**Blocking under the lock.** The section blocks: READDIRPLUS RPCs and their
hard-mount retries.  That is the documented case for a ylock (`lock.h`:
"process, does I/O/blocks -> ylock_t"), like `fat_lock` across virtio-blk I/O and
`ext2_lock`.  No spinlock is ever held when `nfs_dir_lock` is taken.  In this
kernel a ylock waiter does not park; `lockscale.c` explains why.  It spins
preemptibly with interrupts on.  Under the guard's 6-lister storm this gave one
report-only `[YLOCK-STALL] site=nfs_dir_lock` row.  The game has at most 2-3
listers at a time.

The cache stays one directory deep.  Two concurrent listers of different
directories now take turns: correct, with at most one refill per getdents call.

## 4. Tests

**rsize_guard G11 (new).** The bench phase `dirs6`
(`src/user/cpnfsbench.c`, `tools/cp/nfs/nfs_bench.sh`) runs 6 forked listers:
the even ones list `/NFS/dA` (a00..a39), the odd ones `/NFS/dB` (b00..b49).
Each lister lists its directory 40 times at once with the others, and every
listing must hold exactly its own names.
`docs/cp-early-evidence/nfs-guard-strix.txt` has both runs:

- pre-fix kernel: G11 FAIL, 6/6 listers wrong, e.g. `dirs6.child k=0 dir=dA
  listings=40 wrong=40 its_count=173 first_bad_name=b00`;
- fixed kernel: G11 PASS.  G1-G10 and `nfs_rsize_scan` are unchanged (32 KiB READs).

**Graders.** `tools/cp/p8/test_p8grade.py` failed 6/8 on the pre-H2 rule
(section 6).

**Game boots.** See section 7.

## 5. What the game itself says

Nothing.  A debugfs walk of the early-exit boot's data image (integ15 lvp2,
everything with an mtime in the boot) found the registry files Wine rewrote at
exit and one new, empty directory,
`drive_c/users/root/AppData/Local/REDEngine/ReportQueue`.  A good boot creates the
same empty directory.  There is no crash dump, no `*.log` and no report under
`AppData/Local/CD Projekt Red/Cyberpunk 2077/`.  `user.reg` was byte-identical
to the seeded one.

## 6. Graders (commit 351b0d49)

`tools/cp/p8/p8grade.py` decides the game-phase verdicts for `boot_p8.py` and
`boot_p9.py`.  Its CLI replays a saved serial, with an optional timeline.

| verdict | when | rc |
|---|---|---|
| `early-exit` | `[P8] end game status=<n>` before the menu.  The detail gives the status, the ms, `REDEngineErrorReporter ran: yes/no` and whether steam_api64 loaded | 3 |
| `game-timeout` | P8_GAME_S ended the game before the menu | 1 |
| `launcher-ended-before-game` | `[P8] done` with no `[P8] begin game` (e.g. a mount failed) | 1 |
| `launcher-done` | pfx/probe phases only | 0 |
| `repro` | a `-DP8_LAUNCHES=N` boot; the detail is early=k n=N, per launch | 0 |

P9's `game-exited-before-menu` became `early-exit`.  verdict.txt line 2 and
run-p8.sh's `== verdict detail:` carry the detail.

## 7. Repro attempts and k/n

`-DP8_LAUNCHES=N` (p8launch.c, commit f0ca9786) launches the game N times in one
boot.  A launch counts as survived once the game is alive 10 s after
REDEngineErrorReporter appears.

| condition | early exits |
|---|---|
| saved boots, first launch, before the fix (strix census) | 7 / ~150 (5 %) |
| integ batch P8 lavapipe boots, before the fix | 2 / 9 |
| r1: 12 fresh-session launches, lavapipe, before the fix | 0 / 12 |
| r2: 40 kept-session launches, lavapipe, relay tracing, before the fix | 0 / 40 |
| c-1, c-2: cold boots (2 launches each), diagnostic Wine, before the fix | 0 / 4 |
| after the fix: P8 boots, one launch each (f1-f6 + the gate batch's lvp1, lvp2, venus) | **0 / 9** (lavapipe 0/6, Venus 0/3), every config listing right |

The loop did not raise the rate.  Repeat launches find the game's config listings
and the fonts listing at different moments, so they rarely overlap.  With the root
cause known, the dirs6 phase reproduces the race 6/6 in one guard boot, which is
far more than any game-launch loop achieved.

## 8. Side finding H2-F2 (passed to H3-winefix)

In r1's fresh-session loop, 7 of 12 `wineserver -k` calls ran 10.5 s
(`kill_lock_owner`'s 20 polls): the session's wineserver ignored SIGINT, was
SIGKILLed, and left its 8 clients (services, winedevice x2, plugplay, svchost,
explorer, rpcss, vkcube) blocked.  The other 5 calls took 0.2-0.6 s.
`docs/cp-early-evidence/repro-loop-strix.txt`.  The repro loop now sweeps these
leftovers.

## 9. cp-hunt items in H2's files

1. **"Game ended early" graded rc 0** (`boot_p8.py` ~287).  351b0d49 already
   covers this: the game phase never grades `launcher-done`, and an early exit is
   `early-exit`, rc 3.  H1's `game-exited-early` diff is not needed.
2. **H1-08, P9 menus at a 1920x1080 game resolution.**
   - `boot_p9.py --game-res` (`P9_GAME_RES` in run-p9.sh).
   - `game_view()` scales a 1920x1080 game's shot to the 1280x720 layout that
     `MENUS`, the save-list search and the prompt/menu OCR crops were measured
     on.  `canon_image()` and `focus_in()` are module-level and covered by
     `tools/cp/p9/test_p9canon.py`, which uses synthetic frames.
   - Not yet checked on a real 1080p boot: the 1.5x UI scale is an assumption.
3. **H1-04, stale prefix stamp.**  run-p8.sh compares `/pfx/.update-timestamp`
   with the stage's `wine.inf` mtime and prints `== WARNING: prefix stamp ...`.
   The integ15 seeded image (wine-ntsync, 1794844150) does not match wine-cp
   (1794987459).
4. **H1-03, the pre-launch stall.**
   - `P8_DRIVE_CACHE` sets a QEMU `cache=` mode on the per-run `ovmf.fd`,
     `disk.img` and `cpdata.img` copies.  boot_p8 sets it on the lavapipe
     drives and passes it to `host-venus.sh` as `HV_DRIVE_CACHE`.
   - The game phase now defaults to `unsafe`; `P8_DRIVE_CACHE=` (empty) gives
     QEMU's default.  The pfx and probe phases keep QEMU's default, because the
     pfx image becomes the seeded original.
   - A/B on a busy strix, 4+4 interleaved caps-only boots, QEMU start to the
     launcher's NFS mount: default 35-49 s (median 39.5), unsafe 3-9 s
     (median 5.8).
   - e2fsck of the data disk after each of the 8 boots: rc 0.
   - Evidence: `docs/cp-early-evidence/hunt-items-strix.txt`.
