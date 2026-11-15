/* tools/jdk/read-bench.c -- measure how long a large file costs on this FAT
 * driver, as a function of offset.
 *
 * The suspicion it exists to test: fat16_read_direct_locked() re-walks the
 * cluster chain from the FIRST cluster on every iteration
 * (get_cluster_for_offset(start, cursor) -> `jumps` read_fat() calls), so
 * reading a file of N clusters costs O(N^2) chain hops.  /JDK/lib/modules is
 * 140 MB; if the walk is quadratic, per-block latency must GROW with offset,
 * and the JVM's "hang" is really a read that will finish in ~an hour.
 *
 * Output is one line per sample, greppable:
 *   READ-BENCH: off=<MB> ms=<per-1MiB-read> mibs=<rate>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
  /* Unbuffered: a probe that hangs must still have said where it was.  A
     block-buffered stdout loses every line when the program is wedged. */
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("READ-BENCH: start\n");
  const char *path = argc > 1 ? argv[1] : "/JDK/lib/modules";
  size_t chunk = (argc > 2 ? (size_t)atoi(argv[2]) : 1) * 1024 * 1024;
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    printf("READ-BENCH: open %s failed errno=%d\n", path, errno);
    return 2;
  }
  off_t size = lseek(fd, 0, SEEK_END);
  printf("READ-BENCH: open %s size=%ld chunk=%lu\n", path, (long)size,
         (unsigned long)chunk);
  char *buf = malloc(chunk);
  if (!buf) {
    printf("READ-BENCH: malloc failed\n");
    return 2;
  }
  /* argv[3]="touch" pre-faults the destination.  A read that CRASHES without
     this and succeeds with it is a kernel-mode #PF on an unmapped user page,
     not a FAT or device defect. */
  if (argc > 3 && strcmp(argv[3], "touch") == 0) {
    memset(buf, 0, chunk);
    printf("READ-BENCH: destination pre-touched\n");
  }
  double t_all = now_ms();
  for (off_t off = 0; off < size; off += (off_t)chunk) {
    size_t want = chunk;
    if (off + (off_t)want > (size_t)size) want = (size_t)((size_t)size - (size_t)off);
    double t0 = now_ms();
    ssize_t got = pread(fd, buf, want, off);
    double dt = now_ms() - t0;
    if (got != (ssize_t)want) {
      printf("READ-BENCH: off=%ld short read %ld/%lu errno=%d\n", (long)off,
             (long)got, (unsigned long)want, errno);
      break;
    }
    printf("READ-BENCH: off=%ldMB ms=%.1f mibs=%.2f\n", (long)(off >> 20), dt,
           (dt > 0 ? (want / 1048576.0) / (dt / 1000.0) : 0.0));
    fflush(stdout);
  }
  double t_total = now_ms() - t_all;
  printf("READ-BENCH: DONE total_ms=%.0f avg_mibs=%.2f\n", t_total,
         t_total > 0 ? ((double)size / 1048576.0) / (t_total / 1000.0) : 0.0);
  close(fd);
  return 0;
}
