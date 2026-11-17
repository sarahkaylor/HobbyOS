/* src/user/cp/vn_probe_transport.c -- Gate P6 kernel transport probe (lane
 * C6-venus, docs/gpu-abi.md §10, docs/cp-venus.md).
 *
 * A Linux-ABI (musl static-PIE) program that drives /dev/gpu's Venus requests
 * with no Mesa at all: parameters, the capset table and the Venus capset blob,
 * a context-init context with 64 rings, a host3d shmem blob (blob_id 0, the
 * render server's own shmem) mapped through the host-visible window and
 * written/read through that mapping, fenced submits on the CPU ring retired
 * by the interrupt path, the error rows, and a fence-latency loop
 * (submit -> signal, N fences, p50/p99/max in microseconds).
 *
 * Rows are "[LP] ..." and the run ends "[LP] done rc=N" (t3 probe protocol).
 * argv[1] = fence count (default 10000).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "gpu_abi.h"

static int fails;

#define ROW(...) do { printf("[LP] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    if (cond) {                                                              \
      ROW("ok   " __VA_ARGS__);                                              \
    } else {                                                                 \
      fails++;                                                               \
      ROW("FAIL " __VA_ARGS__);                                              \
    }                                                                        \
  } while (0)

static int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do {
    r = ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r == -1 ? -errno : 0;
}

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The guest's CLOCK_MONOTONIC is millisecond-granular, so the latency rows use
 * the TSC, calibrated against CLOCK_MONOTONIC over ~500 ms. */
static double tsc_per_ns;
static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
static void tsc_calibrate(void) {
  uint64_t m0 = mono_ns(), t0 = rdtsc();
  while (mono_ns() - m0 < 500000000ull) {
  }
  uint64_t m1 = mono_ns(), t1 = rdtsc();
  tsc_per_ns = (double)(t1 - t0) / (double)(m1 - m0);
}
static uint64_t now_ns(void) { return (uint64_t)((double)rdtsc() / tsc_per_ns); }

static int cmp_u64(const void* a, const void* b) {
  uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
  return x < y ? -1 : x > y;
}

/* struct virgl_renderer_capset_venus (Mesa src/virtio/virtio-gpu/venus_hw.h) */
struct venus_capset {
  uint32_t wire_format_version;
  uint32_t vk_xml_version;
  uint32_t vk_ext_command_serialization_spec_version;
  uint32_t vk_mesa_venus_protocol_spec_version;
  uint32_t supports_blob_id_0;
  uint32_t vk_extension_mask1[32];
  uint32_t allow_vk_wait_syncs;
  uint32_t supports_multiple_timelines;
  uint32_t use_guest_vram;
};

static int fence_wait(int fd, uint32_t ctx, uint32_t ring, uint64_t seq, uint64_t timeout_ns) {
  struct gpu_fence_wait2 w = { .ctx_id = ctx, .ring_idx = ring, .seqno = seq,
                               .timeout_ns = timeout_ns, .deadline_ms = 0 };
  return xioctl(fd, GPU_IOC_FENCE_WAIT2, &w);
}

int main(int argc, char** argv) {
  tsc_calibrate();
  int nfences = argc > 1 ? atoi(argv[1]) : 10000;
  if (nfences < 1) nfences = 1;
  if (nfences > 100000) nfences = 100000;
  ROW("tsc %.3f GHz (calibrated against CLOCK_MONOTONIC)", tsc_per_ns);
  int fd = open("/dev/gpu", O_RDWR);
  CHECK(fd >= 0, "open /dev/gpu fd=%d errno=%d", fd, fd < 0 ? errno : 0);
  if (fd < 0) goto out;

  struct gpu_param gp;
  uint64_t feat = 0, hv = 0, ncap = 0;
  gp = (struct gpu_param){ .param = GPU_PARAM_ABI_VERSION };
  int rc = xioctl(fd, GPU_IOC_GET_PARAM, &gp);
  CHECK(rc == 0 && gp.value == 2, "param abi_version=%llu rc=%d", (unsigned long long)gp.value, rc);
  gp = (struct gpu_param){ .param = GPU_PARAM_FEATURES };
  rc = xioctl(fd, GPU_IOC_GET_PARAM, &gp);
  feat = gp.value;
  CHECK(rc == 0 && (feat & (GPU_FEAT_RESOURCE_BLOB | GPU_FEAT_CONTEXT_INIT | GPU_FEAT_HOST_VISIBLE)) ==
                       (GPU_FEAT_RESOURCE_BLOB | GPU_FEAT_CONTEXT_INIT | GPU_FEAT_HOST_VISIBLE),
        "param features=0x%llx (virgl=%d blob=%d ctx_init=%d host_visible=%d fence_irq=%d)",
        (unsigned long long)feat, !!(feat & GPU_FEAT_VIRGL), !!(feat & GPU_FEAT_RESOURCE_BLOB),
        !!(feat & GPU_FEAT_CONTEXT_INIT), !!(feat & GPU_FEAT_HOST_VISIBLE), !!(feat & GPU_FEAT_FENCE_IRQ));
  gp = (struct gpu_param){ .param = GPU_PARAM_HOST_VISIBLE_SIZE };
  xioctl(fd, GPU_IOC_GET_PARAM, &gp);
  hv = gp.value;
  gp = (struct gpu_param){ .param = GPU_PARAM_NUM_CAPSETS };
  xioctl(fd, GPU_IOC_GET_PARAM, &gp);
  ncap = gp.value;
  CHECK(hv > 0 && ncap >= 1, "param host_visible_mib=%llu num_capsets=%llu",
        (unsigned long long)(hv >> 20), (unsigned long long)ncap);
  gp = (struct gpu_param){ .param = 99 };
  CHECK(xioctl(fd, GPU_IOC_GET_PARAM, &gp) == -EINVAL, "param unknown -> EINVAL");

  int venus_idx = -1;
  uint32_t venus_size = 0;
  for (uint32_t i = 0; i < ncap; i++) {
    struct gpu_capset_info2 ci = { .index = i };
    rc = xioctl(fd, GPU_IOC_GET_CAPSET_INFO2, &ci);
    ROW("capset index=%u id=%u maxver=%u maxsize=%u rc=%d", i, ci.capset_id, ci.max_version,
        ci.max_size, rc);
    if (rc == 0 && ci.capset_id == 4) {
      venus_idx = (int)i;
      venus_size = ci.max_size;
    }
  }
  CHECK(venus_idx >= 0, "venus capset present (id 4) index=%d size=%u", venus_idx, venus_size);
  struct gpu_capset_info2 cbad = { .index = (uint32_t)ncap };
  CHECK(xioctl(fd, GPU_IOC_GET_CAPSET_INFO2, &cbad) == -EINVAL, "capset index past the table -> EINVAL");

  struct venus_capset vc;
  memset(&vc, 0, sizeof(vc));
  struct gpu_capset2 cs = { .capset_id = 4, .version = 0, .size = sizeof(vc),
                            .blob = (uint64_t)(uintptr_t)&vc };
  rc = xioctl(fd, GPU_IOC_GET_CAPSET2, &cs);
  CHECK(rc == 0 && cs.size >= 20 && vc.wire_format_version != 0,
        "venus capset len=%u wire_format=%u vk_xml=0x%x venus_protocol=%u blob_id_0=%u "
        "wait_syncs=%u multi_timelines=%u rc=%d",
        cs.size, vc.wire_format_version, vc.vk_xml_version,
        vc.vk_mesa_venus_protocol_spec_version, vc.supports_blob_id_0, vc.allow_vk_wait_syncs,
        vc.supports_multiple_timelines, rc);

  struct gpu_ctx_create2 cc = { .capset_id = 4, .num_rings = 64 };
  strcpy(cc.name, "vn-probe");
  rc = xioctl(fd, GPU_IOC_CTX_CREATE2, &cc);
  CHECK(rc == 0 && cc.ctx_id != 0, "ctx_create2 capset=4 rings=64 ctx=%u rc=%d", cc.ctx_id, rc);
  if (rc != 0) goto out;
  uint32_t ctx = cc.ctx_id;
  struct gpu_ctx_create2 cc2 = cc;
  CHECK(xioctl(fd, GPU_IOC_CTX_CREATE2, &cc2) == -EBUSY, "second ctx on the fd -> EBUSY");

  /* A host shmem (blob_id 0), mapped and written through the window. */
  struct gpu_res_create_blob rb = { .ctx_id = ctx, .blob_mem = GPU_BLOB_MEM_HOST3D,
                                    .blob_flags = GPU_BLOB_FLAG_MAPPABLE, .blob_id = 0,
                                    .size = 1 << 20 };
  rc = xioctl(fd, GPU_IOC_RES_CREATE_BLOB, &rb);
  CHECK(rc == 0 && rb.res_id != 0, "blob_create host3d mappable blob_id=0 1MiB res=%u rc=%d",
        rb.res_id, rc);
  struct gpu_res_map_blob mb = { .res_id = rb.res_id };
  rc = xioctl(fd, GPU_IOC_RES_MAP_BLOB, &mb);
  CHECK(rc == 0 && mb.va && mb.size == (1u << 20), "map_blob va=0x%llx size=%llu map_info=%u rc=%d",
        (unsigned long long)mb.va, (unsigned long long)mb.size, mb.map_info, rc);
  if (rc == 0) {
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)mb.va;
    for (uint32_t i = 0; i < (1u << 18); i++) p[i] = 0x9E000000u ^ i;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < (1u << 18); i++)
      if (p[i] != (0x9E000000u ^ i)) bad++;
    CHECK(bad == 0, "window write/read 1MiB mismatches=%u", bad);
    struct gpu_res_map_blob mb2 = { .res_id = rb.res_id };
    rc = xioctl(fd, GPU_IOC_RES_MAP_BLOB, &mb2);
    CHECK(rc == 0 && mb2.va == mb.va, "map_blob again -> same va rc=%d", rc);
    /* Bandwidth through the mapping (a 1 MiB memset and a read pass). */
    uint64_t t0 = now_ns();
    for (int k = 0; k < 64; k++) memset((void*)(uintptr_t)mb.va, k, 1 << 20);
    uint64_t t1 = now_ns();
    volatile uint64_t sum = 0;
    for (int k = 0; k < 16; k++)
      for (uint32_t i = 0; i < (1u << 17); i++) sum += ((volatile uint64_t*)(uintptr_t)mb.va)[i];
    uint64_t t2 = now_ns();
    ROW("window bandwidth write=%llu MB/s read=%llu MB/s (map_info=%u)",
        (unsigned long long)(64ull * 1000000000ull / (t1 - t0 ? t1 - t0 : 1)),
        (unsigned long long)(16ull * 1000000000ull / (t2 - t1 ? t2 - t1 : 1)), mb.map_info);
  }

  /* A non-mappable blob refuses MAP_BLOB. */
  struct gpu_res_create_blob rn = { .ctx_id = ctx, .blob_mem = GPU_BLOB_MEM_HOST3D,
                                    .blob_flags = 0, .blob_id = 0, .size = 4096 };
  int rcn = xioctl(fd, GPU_IOC_RES_CREATE_BLOB, &rn);
  if (rcn == 0) {
    struct gpu_res_map_blob mn = { .res_id = rn.res_id };
    CHECK(xioctl(fd, GPU_IOC_RES_MAP_BLOB, &mn) == -EOPNOTSUPP, "map of a non-mappable blob -> EOPNOTSUPP");
    struct gpu_res_unref un = { .res_id = rn.res_id };
    xioctl(fd, GPU_IOC_RES_UNREF, &un);
  } else {
    ROW("note non-mappable blob_id=0 create rc=%d (host refused; skipped)", rcn);
  }
  struct gpu_res_create_blob rg = rb;
  rg.blob_mem = GPU_BLOB_MEM_GUEST;
  CHECK(xioctl(fd, GPU_IOC_RES_CREATE_BLOB, &rg) == -EINVAL, "guest blob -> EINVAL (not in v2)");

  /* Fences on the CPU ring (0). */
  struct gpu_submit2 sb = { .ctx_id = ctx, .flags = GPU_SUBMIT2_F_FENCE, .ring_idx = 0 };
  rc = xioctl(fd, GPU_IOC_SUBMIT2, &sb);
  CHECK(rc == 0 && sb.seqno >= 0x100000000ull, "submit2 fenced ring=0 seqno=0x%llx rc=%d",
        (unsigned long long)sb.seqno, rc);
  rc = fence_wait(fd, ctx, 0, sb.seqno, 1000000000ull);
  CHECK(rc == 0, "fence_wait2 ring=0 rc=%d", rc);
  CHECK(fence_wait(fd, ctx, 0, sb.seqno, 0) == 0, "fence_wait2 poll after retire -> 0");
  CHECK(fence_wait(fd, ctx, 0, sb.seqno + 1000, 0) == -EINVAL, "wait on a seqno never issued -> EINVAL");
  CHECK(fence_wait(fd, ctx, 64, sb.seqno, 0) == -EINVAL, "wait on ring 64 -> EINVAL");
  struct gpu_submit2 sbad = sb;
  sbad.ring_idx = 64;
  CHECK(xioctl(fd, GPU_IOC_SUBMIT2, &sbad) == -EINVAL, "submit2 ring 64 -> EINVAL");
  uint8_t dummy[64];
  CHECK(xioctl(fd, GPU_IOC_SCANOUT_BLOB, dummy) == -ENOSYS, "scanout_blob reserved -> ENOSYS");

  /* A burst deeper than the slot table: 100 fences in flight, then wait the last. */
  uint64_t last = 0;
  int burst_rc = 0;
  for (int i = 0; i < 100; i++) {
    struct gpu_submit2 s = { .ctx_id = ctx, .flags = GPU_SUBMIT2_F_FENCE, .ring_idx = 0 };
    if ((burst_rc = xioctl(fd, GPU_IOC_SUBMIT2, &s)) != 0) break;
    last = s.seqno;
  }
  rc = burst_rc ? burst_rc : fence_wait(fd, ctx, 0, last, 2000000000ull);
  CHECK(rc == 0, "burst of 100 fenced submits (> 32 slots) retired rc=%d", rc);

  /* Fence latency: submit -> signal -> waiter woken, one at a time. */
  uint64_t* lat = calloc((size_t)nfences, sizeof(uint64_t));
  uint64_t* sublat = calloc((size_t)nfences, sizeof(uint64_t));
  int ok = 0;
  uint64_t tstart = now_ns();
  for (int i = 0; lat && sublat && i < nfences; i++) {
    struct gpu_submit2 s = { .ctx_id = ctx, .flags = GPU_SUBMIT2_F_FENCE, .ring_idx = 0 };
    uint64_t a = now_ns();
    if (xioctl(fd, GPU_IOC_SUBMIT2, &s) != 0) break;
    uint64_t b = now_ns();
    if (fence_wait(fd, ctx, 0, s.seqno, 1000000000ull) != 0) break;
    uint64_t c = now_ns();
    sublat[i] = b - a;
    lat[i] = c - a;
    ok++;
  }
  uint64_t tend = now_ns();
  CHECK(ok == nfences, "fence loop %d/%d completed in %llu ms", ok, nfences,
        (unsigned long long)((tend - tstart) / 1000000ull));
  if (ok > 0) {
    qsort(lat, (size_t)ok, sizeof(uint64_t), cmp_u64);
    qsort(sublat, (size_t)ok, sizeof(uint64_t), cmp_u64);
    /* Ring 0 is the CPU ring: QEMU retires it from its virgl fence poll, so
     * this row is the transport's floor, not a VkQueue fence (see vn_fence). */
    ROW("FENCE ring=0 n=%d submit_us p50=%.1f p99=%.1f | submit->signal_us p50=%.1f p90=%.1f "
        "p99=%.1f max=%.1f",
        ok, sublat[ok / 2] / 1000.0, sublat[(ok * 99) / 100] / 1000.0, lat[ok / 2] / 1000.0,
        lat[(ok * 9) / 10] / 1000.0, lat[(ok * 99) / 100] / 1000.0, lat[ok - 1] / 1000.0);
  }

  struct gpu_res_unmap_blob ub = { .res_id = rb.res_id };
  CHECK(xioctl(fd, GPU_IOC_RES_UNMAP_BLOB, &ub) == 0, "unmap_blob");
  struct gpu_res_unref ur = { .res_id = rb.res_id };
  CHECK(xioctl(fd, GPU_IOC_RES_UNREF, &ur) == 0, "unref blob");
  struct gpu_ctx_destroy cd = { .ctx_id = ctx };
  CHECK(xioctl(fd, GPU_IOC_CTX_DESTROY, &cd) == 0, "ctx_destroy");
  close(fd);
out:
  ROW("done rc=%d", fails);
  return fails;
}
