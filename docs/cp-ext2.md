# cp/ext2 — a read/write ext2 data disk at `/EXT` (lane C3-ext2, P3 part 1)

Plan: `docs/cyberpunk.md` §5 AD-7, §6 P3.  This lane adds the writable,
symlink-capable, large-file filesystem Wine's prefix needs, on a **second**
disk, leaving the FAT16 `disk.img` untouched.

## 1. What exists now

| Piece | File | Role |
|---|---|---|
| ext2 core | `src/kernel/ext2.c`, `src/include/ext2.h` | freestanding rev-0/1 ext2: cache, allocator, block map, files, dirs, symlinks, rename, path resolution |
| kernel glue | `src/kernel/ext2_vfs.c`, `src/include/ext2_vfs.h` | mounts the data disk at `/EXT`, one ylock, flush at every mutating syscall boundary |
| data disk | `src/kernel/virtio_blk.c` (`blk_data_*`), `src/include/virtio_blk.h` | NVMe **namespace 2** of the boot controller (x86_64); ARM stubs report "absent" |
| VFS routing | `src/kernel/vfs.c`, `src/kernel/fs.c`, `src/include/fs.h` | `FILE_TYPE_EXT2` / `FILE_TYPE_EXT2DIR`; open/read/write/seek/fstat/getdents64/ftruncate/close, stat, mkdir/rmdir/unlink/rename, chdir, native SYS_READ_DIR, statfs of the cwd |
| Linux-ABI rows | `src/kernel/ext2_compat.c` (`l_tab_ext2`), 13 lines in `src/kernel/linux_compat.c` | consulted first, **declines** (returns 0) anything not on `/EXT` |
| image tool | `tools/cp/mkcpdata.sh` | makes `cpdata.img` with the supported `mke2fs` flags |
| gates | `src/host/ext2_test.c` (`make ext2_test_run`, part of `make host_tests`), `src/user/linux_probe_ext2.c`, `tools/cp/ext2_gate.sh`, `tools/cp/ext2_expect.py` | host golden vs e2fsprogs; two-boot in-OS gate judged by `e2fsck -fn` + `debugfs` |
| Makefile | 13-line `CPDATA` block after the arch `QEMU_CMD`s + the host-test rule | attaches the disk only when the file exists |

## 2. Disk recipe and attachment

```
bash tools/cp/mkcpdata.sh [cpdata.img] [8G]
#  = truncate -s 8G cpdata.img
#    mke2fs -q -F -t ext2 -b 4096 -I 256 -O ^dir_index -m 0 -L cpdata -E root_owner=0:0 cpdata.img
#  features: ext_attr resize_inode filetype sparse_super large_file   (all read-write)
```

Makefile (x86_64 only): `CPDATA ?= cpdata.img`; when that file exists,
`QEMU_CMD += -drive file=$(CPDATA),format=raw,id=cpdata,if=none -device nvme-ns,drive=cpdata,nsid=2`.
No file → the QEMU line is unchanged; `CPDATA=` (empty) never attaches; an
environment `CPDATA=/abs/path.img` reaches every tier (`t3_tier.sh` passes the
environment to make).  The existing NVMe device line is not edited, so the
C1-cpu `-cpu host` change and this hook touch different lines.

**Why namespace 2, not a second controller and not virtio-blk.**  The x86_64
"virtio_blk" driver is really a single-controller NVMe driver (one BAR window
at `cpu_pd7[480]`, one admin/IO queue pair, a single-in-flight token, a reset
/epoch recovery machine).  A second controller means duplicating all of that
state; the ARM virtio-blk driver is MMIO-only (q35 has no virtio-mmio).  A
second namespace on the same controller reuses everything unchanged: the same
token serialises both disks, a controller reset recovers both, and the M6/L69
epoch rule covers both.  Boot cost without the disk: one Identify(nsid=2)
admin command that fails silently.  Transfers are 8 sectors (one 4 KiB bounce
page) per command, and namespace 2 never enters `blk_cache` (keyed by
namespace-1 sectors).

## 3. Feature policy (what mounts read-write)

| Superblock bits | Action |
|---|---|
| INCOMPAT = FILETYPE only (or none) | mount |
| any other INCOMPAT bit (extents, 64bit, flex_bg, meta_bg, journal_dev, recover, inline_data, …) | **refuse** (`-ENOTSUP`, `[EXT2] … NOT mounted`) |
| RO_COMPAT ⊆ {SPARSE_SUPER, LARGE_FILE} | read-write |
| any other RO_COMPAT bit (huge_file, gdt_csum, dir_nlink, extra_isize, metadata_csum, …) | **read-only** (writes `-EROFS`) |
| COMPAT bits | never block (ext_attr, resize_inode, dir_index ok) |

So plain `mke2fs -t ext2` (default features incl. `dir_index`) mounts read-write
too.  **dir_index policy:** the driver never builds hash trees; it reads indexed
directories linearly (their blocks are valid linear directories) and **clears
`EXT2_INDEX_FL` on every directory it modifies**, so no stale index can be left.
`mkcpdata.sh` additionally creates the disk with `-O ^dir_index`.  The host test
builds an indexed directory with `e2fsck -fyD`, modifies it, and checks
`e2fsck -fn` is clean and the flag is gone.

Block sizes 1, 2 and 4 KiB (all three are host-tested).  Inode sizes 128..block
size (the driver writes the 128-byte base, zero `i_extra_isize`).  `s_state` is
left as found (QEMU powers off without an unmount, so the volume is never marked
"not clean"); `s_mnt_count`/`s_mtime` on mount and `s_wtime` on flush are updated.

## 4. Supported operations

lookup, readdir (byte-offset `d_off`, `getdents64` and native `SYS_READ_DIR`),
create (with open's mode, umask 022), read, write (allocates direct, single,
double and triple indirect blocks), sparse files and holes, files > 4 GiB
(`i_size_high`; `RO_COMPAT_LARGE_FILE` is set on the fly when a file passes
2 GiB), truncate/ftruncate both ways (freed indirect trees, tail of the last
block zeroed), unlink (POSIX: an open file is freed at its last close), mkdir,
rmdir, rename (same dir, across dirs, replacing a file, replacing an empty dir,
`EINVAL` into its own subtree, `ENOTEMPTY`/`EISDIR`/`ENOTDIR`, `renameat2`
`RENAME_NOREPLACE`), hard links, fast (< 60 B) and slow symlinks, readlink,
symlink resolution in paths (relative and absolute targets, 40-hop `ELOOP`,
targets that leave the volume are handed back to the VFS — e.g.
`dosdevices/z: -> /` reaches the FAT boot volume), stat fields
(mode/uid/gid/size/blocks/times/nlink/ino, `st_dev` 0x707), statfs/fstatfs
(`f_type` 0xEF53, real counts), fsync/fdatasync, superblock and group-descriptor
free counts kept exact.  Freed xattr blocks are refcount-released.

**Never trust the disk:** every block number (`>= s_first_data_block`,
`< s_blocks_count`, `!= 0`), inode number, group descriptor (bitmaps/table in
range), `rec_len` (>= 8, multiple of 4, inside the block), `name_len`
(`8 + name_len <= rec_len`), dirent inode (`<= s_inodes_count`) and indirect
pointer is checked; a violation is `-EIO` and sets a sticky `corrupt` flag.  An
allocation never returns a block of its group's own bitmap/inode table, a
double free is refused.  Superblock geometry is checked once at mount
(magic, log block size <= 2, inode size, groups × ipg = inodes_count, device
large enough, `s_first_data_block` consistent).

Linux-ABI rows (`l_tab_ext2`, consulted first, decline when not on `/EXT`):
open, openat (incl. a real dirfd on `/EXT`), creat, stat, lstat, newfstatat
(`AT_SYMLINK_NOFOLLOW`), readlink, readlinkat, symlink, symlinkat, link, linkat,
mkdir, mkdirat, rmdir, unlink, unlinkat, rename, renameat, renameat2, truncate,
fsync, statfs; and for every fd: ftruncate (77), fdatasync (75), fstatfs (138)
(previously `-ENOSYS`).  read/write/pread/pwrite/lseek/fstat/getdents64/chdir/
access go through the generic rows, which now reach ext2 via `fs.c`/`vfs.c`.
statx stays `-ENOSYS` (callers fall back to newfstatat).

## 5. Not done (out of scope for this pass)

- `flock`/`fcntl` record locks on ext2 fds (FAT16-only today).
- **mmap of ext2 files** (`vm.c` maps only FAT16/NFS/memfd objects → `-ENOTSUP`);
  Wine maps PE images and `.so` files — needed before the prefix can hold DLLs.
- exec of programs stored on ext2 (`fs_read_image`).
- chmod/fchmod/chown/utimensat on ext2 (modes come only from create).
- xattrs, htree indexing (read is linear, writes linearise), ext3 journal,
  ext4 features, NFS large files, ARM data disk.
- `VFS_PATH_MAX` is still 256 bytes (Wine paths can be longer); the VFS
  canonicaliser's component limit was raised 16 → 64 (it used to drop deeper
  components silently).
- Performance: write-through at syscall granularity, 4 KiB per NVMe command,
  4 MiB block cache; no read-ahead.

## 6. Evidence (linuxci, 2026-11-15; raw logs in the lane report)

| Gate | Command | Result |
|---|---|---|
| host golden | `make ext2_test_run` (also inside `make host_tests`) | `ext2_test: 624 checks, 0 failures` — every `e2fsck -fn` rc=0 (bs 1/2/4 KiB, default and `^dir_index` features, cache 8..512 slots, htree dir modified, 9 GiB sparse file on a 1 KiB fs, ENOSPC fill); 11 corrupted/unsupported images fail cleanly. Also clean under `-fsanitize=address,undefined`. |
| Linux reference | same probe binary, loop-mounted fresh `mkcpdata.sh` image, Linux's own ext2 | phase 1: 75 ok rows, phase 2: 14 ok rows, `done rc=0` both; `e2fsck -fn` rc=0, `29/524288 files, 37794/2097152 blocks` |
| in-OS, boot 1 | `bash tools/cp/ext2_gate.sh` | `[EXT2] mounted /EXT bs=4096 …`, 75 ok rows, `[LP] done rc=0`; `e2fsck -fn` rc=0, `29/524288 files, 37794/2097152 blocks` (the same counts Linux produced); every file's sha256 equals `ext2_expect.py`, 5 symlink targets, 8 removed names absent, hard link = one inode with `Links: 2`, `sparse5g` Size 5368709135 with lblk 1310720 holding the record and lblk 1000 a hole |
| in-OS, boot 2 | (same script) | phase 2: 14 ok rows (every byte of phase 1 re-read after the reboot), `done rc=0`; `e2fsck -fn` rc=0, `30/524288 files, 37795/2097152 blocks`; phase-2 sha256s match; `ext2_gate: PASS` |
| no disk = no change | `make --eval` of `QEMU_CMD`, base vs lane Makefile | identical sha256 without the image; `-drive …,id=cpdata … -device nvme-ns,drive=cpdata,nsid=2` appended with it; ARM never |
| unit tier x86_64 | `./run_unit_tests_intel.sh` (lane and base `f7a51cfa`) | both `Tests run: 448, Tests failed: 3` — the same three pre-existing reds (virtio_gpu_test.c:213, :243, irq-masked budget); no `[EXT2]`/data-disk rows |
| host_tests | `make -k host_tests` (lane and base) | only `sync_gaming_md_test.py` and `roadmap_doc_test.py` fail (missing `gaming.md`), identically on base |
| T3 wave x86_64 | `make test_intel` (lane and base, KVM) | both wedge in the same place (after `MD5SUM_T.BIN`, `[ALIVE-SET] no READY work`, one `[BLOCKHOLD]`, no `System halt`), 0 FAIL lines in either: the known x64 T3 instability (skills/hobbyos-run-tests), not this lane. First lane attempt died at build on a host `Input/output error` (dmesg: `I/O error, dev sda`) and was re-run |
| ARM | `make ARCH=arm MODE=test hobbyos.elf` | links; no SIMD register in the ext2 objects' disassembly |
