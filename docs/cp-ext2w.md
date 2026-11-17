# cp/ext2w — ext2 write-back, multi-block NVMe, FAT truncate under a mapping (lane C3-ext2w, wave 5)

Plan: `docs/cyberpunk.md` §7.2e (C3-ext2w row), §0.2 item 18 ("Open, ext2
speed"; "open: `ftruncate`/`O_TRUNC` of a mapped FAT file").  Builds on
`docs/cp-ext2.md` (the driver), `docs/cp-ext2b.md` (mmap, locks) and
`docs/cp-pfx.md` (the prefix on ext2, `sync`/`syncfs`, the cut-short hive
save).  Every number is from **strix** (Ryzen AI MAX+ 395, Zen 5, KVM, 12 GiB
guest, other lanes' guests running beside it) unless it says otherwise.

## 1. What changed

| Piece | File | Change |
|---|---|---|
| block cache | `src/kernel/ext2.c` §2, `src/include/ext2.h` | write-back across syscalls; buffers tagged with a commit class (`K_DATA`, bitmaps, inode tables, GDT, other metadata); committed images (`pre`) of bitmap/inode-table blocks; clock (second-chance) eviction; only early-safe buffers are written outside a commit; 4096 slots (16 MiB) |
| allocator | `ext2.c` `find_zero`, `balloc`/`ialloc` | a block or inode freed since the last commit is not reused before the next commit (it is still referenced on disk); an allocation that finds nothing else commits once and retries |
| commit | `ext2.c` `ext2_commit` (= `ext2_flush`) | the ordered commit, eight write passes (section 2) |
| reads | `ext2.c` `ext2_read`, `read_ahead` | whole uncached blocks go straight to the caller in runs of physically consecutive blocks (one device call each); a sequential miss reads up to 32 blocks ahead into the cache |
| policy | `src/kernel/ext2_vfs.c` | when to commit (section 3); the `ext2flush` kernel task; `[E2W]` diagnostics; `ext2v_close` writes back data only |
| msync | `src/kernel/ext2_compat.c` | `MS_SYNC` commits too |
| NVMe | `src/kernel/virtio_blk.c` (ns2 command path), `src/include/virtio_blk.h` | `blk_data_rw_vec`: up to MDTS (QEMU: 512 KiB, 1024 sectors) per command through one physically contiguous bounce region and a PRP list; `blk_data_read/write` use it |
| FAT truncate | `src/kernel/fat16.c` `fat16_mapped_resize` | `O_TRUNC` / `ftruncate` resize every live `VM_OBJ_FILE` object of the file with `vm_object_truncate` (section 4) |
| probes / gates | `src/user/linux_probe_ext2w.c`, `src/user/linux_probe_fattrunc.c`, `tools/cp/ext2w/ext2w_gate.sh`, `tools/cp/ext2w/reg_gate.sh` + `regp.c`, `src/host/ext2_test.c` (`scenario_crash`, the vectored device) | section 6 |

## 2. The ordered commit (the ordering rules)

ext2 has no journal, so no write order makes every instant of a multi-block
update e2fsck-clean.  The rules below give two guarantees instead:

1. **Between commits the disk is exactly the last commit's state** (plus file
   data and never-referenced new blocks, which e2fsck cannot see).  So a crash
   at any moment when no commit is running -- in particular right after `sync`
   / `fsync` returns, while other writers keep writing into the cache -- leaves
   an image `e2fsck -fn` passes, with every synced byte intact.
2. **A crash in the middle of a commit leaves only repairable leaks**: blocks
   or inodes marked in use that nothing references (`-` bitmap differences),
   wrong free/directory counts, unattached inodes / unconnected directories
   (e2fsck reconnects them to `lost+found`), link counts that are too high, and
   one name too many after a rename (Linux ext2 has the same window).  Never: a
   reference to a free block or inode (`+` differences), a directory entry
   naming a deleted or uninitialised inode, an inode pointing at a block that
   holds garbage, a block owned twice.  Proven exhaustively per prefix by the
   host test (section 6.1).

### 2.1 Classes

| class | what | may reach the disk |
|---|---|---|
| `K_DATA` | regular-file contents, slow-symlink text | any time (eviction, last close, commit pass 1) |
| NEW | any block allocated since the last commit (its bit is clear in the committed bitmap) | any time: nothing on the disk refers to it |
| bitmaps, inode tables, GDT, existing directory / indirect / xattr blocks | metadata the on-disk structure points at | only in a commit |

`blk_is_new` reads the committed bitmap image without I/O; the group
descriptor blocks are pinned in the cache for that (`fs->ordered`; a GDT larger
than a quarter of the cache falls back to plain block order).

**No reuse before commit.**  A freed block or inode is still referenced by the
on-disk state until the commit lands, so the allocator skips bits set in the
committed image.  Without this, a freed directory or indirect block could be
handed to a file and overwritten by data before the inode that points at it
on disk is rewritten.

**A new directory is committed by the `mkdir` that creates it** (end of
`ext2_mkdir`).  The inode-table passes below order one level of new directory
(its block before its inode before its entry in the parent); a chain of new
directories in one batch (`mkdir -p a/b/c` without a commit) would need one
pass per level.

### 2.2 The passes (`pass_img`)

| pass | blocks | image |
|---|---|---|
| 1 | `K_DATA` and NEW | current |
| 2 | block and inode bitmaps | committed OR current: the allocations land, the frees do not |
| 3 | inode tables | current, except held at the committed record: inodes freed in this batch, every changed **directory** inode, and the link count of a live inode whose count dropped |
| 4 | inode tables holding a changed directory | current, frees and link drops still held (the directory's new blocks came in pass 1, the inodes its entries name in pass 3) |
| 5 | existing directory, indirect, xattr blocks | current (entries added name inodes passes 3-4 wrote; entries removed go before the inode is freed) |
| 6 | inode tables holding a freed inode | the frees land, link drops still held (a removed subdirectory's `..` stops counting before its parent's count drops) |
| 7 | inode tables holding a link drop | current |
| 8 | bitmaps that lost bits; group descriptors | current; then the superblock |

Within a pass, blocks are sorted and written in runs of consecutive blocks
(one device call each).  The held-back images are built per run into scratch
pages; the cache keeps the current state.  A commit that fails with `-EIO`
leaves every buffer dirty (the next commit retries).

Dependencies this order satisfies (A before B):
bitmap bit set → any reference to the block/inode;
block contents (new indirect, directory, data) → the pointer to it;
inode initialised → any entry naming it;
entry removed → inode freed → bitmap bit cleared;
link count raised → entry added; entry removed → link count lowered.

### 2.3 What a commit costs

A 4 KiB append + `fsync` writes data, the block bitmap, the inode-table block,
the group-descriptor block and the superblock: 5 commands, as the old
write-through did on every `write()`.  The difference is that 1,000 `write()`s
in a row now cost 5 commands in total, and data runs go 1024 sectors per
command.

## 3. When the cache is committed (`ext2_vfs.c`)

| event | action |
|---|---|
| `fsync`, `fdatasync`, `syncfs`, `sync`, `msync(MS_SYNC)` | commit (`ext2v_sync`) |
| the close of a writable file description | **data only**: `ext2_writeback_early` (file contents + new blocks); the metadata pointing at them follows with the next commit |
| timer | the `ext2flush` kernel task commits when the oldest dirty buffer is 1 s old (`EXT2W_TIMER_MS`) |
| pressure | more than half the cache dirty at a call boundary: commit |
| inside an operation | only when no clean or early-writable slot is left, or an allocation needs this batch's frees (`st_midop`; 0 in every run measured) |
| shutdown | the flusher is a process: the wave's halt waits for it, and it exits only with the cache clean (≤ 1.2 s after the last write) |
| `mkdir` | commit at its end (section 2.1) |

Every commit except the in-operation fallback runs at a syscall boundary under
the ext2 lock, so it writes a state some sequence of complete syscalls
produced.

**Why the close does not commit.**  The brief asked for write-back "on last
close".  A full commit at every close keeps the disk mid-commit most of the
time under a program that churns small files: the first pull-the-plug run of
this lane (writer + syncer) made 647 close-commits in 2 s, and `QMP quit` right
after `sync` returned found a half-written commit (leak-class only, but
`e2fsck -fn` rc 4).  Writing only the early-safe class at close keeps
guarantee 1 (section 2) with any number of concurrent writers, and a closed
file's bytes still reach the disk at close; its inode follows within a second.

**The flusher.**  `process_create_kernel_nowait(flusher_task)` when the cache
first gets dirty (outside the ext2 lock), named `ext2flush`; it polls every
50 ms (`safe_wfi`, like the other driver threads), commits when the timer is
due, and exits 200 ms after the cache is clean (under the lock, so a writer
that dirties the cache afterwards starts a new one).  Process censuses see it
(`tools/cp/ext2w/regp.c` ignores the name; winep2's census waits 10 s for
extras, longer than the flusher lives).

## 4. FAT `ftruncate`/`O_TRUNC` under a live mapping

Before: `fat16_truncate` / `fat16_truncate_to` changed the chain and the
directory entry only.  Live `VM_OBJ_FILE` objects kept their old size and
frames: pages past the new end stayed mapped (stale bytes), the page holding
the end kept the cut-off bytes, and a later fill could read clusters already
freed (and reused by another file).

Now `fat16_mapped_resize(first_cluster, new_size)` runs after the chain and
entry are updated (no lock held): every live object of the file
(`vm_object_file_writers`, which also bumps the write generation so a racing
fill re-reads) gets `vm_object_truncate(o, new_size)` -- a shrink drops the cut
pages from every address space (shared frames and private COW copies, as
Linux's `truncate_pagecache` does) so a later touch is SIGBUS, a grow makes the
range fillable again -- and the frame holding the new end is zeroed past it.
The cache slots are dropped (`file_obj_invalidate_all`, as the FAT write path
already does) so the next fill uses the object's new size.  `O_TRUNC` resizes
to 0: every page is SIGBUS.

Divergence: after `O_TRUNC` the object is keyed by the freed first cluster; a
mapping made before the file is rewritten keeps SIGBUSing (Linux would show
the new bytes).  A mapping made after the rewrite is correct.

## 5. Results (strix; raw logs in `docs/cp-ext2w-evidence/`)

### 5.1 Speed (`linux_probe_ext2w`, `ext2w_gate.sh bench`, same host, same probe)

Base = `cyberpunk` 949ba008 (base5) with only the probe added
(`base-bench-crash-strix.txt`); lane = this branch before the main merge
(`lane-bench-lazysb-strix.txt`, `lane-bench-run4-strix.txt`).  The probe clock
has 1 ms resolution, so the 3-9 ms rows are ±15 %.

| row | base5 | lane | × |
|---|---|---|---|
| 4 KiB writes, 4 MiB (`seq4k`, warm) | 14.1 MB/s | 683 MB/s | 48 |
| 4 KiB writes, 4 MiB, first write of the boot (`cold4k`) | 12.8 MB/s | 195–455 MB/s | 15–35 |
| 4 KiB writes, 32 MiB = 2× the cache (`seq4k32m`) | 13.8 MB/s | 546–575 MB/s | 40–42 |
| the same + the final fsync | 13.8 MB/s | 546–555 MB/s | 40 |
| 1 MiB writes, 32 MiB (`seq1m`) | 76 MB/s | 669–728 MB/s | 9 |
| 64 KiB reads, 32 MiB, uncached (`read64k`) | 79 MB/s | 512–546 MB/s | 7 |
| 4 KiB random rewrites (`rand4k`) | 39 MB/s | 683 MB/s | 17 |
| 4 KiB write + fsync (`fsync4k`) | 3,555/s | 3,849/s | 1.1 |
| registry-like: tmp + 2 KiB + close + rename (`smallfile`) | 1,162/s | 5,555–6,060/s | 5 |
| create + 1 B + close + unlink (`churn`) | 1,244/s | 7,894–8,823/s | 6–7 |

`cold4k` pays KVM's first touch of each fresh guest page (the cache's frames
are allocated here) and moves with host load (21 MB/s in one run with all four
slots busy).  `fsync4k` is command-bound: 5 commands per fsync after the
superblock deferral (6 before: 2,000–2,300/s).  The "~125 KB/s" of §0.2 item
18 is not reproducible on base5 (14 MB/s here); §5.3.

### 5.2 Pull-the-plug (`ext2w_gate.sh crash`)

`crashs` (64 KiB file, `sync`, `[LP] SYNCED n=N`) beside `crashw` (an
unsynced writer of small files: create/write/close/rename/unlink/mkdir/rmdir
for ever).  The host sends QMP `quit` when `SYNCED n=K` appears, plus a
delay; then `e2fsck -fn` and s0..sK byte for byte.

| | points (K:delay ms) | e2fsck -fn clean | synced data intact |
|---|---|---|---|
| base5 | 1:0 3:0 6:0 2:400 4:1300 5:2600 8:0 3:900 | **5 of 8** (runs 1, 5, 7 rc 4) | 8 of 8 |
| lane | same | **8 of 8** | 8 of 8 |

Base writes through at every syscall, so the writer is always mid-update; the
lane's disk holds the last commit's state between commits.  The first lane
run (full commit at every close) was 0 of 1 clean: section 3.

### 5.3 The registry (`reg_gate.sh`)

Boot 1 `wineboot -i`, `wine reg add HKCU\Software\CpTest /v X /d Y /f` and the
same under HKLM (so the 1.8 MB `system.reg` is re-saved), `wineserver -k`,
NO `sync`; boot 2 `wine reg query` of both, then 4 rounds of 3 Wine clients
started at the same moment (racing wineservers, C8 stop 6).  Lane and base5
both PASS: the value survives, `user.reg` / `system.reg` (1,811,981 bytes) on
the disk hold it, no `reg*.tmp` is left, `e2fsck -fn` rc 0 after both boots.
`wineserver -k` (the hive save) took 391–510 ms on the lane and 569–611 ms on
base5, far inside Wine's 10.5 s: C3-pfx's cut-short save (1.3 MB in ~10.5 s)
came from a heavily loaded pre-k4/pre-nfs2b kernel and does not reproduce on
base5.  The lane still matters there: a hive save is ~450 write(2)s, 48× less
device time.

### 5.4 FAT truncate under a mapping (`linux_probe_fattrunc`)

Linux host 21/21; base5 **10 FAIL** (pages past the end readable with the old
bytes, the end page not zeroed, a regrown range showing old bytes, nothing
SIGBUS after `O_TRUNC`); lane 0 FAIL.  After the main merge this agrees with
main's FAT16 MAP_SHARED write-back (464dafcf/0259c84c): that path writes only
resident pages and clamps to the object's size, which the resize sets.

### 5.5 Host test (`make ext2_test_run`)

`ext2_test: 1712 checks, 0 failures`.  Crash prefixes (section 6.1): bs 1 KiB
234 prefixes, bs 4 KiB 345, bs 4 KiB with a 12-slot cache (11 eviction
write-backs interleaved) 173: **0 unsafe**; the same batches in plain block
order: **44 unsafe of 156** (first: `Directory inode 16, block #0, offset 0:
directory corrupted`).

## 6a. Lock order (C8 stop 6 review)

| lock | kind | taken under | takes |
|---|---|---|---|
| `f1_pos_locks[]` (pread/pwrite) | ylock | — | `f->lock` |
| `f->lock` (per open file) | ylock | `f1_pos_locks` | `ext2_lock` (read, write, getdents, ftruncate, close) |
| `ext2_lock` | ylock | `f->lock`, or nothing (path calls, page fills, the flusher) | `e2o_lock`, `vm_lock`, `frame_lock` (spinlocks), the NVMe token |
| `vfs_lock` table | spinlock | `fs.c` after `f->lock` is released | nothing of ext2's |

Nothing takes `f->lock`, `f1_pos_locks` or `vfs_lock` while holding
`ext2_lock`, and ext2 never touches `blk_cache` (namespace 1 only).  The
flusher holds `ext2_lock` alone (it has no file); it is spawned after the
lock is released; a commit inside an operation only adds NVMe I/O.  Page fills
of ext2 mappings take `ext2_lock` from the fault path, so the kernel must not
fault on user memory while holding it: read/write prefault before the lock
(and since the merge `file_read_user` makes the destination resident), and
`ext2v_getdents64` now prefaults too (it stored records into user memory
under the lock).  Stop 6 was not reproduced (§5.3's 4 rounds on both trees,
`linux_probe_e2stress` 3 + 3 runs); `[YLOCK]` rows in the evidence show both
`f->lock`s with `nwait=0` and the holder RUNNING in `read`, i.e. the holder
was not waiting on a ylock.  A waiter that sees `ext2_lock` held for 2 s now
prints `[E2W] ext2_lock wait <ms> ms: holder pid=… site=… held_ms=…` with the
holder's `lock()` call site, so the next occurrence names where it entered.

## 6. Tests

### 6.1 Host: every crash point of every commit (`make ext2_test_run`)

`scenario_crash` (`src/host/ext2_test.c`) runs batches of 24 random
operations (writes past 12 blocks, rename-over, unlink, truncate, mkdir/rmdir,
hard links, files in subdirectories, create+delete in one batch) on a 24 MiB
image, records every device write of the batch and its commit, and runs
`e2fsck -fn` on the image after EVERY prefix of those writes, block by block.
Output lines are classified (section 2, guarantee 2).  The same batches with
the commit in plain block order (`fs->ordered = 0`, the old behaviour) must
show unsafe prefixes, or the check proves nothing.

### 6.2 In-OS

- `linux_probe_ext2w` (`tools/cp/ext2w/ext2w_gate.sh bench`): perf rows and
  write-back correctness rows; boot 2 `verify` re-reads files written without
  `fsync` before the halt.
- `ext2w_gate.sh crash`: the pull-the-plug test.
- `linux_probe_fattrunc`: section 4, on the FAT boot volume (runs in the
  normal probe batch).
- `tools/cp/ext2w/reg_gate.sh`: `wine reg add` + `wineserver -k` + reboot,
  then racing Wine clients.
- `linux_probe_e2stress` (in `pfx_gate.sh`'s default list): 6 processes × 3
  threads and two waves of 4 racing "servers" in one ext2 directory for 20 s
  (create/write/fsync/rename-over/unlink, F_SETLK/F_SETLKW/F_GETLK,
  getdents64 into fresh mappings, read() into an untouched ext2 MAP_PRIVATE
  page, MAP_SHARED + msync, fchdir, ftruncate, exit_group with threads
  mid-syscall).  Linux host, base5 and lane: PASS.
