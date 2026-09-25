#ifndef HOBBYOS_DIRENT_H
#define HOBBYOS_DIRENT_H

/* HobbyOS sysroot: <dirent.h> — directory iteration surface for ported
 * GNU tools.  The FAT16 filesystem can list directories; the kernel syscall
 * pair behind opendir/readdir is still to come, so the current
 * implementation reports ENOSYS and callers (e.g. grep -r) fail loudly
 * instead of silently seeing empty directories. */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

  typedef struct __ho_DIR DIR;

  struct dirent {
    unsigned long d_ino;    /* inode number (0 on FAT16) */
    char d_name[256];       /* NUL-terminated file name */
  };

  DIR *opendir(const char *name);
  struct dirent *readdir(DIR *dirp);
  int closedir(DIR *dirp);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_DIRENT_H */
