/* tools/cp/pfx/pfxsync.c - the prefix-on-ext2 boots' last step (lane C3-pfx):
 * a musl Linux program the native winep2 launcher runs before it exits, so
 * sync(2) -- ext2 write-through plus MAP_SHARED write-back -- is called by a
 * compat process (the native ABI has no sync).  Prints "[PFX] sync rc=<n>". */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void) {
  long r = syscall(SYS_sync);
  printf("[PFX] sync rc=%ld errno=%d\n", r, r ? errno : 0);
  return r ? 1 : 0;
}
