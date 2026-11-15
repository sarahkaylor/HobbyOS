# cp/ext2b — /EXT can hold a Wine prefix (lane C3-ext2b, P3 remainder)

Plan: `docs/cyberpunk.md` §5 AD-7, §6 P3, §7.2b.  Builds on `docs/cp-ext2.md`
(the read/write ext2 driver).  This lane adds what a prefix needs beyond
plain files: mmap and exec of ext2 files, chmod/chown/utimensat, file locks,
and paths longer than 256 bytes.

## 1. What exists now

| Piece | File | Role |
|---|---|---|
| path limit | `src/include/vfs.h` (`VFS_PATH_MAX 1024`), `src/kernel/vfs.c` (`vfs_clean`, `vfs_abs_path`), `src/include/ext2.h` (`EXT2_PATH_MAX 1024`), `src/kernel/ext2.c` (`e2_canon`) | refuse, never truncate |
| setattr op | `src/include/vfs.h` (`struct vfs_setattr`, `vfs_setattr_path`, `vfs_setattr_file`), `src/kernel/vfs.c`, `src/kernel/fs.c` (`file_setattr_fd`) | one generic op, per-filesystem backends |
| ext2 setattr | `src/kernel/ext2.c` (`ext2_setattr`), `src/kernel/ext2_vfs.c` (`ext2v_setattr`, `ext2v_fsetattr`, `ext2v_access`) | stored in the inode |
| lock manager | `src/include/vfs_lock.h`, `src/kernel/vfs_lock.c`, `src/kernel/fs.c` (`file_lock_key`, close hooks, native `file_lock_fcntl` adapter) | POSIX / OFD / flock, keyed by (fs id, inode) |
| compat rows | `src/kernel/vfs_compat.c` (`l_tab_vfsx`, consulted after `l_tab_ext2`), `src/kernel/ext2_compat.c` (access, faccessat; msync/munmap write-back hooks that decline) | Linux decoding |
| mmap | `src/include/ext2_obj.h`, `src/kernel/ext2_obj.c` (`VM_OBJ_EXT2` page cache), `src/include/vm.h` (`VM_OBJ_EXT2 5`, `vm_object.ext2_ino`), `src/kernel/vm.c` (kind added to the file-object fault/touch/exec-gate/fork paths and `vm_mmap`), `fs.c` `file_ext2_ident` | MAP_PRIVATE CoW, MAP_SHARED read and write-back |
| exec | `src/kernel/ext2_vfs.c` (`ext2v_read_image`), `fs.c` (`fs_read_image` ext2 arm, `fs_read_ext2_file`), `src/kernel/elf_loader.c` (PT_INTERP on /EXT first) | spawn-loader images and interpreters from ext2 |
| gates | `src/user/linux_probe_ext2b.c`, `tools/cp/ext2b/exec_hello.c`, `tools/cp/ext2_gate.sh` (boot 3), `src/host/ext2_test.c` (setattr + long path scenarios) | |

## 2. The setattr operation (contract for every filesystem)

```c
#define VFS_SA_MODE      0x01 /* mode: 07777 bits (file type kept) */
#define VFS_SA_UID       0x02
#define VFS_SA_GID       0x04
#define VFS_SA_ATIME     0x08 /* atime = atime_sec/atime_nsec */
#define VFS_SA_MTIME     0x10
#define VFS_SA_ATIME_NOW 0x20 /* atime = the filesystem's clock */
#define VFS_SA_MTIME_NOW 0x40
struct vfs_setattr { uint32_t valid, mode, uid, gid;
                     int64_t atime_sec, mtime_sec; uint32_t atime_nsec, mtime_nsec; };
int vfs_setattr_path(const char *path, int follow, const struct vfs_setattr *sa);
int vfs_setattr_file(struct file *f, const struct vfs_setattr *sa);
```

The compat rows (`vfs_compat.c`) decode Linux's arguments before calling:
uid/gid `-1` is not in `valid`; `UTIME_OMIT` is not in `valid`; `UTIME_NOW`
is the `*_NOW` bit; `times == NULL` is both `*_NOW`; both-OMIT returns 0
without a lookup (Linux `fs/utimes.c`); bad `tv_nsec` is EINVAL; bad
`fchownat`/`utimensat` flags EINVAL; `utimensat(fd, NULL, …)` is futimens.

A backend:
1. changes exactly the fields in `valid` and sets ctime to now when `valid`
   is non-empty (an empty set is a successful no-op);
2. MODE replaces the 07777 bits, keeps the type;
3. a UID or GID change on a non-directory clears S_ISUID, and S_ISGID when
   S_IXGRP is set (Linux `notify_change`, ATTR_KILL_SUID/SGID);
4. stores times at its granularity (ext2: seconds, clamped to signed 32-bit;
   Linux's ext2 also stores seconds), never rounds up;
5. answers `-EROFS` on a read-only volume, lookup errors as usual, and never
   `-EPERM` for these fields: the compat class is uid 0.

### 2.1 Backends

| Filesystem | Answer |
|---|---|
| ext2 (`/EXT`) | stored in the inode (uid/gid 32-bit via the osd2 high halves) |
| FAT16 | Linux vfat with `quiet` for chmod and times (success, not stored; stat keeps the synthesized 0644/0755 — `linux_probe_glibc`'s fchmod row depends on it); chown succeeds only to uid/gid 0, a foreign owner is **EPERM** (vfat without `quiet`) — never a success the next stat would contradict |
| NFS | `-EROFS` after the path is found (the mounts are read-only) |
| pipes, sockets, `/dev` nodes, memfd | FAT16 rule (chmod/times accepted, chown only to 0) — the existing `l_fd_fchmod`/`l_fd_fchown` answers, now behind the one op |

### 2.2 Divergences

- `chown(f, -1, -1)` is a no-op; Linux still bumps ctime.
- Nanoseconds are not stored on ext2 (as on Linux's ext2).

### 2.3 tmpfs (lane C2-sys) after both branches merge

cp-sys exports `tmpfs_setattr(abs, follow, mask, mode, uid, gid, atime, mtime)`
/ `tmpfs_fsetattr(f, …)` with `TMPFS_SET_*` bits and seconds.  The adapter is
three lines in `vfs.c` (the call sites are already written there as comments):

```c
static void vfs_sa_to_tmpfs(const struct vfs_setattr *sa, int *m, int64_t *at, int64_t *mt) {
  int64_t now = (int64_t)(hb_clock_realtime_ms() / 1000);
  *m = 0;
  if (sa->valid & VFS_SA_MODE) *m |= TMPFS_SET_MODE;
  if (sa->valid & VFS_SA_UID)  *m |= TMPFS_SET_UID;
  if (sa->valid & VFS_SA_GID)  *m |= TMPFS_SET_GID;
  if (sa->valid & (VFS_SA_ATIME | VFS_SA_ATIME_NOW)) *m |= TMPFS_SET_ATIME;
  if (sa->valid & (VFS_SA_MTIME | VFS_SA_MTIME_NOW)) *m |= TMPFS_SET_MTIME;
  *at = (sa->valid & VFS_SA_ATIME_NOW) ? now : sa->atime_sec;
  *mt = (sa->valid & VFS_SA_MTIME_NOW) ? now : sa->mtime_sec;
}
```
then `if (tmpfs_owns(abs))` in `vfs_setattr_path` before the FAT/NFS branch,
and `case FILE_TYPE_TMPFS/TMPFSDIR` in `vfs_setattr_file`.  tmpfs must follow
rules 1–5 (ctime, the S_ISUID/S_ISGID kill on chown).

## 3. The lock manager

`src/include/vfs_lock.h`.  One table (512 entries, a split needs a spare
slot; ENOLCK when full) for every filesystem, keyed by

```c
struct vfs_lock_key { uint32_t fsid; uint64_t ino; };
void file_lock_key(const struct file *f, struct vfs_lock_key *key);   /* fs.c */
```

| Family | Syscalls | Owner | Released |
|---|---|---|---|
| POSIX | fcntl `F_GETLK/F_SETLK/F_SETLKW` (5/6/7) | the process (tgid) | the process closes ANY fd of the file (`file_close`; exit closes every fd through it); not inherited by fork |
| OFD | `F_OFD_GETLK/SETLK/SETLKW` (36/37/38) | the open file description (slot + 1) | its last close (`file_close` / `fs_close_global`) |
| flock | `flock` (73) | the open file description | its last close |

POSIX and OFD locks conflict with each other (different owners, even in one
process); flock is a separate namespace.  A new lock replaces the owner's
own range, splitting and merging (adjacent/overlapping same-type) like
`posix_lock_inode`.  Semantics in the compat row: SEEK_SET/CUR/END,
negative `l_len`, `F_RDLCK` needs a readable fd and `F_WRLCK` a writable one
(EBADF), `F_SETLK` conflict EAGAIN, `F_GETLK` reports the blocker (SEEK_SET,
`l_len` 0 = to EOF, `l_pid` = tgid, -1 for OFD) or only `l_type = F_UNLCK`,
OFD with `l_pid != 0` EINVAL, EDEADLK on a cycle of waiting POSIX owners
(bounded 16 hops, like `MAX_DEADLK_ITERATIONS`).  Blocking (`F_SETLKW`,
`F_OFD_SETLKW`, `flock` without `LOCK_NB`): the rewound-syscall park of the
compat layer (state BLOCKED, 10 ms slice, re-executed on wake); the unlock
path wakes registered waiters at once with `process_wakeup`; a deliverable
signal ends the wait with EINTR.  The native `SYS_FCNTL` lock commands
(SQLite) use the same table with their documented native contract (one
attempt for `F_SETLKW`, no access-mode check), so native and compat
processes see each other's locks.

Keys: FAT16 `0x0801`/dir-entry location; ext2 `0x0707`/inode; NFS
`0x4E00|mount`/FNV-1a of the file handle; every type without an inode
identity (pipe, socket, `/dev`, memfd) a per-description key.

### 3.1 Wine 11.0 usage (source on linuxci, `~/cp/wine-build/src/wine-11.0`)

- `server/request.c:711` `wait_for_lock()`: `F_SETLKW` `F_WRLCK` start 0 len 1 on `<server dir>/lock`
- `server/request.c:735`, `:802`: `F_GETLK`, reads `l_pid` of the holder (`kill_lock_owner` signals it)
- `server/request.c:777` `acquire_lock()`: `F_SETLK`; EAGAIN → `exit(2)`, ENOLCK → fall back to the socket
- `server/fd.c:1283/1289` `set_unix_lock()`: `F_SETLK` ranges for `NtLockFile`, `F_GETLK` on EACCES; EIO/ENOLCK/ENOTSUP disable fs locks for that fd
- `dlls/ntdll/unix/server.c:1382` `server_connect_error()`: `F_GETLK`
- No `flock()` and no OFD commands in `server/`, `dlls/ntdll/unix/` or `loader/`.

### 3.4 tmpfs (lane C2-sys) after both branches merge

cp-sys extended the *old* FAT-only engine (`file_lock_fcntl` in fs.c) for tmpfs
with identity `0x8000000000000000 | tmpfs_file_ino(f)` and a matching
release in `file_close`.  This branch replaced that engine; the merge takes
this side of `file_lock_fcntl`/`file_close` and adds one case to
`file_lock_key()` (already written there as a comment):

```c
case FILE_TYPE_TMPFS: case FILE_TYPE_TMPFSDIR:
  key->fsid = VFS_LKFS_TMPFS;                       /* 0x0808 == K_DEV_TMPFS */
  key->ino = VFS_LK_TMPFS_ID_BIT | tmpfs_file_ino(f); /* cp-sys's identity */
  break;
```
and `FILE_TYPE_TMPFS` to the native adapter's type check.  Release-on-close
then works for tmpfs exactly as for ext2 (the hooks are type-independent);
`file_locks_release(pid, id)` maps an id with the top bit set to the tmpfs
key, so any cp-sys call written against the old API stays correct.

## 4. mmap of ext2 files (VM_OBJ_EXT2)

One `vm_object` per (inode, capacity), cached in `ext2_obj.c` (256 entries),
shared by every mapping of the file.  The fault path is vm.c's existing
file-object path (`vm_file_obj_materialize`): MAP_PRIVATE pages are mapped
read-only from the cache and copied on write (and executable private pages
are copied and verified by the L63 exec gate, which reads the page twice via
`ext2_obj_read`); MAP_SHARED pages map the cache frame itself.

- Access (Linux): the fd must be readable (else EACCES); MAP_SHARED +
  PROT_WRITE needs O_RDWR (else EACCES); MAP_PRIVATE + PROT_WRITE on an
  O_RDONLY fd is fine (Wine maps every view RW-private).
- **MAP_SHARED writable policy: write-back.**  Stores land in the cache
  frames; they reach the file at `msync()`, `munmap()`, `fsync()`/
  `fdatasync()`/`sync`, `close()` of any fd of the file, `read()` of the file
  (so `read()` sees mapped stores), and when the entry is reaped.  Only pages
  whose bytes differ from the file are written (holes stay holes).  Stores
  past EOF never extend the file, and are re-zeroed when the file grows.
- Coherence: `write()`/`truncate()` update the cached bytes they change, so
  a shared mapping, and a private mapping's not-yet-copied pages, see later
  writes (Linux).
- Lifetime: the entry holds an `ext2_iget` reference, so an unlinked file
  stays allocated while mapped; an entry only the cache references is reaped
  (written back, inode released) at the next ext2 open/close/unlink/sync or
  mmap.
- Capacity: max(file size, offset + length), cap 64 MiB (VM_OBJ_MAX_BYTES,
  the FAT rule: a larger file is ENOMEM); a later bigger mapping gets a new
  object (the old one keeps its mappings and still receives write() updates).

Divergences: a page wholly past EOF reads as zeros instead of SIGBUS; a
mapping made before a file grew beyond its capacity and one made after hold
different frames for the same page (both receive `write()`, but stores into
one are not seen by the other until write-back + re-fault).  Lock order:
ext2 ylock → ext2_obj spinlock → vm_lock.  A user buffer is pre-touched
before the ext2 lock is taken (a fault on an ext2 mapping under the lock
would re-enter it).

## 5. exec from ext2

- `fs_read_image()` (the spawn loader's image source) reads an image on
  `/EXT` whole (needs an x bit: execve's rule for root), including through a
  symlink that leaves the volume (a FAT16 target is read there too).
- `elf_loader_load_image()`: a PT_INTERP path on `/EXT` is read from ext2
  before the FAT16 resolver (≤ one USER_LIB slot, 16 MiB); other names keep
  the FAT16 lookup and alias table.  ld.so then opens and mmaps its DSOs
  (§4).  The PT_INTERP name is bounded by `ELF_INTERP_PATH_MAX` (64).
- **Not covered: compat `execve(2)`** — `process_exec_current()` copies the
  path into 32 bytes, opens it with `fat16_open` and maps a flat image only
  (no ELF at all, on any filesystem).  Exec from ext2 is therefore proven
  through the spawn loader (WAVE.CFG lines naming `/EXT/...`).

## 6. Paths

`VFS_PATH_MAX` 1024 (not more: a rename holds five such buffers on the 64
KiB kernel stack).  `vfs_clean` builds in place, no component cap, returns
-1 (empty output) when the result does not fit; `vfs_abs_path` refuses a
cwd+relative join that does not fit; `e2_canon` allows EXT2_PATH_MAX/2
components.  Still smaller, owned elsewhere: `struct process.cwd[128]`
(chdir into a deeper directory fails), `exe_path[160]`, the generic compat
path rows' 256-byte copies (`linux_compat_fd.c`, `_misc.c`, `_proc.c`,
trap.c helpers) — ext2 paths never reach them (`l_tab_ext2` answers first,
now including access/faccessat), but a tmpfs or FAT path does.

## 7. Evidence

See the lane report (raw tails): `make ext2_test_run`, `tools/cp/ext2_gate.sh`
boot 3, unit/host/ARM parity.
