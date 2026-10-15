#ifndef HOBBYOS_DIRENT_H
#define HOBBYOS_DIRENT_H

/* HobbyOS sysroot: <dirent.h> — directory iteration surface for ported
 * GNU tools.  opendir/readdir/closedir are implemented in dirent.c over
 * the kernel's SYS_READ_DIR (row 15) — the same syscall the legacy
 * read_dir() helper wraps.  The kernel caps entry names at 31 chars
 * (FROZEN per AD-13; see sys_read_dir in both arch trap.c files), so
 * d_name's 256-byte buffer always accepts the longest kernel name. */

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
