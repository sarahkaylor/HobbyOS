/* Holds an exclusive fcntl write lock on argv[1] for N seconds (argv[2]),
   printing when it is held and when it is released.  The peer that proves
   F_SETLKW blocks: LockWait's measure is only meaningful if someone else
   really holds the region. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/JDKTMP/lk.txt";
  int hold_s = argc > 2 ? atoi(argv[2]) : 45;
  int fd = open(path, O_RDWR | O_CREAT, 0644);
  if (fd < 0) { printf("LOCKHOLD open failed\n"); return 1; }
  struct flock fl;
  memset(&fl, 0, sizeof fl);
  fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 1;
  printf("LOCKHOLD trying\n");
  fflush(stdout);
  if (fcntl(fd, F_SETLKW, &fl) != 0) { printf("LOCKHOLD setlkw failed\n"); return 2; }
  printf("LOCKHOLD held for %ds\n", hold_s);
  fflush(stdout);
  struct timespec ts = { hold_s, 0 };
  while (nanosleep(&ts, &ts) != 0) { }
  fl.l_type = F_UNLCK;
  fcntl(fd, F_SETLK, &fl);
  printf("LOCKHOLD released\n");
  fflush(stdout);
  close(fd);
  return 0;
}
