/* A writable MAP_SHARED mapping of an O_RDONLY fd must fail with EACCES; the
   read-only and MAP_PRIVATE cases must still work.  Java cannot test this --
   FileChannel rejects a non-writable mode before any syscall -- so it is native. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <sys/stat.h>

int main(void) {
  const char *path = "/JDKTMP/ro.txt";
  mkdir("/JDKTMP", 0755);   /* a rebuild recreates disk.img and wipes this */
  int wfd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (wfd < 0) { printf("MMAPRO cannot create errno=%d\n", errno); return 1; }
  char buf[4096];
  memset(buf, 'A', sizeof buf);
  if (write(wfd, buf, sizeof buf) != (ssize_t)sizeof buf) { printf("MMAPRO short write\n"); return 1; }
  close(wfd);

  int fd = open(path, O_RDONLY);
  if (fd < 0) { printf("MMAPRO cannot open ro errno=%d\n", errno); return 1; }

  errno = 0;
  void *p1 = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  int e1 = errno;
  printf("MMAPRO shared+write on O_RDONLY: %s errno=%d\n",
         p1 == MAP_FAILED ? "refused" : "GRANTED", e1);

  errno = 0;
  void *p2 = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
  int okRead = (p2 != MAP_FAILED);
  if (okRead) munmap(p2, 4096);
  errno = 0;
  void *p3 = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  int okPriv = (p3 != MAP_FAILED);
  if (okPriv) munmap(p3, 4096);

  printf("MMAPRO shared+read=%s private+write=%s\n",
         okRead ? "ok" : "BROKEN", okPriv ? "ok" : "BROKEN");
  /* Empty file: the page-cache object refuses it, and the caller used to get
     -ENOMEM ("out of memory" for a file with no pages).  ENODEV is actionable. */
  const char *ep = "/JDKTMP/empty.txt";
  int efd = open(ep, O_RDWR | O_CREAT | O_TRUNC, 0644);
  int okEmpty = 0, eEmpty = 0;
  if (efd >= 0) {
    errno = 0;
    void *p4 = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, efd, 0);
    eEmpty = errno;
    okEmpty = (p4 == MAP_FAILED);
    if (!okEmpty) munmap(p4, 4096);
    printf("MMAPRO empty-file map: %s errno=%d (ENODEV=%d ENOMEM=%d)\n",
           okEmpty ? "refused" : "GRANTED", eEmpty, ENODEV, ENOMEM);
    close(efd);
  }
  printf("MMAPRO VERDICT %s\n",
         (p1 == MAP_FAILED && e1 == EACCES && okRead && okPriv
          && okEmpty && eEmpty == ENODEV) ? "PASS" : "FAIL");
  return 0;
}
