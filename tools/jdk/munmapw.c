/* MAP_SHARED write-back on munmap, with NO msync/force: Linux writes dirty
   pages back when the mapping goes away.  Second wave line (READIT) reads the
   file as another process. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(void) {
  mkdir("/JDKTMP", 0755);
  const char *p = "/MU.TXT";
  int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { printf("MUNMAPW cannot create errno=%d\n", errno); return 1; }
  char b[4096]; memset(b, 'A', sizeof b);
  if (write(fd, b, sizeof b) != (ssize_t)sizeof b) { printf("MUNMAPW short write\n"); return 1; }
  char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) { printf("MUNMAPW map failed errno=%d\n", errno); return 1; }
  m[0] = 'M';
  printf("MUNMAPW wrote M@0, in-mapping=%c\n", m[0]);
  if (munmap(m, 4096) != 0) { printf("MUNMAPW munmap errno=%d\n", errno); return 1; }
  printf("MUNMAPW unmapped (no msync)\n");
  close(fd);
  /* Control: a plain file, never mapped.  If READIT cannot see THIS either, the
     problem is cross-process dirent visibility and has nothing to do with mmap. */
  int pf = open("/PLAIN.TXT", O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (pf >= 0) { write(pf, b, sizeof b); close(pf); printf("MUNMAPW wrote PLAIN.TXT\n"); }
  else printf("MUNMAPW PLAIN create failed errno=%d\n", errno);
  return 0;
}
