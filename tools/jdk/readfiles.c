/* Reads the six files MKFILES created in an earlier wave line (a different
   process, same boot).  Reports each one so a partial-visibility pattern shows
   up instead of a single yes/no. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
  int found = 0;
  for (int i = 0; i < 6; i++) {
    char path[32];
    snprintf(path, sizeof path, "/FL%d.TXT", i);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("RF %d MISSING\n", i); continue; }
    char b[8] = {0};
    int n = read(fd, b, sizeof b);
    close(fd);
    printf("RF %d n=%d first=%c\n", i, n, n > 0 ? b[0] : '?');
    if (n == 8 && b[0] == (char)('A' + i)) found++;
  }
  printf("RF VERDICT found=%d/6 %s\n", found,
         found == 6 ? "all-visible" : "PARTIAL-VISIBILITY");
  return 0;
}
