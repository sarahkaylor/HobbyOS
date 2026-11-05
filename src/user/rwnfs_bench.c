/*
 * rwnfs_bench.c — L35: measure NFS READ throughput from inside the guest.
 *
 * Why a separate program: the launch's speed wall is the NFS data path, and
 * the only honest number is a guest `read()` over the real export.  A host
 * `dd` measures nfsd (944 MB/s here), not the guest's transport.
 *
 * What it prints, one [BENCH] row per phase, all recomputable from the row
 * itself (bytes, RPC count delta, elapsed ms):
 *
 *   open        - a 1 KiB single read (one RPC; latency, not bandwidth)
 *   seq1024     - resources.assets read with 1 KiB read()s (1 RPC per call)
 *   seq4096     - ... with 4 KiB reads
 *   seq16384    - ... with 16 KiB reads
 *   seq65536    - ... with 64 KiB reads (the game's asset-read shape)
 *   span64m     - 64 MiB of 64 KiB reads across resources.assets and
 *                 UnityPlayer.so, i.e. the sustained stream
 *
 * Each phase reports MB/s (10^6 bytes/s) and the kernel's RPC count, so
 * "bytes/RPC x RPCs in flight / latency" can be checked against reality.
 *
 * Built and run by tools/rw/run-nfs-bench.sh (the Makefile is L0-owned and
 * gains no rows for probes).
 */
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include "libc.h"

#ifndef RW_NFS_SERVER
#define RW_NFS_SERVER "10.0.2.2"
#endif
#ifndef RW_NFS_ALT
#define RW_NFS_ALT "192.168.0.123"
#endif
#define RW_NFS_EXPORT "/srv/nfs/rimworld"
#define RW_NFS_MNT "/NFS"

#define ASSETS "/NFS/RimWorldLinux_Data/resources.assets"
#define PLAYER "/NFS/UnityPlayer.so"

/* Bytes per sequential phase.  The script sets RW_BENCH_BYTES (a small value
 * for the "before" transport measurement, 0 = the whole file / 64 MiB span
 * for the real number).  The row always prints the bytes it actually read. */
#ifndef RW_BENCH_BYTES
#define RW_BENCH_BYTES 0
#endif

static int failures = 0;

static void put_dec(uint64_t v) { print_dec((long)v); }
static void put_mb_s(uint64_t bytes, uint64_t ms) {
  /* MB/s with one decimal, computed as bytes*1000/(ms*1000000) tenths. */
  if (ms == 0) { print("inf"); return; }
  uint64_t tenths = (bytes * 1000ULL) / (ms * 100000ULL);
  put_dec(tenths / 10);
  print(".");
  put_dec(tenths % 10);
}

static uint64_t now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* KB/s (10^3 bytes/s), one decimal.  Printed next to MB/s because a
 * sub-1 MB/s transport rounds to 0.0 MB/s and hides the real rate. */
static void put_kb_s(uint64_t bytes, uint64_t ms) {
  if (ms == 0) { print("inf"); return; }
  uint64_t tenths = (bytes * 10ULL) / ms;      /* bytes/ms == KB/s */
  put_dec(tenths / 10);
  print(".");
  put_dec(tenths % 10);
}

/* Kernel-side counters: bytes moved and READ RPCs issued. */
static uint64_t nfs_ops(void) {
  struct sys_nfsio io;
  if (sysinfo(10, &io, sizeof io) != 0) return 0;
  return io.ops;
}

static int wait_for_ip(void) {
  struct sys_netinfo ni;
  for (int i = 0; i < 80; i++) {
    if (sysinfo(4, &ni, sizeof ni) == 0 && ni.ip != 0) return 0;
    usleep(100000);
  }
  return -1;
}

static int do_mount(const char *src) {
  if (mount(src, RW_NFS_MNT) == 0) return 0;
  return -1;
}

/* Read `total` bytes from fd in `chunk`-sized read()s (rewinding as needed),
 * printing the phase row.  Validates the byte count too: a short read would
 * silently inflate the apparent speed. */
static void phase(const char *tag, int fd, uint32_t chunk, uint64_t total,
                  int rewind_first) {
  static char buf[262144];
  if (chunk > sizeof buf) chunk = sizeof buf;
  if (rewind_first) lseek(fd, 0, SEEK_SET);

  uint64_t ops0 = nfs_ops();
  uint64_t t0 = now_ms();
  uint64_t got_total = 0;
  int short_reads = 0;
  uint32_t file_size = 0;
  {
    struct stat st;
    if (fstat(fd, &st) == 0) file_size = (uint32_t)st.st_size;
  }
  while (got_total < total) {
    int n = (int)read(fd, buf, chunk);
    if (n <= 0) {
      /* Reached EOF of this file: rewind and keep streaming (the span phase
       * deliberately re-reads the same bytes). */
      if (file_size && lseek(fd, 0, SEEK_SET) == 0) continue;
      break;
    }
    if ((uint32_t)n != chunk && got_total + (uint64_t)n < total) short_reads++;
    got_total += (uint64_t)n;
  }
  uint64_t ms = now_ms() - t0;
  uint64_t ops = nfs_ops() - ops0;

  print("[BENCH] ");
  print(tag);
  print(" chunk=");
  put_dec(chunk);
  print(" bytes=");
  put_dec(got_total);
  print(" ms=");
  put_dec(ms);
  print(" rpcs=");
  put_dec(ops);
  print(" bytes_per_rpc=");
  put_dec(ops ? got_total / ops : 0);
  print(" mb_s=");
  put_mb_s(got_total, ms);
  print(" kb_s=");
  put_kb_s(got_total, ms);
  print(" short=");
  put_dec((uint64_t)short_reads);
  print("\n");
  if (got_total < total) {
    print("[BENCH] FAIL ");
    print(tag);
    print(" short total\n");
    failures++;
  }
}

static void single_read_latency(int fd) {
  static char buf[4096];
  lseek(fd, 0, SEEK_SET);
  uint64_t ops0 = nfs_ops();
  uint64_t t0 = now_ms();
  int n = (int)read(fd, buf, 1024);
  uint64_t ms = now_ms() - t0;
  print("[BENCH] open1024 bytes=");
  put_dec((uint64_t)(n > 0 ? n : 0));
  print(" ms=");
  put_dec(ms);
  print(" rpcs=");
  put_dec(nfs_ops() - ops0);
  print("\n");
  if (n != 1024) { print("[BENCH] FAIL open1024\n"); failures++; }
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  print("[BENCH] rwnfs_bench start\n");

  if (wait_for_ip() != 0) { print("[BENCH] FAIL no ip\n"); return 1; }
  if (do_mount(RW_NFS_SERVER ":" RW_NFS_EXPORT) != 0) {
    print("[BENCH] mount " RW_NFS_SERVER " failed, trying " RW_NFS_ALT "\n");
    if (do_mount(RW_NFS_ALT ":" RW_NFS_EXPORT) != 0) {
      print("[BENCH] FAIL mount\n");
      return 1;
    }
  }
  print("[BENCH] mounted\n");

  int fd = open(ASSETS, O_RDONLY);
  if (fd < 0) { print("[BENCH] FAIL open " ASSETS "\n"); return 1; }
  struct stat st;
  if (fstat(fd, &st) != 0) { print("[BENCH] FAIL fstat\n"); return 1; }
  print("[BENCH] assets size=");
  put_dec((uint64_t)st.st_size);
  print("\n");

  single_read_latency(fd);
  uint64_t budget = RW_BENCH_BYTES;
  if (budget == 0 || budget > (uint64_t)st.st_size) budget = (uint64_t)st.st_size;

  /* Two shapes in one boot, so the sweep and the sustained number are taken
   * under the same host conditions:
   *   - the SWEEP: 1 MiB per phase, 1/4/16/64 KiB read()s (latency vs
   *     payload, same total bytes per row);
   *   - the STREAM: the whole file and a 64 MiB span at 64 KiB reads.
   * A 1 MiB row that disagrees with the same phase's full-file row in the
   * same boot means the number is environment-bound, not transport-bound. */
  uint64_t sweep = 1024 * 1024;
  if (sweep > budget) sweep = budget;
  phase("sweep1024",  fd, 1024,  sweep, 1);
  phase("sweep4096",  fd, 4096,  sweep, 1);
  phase("sweep16384", fd, 16384, sweep, 1);
  phase("sweep65536", fd, 65536, sweep, 1);

  phase("file64k", fd, 65536, budget, 1);
  phase("file1k",  fd, 1024,  budget, 1);
  if (RW_BENCH_BYTES == 0) {
    /* The full run: 64 MiB of sustained 64 KiB reads, wrapping at EOF. */
    phase("span64m", fd, 65536, 64ULL * 1024 * 1024, 1);
  }
  close(fd);

  /* A second, larger file, so the number is not one file's page cache. */
  int fd2 = open(PLAYER, O_RDONLY);
  if (fd2 >= 0 && RW_BENCH_BYTES == 0) {
    struct stat st2;
    if (fstat(fd2, &st2) == 0) {
      print("[BENCH] player size=");
      put_dec((uint64_t)st2.st_size);
      print("\n");
      phase("player64k", fd2, 65536, (uint64_t)st2.st_size, 1);
    }
    close(fd2);
  }

  print("[BENCH] done failures=");
  put_dec((uint64_t)failures);
  print("\n");
  return failures ? 1 : 0;
}
