/* tools/jdk/dir-probe.c -- the directory-enumeration ABI, one call per line.
 *
 * Files.list() on every directory failed with NoSuchFileException, including
 * "/".  That is either the kernel refusing opendir or the JDK disagreeing about
 * the flags, and the two live on different sides of the syscall.  This probe is
 * the syscall side: exactly what libc's opendir() does (open with
 * O_RDONLY|O_DIRECTORY|O_CLOEXEC, then getdents64 to EOF), plus the
 * without-O_DIRECTORY form Linux accepts too.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#ifndef SYS_getdents64
#define SYS_getdents64 217
#endif
#include <unistd.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 00200000
#endif

struct linux_dirent64 {
  unsigned long long d_ino;
  long long d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

static void probe(const char *path) {
  /* 1. opendir form. */
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  printf("DIR %-12s opendir fd=%d errno=%d\n", path, fd, fd < 0 ? errno : 0);
  if (fd >= 0) {
    char buf[1024];
    int total = 0, names = 0;
    for (int guard = 0; guard < 64; guard++) {
      long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
      if (n <= 0) {
        if (n < 0) printf("DIR %-12s getdents errno=%d\n", path, errno);
        break;
      }
      total += (int)n;
      for (int off = 0; off < (int)n;) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
        if (d->d_reclen == 0) break;
        names++;
        if (names <= 6) printf("DIR %-12s   - %s (type %d)\n", path, d->d_name, d->d_type);
        off += d->d_reclen;
      }
      if (names > 6) break;
    }
    printf("DIR %-12s entries_first_page=%d bytes=%d\n", path, names, total);
    close(fd);
  }

  /* 2. Plain O_RDONLY open of a directory: Linux allows it and fails read()
     with EISDIR; a program that only uses it as a dirfd must still work. */
  int fd2 = open(path, O_RDONLY);
  printf("DIR %-12s plain   fd=%d errno=%d\n", path, fd2, fd2 < 0 ? errno : 0);
  if (fd2 >= 0) close(fd2);
}

int main(void) {
  /* O_DIRECTORY must be the octAL 00200000 = 0x10000 the kernel expects. */
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("DIRPROBE start\n");
  probe("/");
  probe("/JDK");
  probe("/JDK/lib");
  probe("/C");
  probe("/JDKTMP");
  probe("/nope-not-here");
  printf("DIRPROBE O_DIRECTORY=0x%X\n", (unsigned)O_DIRECTORY);
  printf("DIRPROBE done\n");
  return 0;
}
