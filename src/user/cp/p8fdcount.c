/* src/user/cp/p8fdcount.c - lane C8-game (docs/cp-p8.md, stop 2): how many
 * more files can be opened right now.  A static musl program the P8 launcher
 * runs at census points: it opens /dev/null up to CAP times, reports how many
 * opens succeeded and the errno of the first failure, then closes them all.
 *
 * Its own fd table starts nearly empty (stdin/out/err), so:
 *   opened == CAP                 nothing is near a limit
 *   opened ~= MAX_OPEN_FDS - 3    the per-process table stopped it
 *   opened <  that, errno EMFILE  the SYSTEM-WIDE file table (MAX_GLOBAL_FILES)
 *                                 is what is left: HobbyOS answers EMFILE for
 *                                 both (fs.c file_alloc failure), Linux would
 *                                 say ENFILE for the second.
 * usage: p8fdcount [CAP]   (default 600)
 * Row: "[P8FD] cap=<n> opened=<n> first_errno=<n>"
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int cap = argc > 1 ? atoi(argv[1]) : 600;
  if (cap < 1 || cap > 4096) cap = 600;
  static int fds[4096];
  int n = 0, err = 0;
  while (n < cap) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) { err = errno; break; }
    fds[n++] = fd;
  }
  for (int i = 0; i < n; i++) close(fds[i]);
  printf("[P8FD] cap=%d opened=%d first_errno=%d%s\n", cap, n, err,
         err == EMFILE ? " (EMFILE)" : err == ENFILE ? " (ENFILE)" : "");
  return 0;
}
