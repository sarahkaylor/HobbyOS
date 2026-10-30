/* dirfd()/rewinddir()/openat()/fstatat()/faccessat() for the HobbyOS IceWM
 * build.  The repo libc's DIR is path based (src/libc/src/dirent.c keeps
 * `char *path; int index;` as its first members and re-reads entries by
 * index), and the kernel has no directory fds, so:
 *   dirfd(d)      registers d->path and returns a pseudo descriptor
 *                 HB_DIRFD_BASE + slot (never a real fd; close() is not
 *                 needed -- slots are recycled by path);
 *   *at(fd, p)    with an absolute p or AT_FDCWD use the plain call, with
 *                 a pseudo fd join "<dir>/<p>", anything else is EBADF;
 *   rewinddir(d)  resets the entry index.
 * The layout mirror below must track src/libc/src/dirent.c. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AT_FDCWD (-100)
#define HB_DIRFD_BASE 0x4000
#define HB_DIRFD_SLOTS 64

struct hb_dir_view { char *path; int index; }; /* == struct __ho_DIR head */

static char *g_paths[HB_DIRFD_SLOTS];
static int g_next;

int dirfd(DIR *d) {
  if (!d) { errno = EINVAL; return -1; }
  const char *p = ((struct hb_dir_view *)d)->path;
  if (!p) { errno = EINVAL; return -1; }
  for (int i = 0; i < HB_DIRFD_SLOTS; i++)
    if (g_paths[i] && strcmp(g_paths[i], p) == 0) return HB_DIRFD_BASE + i;
  int s = g_next;
  g_next = (g_next + 1) % HB_DIRFD_SLOTS;
  free(g_paths[s]);
  g_paths[s] = strdup(p);
  if (!g_paths[s]) { errno = ENOMEM; return -1; }
  return HB_DIRFD_BASE + s;
}

void rewinddir(DIR *d) {
  if (d) ((struct hb_dir_view *)d)->index = 0;
}

/* Resolves (fd, path) into buf; returns the path to use or NULL (errno). */
static const char *resolve(int fd, const char *path, char *buf, size_t n) {
  if (!path) { errno = EFAULT; return NULL; }
  if (path[0] == '/' || fd == AT_FDCWD) return path;
  int s = fd - HB_DIRFD_BASE;
  if (s < 0 || s >= HB_DIRFD_SLOTS || !g_paths[s]) { errno = EBADF; return NULL; }
  size_t dl = strlen(g_paths[s]);
  int slash = dl > 0 && g_paths[s][dl - 1] == '/';
  if ((size_t)snprintf(buf, n, "%s%s%s", g_paths[s], slash ? "" : "/", path) >= n) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  return buf;
}

int openat(int fd, const char *path, int flags, ...) {
  char buf[1024];
  int mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
  const char *p = resolve(fd, path, buf, sizeof buf);
  return p ? open(p, flags, mode) : -1;
}

int fstatat(int fd, const char *path, struct stat *st, int flags) {
  char buf[1024];
  (void)flags; /* no symlinks on this VFS */
  const char *p = resolve(fd, path, buf, sizeof buf);
  return p ? stat(p, st) : -1;
}

int faccessat(int fd, const char *path, int mode, int flags) {
  char buf[1024];
  (void)flags;
  const char *p = resolve(fd, path, buf, sizeof buf);
  return p ? access(p, mode) : -1;
}
