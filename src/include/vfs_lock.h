#ifndef VFS_LOCK_H
#define VFS_LOCK_H

/*
 * vfs_lock.h - the generic advisory file-lock manager (cp/ext2b,
 * docs/cp-ext2b.md section 3).
 *
 * One table of byte-range locks for every filesystem, keyed by
 * (filesystem id, inode number) -- struct vfs_lock_key -- so FAT16, ext2,
 * NFS and (lane C2-sys) tmpfs files all lock the same way, and two opens of
 * one file see each other's locks.  Three lock families share the table but
 * only POSIX and OFD locks conflict with each other (Linux: flock(2) locks
 * are a separate namespace from fcntl record locks):
 *
 *   VFS_LK_POSIX  fcntl F_SETLK/F_SETLKW/F_GETLK.  Owner = the process
 *                 (thread-group pid).  Released when the process closes ANY
 *                 fd referring to the file (file_close) and when it exits
 *                 (its fds are closed through file_close).  Not inherited by
 *                 fork.  Deadlock detection (EDEADLK) on the wait chain.
 *   VFS_LK_OFD    fcntl F_OFD_SETLK/F_OFD_SETLKW/F_OFD_GETLK.  Owner = the
 *                 open file description (global file slot); released at the
 *                 description's last close.  F_GETLK reports l_pid = -1.
 *   VFS_LK_FLOCK  flock(2): whole-file shared/exclusive.  Owner = the open
 *                 file description; released at its last close.
 *
 * Ranges are inclusive [start, end]; end == VFS_LK_EOF is "to end of file"
 * (l_len 0).  A new lock replaces the owner's own locks over its range,
 * splitting them as needed, and merges with the owner's adjacent or
 * overlapping locks of the same type (Linux posix_lock_inode).
 *
 * The manager never sleeps: a conflict is reported to the caller, which
 * parks (vfs_compat.c: the rewound-syscall park, woken by vfs_lock_*'s
 * unlock path through process_wakeup and by a 10 ms slice) and re-asks.
 *
 * Adding a filesystem: give its open files a key in file_lock_key() (fs.c).
 * Nothing else: the close/exit release hooks are generic.
 *
 * Locking: one leaf spinlock (irqsave) inside vfs_lock.c; no caller lock is
 * required and none is taken while it is held (wakeups run after release).
 */

#include <stdint.h>

/* Linux errno values errno.h does not name yet (additive, guarded). */
#ifndef EDEADLK
#define EDEADLK 35
#endif
#ifndef ENOLCK
#define ENOLCK 37
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif

#define VFS_LK_POSIX 0
#define VFS_LK_OFD 1
#define VFS_LK_FLOCK 2

/* Lock types: the Linux F_RDLCK/F_WRLCK/F_UNLCK numbers. */
#define VFS_LK_RD 0
#define VFS_LK_WR 1
#define VFS_LK_UN 2

#define VFS_LK_EOF 0x7FFFFFFFFFFFFFFFLL

/* Filesystem ids used in keys (st_dev values where the fs reports one). */
#define VFS_LKFS_ANON 0x0000u  /* no inode identity: per open description */
#define VFS_LKFS_FAT16 0x0801u
#define VFS_LKFS_EXT2 0x0707u  /* == K_DEV_EXT2 (ext2_vfs.c) */
#define VFS_LKFS_NFS 0x4E00u   /* | mount index */
/* tmpfs (lane C2-sys, branch cp-sys): K_DEV_TMPFS, and as `ino` the SAME
 * 64-bit identity cp-sys's interim lock path already uses,
 * 0x8000000000000000 | tmpfs_file_ino(f), so its file_locks_release(pid, id)
 * calls keep working unchanged after the merge (docs/cp-ext2b.md 3.4). */
#define VFS_LKFS_TMPFS 0x0808u
#define VFS_LK_TMPFS_ID_BIT 0x8000000000000000ULL

struct vfs_lock_key {
  uint32_t fsid;
  uint64_t ino;
};

struct vfs_lock_req {
  struct vfs_lock_key key;
  int kind;    /* VFS_LK_POSIX / OFD / FLOCK */
  int owner;   /* POSIX: tgid; OFD/FLOCK: global file slot + 1 */
  int pid;     /* what F_GETLK reports as l_pid (tgid, or -1 for OFD) */
  int type;    /* VFS_LK_RD / WR / UN */
  int64_t start, end; /* inclusive; FLOCK: ignored (whole file) */
};

/* A lock that blocks a request (F_GETLK's answer). */
struct vfs_lock_info {
  int type;
  int pid;
  int64_t start, end;
};

/* Set / change / remove (type UN) a lock.
 *   0         done
 *   -EAGAIN   a conflicting lock exists; *blk (optional) describes it and
 *             *blk_owner/kind (optional) name its owner (for the wait chain)
 *   -EDEADLK  POSIX only, and only when `waiter` != 0: waiting would close a
 *             cycle of POSIX owners (Linux posix_locks_deadlock)
 *   -ENOLCK   the table is full
 * `waiter` != 0: the caller intends to wait (F_SETLKW / blocking flock) as
 * thread `waiter`; a conflict then registers it for the unlock wakeup.  Any
 * other outcome unregisters it. */
int vfs_lock_set(const struct vfs_lock_req *rq, int waiter,
                 struct vfs_lock_info *blk);

/* F_GETLK: 1 and *blk filled when a lock would block `rq`, else 0. */
int vfs_lock_test(const struct vfs_lock_req *rq, struct vfs_lock_info *blk);

/* Drop a waiter registration (the waiting thread gave up: EINTR, exit). */
void vfs_lock_unwait(int waiter);

/* Release hooks (fs.c close paths). */
void vfs_lock_release_posix(int tgid, const struct vfs_lock_key *key);
void vfs_lock_release_desc(int desc_owner); /* OFD + FLOCK of a description */
void vfs_lock_release_tgid(int tgid);       /* every POSIX lock of a process */

/* Diagnostics / unit tests: live lock entries. */
int vfs_lock_count(void);

/* fs.c: the lock key of an open file (every type has one; types without an
 * inode identity get a per-description key). */
struct file;
void file_lock_key(const struct file *f, struct vfs_lock_key *key);

#endif /* VFS_LOCK_H */
