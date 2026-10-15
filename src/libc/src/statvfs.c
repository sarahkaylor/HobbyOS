/*
 * HobbyOS selfhost W2-6: statvfs/fstatvfs over the kernel's volume
 * statistics (sysinfo cmd 7: total/free bytes for the cwd's filesystem).
 *
 * The VFS (FAT16 local volume + optional NFS mounts) has no inode or
 * permission-bit accounting, so the honest report is:
 *   f_bsize/f_frsize 512 (sector granularity at minimum allocation)
 *   f_blocks/f_bfree/f_bavail derived from the kernel's byte counts
 *   f_files/f_ffree/f_favail 0 — no inode tables exist
 *   f_flag MNT_LOCAL for the FAT root (the smoke path's cwd); an NFS cwd
 *     would ideally clear it, but cmd 7 reports bytes only, so the flag
 *     stays LOCAL (W2-17 tracks finer per-mount reporting)
 *   f_namemax 255 (VFAT long names)
 * fstatvfs has no fd->path lookup in this VFS; the volume-wide totals are
 * the same honest answer, so it shares statvfs's body.
 *
 * Under HOST_TEST there is no kernel — the hb_* host unit sees ENOSYS
 * stubs (host ports link glibc's real statvfs instead).
 */
#include <sys/statvfs.h>
#include <errno.h>
#include <string.h>

#ifdef HOST_TEST
#define HB_NAME(name) hb_##name
#else
#define HB_NAME(name) name
#endif

#ifndef HOST_TEST

#include "syscall.h"

/* Dual-arch syscall helper (aarch64 svc #0 / x86_64 syscall), matching
 * user/libc.c's ABI. */
static long hb_syscall3(long num, long a0, long a1, long a2) {
#ifdef __x86_64__
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2)
                   : "memory");
  return x0;
#endif
}

/* sysinfo cmd 7 (src/user_include/libc.h, struct sys_fsinfo): volume
 * statistics for the filesystem containing the cwd. */
static int hb_fsinfo(void *buf, size_t size) {
  return (int)hb_syscall3(SYS_SYSINFO, 7, (long)buf, (long)size);
}

#endif /* !HOST_TEST */

struct hb_vol_stats {
  unsigned long long total_bytes;
  unsigned long long free_bytes;
};

#ifdef HOST_TEST

/* No kernel on the host: honest ENOSYS (host ports link glibc's own
 * statvfs/fstatvfs; these hb_* only exist so the unit compiles). */
int HB_NAME(statvfs)(const char *path, struct statvfs *buf) {
  (void)path;
  if (!buf) {
    errno = EFAULT;
    return -1;
  }
  errno = ENOSYS;
  return -1;
}

int HB_NAME(fstatvfs)(int fd, struct statvfs *buf) {
  (void)fd;
  if (!buf) {
    errno = EFAULT;
    return -1;
  }
  errno = ENOSYS;
  return -1;
}

#else /* !HOST_TEST */

static int hb_fill(struct statvfs *buf) {
  struct hb_vol_stats fs;

  memset(buf, 0, sizeof(*buf));
  if (hb_fsinfo(&fs, sizeof fs) != 0 || fs.total_bytes == 0) {
    /* Volume unknown: an all-zero report is the honest "don't know" (LLVM
     * reads it as a zero-size volume, which only affects disk_space()'s
     * "Is there enough space?" double-check). */
    return 0;
  }
  buf->f_bsize = 512;
  buf->f_frsize = 512;
  buf->f_blocks = (unsigned long)(fs.total_bytes / 512);
  buf->f_bfree = (unsigned long)(fs.free_bytes / 512);
  buf->f_bavail = buf->f_bfree;
  buf->f_flag = MNT_LOCAL;  /* the FAT root volume is local */
  buf->f_flags = buf->f_flag;
  buf->f_namemax = 255;     /* VFAT long names */
  return 0;
}

int HB_NAME(statvfs)(const char *path, struct statvfs *buf) {
  (void)path; /* volume-wide totals; the path only picks the volume */
  if (!buf) {
    errno = EFAULT;
    return -1;
  }
  return hb_fill(buf);
}

int HB_NAME(fstatvfs)(int fd, struct statvfs *buf) {
  (void)fd; /* no fd->path lookup in this VFS; see the header comment */
  if (!buf) {
    errno = EFAULT;
    return -1;
  }
  return hb_fill(buf);
}

#endif /* !HOST_TEST */
