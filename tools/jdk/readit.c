/* Reads /MU.TXT as a separate process: 'M' means munmap wrote back. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
  int fd = open("/MU.TXT", O_RDONLY);
  if (fd < 0) { printf("READIT absent\n"); return 1; }
  char b[4096];
  int n = read(fd, b, sizeof b);
  close(fd);
  printf("READIT n=%d first=%c VERDICT %s\n", n, n > 0 ? b[0] : '?',
         (n == 4096 && b[0] == 'M') ? "PASS-munmap-wrote-back" : "FAIL-page-lost");
  int pf = open("/PLAIN.TXT", O_RDONLY);
  if (pf < 0) { printf("READIT PLAIN absent (cross-process visibility broken)\n"); }
  else {
    char c[8]; int m = read(pf, c, sizeof c); close(pf);
    printf("READIT PLAIN n=%d first=%c (control: never mapped)\n", m, m > 0 ? c[0] : '?');
  }
  return 0;
}
