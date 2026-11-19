/*
 * vfs_compat.c - the generic (every-filesystem) Linux-ABI rows of cp/ext2b
 * (docs/cp-ext2b.md): the attribute-change family on the vfs.h setattr
 * contract.
 *
 *   chmod 90  fchmod 91  chown 92  fchown 93  lchown 94  utime 132
 *   utimes 235  fchownat 260  futimesat 261  fchmodat 268  utimensat 280
 *
 * This table (l_tab_vfsx) is consulted right after l_tab_ext2 and before the
 * subsystem tables (linux_compat.c).  Unlike l_tab_ext2 its rows ANSWER for
 * every path and fd: the per-filesystem policy lives behind
 * vfs_setattr_path()/vfs_setattr_file() (vfs.c), so FAT16, NFS, ext2 and --
 * once lane C2-sys adds its case -- tmpfs all go through one decoder.
 * And the lock family on the generic manager (vfs_lock.h):
 *
 *   fcntl 72 (F_GETLK/F_SETLK/F_SETLKW, F_OFD_GETLK/F_OFD_SETLK/F_OFD_SETLKW;
 *             every other command DECLINES to l_tab_fd)   flock 73
 *
 * fchmod/fchown used to be answered by l_tab_fd (linux_compat_fd.c); the
 * FAT16/pipe answers are unchanged (vfs.c vfs_setattr_unstored keeps the
 * same rules: chmod accepted, chown only to uid/gid 0, EBADF for a closed fd).
 *
 * Kernel constraints: x86_64-only (the compat class is), no FP/SIMD, guest
 * memory only through the l_copy_* helpers.
 */

#include "linux_compat.h"

#include <stdint.h>

#ifdef __x86_64__

#include "arch/trap.h"
#include "errno.h"
#include "fs.h"
#include "linux_syscall.h"
#include "process.h"
#include "vfs.h"
#include "vfs_lock.h"

extern uint64_t timer_get_ms(void);

/* Numbers linux_syscall.h does not name yet (Linux x86_64 values). */
#define VX_SYS_chown 92
#define VX_SYS_lchown 94
#define VX_SYS_utime 132
#define VX_SYS_utimes 235
#define VX_SYS_fchownat 260
#define VX_SYS_futimesat 261
#define VX_SYS_flock 73

#define VX_F_GETLK 5
#define VX_F_SETLK 6
#define VX_F_SETLKW 7
#define VX_F_OFD_GETLK 36
#define VX_F_OFD_SETLK 37
#define VX_F_OFD_SETLKW 38
#define VX_LOCK_SH 1
#define VX_LOCK_EX 2
#define VX_LOCK_NB 4
#define VX_LOCK_UN 8
#define VX_O_ACCMODE 3
#define VX_O_RDONLY 0
#define VX_O_WRONLY 1

#define VX_UTIME_NOW ((1l << 30) - 1l)
#define VX_UTIME_OMIT ((1l << 30) - 2l)

static int vx_ret(struct trap_frame *tf, int64_t v) {
  tf->regs[0] = (uint64_t)v;
  return 1;
}

/* (dirfd, upath) -> canonical absolute path.
 *   0  abs filled
 *   1  the path is empty and allow_empty: the caller acts on dirfd itself
 *  <0  -errno (EFAULT, ENAMETOOLONG, ENOENT for "", EBADF for a bad dirfd) */
static int vx_path(struct process *p, int dirfd, uint64_t upath, char *abs,
                   int allow_empty) {
  char kp[VFS_PATH_MAX];
  if (!upath) return -EFAULT;
  int r = l_copy_cstr(p, kp, upath, sizeof kp);
  if (r != 0) return r;
  if (kp[0] == '\0') return allow_empty ? 1 : -ENOENT;
  if (kp[0] == '/' || dirfd == L_AT_FDCWD)
    return vfs_abs_path(kp, abs, VFS_PATH_MAX) == 0 ? 0 : -ENAMETOOLONG;
  char base[VFS_PATH_MAX];
  int br = file_path_of(p, dirfd, base, sizeof base);
  if (br == -EBADF) return -EBADF;
  if (br != 0) return -ENOTDIR; /* a nameless fd (pipe, socket) is no dir */
  char joined[VFS_PATH_MAX];
  int n = 0;
  for (int i = 0; base[i]; i++) {
    if (n >= VFS_PATH_MAX - 1) return -ENAMETOOLONG;
    joined[n++] = base[i];
  }
  if (n >= VFS_PATH_MAX - 1) return -ENAMETOOLONG;
  joined[n++] = '/';
  for (int i = 0; kp[i]; i++) {
    if (n >= VFS_PATH_MAX - 1) return -ENAMETOOLONG;
    joined[n++] = kp[i];
  }
  joined[n] = '\0';
  return vfs_abs_path(joined, abs, VFS_PATH_MAX) == 0 ? 0 : -ENAMETOOLONG;
}

/* Apply `sa` to (dirfd, upath); an empty path with allow_empty means dirfd. */
static int vx_apply(struct process *p, int dirfd, uint64_t upath, int follow,
                    int allow_empty, const struct vfs_setattr *sa) {
  char abs[VFS_PATH_MAX];
  int r = vx_path(p, dirfd, upath, abs, allow_empty);
  if (r < 0) return r;
  if (r == 1) return file_setattr_fd(p, dirfd, sa);
  return vfs_setattr_path(abs, follow, sa);
}

/* ---- chmod ---------------------------------------------------------------- */

static void vx_mode(struct vfs_setattr *sa, uint64_t mode) {
  sa->valid = VFS_SA_MODE;
  sa->mode = (uint32_t)mode & 07777u;
}

static int vx_chmod(struct process *p, struct trap_frame *tf) {
  struct vfs_setattr sa = {0};
  vx_mode(&sa, l_arg(tf, 1));
  return vx_ret(tf, vx_apply(p, L_AT_FDCWD, l_arg(tf, 0), 1, 0, &sa));
}

static int vx_fchmod(struct process *p, struct trap_frame *tf) {
  struct vfs_setattr sa = {0};
  vx_mode(&sa, l_arg(tf, 1));
  return vx_ret(tf, file_setattr_fd(p, (int)l_argi(tf, 0), &sa));
}

/* fchmodat(dirfd, path, mode): the system call has no flags argument (glibc
   emulates AT_SYMLINK_NOFOLLOW with EOPNOTSUPP); it always follows. */
static int vx_fchmodat(struct process *p, struct trap_frame *tf) {
  struct vfs_setattr sa = {0};
  vx_mode(&sa, l_arg(tf, 2));
  return vx_ret(tf, vx_apply(p, (int)l_argi(tf, 0), l_arg(tf, 1), 1, 0, &sa));
}

/* ---- chown ---------------------------------------------------------------- */

/* uid/gid (uid_t)-1 means "leave alone" (Linux).  chown(-1, -1) is a no-op
   here; Linux still bumps ctime (docs/cp-ext2b.md, divergences). */
static void vx_owner(struct vfs_setattr *sa, uint64_t uid, uint64_t gid) {
  sa->valid = 0;
  if ((uint32_t)uid != 0xFFFFFFFFu) {
    sa->valid |= VFS_SA_UID;
    sa->uid = (uint32_t)uid;
  }
  if ((uint32_t)gid != 0xFFFFFFFFu) {
    sa->valid |= VFS_SA_GID;
    sa->gid = (uint32_t)gid;
  }
}

static int vx_chown(struct process *p, struct trap_frame *tf) {
  struct vfs_setattr sa = {0};
  vx_owner(&sa, l_arg(tf, 1), l_arg(tf, 2));
  return vx_ret(tf, vx_apply(p, L_AT_FDCWD, l_arg(tf, 0), 1, 0, &sa));
}

static int vx_lchown(struct process *p, struct trap_frame *tf) {
  struct vfs_setattr sa = {0};
  vx_owner(&sa, l_arg(tf, 1), l_arg(tf, 2));
  return vx_ret(tf, vx_apply(p, L_AT_FDCWD, l_arg(tf, 0), 0, 0, &sa));
}

static int vx_fchown(struct process *p, struct trap_frame *tf) {
  struct vfs_setattr sa = {0};
  vx_owner(&sa, l_arg(tf, 1), l_arg(tf, 2));
  return vx_ret(tf, file_setattr_fd(p, (int)l_argi(tf, 0), &sa));
}

/* fchownat(dirfd, path, uid, gid, flags): AT_SYMLINK_NOFOLLOW, AT_EMPTY_PATH. */
static int vx_fchownat(struct process *p, struct trap_frame *tf) {
  int64_t flags = l_argi(tf, 4);
  if (flags & ~(int64_t)(L_AT_SYMLINK_NOFOLLOW | L_AT_EMPTY_PATH))
    return vx_ret(tf, -EINVAL);
  struct vfs_setattr sa = {0};
  vx_owner(&sa, l_arg(tf, 2), l_arg(tf, 3));
  return vx_ret(tf, vx_apply(p, (int)l_argi(tf, 0), l_arg(tf, 1),
                             !(flags & L_AT_SYMLINK_NOFOLLOW),
                             (flags & L_AT_EMPTY_PATH) != 0, &sa));
}

/* ---- times ---------------------------------------------------------------- */

/* Two (sec, sub) pairs -> sa.  `ns` = 1 for timespec (UTIME_NOW/OMIT legal),
   0 for timeval (microseconds).  NULL = both "now".  0 or -errno. */
static int vx_times(struct process *p, uint64_t utimes, int ns,
                    struct vfs_setattr *sa) {
  sa->valid = 0;
  if (!utimes) {
    sa->valid = VFS_SA_ATIME_NOW | VFS_SA_MTIME_NOW;
    return 0;
  }
  int64_t t[4];
  if (l_copy_from_user(p, t, utimes, sizeof t) != 0) return -EFAULT;
  for (int i = 0; i < 2; i++) {
    int64_t sec = t[2 * i], sub = t[2 * i + 1];
    uint32_t setb = i == 0 ? VFS_SA_ATIME : VFS_SA_MTIME;
    uint32_t nowb = i == 0 ? VFS_SA_ATIME_NOW : VFS_SA_MTIME_NOW;
    if (ns && sub == VX_UTIME_OMIT) continue;
    if (ns && sub == VX_UTIME_NOW) {
      sa->valid |= nowb;
      continue;
    }
    if (sub < 0 || sub >= (ns ? 1000000000l : 1000000l)) return -EINVAL;
    sa->valid |= setb;
    uint32_t nsec = (uint32_t)(ns ? sub : sub * 1000);
    if (i == 0) {
      sa->atime_sec = sec;
      sa->atime_nsec = nsec;
    } else {
      sa->mtime_sec = sec;
      sa->mtime_nsec = nsec;
    }
  }
  return 0;
}

/* utimensat(dirfd, path, times[2], flags).  path == NULL operates on dirfd
   itself (glibc's futimens); both UTIME_OMIT returns 0 before any lookup
   (Linux fs/utimes.c: "we must not even check the path"). */
static int vx_utimensat(struct process *p, struct trap_frame *tf) {
  int dirfd = (int)l_argi(tf, 0);
  uint64_t upath = l_arg(tf, 1);
  uint64_t utimes = l_arg(tf, 2);
  int64_t flags = l_argi(tf, 3);
  struct vfs_setattr sa = {0};
  if (utimes) {
    int64_t t[4];
    if (l_copy_from_user(p, t, utimes, sizeof t) != 0) return vx_ret(tf, -EFAULT);
    if (t[1] == VX_UTIME_OMIT && t[3] == VX_UTIME_OMIT) return vx_ret(tf, 0);
  }
  int r = vx_times(p, utimes, 1, &sa);
  if (r) return vx_ret(tf, r);
  if (flags & ~(int64_t)(L_AT_SYMLINK_NOFOLLOW | L_AT_EMPTY_PATH))
    return vx_ret(tf, -EINVAL);
  if (!upath) {
    if (dirfd == L_AT_FDCWD) return vx_ret(tf, -EFAULT);
    if (flags) return vx_ret(tf, -EINVAL);
    return vx_ret(tf, file_setattr_fd(p, dirfd, &sa));
  }
  return vx_ret(tf, vx_apply(p, dirfd, upath, !(flags & L_AT_SYMLINK_NOFOLLOW),
                             (flags & L_AT_EMPTY_PATH) != 0, &sa));
}

static int vx_utimes(struct process *p, struct trap_frame *tf) {
  struct vfs_setattr sa = {0};
  int r = vx_times(p, l_arg(tf, 1), 0, &sa);
  if (r) return vx_ret(tf, r);
  return vx_ret(tf, vx_apply(p, L_AT_FDCWD, l_arg(tf, 0), 1, 0, &sa));
}

static int vx_futimesat(struct process *p, struct trap_frame *tf) {
  struct vfs_setattr sa = {0};
  int r = vx_times(p, l_arg(tf, 2), 0, &sa);
  if (r) return vx_ret(tf, r);
  return vx_ret(tf, vx_apply(p, (int)l_argi(tf, 0), l_arg(tf, 1), 1, 0, &sa));
}

/* utime(path, struct utimbuf { time_t actime, modtime; }). */
static int vx_utime(struct process *p, struct trap_frame *tf) {
  struct vfs_setattr sa = {0};
  uint64_t ub = l_arg(tf, 1);
  if (!ub) {
    sa.valid = VFS_SA_ATIME_NOW | VFS_SA_MTIME_NOW;
  } else {
    int64_t t[2];
    if (l_copy_from_user(p, t, ub, sizeof t) != 0) return vx_ret(tf, -EFAULT);
    sa.valid = VFS_SA_ATIME | VFS_SA_MTIME;
    sa.atime_sec = t[0];
    sa.mtime_sec = t[1];
  }
  return vx_ret(tf, vx_apply(p, L_AT_FDCWD, l_arg(tf, 0), 1, 0, &sa));
}

/* ---- locks ---------------------------------------------------------------- */

/* Park the calling thread and re-execute the syscall: the same "rewound
   syscall" park every blocking compat row uses (linux_compat_fd.c l_park).
   A pending, unblocked signal ends the wait with -EINTR instead.  The 10 ms
   slice bounds a lost early wakeup; the unlock path wakes waiters at once
   (vfs_lock.c process_wakeup). */
static int vx_park(struct process *p, struct trap_frame *tf) {
  if (linux_compat_sig_deliverable(p)) {
    vfs_lock_unwait(p->pid);
    return vx_ret(tf, -EINTR);
  }
  {
    uint64_t fl = spinlock_acquire_irqsave(&proc_lock);
    if (process_park_ok_locked(p)) { /* C1-k4: never re-park the dead */
      p->wake_ms = timer_get_ms() + 10;
      p->state = PROC_STATE_BLOCKED;
    }
    spinlock_release_irqrestore(&proc_lock, fl);
  }
  if ((tf->cs & 3) == 3) tf->elr -= 2; /* rewind over the 2-byte `syscall` */
  p->sig_park_kind = 2;
  p->sig_park_nr = 0;
  save_context(p, tf);
  schedule(tf, 0);
  return 1;
}

static int64_t vx_ld64(const uint8_t *b) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= (uint64_t)b[i] << (8 * i);
  return (int64_t)v;
}

static void vx_st64(uint8_t *b, int64_t v) {
  for (int i = 0; i < 8; i++) b[i] = (uint8_t)((uint64_t)v >> (8 * i));
}

/* fcntl record locks with Linux's semantics (fs/locks.c):
 *   struct flock (x86-64, 32 bytes): l_type s16 @0, l_whence s16 @2,
 *   l_start s64 @8, l_len s64 @16, l_pid s32 @24.
 *   - SEEK_SET/CUR/END; l_len < 0 covers [start+len, start-1]; 0 = to EOF;
 *   - F_SETLK(W) F_RDLCK needs a readable fd, F_WRLCK a writable one (EBADF);
 *   - F_SETLK conflict -> EAGAIN; F_SETLKW waits (EDEADLK on a POSIX cycle,
 *     EINTR on a signal);
 *   - F_GETLK: F_UNLCK when nothing blocks (other fields untouched), else
 *     the blocker with l_whence = SEEK_SET and l_pid (-1 for an OFD lock);
 *   - OFD commands: l_pid must be 0 on input (EINVAL), owner = the open file
 *     description.
 * Wine 11.0 uses exactly F_SETLK/F_SETLKW/F_GETLK on whole-file 1-byte and
 * range locks: server/request.c:711 (wait_for_lock, F_SETLKW), :735 and :802
 * (F_GETLK of the lock holder's pid), :777 (acquire_lock, F_SETLK on
 * <server dir>/lock), server/fd.c:1283/1289 (set_unix_lock: F_SETLK, then
 * F_GETLK on EACCES), dlls/ntdll/unix/server.c:1382 (F_GETLK). */
static int vx_fcntl(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  int cmd = (int)l_argi(tf, 1);
  uint64_t uarg = l_arg(tf, 2);
  int ofd = (cmd == VX_F_OFD_GETLK || cmd == VX_F_OFD_SETLK ||
             cmd == VX_F_OFD_SETLKW);
  int getlk = (cmd == VX_F_GETLK || cmd == VX_F_OFD_GETLK);
  int wait = (cmd == VX_F_SETLKW || cmd == VX_F_OFD_SETLKW);
  if (!ofd && cmd != VX_F_GETLK && cmd != VX_F_SETLK && cmd != VX_F_SETLKW)
    return 0; /* not a lock command: l_tab_fd answers */

  struct vfs_lock_req rq;
  int desc = 0;
  uint32_t oflags = 0;
  int r = file_lock_target(p, fd, &rq.key, &desc, &oflags);
  if (r) return vx_ret(tf, r);
  uint8_t b[32];
  if (!uarg || l_copy_from_user(p, b, uarg, sizeof b) != 0)
    return vx_ret(tf, -EFAULT);
  int type = (int)(int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
  int whence = (int)(int16_t)((uint16_t)b[2] | ((uint16_t)b[3] << 8));
  int64_t start = vx_ld64(b + 8);
  int64_t len = vx_ld64(b + 16);
  int32_t lpid = (int32_t)((uint32_t)b[24] | ((uint32_t)b[25] << 8) |
                           ((uint32_t)b[26] << 16) | ((uint32_t)b[27] << 24));
  if (type != VFS_LK_RD && type != VFS_LK_WR && type != VFS_LK_UN)
    return vx_ret(tf, -EINVAL);
  if (getlk && type == VFS_LK_UN) return vx_ret(tf, -EINVAL);
  if (ofd && lpid != 0) return vx_ret(tf, -EINVAL);

  int err = 0;
  if (whence == 1) { /* SEEK_CUR */
    int64_t cur = file_seek(p, fd, 0, 1, &err);
    if (cur < 0) return vx_ret(tf, -(err ? err : EINVAL));
    start += cur;
  } else if (whence == 2) { /* SEEK_END */
    struct k_stat ks;
    if (file_stat_fd(p, fd, &ks, &err) != 0) return vx_ret(tf, -(err ? err : EBADF));
    start += (int64_t)ks.st_size;
  } else if (whence != 0) {
    return vx_ret(tf, -EINVAL);
  }
  int64_t end;
  if (len > 0) {
    if (start > VFS_LK_EOF - (len - 1)) return vx_ret(tf, -EOVERFLOW);
    end = start + len - 1;
  } else if (len < 0) {
    end = start - 1;
    start += len;
  } else {
    end = VFS_LK_EOF;
  }
  if (start < 0 || end < start) return vx_ret(tf, -EINVAL);

  struct process *g = process_group(p);
  rq.kind = ofd ? VFS_LK_OFD : VFS_LK_POSIX;
  rq.owner = ofd ? desc : g->pid;
  rq.pid = ofd ? -1 : g->pid;
  rq.type = type;
  rq.start = start;
  rq.end = end;

  if (getlk) {
    struct vfs_lock_info bi;
    if (!vfs_lock_test(&rq, &bi)) {
      b[0] = (uint8_t)VFS_LK_UN;
      b[1] = 0;
    } else {
      b[0] = (uint8_t)bi.type;
      b[1] = 0;
      b[2] = 0; /* SEEK_SET */
      b[3] = 0;
      vx_st64(b + 8, bi.start);
      vx_st64(b + 16, bi.end == VFS_LK_EOF ? 0 : bi.end - bi.start + 1);
      uint32_t up = (uint32_t)bi.pid;
      for (int i = 0; i < 4; i++) b[24 + i] = (uint8_t)(up >> (8 * i));
    }
    return vx_ret(tf, l_copy_to_user(p, uarg, b, sizeof b) == 0 ? 0 : -EFAULT);
  }

  int acc = (int)(oflags & VX_O_ACCMODE);
  if (type == VFS_LK_RD && acc == VX_O_WRONLY) return vx_ret(tf, -EBADF);
  if (type == VFS_LK_WR && acc == VX_O_RDONLY) return vx_ret(tf, -EBADF);

  r = vfs_lock_set(&rq, wait ? p->pid : 0, 0);
  if (r == -EAGAIN && wait) return vx_park(p, tf);
  return vx_ret(tf, r);
}

/* flock(fd, op): whole-file LOCK_SH / LOCK_EX / LOCK_UN (+ LOCK_NB); owner =
   the open file description (shared across dup and fork, released at its
   last close).  Independent of fcntl locks (Linux).  A conversion that
   conflicts keeps the old lock (Linux drops it first; docs/cp-ext2b.md). */
static int vx_flock(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  int op = (int)l_argi(tf, 1);
  int nb = (op & VX_LOCK_NB) != 0;
  int base = op & ~VX_LOCK_NB;
  int type;
  if (base == VX_LOCK_SH) type = VFS_LK_RD;
  else if (base == VX_LOCK_EX) type = VFS_LK_WR;
  else if (base == VX_LOCK_UN) type = VFS_LK_UN;
  else return vx_ret(tf, -EINVAL);
  struct vfs_lock_req rq;
  int desc = 0;
  uint32_t oflags = 0;
  int r = file_lock_target(p, fd, &rq.key, &desc, &oflags);
  if (r) return vx_ret(tf, r);
  rq.kind = VFS_LK_FLOCK;
  rq.owner = desc;
  rq.pid = process_group(p)->pid;
  rq.type = type;
  rq.start = 0;
  rq.end = VFS_LK_EOF;
  int waiter = (!nb && type != VFS_LK_UN) ? p->pid : 0;
  r = vfs_lock_set(&rq, waiter, 0);
  if (r == -EAGAIN && waiter) return vx_park(p, tf);
  return vx_ret(tf, r);
}

const struct l_sysent l_tab_vfsx[] = {
  {L_SYS_chmod, vx_chmod, "chmod"},
  {L_SYS_fchmod, vx_fchmod, "fchmod"},
  {L_SYS_fchmodat, vx_fchmodat, "fchmodat"},
  {VX_SYS_chown, vx_chown, "chown"},
  {L_SYS_fchown, vx_fchown, "fchown"},
  {VX_SYS_lchown, vx_lchown, "lchown"},
  {VX_SYS_fchownat, vx_fchownat, "fchownat"},
  {L_SYS_utimensat, vx_utimensat, "utimensat"},
  {VX_SYS_utimes, vx_utimes, "utimes"},
  {VX_SYS_futimesat, vx_futimesat, "futimesat"},
  {VX_SYS_utime, vx_utime, "utime"},
  {L_SYS_fcntl, vx_fcntl, "fcntl"},
  {VX_SYS_flock, vx_flock, "flock"},
  L_SYSENT_END
};

#endif /* __x86_64__ */
