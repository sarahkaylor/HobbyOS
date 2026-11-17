/* tools/cp/sync/munmapwb.c -- cp-sync: main's GB-308 munmap write-back on the
 * merged tree, in ONE process (a wave runs its lines concurrently, so main's
 * MUNMAPW + READIT pair races; measured READIT before MUNMAPW).
 *
 *   W1  MAP_SHARED write, munmap with no msync, read back through a FRESH fd:
 *       the byte must be on the file (mem_writeback_range, region walk, frame
 *       through phys_to_virt).
 *   W2  the same, but the munmap span also covers a 16 GiB PROT_NONE
 *       reservation and the hole between them -- the shape Wine unmaps.  The
 *       write-back must still land, and the call must be quick (a per-page
 *       walk of 16 GiB is 4M region lookups).
 *   W3  a private mapping's write must NOT reach the file.
 * Build: like tools/jdk/build-probes.sh (glibc 2.31 PIE).  Stage on the FAT
 * root; prints "MUNMAPWB VERDICT PASS|FAIL".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static long ms_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int make_file(const char *p, char fill) {
  int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  char b[8192];
  memset(b, fill, sizeof b);
  if (write(fd, b, sizeof b) != (ssize_t)sizeof b) { close(fd); return -1; }
  return fd;
}

static char disk_byte(const char *p, off_t off) {
  int fd = open(p, O_RDONLY);
  if (fd < 0) return '?';
  char c = '?';
  if (pread(fd, &c, 1, off) != 1) c = '?';
  close(fd);
  return c;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int ok = 1;

  /* W1 */
  int fd = make_file("/MWB1.TXT", 'A');
  char *m = fd < 0 ? MAP_FAILED : mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) { printf("MUNMAPWB W1 setup failed errno=%d\n", errno); return 1; }
  m[0] = 'M';
  m[4096 + 7] = 'N';
  int r = munmap(m, 8192);
  char c0 = disk_byte("/MWB1.TXT", 0), c1 = disk_byte("/MWB1.TXT", 4096 + 7);
  int w1 = (r == 0 && c0 == 'M' && c1 == 'N');
  printf("MUNMAPWB W1 munmap=%d disk[0]=%c disk[4103]=%c %s\n", r, c0, c1, w1 ? "ok" : "BAD");
  ok &= w1;
  close(fd);

  /* W2: [file map][hole][16 GiB PROT_NONE reservation], one munmap over it all */
  fd = make_file("/MWB2.TXT", 'B');
  const size_t big = 16UL << 30;
  char *res = mmap(0, big + (1UL << 30), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (res == MAP_FAILED) { printf("MUNMAPWB W2 reserve failed errno=%d\n", errno); return 1; }
  /* carve: keep [res, res+8K) for the file, leave a 1 GiB hole, then 16 GiB reserved */
  munmap(res + 8192, (1UL << 30) - 8192);
  char *f2 = mmap(res, 8192, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
  if (f2 == MAP_FAILED) { printf("MUNMAPWB W2 map failed errno=%d\n", errno); return 1; }
  f2[100] = 'W';
  long t0 = ms_now();
  r = munmap(res, big + (1UL << 30));
  long dt = ms_now() - t0;
  char c2 = disk_byte("/MWB2.TXT", 100);
  int w2 = (r == 0 && c2 == 'W' && dt < 2000);
  printf("MUNMAPWB W2 munmap(17 GiB span)=%d ms=%ld disk[100]=%c %s\n", r, dt, c2, w2 ? "ok" : "BAD");
  ok &= w2;
  close(fd);

  /* W3 */
  fd = make_file("/MWB3.TXT", 'C');
  char *p3 = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p3 == MAP_FAILED) { printf("MUNMAPWB W3 map failed errno=%d\n", errno); return 1; }
  p3[0] = 'P';
  munmap(p3, 4096);
  char c3 = disk_byte("/MWB3.TXT", 0);
  int w3 = (c3 == 'C');
  printf("MUNMAPWB W3 private disk[0]=%c %s\n", c3, w3 ? "ok" : "BAD");
  ok &= w3;
  close(fd);

  printf("MUNMAPWB VERDICT %s\n", ok ? "PASS" : "FAIL");
  return 0;
}
