/* src/user/cp/vn_probe_fencehold.c -- a host stall must not lose the device
 * (H7-fault round 3, cp-hunt H6-07; docs/cp-fault.md §20).
 *
 * A Linux-ABI (musl static-PIE) program for tools/cp/venus/run_probe.sh (the
 * guest gets the Venus device).  It needs a MODE=test kernel: the fence-hold
 * injection is a test-build-only SUBMIT2 (unfenced, 8 bytes: the magic
 * 0x4A4E4937 "7INJ", then a hold in ms).  While the hold lasts, the kernel
 * keeps every fence the host retires in flight, which is what a host stall
 * looks like from the guest: all VGPU_ASYNC_SLOTS (32) fence slots fill up
 * and the next fenced SUBMIT2 has to wait for one.
 *
 *   H1  hold 7000 ms, then 40 fenced submits on ring 0 (8 more than the
 *       slots): every one must succeed (H6-07: the 33rd failed with EIO after
 *       5 s, "submit2: no fence slot for 5 s", vkd3d's DEVICE_LOST)
 *   H2  meanwhile one busy thread per CPU counts work: the waiting submitter
 *       must not take a CPU from them (the 5 s wait spun with pause):
 *       cpus_used >= ncpu - 0.5
 *   H3  the last fence retires once the hold ends
 *
 * Rows are "[LP] ..." and the run ends "[LP] done rc=N".
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "gpu_abi.h"

static int fails;
#define ROW(...) do { printf("[LP] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(cond, ...) do { if (cond) ROW("ok   " __VA_ARGS__); else { fails++; ROW("FAIL " __VA_ARGS__); } } while (0)

static int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do {
    r = ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r == -1 ? -errno : 0;
}

static uint64_t ms_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull;
}

static volatile int stop;
static volatile uint64_t sink;
static uint64_t cnt[64];
static void* burn(void* a) {
  long me = (long)a;
  uint64_t x = 1, n = 0;
  while (!stop) {
    for (int i = 0; i < 10000; i++) x = x * 6364136223846793005ull + 1442695040888963407ull;
    n++;
  }
  sink = x;
  cnt[me] = n;
  return 0;
}
/* blocks of work per second of k threads over ms */
static double rate(long k, int ms) {
  pthread_t t[64];
  stop = 0;
  for (long i = 0; i < k; i++) pthread_create(&t[i], 0, burn, (void*)i);
  uint64_t a = ms_now();
  usleep((useconds_t)ms * 1000);
  stop = 1;
  for (long i = 0; i < k; i++) pthread_join(t[i], 0);
  uint64_t b = ms_now(), tot = 0;
  for (long i = 0; i < k; i++) tot += cnt[i];
  return (double)tot * 1000.0 / (double)(b - a ? b - a : 1);
}

static int gfd;
static uint32_t gctx;
static int sub_rc[64];
static uint64_t sub_ms[64], last_seq;
static void* submitter(void* a) {
  (void)a;
  uint64_t t0 = ms_now();
  for (int i = 0; i < 40; i++) {
    struct gpu_submit2 sb = { .ctx_id = gctx, .flags = GPU_SUBMIT2_F_FENCE, .ring_idx = 0 };
    sub_rc[i] = xioctl(gfd, GPU_IOC_SUBMIT2, &sb);
    sub_ms[i] = ms_now() - t0;
    if (sub_rc[i] == 0) last_seq = sb.seqno;
  }
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1 || ncpu > 63) ncpu = 8;
  gfd = open("/dev/gpu", O_RDWR);
  CHECK(gfd >= 0, "open /dev/gpu fd=%d", gfd);
  if (gfd < 0) goto out;
  struct gpu_ctx_create2 cc = { .capset_id = 4, .num_rings = 64 };
  strcpy(cc.name, "vn-fencehold");
  int rc = xioctl(gfd, GPU_IOC_CTX_CREATE2, &cc);
  CHECK(rc == 0 && cc.ctx_id, "ctx_create2 ctx=%u rc=%d", cc.ctx_id, rc);
  if (rc) goto out;
  gctx = cc.ctx_id;

  double one = rate(1, 1000);
  uint32_t inj[2] = { 0x4A4E4937u, 7000 };
  struct gpu_submit2 si = { .ctx_id = gctx, .flags = 0, .ring_idx = 0, .cs_size = 8,
                            .cs = (uint64_t)(uintptr_t)inj };
  rc = xioctl(gfd, GPU_IOC_SUBMIT2, &si);
  CHECK(rc == 0, "fence hold 7000 ms armed rc=%d (needs a MODE=test kernel)", rc);
  if (rc) goto out;
  pthread_t st;
  pthread_create(&st, 0, submitter, 0);
  usleep(300000); /* the submitter is waiting for a slot by now */
  double rn = rate(ncpu, 4000);
  pthread_join(st, 0);
  int bad = 0, first_bad = -1;
  for (int i = 0; i < 40; i++)
    if (sub_rc[i]) {
      bad++;
      if (first_bad < 0) first_bad = i;
    }
  CHECK(bad == 0, "H1 40 fenced submits through a 7 s fence hold: failed=%d first_failed=#%d rc=%d at %llu ms; #32 at %llu ms, #39 at %llu ms",
        bad, first_bad, first_bad >= 0 ? sub_rc[first_bad] : 0,
        (unsigned long long)(first_bad >= 0 ? sub_ms[first_bad] : 0), (unsigned long long)sub_ms[32],
        (unsigned long long)sub_ms[39]);
  double used = one > 0 ? rn / one : 0;
  CHECK(used >= (double)ncpu - 0.5, "H2 %ld busy threads beside the waiting submitter: cpus_used=%.2f of %ld",
        ncpu, used, ncpu);
  if (last_seq) {
    struct gpu_fence_wait2 w = { .ctx_id = gctx, .ring_idx = 0, .seqno = last_seq,
                                 .timeout_ns = 10000000000ull, .deadline_ms = 0 };
    rc = xioctl(gfd, GPU_IOC_FENCE_WAIT2, &w);
    CHECK(rc == 0, "H3 the last fence retires after the hold rc=%d", rc);
  } else {
    CHECK(0, "H3 no fenced submit succeeded");
  }
out:
  ROW("done rc=%d", fails);
  return fails ? 1 : 0;
}
