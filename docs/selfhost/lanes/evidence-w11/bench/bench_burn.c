/* bench_burn.c — w11-makej in-OS synthetic build benchmark helper.
 *
 * Busy-waits for ARGV[1] milliseconds of wall time, read from the kernel's
 * uptime-ms counter (sysinfo command 1).  The synthetic project's make
 * recipes use it as a stand-in for cc1, so each "TU" has a deterministic,
 * calibration-free duration on any CPU (TCG included), and a recipe line
 * that calls it uses the same fork+exec(sh -> BURN) cost shape as a real
 * in-OS compile line.
 *
 * Built for the ARM target with the same commands as the repo's parity_bin
 * pattern (crt0 + libc.a), staged at /SRC/BM/BURN.BIN.  Not part of the
 * disk.img target list; lane evidence only.
 */
#include "libc.h"

int main(int argc, char **argv) {
  int ms = (argc > 1) ? atoi(argv[1]) : 1000;
  int start = sysinfo(1, 0, 0);
  if (start < 0)
    start = 0;
  for (;;) {
    int now = sysinfo(1, 0, 0);
    if (now < 0)
      now = 0;
    if (now - start >= ms)
      break;
  }
  return 0;
}
