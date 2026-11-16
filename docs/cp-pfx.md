# cp/pfx — the storage half of Gate P3 (lane C3-pfx, wave 4)

Plan: `docs/cyberpunk.md` §6 P3, §7.2d (C3-pfx row).  Builds on
`docs/cp-ext2.md`, `docs/cp-ext2b.md` (the ext2 driver, mmap, locks) and
`docs/cp-wine2.md` (the P2 harness).  Every number below was measured on
**strix** (Ryzen AI MAX+ 395, Zen 5, AVX-512, KVM, 12 GiB guest) unless it
says otherwise.

## 1. The Wine prefix on ext2 survives a reboot

### 1.1 Harness: `P2_PFX=1 bash tools/cp/run-wine-p2.sh`

Two boots of one fresh `tools/cp/mkcpdata.sh` image (8 GiB sparse, NVMe
namespace 2, mounted at `/EXT`), `WINEPREFIX=/EXT/pfx`, `/tmp` still the
compat tmpfs (the wineserver socket dir `/tmp/.wine-0/server-707-<ino>`):

| boot | launcher (`src/user/winep2.c`, `-DP2_PFX_PHASE=n`) |
|---|---|
| 1 | `wineboot -i`; wait until the whole tree, wineserver included, has exited by itself (it saves the hives as it goes); `pfxls`; `sync(2)` (`tools/cp/pfx/pfxsync.c`, a compat process: the native ABI has no sync); `[P2] done`; the kernel halts.  Nothing is killed. |
| 2 | `pfxls`; `wine cmd /c echo hello`; wait (at most 600 s, §1.6) for the fresh wineserver's `wineboot --init` to exit; `winprobe.exe` from `Z:`; `cmd /c copy` it to `C:\` (the ext2 prefix) and run it from there (a PE image mapped from ext2); `wineserver -k`; `sync`; ps census. No `wineboot -i`. |

After each boot the host runs `e2fsck -fn` and takes a manifest of the prefix
through **Linux's** ext2 driver (read-only loop mount in a private mount
namespace): path, type, mtime (ns), size, link target.  `WINEDEBUG` gets
`+wineboot` in this mode, so wineboot's `configuration in ... has been
updated` trace is a positive control in boot 1 and must be absent in boot 2.

Grades (all must pass):

| | rule |
|---|---|
| P1 | boot 1 built the prefix: `system.reg`, `user.reg`, `userdef.reg`, `.update-timestamp`, `system32/kernel32.dll`, `system32/ntdll.dll` regular files, `dosdevices/c:` → `../drive_c`, `dosdevices/z:` → `/` (as seen by Linux); `created the configuration directory` and `has been updated` each present |
| P2 | boot 1 shut down cleanly: the quiesce after wineboot ended with no extra process, no timeout/leftover/kill row, `ps-extra count=0`, `sync` rc 0, `System halt` |
| P3 | boot 2 did not re-initialise: no `created the configuration directory`, no `has been updated`, `.update-timestamp` unchanged, and **every** entry boot 1 left is identical (type, mtime, size, target) except the set a second start rewrites on Linux too (§1.2): the three hives, the `pfx` and `dosdevices` directories, `dosdevices/com*` |
| P4 | boot 2: G1 `hello`, G2 all 41 winprobe markers + exit 0, G3 `wineserver -k` leaves nothing, and winprobe from `C:\` on ext2 |
| P5 | `e2fsck -fn` rc 0 after both boots |

`P2_PFX_ONLY=2` reruns boot 2 on the image of an earlier boot 1 (iteration
aid, never a gate result).

### 1.2 Linux reference: what a second start rewrites

`bash tools/cp/pfx/host_pfx_ref.sh OUT` runs the same staged Wine and glibc
2.31 in a chroot on strix, prefix on a fresh `mkcpdata.sh` image mounted by
Linux's ext2, two separate namespaces (= two boots, each with a fresh
wineserver).  Measured (strix, 2026-11-16):

- boot 1 `wineboot -i`: 1,055 inodes, 883 regular files / 216,658,776 bytes in
  the prefix; `created the configuration directory` 1, `has been updated` 1;
- boot 2 `wine cmd /c echo hello` prints `hello`, `has been updated` 0,
  `created ...` 0; the manifest before boot 2 equals the one after boot 1;
- boot 2 changes exactly: `system.reg`, `user.reg`, `userdef.reg`
  (wineserver's `save_branch` writes `reg<pid><n>.tmp` and renames it over
  the hive), the 32 `dosdevices/com1..com32` links (mountmgr re-creates them),
  and the mtimes of `pfx/` and `pfx/dosdevices/`.  Nothing under `drive_c`,
  and not `.update-timestamp`.
- `e2fsck -fn` rc 0.

That set is P3's allow-list.

### 1.3 What the prefix needs from ext2/vfs (probe `linux_probe_pfx`)

`src/user/linux_probe_pfx.c` replays the prefix's file-system operations, each
row named after the Wine code it stands for (setup_config_dir, save_branch's
temp+rename, fcntl locks between two processes, ftruncate/fsync, the
wineserver's fchdir round trips, *at() through `dosdevices/c:`, PE-style
mmap, utimensat, statfs/xattr, a 900-entry directory, a fake-DLL copy,
directory renames).  Linux host (ext4 dir): every row passes.  HobbyOS base
`cyberpunk` @ 5eea3712 with the data disk: **72/72 rows already pass** --
the ext2b lane had covered the prefix's needs; `fgetxattr` answers
`EOPNOTSUPP` (Linux ext4 `ENODATA`; Wine tolerates both).  The probe is a
regression guard; the proof is the Wine run.

New compat rows: `sync` (162) and `syncfs` (306), which flush ext2 and the
`MAP_SHARED` write-back pages (`src/kernel/ext2_compat.c`); they were ENOSYS.

### 1.4 Result (strix, KVM, up to four guests on the box)

Validity rule (integrator): a run on a pre-C3-nfs2 kernel with `rpc: no
reply` rows is VOID (a lost NFS reply used to come back as zero-filled
pages); on 8b9fc2c9+ only `[NFSHARD] GIVE UP` / `[NFLK] ... rc=-1` void it.

| run | kernel | no-reply rows b1/b2 | P1 | P2 | P3 | P4 | P5 | note |
|---|---|---|---|---|---|---|---|---|
| 1 | lane on 5eea3712 (pre-nfs2) | 0 / 0 | PASS | PASS | PASS | FAIL | PASS | valid; boot-2 steps hit 300 s timeouts behind the hung --init |
| 3 | lane on c7e40bc3 (pre-nfs2) | 62 / 0 | PASS | PASS | PASS | FAIL (G3) | PASS | **VOID**; winprobe Z: and `C:\` 41/41 after the boot-event wait, hung pid 29 left |
| 3-b2 | same, boot 2 only on run 3's boot-1 disk | – / 124 | PASS | PASS | PASS | PASS | PASS | **VOID** and an iteration aid only |
| 4 | same | – | – | – | – | – | – | stopped (void kernel) |
| 5 | merge of 8b9fc2c9 (pre-k4) | – | – | – | – | – | – | stopped in boot 1 when C1-k4 merged: wineboot done, two orphan services.exe threads BLOCKED in `__read` after wineserver exited |
| 6 | merge of 030a7f10 (C1-k4 in) | 116 / – | – | – | – | – | – | **VOID**: `[NFSHARD] GIVE UP op=RPC ... after_ms=180000 -> EIO` 20 min into boot 1 (the host stopped answering for 3 min with four guests up); stopped |
| **7** | **merge of 030a7f10 (cp-pfx d46f438e)** | **19 / 0** (19 `[NFSHARD] retry`, longest waited 1,504 ms; no GIVE UP, no NFLK rc=-1) | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** | **the gate run**, §1.5 |

In every boot 2 of runs 1–3: no `created the configuration directory`, no `has been
updated` (both present in every boot 1), `.update-timestamp` `0` → `0`, all
1,008 boot-1 entries kept with type/mtime/size/target identical except the
three hives (re-saved by wineserver, as on Linux) and the `pfx`/`drive_c`
directory mtimes; the only new entry is `drive_c/winprobe.exe`.  No
`dosdevices/com*` links exist in HobbyOS (no serial ports), so that part of
the Linux allow-list is unused.  Nothing in ext2/vfs/locks needed fixing for
Wine: the only kernel addition for item 1 is `sync`/`syncfs`.

### 1.5 Gate run 7 (`docs/cp-pfx-evidence/p2pfx-run7-PASS-strix.txt`)

- Boot 1: `wine: created the configuration directory '/EXT/pfx'`; `wineboot -i`
  304,761 ms to Wine's boot-event timeout (status 53), the install then ran
  on and the whole tree exited by itself (`quiesce after wineboot: extra=0`,
  no kill); `sync` rc 0; `System halt`.  e2fsck -fn rc 0 (1023 inodes, 91,202
  blocks); 1,012 prefix entries.  `has been updated` trace present.
- Boot 2 (no wineboot -i): `hello` exit 0 in 17,458 ms (the fresh server's
  --init finished: no boot-event wait), winprobe from `Z:` 41/41 + exit 0,
  `copy` to `C:\` and winprobe from `C:\` (the PE mapped from ext2) 41/41,
  `wineserver -k` exit 0, `ps-extra count=0`.  No `created ...`, no `has
  been updated`; `.update-timestamp` `1794762086` → `1794762086` (the real
  wine.inf mtime now: C3-nfs2 fixed fstat on NFS); all 1,012 boot-1 entries
  identical; new: `drive_c/winprobe.exe` and `reg80000.tmp` (below).
  e2fsck -fn rc 0.
- **Finding: the hive save at `wineserver -k` is cut short.**  `-k` sends
  SIGINT, polls the server lock for 50 ms × (1..20) = 10.5 s, then SIGKILLs
  (`server/request.c` `kill_lock_owner`).  On SIGINT the server saves the
  dirty hives (`save_branch`: `reg<pid><n>.tmp` + rename).  In boot 2 it got
  1,318,912 of system.reg's 1,830,541 bytes into `reg80000.tmp` before the
  SIGKILL (the kill step took 11,666 ms), so the old `system.reg` stayed
  (consistent: the rename never happened) and a stray temp file is left.  That
  is ≈ 125 KB/s through stdio's 4 KiB writes, i.e. ~30 ms per write(2) on
  /EXT: ext2 writes through at every mutating syscall (data block, inode
  block, bitmap, group descriptor, superblock: ~5 single-4-KiB NVMe commands
  each).  On Linux the same save takes milliseconds.  Nothing is corrupted,
  but a session's registry changes are lost whenever a hive save takes more
  than 10.5 s.  Next storage step: batch the write-through (flush once per
  syscall but coalesce the superblock/GDT, or flush on fsync/close/timer with
  the FAT-style crash window documented), and multi-block NVMe commands.

### 1.6 The boot-2 hang (not storage; C1-k4's, merged as 030a7f10)

In 2 of 3 boot 2s (runs 1 and 3) the fresh wineserver's `wineboot --init`
never finished: every Wine client BLOCKED in `read()` (pc = libpthread-2.31
`__read`+0x10/+0x4a, the syscall instruction), wineserver running its loop,
`[ALIVE-SET] no READY work`; a service process that services.exe exec'd (run
3: pid 29, from pid 28) blocks right after `readlink(/proc/self/exe)`,
before it ever reaches the server, and survives `wineserver -k` (G3's only
leftover).  Later clients wait out Wine's 5-minute boot-event timeout and
then run normally, which is why G1, G2 and winprobe-from-`C:\` pass anyway.
The same boot-2 workload on the same disk ran clean once (row 3 above).  It
is the shape of §0.2 item 18's open "wineboot-tree hang"; census in
`docs/cp-pfx-evidence/p2pfx-run3-strix.txt`.  C1-k4 (epoll 32-fd cap,
512-byte pipes, killed tasks re-parking, exec from a thread) is merged in
030a7f10; on it, gate run 7's boot 2 finished --init in seconds.  Run 5
(8b9fc2c9 + this lane, before C1-k4) showed a sibling: after boot 1's
wineserver exited, two services.exe threads stayed BLOCKED in `__read`.

## 2. `VM_OBJ_MAX_BYTES`

**Already 1 GiB on base.**  cp/sys (ca51e8cf, merged before wave 4) raised it
from 64 MiB and made `vm_object_truncate` grow the frame array geometrically;
the brief's "64 MiB" and §0.2 item 18's "64 MiB per-file ext2 mapping cap"
are stale (`ext2_obj.h` and a `vm.c` comment still said 64 MiB; fixed).

What the cap protects: one **contiguous** frame allocation per object, the
page array (8 bytes per page).  Nothing is sized by `VM_OBJ_MAX_FRAMES`; the
slot table is `VM_OBJ_MAX` (2048) fixed records; data frames are allocated on
first touch.  Unit test `test_vm_big_object_cost` (`vm_test.c`), against the
real frame allocator:

| object | array (contiguous) | per mapping per address space |
|---|---|---|
| 200 MiB | 101 frames (404 KiB) | 101 page-table frames for a full MAP_SHARED mapping |
| 1 GiB (the cap) | 513 frames (2 MiB) | ~ 1 frame per 2 MiB mapped |

So 1 GiB stays: the array is 0.2 % of the object, and a failed contiguous
allocation is a clean `ENOMEM`.  Not raised further: F3 GPU blob mappings
must not go through `vm_object` at all (P0: BOs ≥ 192 MiB).

Probe `linux_probe_bigobj` (sizes from `docs/cp-p0-host.md` §5): B1 200 MiB
memfd (shared RW every page, a second shared view, `pread`, a private view
reading through, private writes staying private, a later shared store seen
by an untouched private page), B2 200 MiB `MAP_SHARED|MAP_ANONYMOUS` across
fork, B3 a 200 MiB ext2 file (shared RO, private RW COW, shared RW + msync →
`read`), B4 **the Gate P3 "largest mapped file"**: P0's largest mapping of a
game file is `Cyberpunk2077.exe` (66,704,848 bytes = 63.6 MiB: 404,016 bytes
under the old cap, so it never needed the raise) mapped
`MAP_PRIVATE|MAP_FIXED` RW at offset `0x397a000` for 3,166,208 bytes
(`.pdata`); B4 builds a file of exactly that size on ext2 (synthetic bytes:
game files never enter a tree or a guest disk here) and maps that window
into a `PROT_NONE` reservation as Wine does, B5 a memfd one page past 64 MiB.
Base passes it (the cap is already 1 GiB); with the cap set back to 64 MiB
(the pre-cp/sys value, A/B build) 6 rows fail: B5 and B1 `ftruncate` EINVAL,
B2 and the three B3 mmaps ENOMEM (B4 passes: the exe is under 64 MiB).

Two `vm_lock` windows that scaled with the object were moved out of the lock
(`vm_object_unref`'s last reference frees the frames after dropping it;
`vm_object_create` allocates and zeroes the array before taking it).  The
first version of the unit test, which faulted all 51,200 pages through
`vm_handle_fault`, tripped the unit tier's IRQ-off budget: one fault held
`vm_lock` 930 ms once (the fault path emits `[PUB]`/`[PUBS]` console rows
under the lock; base's worst is 10.9 ms).  The test now materializes pages
through the object and faults one page per 2 MiB; tier max 3.4 ms.  The
locked unref path (region removal, i.e. munmap of the last mapping) still
frees under the lock: C1-k4's code.

## 3. FAT private mappings see later `write()`s

Before: every FAT16 `write()` called `file_obj_invalidate_all()`, which
detaches the cached `VM_OBJ_FILE` objects; a live mapping keeps its object and
its already-materialized frames, so a page it had read showed the old bytes
for ever (`MAP_PRIVATE` *and* `MAP_SHARED`), while a page never touched read
the disk and was current.

Now (`src/kernel/file_obj.c` `file_obj_note_write`, called from `fs.c`'s FAT
write path after `fat16_write`): every live `VM_OBJ_FILE` object of that file
-- found by source cluster in the object table (`vm_object_file_writers`),
cached or detached -- has the written byte range copied, from the disk, into
the frames it already holds.  Private COW copies are separate frames and are
not touched (Linux).  Race with a concurrent first-touch fill: the object
carries a write generation (`vm_object.wgen`), bumped under `vm_lock` after
the write reached the disk and before the refresh; a fill that read the disk
under an older generation is not published and re-reads
(`vm_object_install_page_gen`).  Refreshes are serialized by a ylock.
`invalidate_all` stays (a size change must re-key new mappings); old and new
objects now agree.

Probe `linux_probe_fatcoh` (file on the FAT boot volume): F1 a faulted private
page sees `pwrite`/`write`, F2 an untouched page, F3 a written page stays
private, F4 a `MAP_SHARED` RO view, F5 an extending write in the mapped last
page, F6 a mapping made before and one made after agree, F7 a fork child's
page.  Base: 7 rows FAIL (F1×2, F3, F4, F5, F6, F7); lane 17/17.

Not covered: `ftruncate`/`O_TRUNC` of a mapped FAT file (no unmap-past-EOF
here, unlike ext2/memfd); a detached object whose file grew reads its
never-touched last page only up to its old size.

## 4. Files changed (vm.c functions, for the C1-k4 merge)

`vm.c` (C3-pfx's functions only): `vm_object_create` (array allocated and
zeroed before `vm_lock`; `wgen = 0`), `vm_object_unref` (last reference frees
after the lock), new `vm_object_wgen`, `vm_object_install_page_gen`,
`vm_object_file_writers` (placed before `vm_obj_unmap_cut_locked`), one
comment in `vm_mmap`'s ext2 branch.  No unmap/mprotect/mremap/TLB/
fault-to-signal code is touched.  Also: `vm.h` (`vm_object.wgen`, comments),
`file_obj.c`/`file_obj.h` (`file_obj_note_write`, the generation-checked
fill), `fs.c` (the FAT write hook, 6 lines in `file_write`),
`ext2_compat.c` (sync/syncfs), `vm_test.c` (`test_vm_big_object_cost`).

## 5. Evidence (`docs/cp-pfx-evidence/`, strix, 2026-11-16)

| file | what |
|---|---|
| `host-pfx-ref-strix.txt` | §1.2 Linux reference (two namespaces, Linux ext2) |
| `p2pfx-run7-PASS-strix.txt` | **the gate run** (§1.5): grade, launcher rows, manifest diff, e2fsck, NFS validity counts, the cut-short hive save |
| `p2pfx-run1-strix.txt`, `p2pfx-run3-strix.txt`, `p2pfx-run3-boot2only-strix.txt` | earlier runs (§1.4; run 3 and 3-b2 VOID by the NFS rule), the hang census |
| `probes-linux-host-strix.txt` | `linux_probe_{pfx,fatcoh,bigobj}` on the Linux host: 119 PASS, 0 FAIL |
| `probes-base-strix.txt` | the same on base 5eea3712 in HobbyOS: pfx 72/72, bigobj 30/30, fatcoh 10/17 (7 FAIL) |
| `bigobj-cap64-ab-strix.txt` | bigobj on base with the cap set back to 64 MiB: 6 FAIL |
| `gates-lane-strix.txt` | the lane's full gate batch on d46f438e (merge of 030a7f10), on 76942554 and 205a73b6, next to `~/cp/base4-gates.txt` |
