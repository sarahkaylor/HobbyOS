#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include "syscall.h"

/* HobbyOS sysroot: dirent — directory iteration over the kernel's read_dir
 * syscall (SYS_READ_DIR, row 15).
 *
 * The kernel side was already live (vfs_read_dir -> fat16_read_dir returns
 * one entry per index); this layer was shipping ENOSYS stubs, which blocked
 * GNU make wildcards/VPATH, grep -r (savedir), and mtools `mcopy -s`
 * recursion — every ported tool that lists a directory.  It is wired here
 * directly to the syscall through the standard dual-arch helper, so no
 * dependency on user-side wrappers.
 *
 * Semantics follow the kernel ABI: read_dir(path, index, ent) returns 0 and
 * fills {name, attr, size} for entry `index`, or a negative value past the
 * end of the listing.  The name is capped at 31 chars by the kernel (the
 * FROZEN AD-13 31-char cap); dirent.d_name is a 256-byte buffer so the
 * LONGEST kernel-provided name always fits.
 *
 * opendir() validates that `path` is a directory via stat(2) first (read_dir
 * has no "is-a-directory" probe of its own; a non-directory path or an empty
 * directory both enumerate as zero entries, and POSIX requires ENOTDIR for
 * the former).  readdir() returns the entries in kernel listing order; NULL
 * at end-of-listing with errno cleared (Linux convention).
 *
 * Under HOST_TEST keep the ENOSYS stubs like stat.c: there is no kernel on
 * the host, and host ports link glibc's real dirent instead.
 */

#ifndef HOST_TEST

extern int errno;

/* Dual-arch syscall helper (aarch64 svc #0 / x86_64 syscall), matching
 * user/libc.c's ABI: args in x0-x4 / rdi,rsi,rdx,r10,r8. */
static long hb_syscall4(long num, long a0, long a1, long a2, long a3) {
#ifdef __x86_64__
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                   : "memory");
  return x0;
#endif
}

#include "syscall.h"

/* The kernel's on-wire directory-entry layout (sys_read_dir's local_dirent):
 * a 31-char name, attr byte, and 32-bit size.  Mirrors sys_dirent in
 * user_include/libc.h; declared here so dirent.c does not reach into the
 * legacy umbrella header. */
struct hb_sys_dirent {
  char name[32];
  uint8_t attr;
  uint32_t size;
} __attribute__((packed));

struct __ho_DIR {
  char *path;        /* heap copy of the directory path */
  int index;         /* next entry index to fetch from the kernel */
  struct dirent ent; /* one-entry return slot (POSIX) */
};

DIR *opendir(const char *name) {
  if (!name) {
    errno = EINVAL;
    return NULL;
  }
  /* POSIX requires ENOTDIR when `name` is not a directory; read_dir alone
     cannot tell ENOTDIR from an empty directory, so use stat(2). */
  {
    struct stat st;
    if (stat(name, &st) != 0)
      return NULL; /* errno already set (ENOENT/EIO/...) */
    if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return NULL;
    }
  }

  size_t len = strlen(name);
  DIR *d = (DIR *)malloc(sizeof *d);
  if (!d) {
    errno = ENOMEM;
    return NULL;
  }
  d->path = (char *)malloc(len + 1);
  if (!d->path) {
    free(d);
    errno = ENOMEM;
    return NULL;
  }
  memcpy(d->path, name, len + 1);
  d->index = 0;
  memset(&d->ent, 0, sizeof d->ent);
  return d;
}

struct dirent *readdir(DIR *dirp) {
  if (!dirp) {
    errno = EINVAL;
    return NULL;
  }
  struct hb_sys_dirent raw;
  long r = hb_syscall4(SYS_READ_DIR, (long)dirp->path, (long)dirp->index,
                       (long)&raw, 0);
  if (r != 0) {
    /* End of listing: return NULL with errno clear (Linux convention;
       make's dir cache and gnulib savedir both treat errno==0 as EOF). */
    errno = 0;
    return NULL;
  }
  dirp->index++;

  /* Copy the 31-char-capped kernel name into the 256-byte d_name slot. */
  size_t k = 0;
  while (raw.name[k] && k < sizeof dirp->ent.d_name - 1)
    k++;
  memcpy(dirp->ent.d_name, raw.name, k);
  dirp->ent.d_name[k] = '\0';
  dirp->ent.d_ino = 0; /* FAT16 has no inode numbers */
  return &dirp->ent;
}

int closedir(DIR *dirp) {
  if (!dirp) {
    errno = EINVAL;
    return -1;
  }
  free(dirp->path);
  free(dirp);
  return 0;
}

#else /* HOST_TEST */

DIR *opendir(const char *name) {
  (void)name;
  errno = ENOSYS;
  return NULL;
}

struct dirent *readdir(DIR *dirp) {
  (void)dirp;
  errno = ENOSYS;
  return NULL;
}

int closedir(DIR *dirp) {
  (void)dirp;
  errno = ENOSYS;
  return -1;
}

#endif /* HOST_TEST */
