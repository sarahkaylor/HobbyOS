/*
 * src/kernel/linux_compat_misc.c — the file-API half of the Linux ABI that has
 * no home in the fd/mem/proc lanes: seek, scatter/gather reads, positioned I/O,
 * and the path syscalls that are a straight translation onto the VFS.
 *
 * WHY THIS EXISTS (measured, not planned): upstream Doom on HobbyOS loaded
 * /DOOM1.WAD, printed the Shareware banner and then reported
 * "W_GetNumForName: PNAMES not found!" while the same binary walks the same file
 * on Linux.  A WAD-directory probe (src/user/linux_probe_wad.c) named the
 * reason in one boot:
 *
 *     [LCOMPAT] ENOSYS nr=8            <- lseek
 *     [LP] wad raw size=-1 SHORT
 *     [LP] wad raw seek FAILED got=-1 want=4175796 errno=38
 *     [LP] wad raw first3=<garbage>    <- the directory was read from offset 0
 *
 * i.e. W_AddFile's fseek()/ftell() did nothing, so the lump table came from the
 * top of the file.  `readv` (19) is missing for the same reason and musl's stdio
 * uses it for buffered reads.
 *
 * Every handler here is a translation onto an existing kernel primitive
 * (file_seek / file_read / file_write / vfs_*); none is a second implementation.
 */

#include "linux_compat.h"

#include <stdint.h>

/* The compat track is x86_64-only (docs/linux-abi.md §2); the ARM build compiles
   this TU to nothing so the arch-agnostic kernel link stays whole. */
#ifdef __x86_64__

#include "arch/trap.h"
#include "errno.h"
#include "fs.h"
#include "linux_syscall.h"
#include "process.h"
#include "vfs.h"

/* vfs.h does not declare rmdir (the native handler declares it locally); keep
   the same shape here rather than growing a shared header for one call. */
extern int vfs_rmdir(const char *path);

/* Linux caps a single readv/writev at 1024 iovecs; the same bound the legacy
   writev shim uses, so the two cannot disagree. */
#define L_MISC_MAX_IOV 1024

static int l_misc_ret(struct trap_frame *tf, int64_t v) {
  tf->regs[0] = (uint64_t)v;
  return 1;
}

/* ---- seek ---------------------------------------------------------------- */

static int l_misc_lseek(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  int64_t off = l_argi(tf, 1);
  int whence = (int)l_argi(tf, 2);
  int err = 0;
  int64_t r = file_seek(p, fd, off, whence, &err);
  return l_misc_ret(tf, r < 0 ? -(int64_t)err : r);
}

/* ---- scatter/gather ------------------------------------------------------ */

/* readv(fd, const struct iovec *iov, int cnt).  Parking follows the legacy
   writev contract: a parked segment (file_read returns -2) rewinds the 2-byte
   `syscall` and reschedules so the read RESTARTS on wake; if bytes were already
   transferred this becomes a short read (POSIX allows it). */
static int l_misc_readv(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t iov = l_arg(tf, 1);
  int64_t cnt = l_argi(tf, 2);
  if (cnt < 0 || cnt > L_MISC_MAX_IOV) return l_misc_ret(tf, -EINVAL);
  if (cnt == 0) return l_misc_ret(tf, 0);
  if (!l_user_ok(p, iov, (uint64_t)cnt * 16, 0)) return l_misc_ret(tf, -EFAULT);

  int64_t total = 0;
  for (int64_t i = 0; i < cnt; i++) {
    uint64_t base = 0, len = 0;
    if (l_copy_from_user(p, &base, iov + (uint64_t)i * 16, 8) != 0 ||
        l_copy_from_user(p, &len, iov + (uint64_t)i * 16 + 8, 8) != 0)
      return l_misc_ret(tf, -EFAULT);
    if (len == 0) continue;
    /* One segment is one read; the kernel's file_read takes an int size. */
    while (len > 0) {
      int chunk = len > 0x10000000ULL ? 0x10000000 : (int)len;
      int r = file_read(p, fd, (void *)(uintptr_t)base, chunk, tf);
      if (r == -2) {
        if (total > 0) return l_misc_ret(tf, total);
        if ((tf->cs & 3) == 3) tf->elr -= 2;
        save_context(p, tf);
        schedule(tf, 0);
        return 1;
      }
      if (r < 0) return l_misc_ret(tf, total > 0 ? total : r);
      total += r;
      base += (uint64_t)r;
      len -= (uint64_t)r;
      if (r == 0) break; /* EOF: stop here, a short read is legal */
      if (r < chunk) break;
    }
  }
  return l_misc_ret(tf, total);
}

/* pread64/pwrite64: positioned I/O with no cursor change.  The native layer has
   only a cursor, so save it, do the operation, restore it. */
static int l_misc_pread64(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t buf = l_arg(tf, 1);
  int64_t n = l_argi(tf, 2);
  int64_t off = l_argi(tf, 3);
  if (n < 0) return l_misc_ret(tf, -EINVAL);
  int err = 0;
  int64_t saved = file_seek(p, fd, 0, 1 /* SEEK_CUR */, &err);
  if (saved < 0) return l_misc_ret(tf, -err);
  if (file_seek(p, fd, off, 0 /* SEEK_SET */, &err) < 0)
    return l_misc_ret(tf, -err);
  int r = file_read(p, fd, (void *)(uintptr_t)buf, (int)n, tf);
  if (r == -2) {
    /* Parked: let the restart redo the whole positioned read from `off`. */
    file_seek(p, fd, off, 0, &err);
    if ((tf->cs & 3) == 3) tf->elr -= 2;
    save_context(p, tf);
    schedule(tf, 0);
    return 1;
  }
  file_seek(p, fd, saved, 0, &err);
  return l_misc_ret(tf, r);
}

static int l_misc_pwrite64(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t buf = l_arg(tf, 1);
  int64_t n = l_argi(tf, 2);
  int64_t off = l_argi(tf, 3);
  if (n < 0) return l_misc_ret(tf, -EINVAL);
  int err = 0;
  int64_t saved = file_seek(p, fd, 0, 1, &err);
  if (saved < 0) return l_misc_ret(tf, -err);
  if (file_seek(p, fd, off, 0, &err) < 0) return l_misc_ret(tf, -err);
  int r = file_write(p, fd, (const void *)(uintptr_t)buf, (int)n, tf);
  if (r == -2) {
    file_seek(p, fd, off, 0, &err);
    if ((tf->cs & 3) == 3) tf->elr -= 2;
    save_context(p, tf);
    schedule(tf, 0);
    return 1;
  }
  file_seek(p, fd, saved, 0, &err);
  return l_misc_ret(tf, r);
}

/* ---- path operations ----------------------------------------------------- */

/* Copy a guest path into a kernel buffer.  FAT16 paths here are 8.3 and the VFS
   takes a plain char*, so 256 bytes is generous; a longer path is ENAMETOOLONG
   rather than a truncated name that could unlink the wrong file. */
static int l_misc_path(struct process *p, uint64_t uptr, char *kbuf,
                       uint64_t cap) {
  return l_copy_cstr(p, kbuf, uptr, cap);
}

static int l_misc_unlink(struct process *p, struct trap_frame *tf) {
  char path[256];
  int r = l_misc_path(p, l_arg(tf, 0), path, sizeof path);
  if (r != 0) return l_misc_ret(tf, r);
  int rc = vfs_unlink(path);
  return l_misc_ret(tf, rc < 0 ? -ENOENT : 0);
}

static int l_misc_rename(struct process *p, struct trap_frame *tf) {
  char oldp[256], newp[256];
  int r = l_misc_path(p, l_arg(tf, 0), oldp, sizeof oldp);
  if (r != 0) return l_misc_ret(tf, r);
  r = l_misc_path(p, l_arg(tf, 1), newp, sizeof newp);
  if (r != 0) return l_misc_ret(tf, r);
  int rc = vfs_rename(oldp, newp);
  return l_misc_ret(tf, rc < 0 ? -ENOENT : 0);
}

static int l_misc_rmdir(struct process *p, struct trap_frame *tf) {
  char path[256];
  int r = l_misc_path(p, l_arg(tf, 0), path, sizeof path);
  if (r != 0) return l_misc_ret(tf, r);
  int rc = vfs_rmdir(path);
  return l_misc_ret(tf, rc);
}

static int l_misc_chdir(struct process *p, struct trap_frame *tf) {
  char path[256];
  int r = l_misc_path(p, l_arg(tf, 0), path, sizeof path);
  if (r != 0) return l_misc_ret(tf, r);
  struct process *grp = process_group(p);
  char new_cwd[128];
  if (!grp) return l_misc_ret(tf, -EINVAL);
  if (vfs_chdir(path, new_cwd, sizeof new_cwd) != 0) return l_misc_ret(tf, -ENOENT);
  int k = 0;
  while (new_cwd[k] && k < 127) {
    grp->cwd[k] = new_cwd[k];
    k++;
  }
  grp->cwd[k] = '\0';
  return l_misc_ret(tf, 0);
}

/* getcwd(2) returns the buffer (glibc/musl both expect a non-NULL return on
   success); DOOMWADDIR's fallback "." never calls it, but SDL-era clients do. */
static int l_misc_getcwd(struct process *p, struct trap_frame *tf) {
  uint64_t ubuf = l_arg(tf, 0);
  int64_t size = l_argi(tf, 1);
  struct process *grp = process_group(p);
  if (!grp) return l_misc_ret(tf, -EINVAL);
  int len = 0;
  while (grp->cwd[len]) len++;
  if (size < (int64_t)len + 1) return l_misc_ret(tf, -ERANGE);
  if (l_copy_to_user(p, ubuf, grp->cwd, (uint64_t)len + 1) != 0)
    return l_misc_ret(tf, -EFAULT);
  return l_misc_ret(tf, (int64_t)ubuf);
}

/* No write-back cache to flush: the FAT writer goes straight to the block
   device, so fsync on a regular file is honestly a successful no-op.
   GB-032: on /dev/fb0 it is the present verb — the same arm trap.c's number
   translation has — because linux_compat_dispatch() owns fsync(74) for a
   Linux-ABI process, so that arm never ran and a mapped frame stayed on the
   RAM buffer (measured: a correct draw + exit 0 with a black scanout). */
static int l_misc_fsync(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  extern int file_fd_is_devfb(struct process *, int);
  extern void virtio_gpu_flush(void);
  if (file_fd_is_devfb(p, fd))
    virtio_gpu_flush();
  return l_misc_ret(tf, 0);
}

/* unlinkat(dirfd, path, flags) — AT_FDCWD only, flags 0 or AT_REMOVEDIR
   (0x200); anything else is -EINVAL rather than a silent rmdir. */
static int l_misc_unlinkat(struct process *p, struct trap_frame *tf) {
  int dirfd = (int)l_argi(tf, 0);
  int flags = (int)l_argi(tf, 2);
  if (dirfd != -100 /* AT_FDCWD */) return l_misc_ret(tf, -EBADF);
  if (flags == 0) return l_misc_unlink(p, tf);
  if (flags == 0x200) return l_misc_rmdir(p, tf);
  return l_misc_ret(tf, -EINVAL);
}

/* ---- deliberate refusals ------------------------------------------------ */

/* membarrier(2) and rseq(2) are the two memory-model syscalls a modern glibc
 * probes for and a 2.31 pin does not require.  Both are answered -ENOSYS ON
 * PURPOSE, because that is the protocol each caller implements a fallback for:
 *
 *  - membarrier: glibc's malloc() uses MEMBARRIER_CMD_PRIVATE_EXPEDITED and
 *    falls back to its pre-membarrier algorithm when the kernel says ENOSYS.
 *    HobbyOS has no cross-CPU instruction/TLB sync primitive to translate it
 *    onto, and returning 0 without doing the work would silently weaken the
 *    allocator's write barrier.
 *  - rseq: glibc >= 2.35 registers a restartable sequence per thread and
 *    disables the optimisation on ENOSYS (2.31, the pinned runtime, never asks).
 *    There is no per-thread rseq state in this kernel.
 *
 * Both are table rows rather than "falls through to the default" so the
 * behaviour is explicit and the numbers are named in the golden. */
static int l_misc_membarrier(struct process *p, struct trap_frame *tf) {
  (void)p;
  return l_misc_ret(tf, -ENOSYS);
}

static int l_misc_rseq(struct process *p, struct trap_frame *tf) {
  (void)p;
  (void)tf;
  return l_misc_ret(tf, -ENOSYS);
}

const struct l_sysent l_tab_misc[] = { {L_SYS_lseek, l_misc_lseek, "lseek"}, {L_SYS_readv, l_misc_readv, "readv"}, {L_SYS_pread64, l_misc_pread64, "pread64"}, {L_SYS_pwrite64, l_misc_pwrite64, "pwrite64"}, {L_SYS_unlink, l_misc_unlink, "unlink"}, {L_SYS_unlinkat, l_misc_unlinkat, "unlinkat"}, {L_SYS_rename, l_misc_rename, "rename"}, {L_SYS_rmdir, l_misc_rmdir, "rmdir"}, {L_SYS_chdir, l_misc_chdir, "chdir"}, {L_SYS_getcwd, l_misc_getcwd, "getcwd"}, {L_SYS_fsync, l_misc_fsync, "fsync"}, {L_SYS_membarrier, l_misc_membarrier, "membarrier"}, {L_SYS_rseq, l_misc_rseq, "rseq"},
  L_SYSENT_END
};

#endif /* __x86_64__ */
