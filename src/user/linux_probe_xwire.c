/* src/user/linux_probe_xwire.c — Linux-ABI probe: the cost of moving one frame
 * through an AF_UNIX SOCK_STREAM pair, at the shapes the X11 display path uses.
 *
 * An ORDINARY musl x86_64 Linux program (no HobbyOS header), so the same source
 * runs on this workstation as the reference.  Rows are prefixed "[LP] " and the
 * last one is "[LP] done rc=<failures>"; tools/gate/t3_linux_probe.sh accepts
 * only rc=0.
 *
 * Why this exists: the hosted RimWorld window reaches XHOBBY through ONE
 * XPutImage per presented frame, and L88 measured that put at 135 ms for a
 * 1280x720 frame (27 MB/s) while an in-process copy of the same 3.7 MB takes
 * 0.39 ms.  Two candidate causes, and they need different fixes:
 *   - the bytes: the kernel's AF_UNIX ring copies with a byte loop
 *     (src/kernel/unix.c iov_load/iov_store);
 *   - the handshakes: the writer blocks each time it fills the ring, so a frame
 *     crosses K_UNIX_RING-sized chunks and pays one park/wake per chunk.
 * The shapes below separate them: the same payload at six write/read chunk
 * sizes, with an in-process memcpy() as the reference.  A per-byte cost shows
 * up as a rate that does not move with the chunk size; a per-handshake cost
 * shows up as a rate that rises with it.
 *
 * Threads, not fork: this kernel's compat layer refuses clone() without
 * CLONE_VM|CLONE_THREAD (no COW fork), and the socket pair is the same object
 * either way.  Everything is measured on CLOCK_MONOTONIC, whose guest
 * resolution is 1 ms (skill rule 33), so each cell moves 4 x 3.7 MB.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define FRAME_W 1280
#define FRAME_H 720
#define FRAME_BYTES ((size_t)FRAME_W * FRAME_H * 4)   /* 3 686 400 */
#define REPS 4

static int fails;

static void fail(const char *what, const char *why, long a, long b) {
  printf("[LP] FAIL %s: %s (%ld vs %ld)\n", what, why, a, b);
  fails++;
}

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void fill(unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i += 64)
    p[i] = (unsigned char)(i * 7 + 1);
}

struct wr_arg {
  int fd;
  size_t bytes;     /* per frame */
  size_t chunk;     /* write() size */
  int frames;
  unsigned char *buf;
  volatile int wrote_all;
};

/* Writer thread: `frames` frames of `bytes`, in `chunk`-sized writes. */
static void *writer(void *p) {
  struct wr_arg *a = (struct wr_arg *)p;
  for (int f = 0; f < a->frames; f++) {
    size_t done = 0;
    while (done < a->bytes) {
      size_t n = a->bytes - done;
      if (n > a->chunk) n = a->chunk;
      ssize_t w = write(a->fd, a->buf + done, n);
      if (w < 0) {
        if (errno == EINTR) continue;
        return (void *)1;
      }
      done += (size_t)w;
    }
  }
  a->wrote_all = 1;
  return 0;
}

/* One measured cell: one socketpair, one writer thread, the caller reading.
 * `best` is the fastest whole stream divided by frames -- the steady state,
 * which is what a per-frame present cost is. */
static void cell(const char *name, size_t wchunk, size_t rchunk,
                 unsigned char *wbuf, unsigned char *rbuf) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    fail(name, "socketpair", errno, 0);
    return;
  }
  struct wr_arg a = { sv[1], FRAME_BYTES, wchunk, REPS, wbuf, 0 };
  pthread_t th;
  if (pthread_create(&th, 0, writer, &a) != 0) {
    fail(name, "pthread_create", 0, 0);
    close(sv[0]);
    close(sv[1]);
    return;
  }
  uint64_t t0 = now_us();
  size_t total = FRAME_BYTES * REPS, got = 0;
  while (got < total) {
    size_t n = total - got;
    if (n > rchunk) n = rchunk;
    ssize_t r = read(sv[0], rbuf, n);
    if (r < 0) {
      if (errno == EINTR) continue;
      fail(name, "read", errno, 0);
      break;
    }
    if (r == 0) break;
    got += (size_t)r;
  }
  uint64_t dt = now_us() - t0;
  void *rv = 0;
  pthread_join(th, &rv);
  close(sv[0]);
  close(sv[1]);
  if (got != total) {
    printf("[LP] SHORT %s bytes=%zu got=%zu\n", name, total, got);
    fail(name, "short transfer", (long)got, (long)total);
    return;
  }
  double mb = (double)FRAME_BYTES / (1024.0 * 1024.0);
  double frame_ms = ((double)dt / 1000.0) / REPS;
  double rate = frame_ms > 0.0 ? mb / (frame_ms / 1000.0) : 0.0;
  printf("[LP] xfer %-24s w=%6zu r=%6zu %8.2f ms/frame (%6.1f MB/s)\n",
         name, wchunk, rchunk, frame_ms, rate);
}

int main(void) {
  unsigned char *wbuf = malloc(FRAME_BYTES);
  unsigned char *rbuf = malloc(FRAME_BYTES);
  if (!wbuf || !rbuf) {
    printf("[LP] FAIL alloc\n[LP] done rc=1\n");
    return 1;
  }
  fill(wbuf, FRAME_BYTES);
  fill(rbuf, FRAME_BYTES);

  /* Reference: the same 3.7 MB copy inside one process, no kernel involved. */
  uint64_t cbest = 0;
  for (int i = 0; i < REPS; i++) {
    uint64_t t0 = now_us();
    memcpy(rbuf, wbuf, FRAME_BYTES);
    uint64_t dt = now_us() - t0;
    if (cbest == 0 || dt < cbest) cbest = dt;
  }
  {
    double mb = (double)FRAME_BYTES / (1024.0 * 1024.0);
    double ms = (double)cbest / 1000.0;
    printf("[LP] copy %-24s %8.2f ms (%6.1f MB/s)  [reference, no kernel]\n",
           "memcpy 3.7MB", ms, ms > 0.0 ? mb / (ms / 1000.0) : 0.0);
  }

  /* K_UNIX_RING is 512 KiB after L88.  These bracket it: below one ring, one
   * ring exactly, and several rings per frame. */
  cell("w64k/r16k putimage", 65536, 16384, wbuf, rbuf);
  cell("w64k/r64k", 65536, 65536, wbuf, rbuf);
  cell("w256k/r16k", 262144, 16384, wbuf, rbuf);
  cell("w512k/r64k", 524288, 65536, wbuf, rbuf);
  cell("w512k/r512k", 524288, 524288, wbuf, rbuf);
  cell("w1m/r512k", 1048576, 524288, wbuf, rbuf);

  free(wbuf);
  free(rbuf);
  printf("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
