/* fsync(2)/fdatasync(2) must flush pages written through a MAP_SHARED mapping
   that nobody has force()d.  Before this, sync/fsync/fdatasync did not exist at
   all, so "make this durable now" was unsayable.  Reads go through a FRESH fd, so
   this is the disk's answer and not the page cache's. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static int disk_first(const char *path, char *out, int n) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  int r = (int)read(fd, out, n);
  close(fd);
  return r;
}

int main(void) {
  const char *p = "/FS.TXT";
  int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { printf("FSYNC cannot create errno=%d\n", errno); return 1; }
  char b[4096]; memset(b, 'A', sizeof b);
  if (write(fd, b, sizeof b) != (ssize_t)sizeof b) { printf("FSYNC short write\n"); return 1; }
  char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) { printf("FSYNC map failed errno=%d\n", errno); return 1; }

  m[0] = 'N';
  int e1 = fsync(fd);
  char d1[8] = {0}; int n1 = disk_first(p, d1, 4);
  printf("FSYNC after fsync:  rc=%d errno=%d disk[0]=%c (want N)\n", e1, errno, n1 > 0 ? d1[0] : '?');

  m[1] = 'X';
  int e2 = fdatasync(fd);
  char d2[8] = {0}; int n2 = disk_first(p, d2, 4);
  printf("FSYNC after fdatasync: rc=%d errno=%d disk[1]=%c (want X)\n", e2, errno, n2 > 1 ? d2[1] : '?');

  int pass = (e1 == 0) && (n1 > 0) && d1[0] == 'N' && (e2 == 0) && (n2 > 1) && d2[1] == 'X';
  printf("FSYNC VERDICT %s\n", pass ? "PASS" : "FAIL");
  return 0;
}
