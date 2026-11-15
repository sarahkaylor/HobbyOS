/* Creates six files, each closed before the next, and says so.  A second process
   (READFILES) then tries to read all six.  This characterises "a file created by
   one process is only sometimes visible to the next one in the same boot"
   (munmapwb5 saw its control file, munmapwb6 did not see one made seconds
   earlier) -- a build toolchain that writes then re-reads would hit this. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
  char payload[8];
  memset(payload, 'A', sizeof payload);
  for (int i = 0; i < 6; i++) {
    char path[32];
    snprintf(path, sizeof path, "/FL%d.TXT", i);
    payload[0] = (char) ('A' + i);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("MKF %d create-failed errno=%d\n", i, errno); continue; }
    ssize_t w = write(fd, payload, sizeof payload);
    close(fd);
    /* in-process proof that the create+write worked at all */
    int rf = open(path, O_RDONLY);
    char back[8] = {0};
    int n = rf >= 0 ? (int)read(rf, back, sizeof back) : -1;
    if (rf >= 0) close(rf);
    printf("MKF %d wrote=%d inproc=%d first=%c\n", i, (int)w, n,
           n > 0 ? back[0] : '?');
  }
  /* Discriminator: if an explicit sync() makes the six dirents visible to the
     next process, the defect is a deferred flush and sync() is the fix.  If not,
     the reader is being served a stale cached directory sector. */
  sync();
  printf("MKF sync() returned\n");
  printf("MKF DONE created=6\n");
  return 0;
}
