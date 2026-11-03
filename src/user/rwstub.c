/*
 * rwstub.c — a MEASUREMENT STAND-IN, not part of the game and never claimed
 * to be.  Built as an ET_DYN PIE with NEEDED UnityPlayer.so and
 * RUNPATH $ORIGIN, staged at /NFS/RWTEST.ELF, and launched by the same
 * rwlaunch binary the real game uses.
 *
 * Why it exists: the real RimWorldLinux is an ET_EXEC linked at 0x200000 and
 * HobbyOS's compat loader refuses it before any userspace runs (see
 * docs/rimworld-launch-log.md §2).  That refusal hides the NEXT wall, which
 * only appears once glibc's ld.so starts resolving the game's NEEDED list.
 * This stub reproduces exactly that step — same DSO, same directory, same
 * loader — with a main() that says whether it got there.  If ld.so cannot map
 * UnityPlayer.so, the stub never prints and ld.so's own message is the
 * evidence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  printf("[STUB] main reached argc=%d argv[0]=%s\n", argc, argv[0]);
  printf("[STUB] HOME=%s LD_LIBRARY_PATH=%s\n", getenv("HOME"),
         getenv("LD_LIBRARY_PATH"));
  fflush(stdout);
  return 0;
}
