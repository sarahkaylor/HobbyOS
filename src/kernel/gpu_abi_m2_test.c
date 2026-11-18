/* P6 / Cyberpunk F3 (docs/gpu-abi.md §10): unit tests for the Venus
 * amendment's ABI validation, in the virtio_gpu_test.c style -- request
 * numbering and struct sizes (the frozen interface), the per-request
 * validators, the map_info -> memory-type rule, the FENCE_WAIT2 deadline
 * arithmetic, and the host-visible window allocator. */
#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "gpu_abi.h"
#include "errno.h"
#include <stdint.h>
#include "linux_snd.h"
#ifdef __x86_64__
#include "virtio_gpu_blob.h"
#endif

/* H4-kglitch (docs/cp-kglitch.md §3): the ioctl park sentinels are not errno
   values, and not each other.  GPU_ABI_PARKED was -2 == -ENOENT. */
static void test_park_sentinels(void) {
  EXPECT_EQ(GPU_ABI_PARKED < -4095, 1);
  EXPECT_EQ(LSND_PARKED < -4095, 1);
  EXPECT_EQ(GPU_ABI_PARKED != LSND_PARKED, 1);
  EXPECT_EQ(GPU_ABI_PARKED != -ENOENT && GPU_ABI_PARKED != -EAGAIN && GPU_ABI_PARKED != -EINTR, 1);
}

static void test_gpu_m2_layout(void) {
  uart_puts("  Running test_gpu_m2_layout...\n");
  tests_run++;
  /* F3 struct sizes are the frozen user ABI (glibc and kernel agree). */
  EXPECT_EQ((int)sizeof(struct gpu_param), 16);
  EXPECT_EQ((int)sizeof(struct gpu_capset_info2), 16);
  EXPECT_EQ((int)sizeof(struct gpu_capset2), 24);
  EXPECT_EQ((int)sizeof(struct gpu_ctx_create2), 48);
  EXPECT_EQ((int)sizeof(struct gpu_res_create_blob), 48);
  EXPECT_EQ((int)sizeof(struct gpu_res_map_blob), 24);
  EXPECT_EQ((int)sizeof(struct gpu_res_unmap_blob), 8);
  EXPECT_EQ((int)sizeof(struct gpu_submit2), 32);
  EXPECT_EQ((int)sizeof(struct gpu_fence_wait2), 32);
  /* Numbers 11..20 with the 'g' magic, sizes encoded. */
  EXPECT_EQ((int)(GPU_IOC_GET_PARAM & 0xFF), 11);
  EXPECT_EQ((int)(GPU_IOC_FENCE_WAIT2 & 0xFF), 19);
  EXPECT_EQ((int)(GPU_IOC_SCANOUT_BLOB & 0xFF), 20);
  EXPECT_EQ((int)((GPU_IOC_SUBMIT2 >> 8) & 0xFF), 'g');
  EXPECT_EQ((int)((GPU_IOC_SUBMIT2 >> 16) & 0x3FFF), 32);
  EXPECT_EQ((int)(GPU_IOC_SUBMIT2 >> 30), 3); /* read|write */
#ifdef __x86_64__
  /* virtio-gpu wire layouts (Linux uapi virtio_gpu.h). */
  EXPECT_EQ((int)sizeof(struct virtio_gpu_resource_create_blob), 56);
  EXPECT_EQ((int)sizeof(struct virtio_gpu_resource_map_blob), 40);
  EXPECT_EQ((int)sizeof(struct virtio_gpu_resp_map_info), 32);
  EXPECT_EQ((int)sizeof(struct virtio_gpu_resource_unmap_blob), 32);
  EXPECT_EQ((int)sizeof(struct virtio_gpu_ctx_create), 96);
#endif
}

static void test_gpu_m2_req_size(void) {
  uart_puts("  Running test_gpu_m2_req_size...\n");
  tests_run++;
  EXPECT_EQ(gpu_abi_m2_req_size(GPU_IOC_GET_PARAM), (int)sizeof(struct gpu_param));
  EXPECT_EQ(gpu_abi_m2_req_size(GPU_IOC_CTX_CREATE2), (int)sizeof(struct gpu_ctx_create2));
  EXPECT_EQ(gpu_abi_m2_req_size(GPU_IOC_RES_CREATE_BLOB),
            (int)sizeof(struct gpu_res_create_blob));
  EXPECT_EQ(gpu_abi_m2_req_size(GPU_IOC_SUBMIT2), (int)sizeof(struct gpu_submit2));
  EXPECT_EQ(gpu_abi_m2_req_size(GPU_IOC_FENCE_WAIT2), (int)sizeof(struct gpu_fence_wait2));
  /* The two tables are disjoint: M1 numbers are not Venus requests and the
   * Venus numbers are unknown to M1's table (its routing is unchanged). */
  EXPECT_EQ(gpu_abi_m2_req_size(GPU_IOC_SUBMIT), -1);
  EXPECT_EQ(gpu_abi_m2_req_size(GPU_IOC_CTX_CREATE), -1);
  EXPECT_EQ(gpu_abi_req_size(GPU_IOC_SUBMIT2), -1);
  EXPECT_EQ(gpu_abi_req_size(GPU_IOC_GET_PARAM), -1);
  /* Same number, wrong size: not a Venus request. */
  EXPECT_EQ(gpu_abi_m2_req_size(GPU_IOC(GPU_IOC_READ | GPU_IOC_WRITE, 'g', 18, 24)), -1);
  /* Routing: a Venus request without a process is -EINVAL, not -ENOTTY.
   * ARM has no 3D transport: the M2 dispatcher is the -ENODEV stub, which is
   * still "routed to M2", not -ENOTTY (same guard as test_gpu_m2_scanout). */
#ifdef __x86_64__
  EXPECT_EQ(gpu_abi_ioctl(0, 3, GPU_IOC_SUBMIT2, 0x1000), -EINVAL);
#else
  EXPECT_EQ(gpu_abi_ioctl(0, 3, GPU_IOC_SUBMIT2, 0x1000), -ENODEV);
#endif
  EXPECT_EQ(gpu_abi_ioctl(0, 3, GPU_IOC(GPU_IOC_READ, 'g', 21, 8), 0x1000), -ENOTTY);
}

static void test_gpu_m2_ctx2_ok(void) {
  uart_puts("  Running test_gpu_m2_ctx2_ok...\n");
  tests_run++;
  struct gpu_ctx_create2 c = { .capset_id = 4, .num_rings = 64, .flags = 0 };
  EXPECT_EQ(gpu_abi_ctx2_ok(&c, 64), 1);
  c.num_rings = 1;
  EXPECT_EQ(gpu_abi_ctx2_ok(&c, 64), 1);
  c.num_rings = 0;
  EXPECT_EQ(gpu_abi_ctx2_ok(&c, 64), 0);
  c.num_rings = 65;
  EXPECT_EQ(gpu_abi_ctx2_ok(&c, 64), 0);
  c.num_rings = 8;
  EXPECT_EQ(gpu_abi_ctx2_ok(&c, 4), 0); /* above what the device allows */
  c.num_rings = 4;
  c.capset_id = 0;
  EXPECT_EQ(gpu_abi_ctx2_ok(&c, 64), 0);
  c.capset_id = 256; /* context_init carries the id in 8 bits */
  EXPECT_EQ(gpu_abi_ctx2_ok(&c, 64), 0);
  c.capset_id = 4;
  c.flags = 1;
  EXPECT_EQ(gpu_abi_ctx2_ok(&c, 64), 0);
  EXPECT_EQ(gpu_abi_ctx2_ok(0, 64), 0);
}

static void test_gpu_m2_blob_ok(void) {
  uart_puts("  Running test_gpu_m2_blob_ok...\n");
  tests_run++;
  struct gpu_res_create_blob b = { .ctx_id = 2, .blob_mem = GPU_BLOB_MEM_HOST3D,
                                   .blob_flags = GPU_BLOB_FLAG_MAPPABLE,
                                   .blob_id = 0, .size = 4096 };
  EXPECT_EQ(gpu_abi_blob_ok(&b), 1); /* Venus shmem: host3d, blob_id 0 */
  b.blob_flags = GPU_BLOB_FLAG_MAPPABLE | GPU_BLOB_FLAG_SHAREABLE;
  b.blob_id = 77;
  b.size = 256ull << 20;
  EXPECT_EQ(gpu_abi_blob_ok(&b), 1); /* a mappable VkDeviceMemory */
  struct gpu_res_create_blob x = b;
  x.blob_mem = GPU_BLOB_MEM_GUEST;
  EXPECT_EQ(gpu_abi_blob_ok(&x), 0); /* guest blobs are not in v2 */
  x = b; x.blob_mem = GPU_BLOB_MEM_HOST3D_GUEST;
  EXPECT_EQ(gpu_abi_blob_ok(&x), 0);
  x = b; x.blob_flags = 8;
  EXPECT_EQ(gpu_abi_blob_ok(&x), 0);
  x = b; x.size = 0;
  EXPECT_EQ(gpu_abi_blob_ok(&x), 0);
  x = b; x.ctx_id = 0;
  EXPECT_EQ(gpu_abi_blob_ok(&x), 0);
  /* cmd-before-create (Linux's contract): pointer, dword size, <= 128 KiB. */
  x = b; x.cmd = 0x7000; x.cmd_size = 24;
  EXPECT_EQ(gpu_abi_blob_ok(&x), 1);
  x.cmd_size = 26;
  EXPECT_EQ(gpu_abi_blob_ok(&x), 0);
  x.cmd_size = 128 * 1024 + 4;
  EXPECT_EQ(gpu_abi_blob_ok(&x), 0);
  x.cmd = 0; x.cmd_size = 24;
  EXPECT_EQ(gpu_abi_blob_ok(&x), 0);
}

static void test_gpu_m2_submit2_ok(void) {
  uart_puts("  Running test_gpu_m2_submit2_ok...\n");
  tests_run++;
  struct gpu_submit2 s = { .ctx_id = 2, .flags = GPU_SUBMIT2_F_FENCE, .ring_idx = 1,
                           .cs_size = 0, .cs = 0 };
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 64), 1); /* empty fenced submit is legal */
  s.cs = 0x7000; s.cs_size = 32;
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 64), 1);
  s.ring_idx = 64;
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 64), 0);
  s.ring_idx = 3;
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 3), 0); /* ring beyond the context's count */
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 4), 1);
  s.cs_size = 30;
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 4), 0);
  s.cs_size = 32; s.flags = 2;
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 4), 0);
  s.flags = 0; s.cs_size = 0;
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 4), 0); /* unfenced and empty: nothing */
  s.flags = 0; s.cs_size = 128 * 1024;
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 4), 1);
  s.cs_size = 128 * 1024 + 4;
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 4), 0);
  s.cs_size = 32; s.ctx_id = 0;
  EXPECT_EQ(gpu_abi_submit2_ok(&s, 4), 0);
}

static void test_gpu_m2_cache_and_deadline(void) {
  uart_puts("  Running test_gpu_m2_cache_and_deadline...\n");
  tests_run++;
  /* Never more cacheable than the host (docs/gpu-abi.md §10.4). */
  EXPECT_EQ(gpu_abi_map_cache_mode(GPU_MAP_CACHE_CACHED), 0);
  EXPECT_EQ(gpu_abi_map_cache_mode(GPU_MAP_CACHE_WC), 1);
  EXPECT_EQ(gpu_abi_map_cache_mode(GPU_MAP_CACHE_UNCACHED), 2);
  EXPECT_EQ(gpu_abi_map_cache_mode(GPU_MAP_CACHE_NONE), 2);
  EXPECT_EQ(gpu_abi_map_cache_mode(9), 2);
  EXPECT_EQ(gpu_abi_map_cache_mode(0x13), 1); /* only the low nibble counts */
  /* Deadlines round up and saturate; UINT64_MAX means forever. */
  EXPECT_EQ((int)gpu_abi_deadline_ms(1000, 1), 1001);
  EXPECT_EQ((int)gpu_abi_deadline_ms(1000, 1000000), 1001);
  EXPECT_EQ((int)gpu_abi_deadline_ms(1000, 1000001), 1002);
  EXPECT_EQ((int)gpu_abi_deadline_ms(5, 2500000000ull), 2505);
  EXPECT_EQ(gpu_abi_deadline_ms(1000, ~0ull) == ~0ull, 1);
  EXPECT_EQ(gpu_abi_deadline_ms(~0ull - 3, 10000000ull) == ~0ull, 1);
}

#ifdef __x86_64__
#include "frame.h"
#include "kmem.h"
static struct gpu_win* test_winp;
#define test_win (*test_winp)

static void test_gpu_m2_window(void) {
  uart_puts("  Running test_gpu_m2_window...\n");
  tests_run++;
  uint64_t a = 0, b = 0, c = 0, d = 0;
  if (!test_winp) {
    /* ~96 KiB: a frame allocation, not .bss (docs/blk-cache.md §11). */
    uint64_t pa = frame_alloc_contig((int)((sizeof(struct gpu_win) + 4095) / 4096));
    ASSERT(pa != 0);
    test_winp = (struct gpu_win*)phys_to_virt(pa);
  }
  gpu_win_init(&test_win, 64ull << 20); /* 64 MiB window */
  EXPECT_EQ(gpu_win_alloc(&test_win, 100, 1, &a), 0); /* rounded to a page */
  EXPECT_EQ((int)a, 0);
  EXPECT_EQ(gpu_win_alloc(&test_win, 4096, 2, &b), 0);
  EXPECT_EQ((int)b, 4096);
  /* >= 2 MiB is 2 MiB aligned. */
  EXPECT_EQ(gpu_win_alloc(&test_win, 3ull << 20, 3, &c), 0);
  EXPECT_EQ((int)(c & 0x1FFFFF), 0);
  EXPECT_EQ((int)(c >> 20), 2);
  /* First fit reuses a freed hole. */
  EXPECT_EQ(gpu_win_free(&test_win, a), 0);
  EXPECT_EQ(gpu_win_alloc(&test_win, 4096, 4, &d), 0);
  EXPECT_EQ((int)d, 0);
  EXPECT_EQ(gpu_win_free(&test_win, 12345), -1);
  /* Too big, zero, exactly the rest. */
  uint64_t e = 0;
  EXPECT_EQ(gpu_win_alloc(&test_win, 65ull << 20, 5, &e), -1);
  EXPECT_EQ(gpu_win_alloc(&test_win, 0, 5, &e), -1);
  EXPECT_EQ(gpu_win_alloc(&test_win, 58ull << 20, 6, &e), 0); /* [6, 64) MiB: the rest */
  EXPECT_EQ((int)(e >> 20), 6);
  EXPECT_EQ(gpu_win_alloc(&test_win, 4ull << 20, 7, &e), -1); /* only 2 MiB-unaligned crumbs left */
  /* The extent list stays sorted: free in the middle, refill it. */
  EXPECT_EQ(gpu_win_free(&test_win, c), 0);
  EXPECT_EQ(gpu_win_alloc(&test_win, 2ull << 20, 8, &e), 0);
  EXPECT_EQ((int)(e >> 20), 2);
  for (int i = 1; i < test_win.n; i++)
    EXPECT_EQ(test_win.e[i - 1].off + test_win.e[i - 1].len <= test_win.e[i].off, 1);
}
#endif

/* P7 (docs/gpu-abi.md §10.7): the SCANOUT_BLOB request, its argument check
 * and the X server's fullscreen control rect. */
static void test_gpu_m2_scanout(void) {
  uart_puts("  Running test_gpu_m2_scanout...\n");
  tests_run++;
  EXPECT_EQ((int)sizeof(struct gpu_scanout_blob), 64);
  EXPECT_EQ((int)((GPU_IOC_SCANOUT_BLOB >> 16) & 0x3FFF), 64);
  EXPECT_EQ((int)(GPU_IOC_SCANOUT_BLOB >> 30), 3); /* read|write: seqno, timings out */
  EXPECT_EQ(gpu_abi_m2_req_size(GPU_IOC_SCANOUT_BLOB), 64);
#ifdef __x86_64__
  /* No process: refused before anything reaches the device (ARM: no 3D). */
  EXPECT_EQ(gpu_abi_ioctl(0, -1, GPU_IOC_SCANOUT_BLOB, 0), -EINVAL);
#endif
  struct gpu_scanout_blob b = {0};
  b.res_id = 7;
  b.width = 1920;
  b.height = 1080;
  b.format = GPU_SCANOUT_FMT_B8G8R8A8;
  b.stride = 1920 * 4;
  b.window = 0x400007;
  const uint64_t sz = 1920ull * 4 * 1080;
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz), 1);
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz - 4096), 0);  /* image past the blob */
  EXPECT_EQ(gpu_abi_scanout_ok(0, sz), 0);
  b.stride = 1920 * 4 - 4;                             /* row shorter than width*4 */
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz), 0);
  b.stride = 1920 * 4 + 2;                             /* not dword aligned */
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz * 2), 0);
  b.stride = 2048 * 4;                                 /* padded rows are fine */
  EXPECT_EQ(gpu_abi_scanout_ok(&b, 2048ull * 4 * 1080), 1);
  b.stride = 1920 * 4;
  b.format = 3;                                        /* A8R8G8B8: not a swapchain format */
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz), 0);
  b.format = GPU_SCANOUT_FMT_R8G8B8A8;
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz), 1);
  b.window = 0;                                        /* the grant names a window */
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz), 0);
  b.window = 1;
  b.width = 8;                                         /* QEMU's minimum is 16 */
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz), 0);
  b.width = 1920;
  b.flags = 1;                                         /* reserved */
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz), 0);
  b.flags = 0;
  b.reserved[1] = 5;
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz), 0);
  b.reserved[1] = 0;
  b.res_id = 0;                                        /* release is not a flip */
  EXPECT_EQ(gpu_abi_scanout_ok(&b, sz), 0);
#ifdef __x86_64__
  EXPECT_EQ((int)sizeof(struct virtio_gpu_set_scanout_blob), 96);
  uint32_t w = 0, sw = 0, sh = 0;
  EXPECT_EQ(virtio_gpu_x64_fsctl_decode(VGPU_FSCTL_MAGIC, VGPU_FSCTL_OP_FS, 0x400007,
                                        (1920 << 16) | 1080, &w, &sw, &sh), 1);
  EXPECT_EQ((int)w, 0x400007);
  EXPECT_EQ((int)sw, 1920);
  EXPECT_EQ((int)sh, 1080);
  EXPECT_EQ(virtio_gpu_x64_fsctl_decode(VGPU_FSCTL_MAGIC, VGPU_FSCTL_OP_FS, 0, 0, &w, &sw, &sh), 1);
  EXPECT_EQ((int)w, 0); /* "no fullscreen window" */
  EXPECT_EQ(virtio_gpu_x64_fsctl_decode(0, 0, 1920, 1080, 0, 0, 0), 0); /* ordinary damage */
  EXPECT_EQ(virtio_gpu_x64_fsctl_decode(VGPU_FSCTL_MAGIC, 2, 5, (1920 << 16) | 1080, 0, 0, 0), 0);
  EXPECT_EQ(virtio_gpu_x64_fsctl_decode(VGPU_FSCTL_MAGIC, VGPU_FSCTL_OP_FS, 5, (4000 << 16) | 1080,
                                        0, 0, 0), 0); /* bigger than the panel */
  EXPECT_EQ(virtio_gpu_x64_fsctl_decode(VGPU_FSCTL_MAGIC, VGPU_FSCTL_OP_FS, -5, (1920 << 16) | 1080,
                                        0, 0, 0), 0);
  /* The flush path's syscall-boundary check accepts exactly that control rect. */
  struct virtio_gpu_xrect cr = {VGPU_FSCTL_MAGIC, VGPU_FSCTL_OP_FS, 0x400007, (1280 << 16) | 720};
  EXPECT_EQ(virtio_gpu_flush_args_ok(&cr, 1), 1);
  struct virtio_gpu_xrect two[2] = {cr, cr};
  EXPECT_EQ(virtio_gpu_flush_args_ok(two, 2), 0); /* only alone */
  cr.y = 9;
  EXPECT_EQ(virtio_gpu_flush_args_ok(&cr, 1), 0);
#endif
}

void gpu_abi_m2_test_suite(void) {
  uart_puts("gpu_abi_m2_test_suite:\n");
  test_park_sentinels();
  test_gpu_m2_layout();
  test_gpu_m2_req_size();
  test_gpu_m2_ctx2_ok();
  test_gpu_m2_blob_ok();
  test_gpu_m2_submit2_ok();
  test_gpu_m2_cache_and_deadline();
  test_gpu_m2_scanout();
#ifdef __x86_64__
  test_gpu_m2_window();
#endif
}

#endif /* KERNEL_MODE_UNIT_TEST */
