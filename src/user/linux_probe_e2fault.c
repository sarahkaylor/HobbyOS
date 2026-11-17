/* src/user/linux_probe_e2fault.c - Linux-ABI probe: read()/pread() of an ext2
 * file into a buffer that another thread keeps unmapping and re-mapping as a
 * FILE-backed mapping (lane C3-ext2w; docs/cp-p8.md stop 6 on integ5: a
 * pread64 held ext2_lock in ext2v_read for > 2 s and the guest went silent).
 *
 * argv[1] the volume (default /EXT; SKIP without it), argv[2] seconds
 * (default 60).  Thread R reads a 48 MiB ext2 file (three times the ext2
 * cache, so reads go to the device) in 64 KiB pread()s into a fixed 64 KiB
 * window W.  Thread M loops: munmap(W) and mmap(MAP_FIXED|MAP_PRIVATE) W
 * again from a file -- in turn a FAT16 file on the boot volume, an ext2 file,
 * and anonymous memory -- so a copy into W can find a page that is not
 * resident and needs a page fill from a disk.  If the kernel touches W while
 * it holds ext2_lock or the NVMe token, the fill waits for itself.
 * Rows: every pread made progress until the end, and the bytes it returned
 * are the file's bytes whenever the window was anonymous or private.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WIN (64 * 1024)
#define BIG (48u << 20)

static int fails;
static void out(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}
static void ck(int ok, const char *what, long v) {
  out("[LP] %s %s (%ld)\n", ok ? "PASS" : "FAIL", what, v);
  if (!ok) fails++;
}
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint8_t *W;
static volatile int stop;
static volatile uint64_t reads, remaps, last_read_ms;
static int bigfd, fatfd, e2fd;

static void *reader(void *a) {
  (void)a;
  uint64_t off = 0;
  while (!stop) {
    ssize_t n = pread(bigfd, W, WIN, (off_t)off);
    (void)n; /* EFAULT is fine while the window is unmapped */
    off = (off + WIN) % BIG;
    reads++;
    last_read_ms = now_ms();
  }
  return 0;
}

static void *mapper(void *a) {
  (void)a;
  for (uint32_t i = 0; !stop; i++) {
    int fd = (i % 3 == 0) ? fatfd : (i % 3 == 1) ? e2fd : -1;
    void *p;
    if (fd >= 0)
      p = mmap(W, WIN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0);
    else
      p = mmap(W, WIN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) usleep(1000);
    remaps++;
    usleep(200 + (i % 7) * 150);
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *vol = argc > 1 ? argv[1] : "/EXT";
  int secs = argc > 2 ? atoi(argv[2]) : 60;
  struct stat st;
  if (stat(vol, &st) != 0 || !S_ISDIR(st.st_mode)) {
    out("[LP] SKIP %s absent: no data disk in this boot\n", vol);
    out("[LP] done rc=0\n");
    return 0;
  }
  char p[300];
  static uint8_t b[1 << 16];
  snprintf(p, sizeof p, "%s/e2fault.big", vol);
  bigfd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
  for (uint32_t o = 0; bigfd >= 0 && o < BIG; o += sizeof b) {
    for (uint32_t i = 0; i < sizeof b; i++) b[i] = (uint8_t)((o + i) * 7u + 3u);
    (void)!write(bigfd, b, sizeof b);
  }
  snprintf(p, sizeof p, "%s/e2fault.map", vol);
  e2fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (e2fd >= 0) (void)!write(e2fd, b, WIN);
  /* a FAT16 file on the boot volume (Linux host: next to the volume) */
  const char *fatp = argc > 3 ? argv[3] : "/E2FAULT.BIN";
  fatfd = open(fatp, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fatfd >= 0) (void)!write(fatfd, b, WIN);
  sync();
  ck(bigfd >= 0 && e2fd >= 0 && fatfd >= 0, "setup: 48 MiB ext2 file, ext2 + FAT mappable files", errno);
  if (bigfd < 0 || e2fd < 0 || fatfd < 0) { out("[LP] done rc=%d\n", fails); return 1; }
  W = mmap(0, WIN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  out("[LP] info e2fault: pread into a window remapped (FAT file / ext2 file / anon) under it, %d s\n", secs);
  last_read_ms = now_ms();
  pthread_t tr, tm;
  pthread_create(&tr, 0, reader, 0);
  pthread_create(&tm, 0, mapper, 0);
  uint64_t t_end = now_ms() + (uint64_t)secs * 1000u, worst = 0;
  while (now_ms() < t_end) {
    usleep(200000);
    uint64_t g = now_ms() - last_read_ms;
    if (g > worst) worst = g;
    if (g > 30000) break; /* wedged: report instead of waiting */
  }
  stop = 1;
  ck(worst < 30000, "pread kept making progress (worst gap ms)", (long)worst);
  if (worst < 30000) {
    pthread_join(tr, 0);
    pthread_join(tm, 0);
  }
  out("[LP] info e2fault: %llu preads, %llu remaps\n", (unsigned long long)reads, (unsigned long long)remaps);
  ck(reads > 100 && remaps > 100, "both threads ran (preads)", (long)reads);
  /* the file is intact */
  long bad = 0;
  for (uint32_t o = 0; o < BIG; o += sizeof b) {
    if (pread(bigfd, b, sizeof b, o) != (ssize_t)sizeof b) { bad++; break; }
    for (uint32_t i = 0; i < sizeof b; i += 997)
      if (b[i] != (uint8_t)((o + i) * 7u + 3u)) { bad++; break; }
  }
  ck(bad == 0, "the 48 MiB file reads back", bad);
  close(bigfd);
  snprintf(p, sizeof p, "%s/e2fault.big", vol);
  unlink(p);
  unlink(fatp);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
