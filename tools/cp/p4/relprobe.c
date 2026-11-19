/* tools/cp/p4/relprobe.c - the kernel half of P4's relative pointer motion,
 * checked without X (lane C4-x2).
 *
 * A static-PIE Linux program (musl-gcc, like tools/cp/p2/pfxls.c) that opens
 * /dev/input/event0 -- the compat evdev node, fed by the same kernel ring the
 * X server drains -- prints "[REL] ready", and then sums what arrives for
 * `secs` seconds (default 25): REL_X/REL_Y (the raw PS/2 deltas the decoder
 * now reports) and the last ABS_X/ABS_Y.  tools/cp/p4/relmouse.py boots it
 * in a MODE=test wave and injects QMP "rel" motion after the ready row; the
 * sums must equal what it injected, including the motion that pushes the
 * integrated ABS position into the screen edge.
 *
 *   [REL] ready
 *   [REL] ev type=2 code=0 value=7        (the first 40 events)
 *   [REL] total rel_x=<sum> rel_y=<sum> nrel=<n> abs_x=<last> abs_y=<last> nabs=<n>
 *   [REL] done rc=0
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct ev { int64_t sec, usec; uint16_t type, code; int32_t value; };

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
  int secs = argc > 1 ? atoi(argv[1]) : 25;
  int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
  if (fd < 0) { printf("[REL] FAIL open /dev/input/event0\n[REL] done rc=1\n"); return 1; }
  /* drain whatever the boot left in the ring */
  struct ev b[64];
  for (int i = 0; i < 64 && read(fd, b, sizeof b) > 0; i++) {}
  printf("[REL] ready\n");
  fflush(stdout);
  long long rx = 0, ry = 0, nrel = 0, nabs = 0, shown = 0;
  long ax = -1, ay = -1;
  int64_t t0 = now_ms();
  while (now_ms() - t0 < secs * 1000LL) {
    ssize_t n = read(fd, b, sizeof b);
    if (n <= 0) { usleep(5000); continue; }
    for (int i = 0; i < (int)(n / (ssize_t)sizeof b[0]); i++) {
      if (shown < 40 && b[i].type != 0) {
        printf("[REL] ev type=%u code=%u value=%d\n", b[i].type, b[i].code, b[i].value);
        shown++;
      }
      if (b[i].type == 2 && b[i].code == 0) { rx += b[i].value; nrel++; }
      else if (b[i].type == 2 && b[i].code == 1) { ry += b[i].value; nrel++; }
      else if (b[i].type == 3 && b[i].code == 0) { ax = b[i].value; nabs++; }
      else if (b[i].type == 3 && b[i].code == 1) { ay = b[i].value; nabs++; }
    }
  }
  printf("[REL] total rel_x=%lld rel_y=%lld nrel=%lld abs_x=%ld abs_y=%ld nabs=%lld\n",
         rx, ry, nrel, ax, ay, nabs);
  printf("[REL] done rc=0\n");
  fflush(stdout);
  close(fd);
  return 0;
}
