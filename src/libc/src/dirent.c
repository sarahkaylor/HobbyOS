#include <dirent.h>
#include <errno.h>

extern int errno;

/* HobbyOS sysroot: directory iteration stubs.
 *
 * grep -r and friends compile against these; until the kernel grows a
 * directory-listing syscall pair, every entry point fails with ENOSYS so
 * callers report an error rather than an empty directory. */

DIR *opendir(const char *name) {
  (void)name;
  errno = ENOSYS;
  return 0;
}

struct dirent *readdir(DIR *dirp) {
  (void)dirp;
  errno = ENOSYS;
  return 0;
}

int closedir(DIR *dirp) {
  (void)dirp;
  errno = ENOSYS;
  return -1;
}
