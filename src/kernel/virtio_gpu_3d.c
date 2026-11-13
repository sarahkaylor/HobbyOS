/* M1 (docs/rimworld-virgl-plan.md §3, §5): the kernel-side virgl 3D
 * transport for x64.
 *
 * Scope: everything between the virtio-gpu control queue (owned by
 * virtio_gpu_x64.c) and a userspace GPU driver (the /dev/gpu ABI in
 * gpu_abi.c).  No Mesa, no winsys, no presentation policy:
 *
 *   - capset discovery  (GET_CAPSET_INFO / GET_CAPSET)
 *   - context lifetime  (CTX_CREATE / CTX_DESTROY, CTX_ATTACH_RESOURCE)
 *   - resources         (RESOURCE_CREATE_3D / ATTACH_BACKING / UNREF)
 *   - transfers         (TRANSFER_TO_HOST_3D / TRANSFER_FROM_HOST_3D)
 *   - submits           (SUBMIT_3D, fenced)
 *   - completion        (WAIT on the fence, RESOURCE_FLUSH with a fence)
 *
 * Gating: gpu3d_ready is only set when VIRTIO_GPU_F_VIRGL was negotiated
 * AND the capset query answered.  Every entry point checks it, so a
 * plain virtio-gpu-pci host (no bit 0) never issues a 3D command and the
 * 2D desktop is untouched.  A 3D failure at any point after negotiation
 * also clears gpu3d_ready; virtio_gpu_init() keeps the 2D path either way.
 *
 * Sync model: submits are synchronous and fenced.  The kernel assigns a
 * monotonically increasing fence id, sets VIRTIO_GPU_FLAG_FENCE, and the
 * used-ring completion of that command is the host GPU's completion
 * (QEMU defers a fenced command's response until virglrenderer's fence
 * signals).  Therefore a TRANSFER_FROM_HOST_3D issued after a successful
 * submit observes the submit's effect -- the read-after-clear property
 * the M1 gate asserts.  Asynchronous/pipelined submits are M4 work.
 *
 * Buffer model: a resource's backing must be guest-physical and stay
 * resident for the resource's lifetime.  The ABI layer resolves a user
 * VA range into a page list (gpu_abi.c) and this layer stores it; a
 * 16 Ki-page pool (64 MiB) bounds attached memory.  Nothing here copies
 * on submit except the command stream itself (SUBMIT_3D's payload must
 * be one contiguous range, so it is bounced into gpu3d_submit_buf).
 */
#ifdef __x86_64__

#include "virtio_gpu.h"
#include "errno.h"
#include "lock.h"

extern void uart_puts(const char* s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);
extern uint64_t timer_get_ms(void);

extern int virtio_gpu_x64_virgl_offered;
extern int virtio_gpu_x64_virgl_negotiated;

/* One resource's backing may cover 64 MiB per attach (enough for a 4096x4096
 * atlas, and 8x a 1080p RGBA render target); the pool as a whole is 512 MiB
 * of attached backing, and the resource table is sized for a real Unity frame
 * (M3 measured the game's own demand -- see docs/rimworld-virgl-m3.md §1).
 *
 * M1 sized these for the kernel probe alone (64 resources, 64 MiB pool); M2
 * showed a Mesa client fits in 3 resources / 15.6 MB, but a Unity UI with
 * texture atlases, FBOs and VBOs does not, and the resource table -- not
 * performance -- was the likeliest non-performance wall. */
#define GPU3D_MAX_CTX     16
/* L80: the transport's table is what a full client table hits next.  1024 was
 * enough for M3's 618-resource scene and not for the main menu; both sides
 * move together (GPU_ABI_MAX_RES_PER_CLIENT in gpu_abi.c).  4096 x ~72 B is
 * ~290 KiB of kernel BSS. */
#define GPU3D_MAX_RES     16384
/* Attached-backing page pool.  512 MiB was M3's ceiling; a real RimWorld def
 * load asks for more than the old 384 MiB per-client cap before it finishes
 * (see GPU_ABI_CLIENT_BYTES in gpu_abi.c), so the pool is raised to 2.5 GiB --
 * one client can hold 1.5 GiB of it and the window/2D resources the rest.
 * Each slot is one 4 KiB guest page registered with the host, so this costs
 * 8 bytes of kernel BSS per page (5 MiB here), not guest memory. */
#define GPU3D_PAGE_POOL   (1024 * 1024)  /* pages == 4 GiB of attached backing */
#define GPU3D_MAX_ATTACH  16384          /* per resource == 64 MiB */
#define GPU3D_CAPS_MAX    16384
#define GPU3D_SUBMIT_MAX  (128 * 1024)

/* virgl protocol values (Mesa pin: src/virtio/virtio-gpu/virgl_protocol.h
 * and virgl_hw.h).  Hand-mirrored so the kernel does not vendor Mesa. */
#define VIRGL_CCMD_CLEAR_TEXTURE   47
#define VIRGL_CLEAR_TEXTURE_SIZE   12
#define VIRGL_FORMAT_R8G8B8A8_UNORM 67
#define VIRGL_TEXTURE_TARGET_2D    2
#define VIRGL_BIND_RENDER_TARGET   (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW    (1u << 3)

/* M1 gate resource geometry: 64x64 RGBA8 = 16 KiB = 4 pages. */
#define GPU3D_PROBE_W 64
#define GPU3D_PROBE_H 64
#define GPU3D_PROBE_RGBA 0xFF00FF00u /* opaque green, R8G8B8A8_UNORM bytes */
#ifndef VGPU3D_SELFTEST_TIMING
#define VGPU3D_SELFTEST_TIMING 1
#endif
#define GPU3D_TIMING_ITERS 64

struct gpu3d_ctx {
  int used;
  uint32_t id;
  int owner_pid;
  int owner_fd;
  uint32_t capset_id;
  uint32_t capset_version;
  char name[32];
};

struct gpu3d_res {
  int used;
  uint32_t id;
  int owner_pid;
  int owner_fd;
  uint32_t ctx_id;
  uint32_t target, format, bind;
  uint32_t width, height, depth, array_size, last_level, nr_samples, flags;
  uint64_t len;
  int page_start; /* slice of gpu3d_pool */
  int page_count;
  int attached;
  int ctx_attached;
};

static int gpu3d_ready = 0;
/* Suppresses the per-command rows inside a measurement loop (the probe's
 * timing pass would otherwise print two rows per iteration). */
static int gpu3d_quiet = 0;
/* Per-resource rows are capped: a Unity frame creates hundreds of resources,
 * and every row is ~60 bytes down a 115200-baud serial console -- printing
 * them all would cost seconds of the game's load time and distort the very
 * numbers the lane measures.  The first 32 keep the M1/M2 trace shape; the
 * rest are counted and reported in the ctx_destroy summary. */
#define GPU3D_TRACE_RES_ROWS 32
static int gpu3d_trace_res = 0;
/* Demand counters: what the client actually asked for, reported on destroy. */
static int gpu3d_res_in_use = 0;
static int gpu3d_res_peak = 0;
static int gpu3d_pages_in_use = 0;
static int gpu3d_pages_peak = 0;

static int gpu3d_trace_res_row(void) {
  if (gpu3d_quiet) return 0;
  if (gpu3d_trace_res > GPU3D_TRACE_RES_ROWS) return 0;
  if (gpu3d_trace_res == GPU3D_TRACE_RES_ROWS) {
    gpu3d_trace_res++;
    uart_puts("[VIRGL3D] (per-resource rows past 32 suppressed)\n");
    return 0;
  }
  gpu3d_trace_res++;
  return 1;
}
static uint32_t gpu3d_capset_id = 0;
static uint32_t gpu3d_capset_maxver = 0;
static uint32_t gpu3d_capset_maxsize = 0;
static uint32_t gpu3d_capset_version = 0;
static uint32_t gpu3d_caps_len = 0;

static struct gpu3d_ctx gpu3d_ctxs[GPU3D_MAX_CTX];
static struct gpu3d_res gpu3d_ress[GPU3D_MAX_RES];

static uint32_t gpu3d_next_ctx_id = 1;
/* Resource ids are a flat namespace shared with the 2D path (which owns id 1)
 * and with whatever else the host has open on the device.  Start the 3D range
 * at 0x1000 so no amount of 2D/window resources can collide with a virgl
 * resource; ids are opaque u32s to QEMU/virglrenderer. */
#define GPU3D_RES_ID_BASE 0x1000u
static uint32_t gpu3d_next_res_id = GPU3D_RES_ID_BASE;
static uint32_t gpu3d_fence_seq = 0;
static uint32_t gpu3d_fence_done = 0;

/* Attached page pool: a bitmap of GPU3D_PAGE_POOL slots, allocated in
 * contiguous runs.  Slots hold guest-physical page addresses (identity:
 * kernel VA == phys, and user pages resolved through the caller's table). */
static uint64_t gpu3d_pool[GPU3D_PAGE_POOL] __attribute__((aligned(4096)));
static uint8_t gpu3d_pool_used[GPU3D_PAGE_POOL / 8];

/* DMA-visible command/response staging.  One outstanding control command at
 * a time (gpu_lock), so a single submit buffer is enough. */
static uint8_t gpu3d_caps[GPU3D_CAPS_MAX] __attribute__((aligned(4096)));
static uint8_t gpu3d_submit_buf[sizeof(struct virtio_gpu_cmd_submit) + GPU3D_SUBMIT_MAX]
    __attribute__((aligned(4096)));
static struct virtio_gpu_mem_entry gpu3d_ents[GPU3D_MAX_ATTACH]
    __attribute__((aligned(4096)));
static struct virtio_gpu_resource_create_3d gpu3d_req_res3d;
static struct virtio_gpu_transfer_host_3d gpu3d_req_xfer;
static struct virtio_gpu_ctx_create gpu3d_req_ctx;
static struct virtio_gpu_ctx_resource gpu3d_req_ctxres;
static struct virtio_gpu_get_capset_info gpu3d_req_capinfo;
static struct virtio_gpu_get_capset gpu3d_req_cap;
static struct virtio_gpu_resource_unref gpu3d_req_unref;
static struct virtio_gpu_resource_attach_backing gpu3d_req_attach;
static struct virtio_gpu_resource_flush gpu3d_req_flush;

#define GPU3D_RESP_HDR_LEN ((uint32_t)sizeof(struct virtio_gpu_ctrl_hdr))
static struct virtio_gpu_ctrl_hdr gpu3d_resp;
static struct virtio_gpu_resp_capset_info gpu3d_resp_info;
static uint8_t gpu3d_resp_cap[GPU3D_RESP_HDR_LEN + GPU3D_CAPS_MAX]
    __attribute__((aligned(4096)));

static void gpu3d_copy(void* dst, const void* src, uint64_t n) {
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static void gpu3d_zero(void* dst, uint64_t n) {
  uint8_t* d = (uint8_t*)dst;
  for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

static void hdr_init(struct virtio_gpu_ctrl_hdr* h, uint32_t type, uint32_t flags,
                     uint64_t fence, uint32_t ctx) {
  h->type = type;
  h->flags = flags;
  h->fence_id = fence;
  h->ctx_id = ctx;
  h->padding = 0;
}

/* Fire one control-queue command and report what the device said. */
static int gpu3d_cmd(const void* req, uint32_t req_len, const void* data,
                     uint32_t data_len, void* resp, uint32_t resp_cap,
                     uint32_t* resp_len, uint32_t* resp_type) {
  return virtio_gpu_x64_transact_ex(req, req_len, data, data_len, resp, resp_cap,
                                    resp_len, resp_type);
}

/* ------------------------------------------------------------------ */
/* page pool                                                          */
/* ------------------------------------------------------------------ */

static int gpu3d_pool_alloc(int n) {
  if (n <= 0 || n > GPU3D_PAGE_POOL) return -1;
  int run = 0, start = 0;
  for (int i = 0; i < GPU3D_PAGE_POOL; i++) {
    if (gpu3d_pool_used[i >> 3] & (1u << (i & 7))) {
      run = 0;
      start = i + 1;
      continue;
    }
    run++;
    if (run == n) {
      for (int k = start; k < start + n; k++)
        gpu3d_pool_used[k >> 3] |= (uint8_t)(1u << (k & 7));
      return start;
    }
  }
  return -1;
}

static void gpu3d_pool_free(int start, int n) {
  if (start < 0 || n <= 0) return;
  for (int k = start; k < start + n && k < GPU3D_PAGE_POOL; k++)
    gpu3d_pool_used[k >> 3] &= (uint8_t)~(1u << (k & 7));
}

/* ------------------------------------------------------------------ */
/* lookups                                                            */
/* ------------------------------------------------------------------ */

static struct gpu3d_ctx* gpu3d_find_ctx(uint32_t id) {
  for (int i = 0; i < GPU3D_MAX_CTX; i++)
    if (gpu3d_ctxs[i].used && gpu3d_ctxs[i].id == id) return &gpu3d_ctxs[i];
  return 0;
}

static struct gpu3d_res* gpu3d_find_res(uint32_t id) {
  for (int i = 0; i < GPU3D_MAX_RES; i++)
    if (gpu3d_ress[i].used && gpu3d_ress[i].id == id) return &gpu3d_ress[i];
  return 0;
}

int virtio_gpu_3d_available(void) { return gpu3d_ready; }

/* ------------------------------------------------------------------ */
/* capset                                                             */
/* ------------------------------------------------------------------ */

static int gpu3d_capset_query(void) {
  uint32_t rlen = 0, rtype = 0;
  hdr_init(&gpu3d_req_capinfo.hdr, VIRTIO_GPU_CMD_GET_CAPSET_INFO, 0, 0, 0);
  gpu3d_req_capinfo.capset_index = 0;
  gpu3d_req_capinfo.padding = 0;
  gpu3d_zero(&gpu3d_resp_info, sizeof(gpu3d_resp_info));
  if (gpu3d_cmd(&gpu3d_req_capinfo, sizeof(gpu3d_req_capinfo), 0, 0,
                &gpu3d_resp_info, sizeof(gpu3d_resp_info), &rlen, &rtype) != 0) {
    uart_puts("[VIRGL3D] capset_info FAIL resp=");
    uart_print_hex(rtype);
    uart_puts(" len=");
    print_int((int)rlen);
    uart_puts("\n");
    return -1;
  }
  struct virtio_gpu_resp_capset_info* ri = &gpu3d_resp_info;
  gpu3d_capset_id = ri->capset_id;
  gpu3d_capset_maxver = ri->capset_max_version;
  gpu3d_capset_maxsize = ri->capset_max_size;
  uart_puts("[VIRGL3D] capset_info id=");
  print_int((int)gpu3d_capset_id);
  uart_puts(" maxver=");
  print_int((int)gpu3d_capset_maxver);
  uart_puts(" maxsize=");
  print_int((int)gpu3d_capset_maxsize);
  uart_puts(" resp=");
  uart_print_hex(rtype);
  uart_puts("\n");
  if (gpu3d_capset_id == 0 || gpu3d_capset_maxver == 0 ||
      gpu3d_capset_maxsize == 0 || gpu3d_capset_maxsize > GPU3D_CAPS_MAX)
    return -1;
  return 0;
}

int virtio_gpu_3d_capset_info(uint32_t* id, uint32_t* max_version, uint32_t* max_size) {
  if (!gpu3d_ready) return -ENODEV;
  if (id) *id = gpu3d_capset_id;
  if (max_version) *max_version = gpu3d_capset_maxver;
  if (max_size) *max_size = gpu3d_capset_maxsize;
  return 0;
}

int virtio_gpu_3d_capset(uint32_t id, uint32_t version, void* out, uint32_t cap,
                         uint32_t* size_out) {
  if (!gpu3d_ready) return -ENODEV;
  if (!out || cap < gpu3d_capset_maxsize) return -EINVAL;
  if (id != gpu3d_capset_id || version == 0 || version > gpu3d_capset_maxver)
    return -EINVAL;

  uint64_t fl = virtio_gpu_x64_lock();
  uint32_t rlen = 0, rtype = 0;
  hdr_init(&gpu3d_req_cap.hdr, VIRTIO_GPU_CMD_GET_CAPSET, 0, 0, 0);
  gpu3d_req_cap.capset_id = id;
  gpu3d_req_cap.capset_version = version;
  int rc = gpu3d_cmd(&gpu3d_req_cap, sizeof(gpu3d_req_cap), 0, 0, gpu3d_resp_cap,
                     GPU3D_RESP_HDR_LEN + gpu3d_capset_maxsize, &rlen, &rtype);
  if (rc == 0) {
    uint32_t n = 0;
    if (rlen > GPU3D_RESP_HDR_LEN) n = rlen - GPU3D_RESP_HDR_LEN;
    if (n > cap) n = cap;
    if (n > gpu3d_capset_maxsize) n = gpu3d_capset_maxsize;
    gpu3d_copy(out, gpu3d_resp_cap + GPU3D_RESP_HDR_LEN, n);
    if (size_out) *size_out = n;
  }
  virtio_gpu_x64_unlock(fl);
  return rc;
}

/* The cached blob (queried once at bring-up) -- what the ABI hands out
 * without a per-client device round trip. */
int virtio_gpu_3d_capset_cached(const void** blob, uint32_t* size_out,
                                uint32_t* id_out, uint32_t* version_out) {
  if (!gpu3d_ready || !gpu3d_caps_len) return -ENODEV;
  if (blob) *blob = gpu3d_caps;
  if (size_out) *size_out = gpu3d_caps_len;
  if (id_out) *id_out = gpu3d_capset_id;
  if (version_out) *version_out = gpu3d_capset_version;
  return 0;
}

int virtio_gpu_3d_init(void) {
  if (!virtio_gpu_x64_virgl_negotiated) return -ENODEV;
  gpu3d_ready = 0;
  uart_puts("[VIRGL3D] feat virgl=1 ctx_init=0\n");

  uint64_t fl = virtio_gpu_x64_lock();
  int rc = gpu3d_capset_query();
  if (rc == 0) {
    uint32_t ver = gpu3d_capset_maxver > 2 ? 2 : gpu3d_capset_maxver;
    uint32_t rlen = 0, rtype = 0;
    hdr_init(&gpu3d_req_cap.hdr, VIRTIO_GPU_CMD_GET_CAPSET, 0, 0, 0);
    gpu3d_req_cap.capset_id = gpu3d_capset_id;
    gpu3d_req_cap.capset_version = ver;
    rc = gpu3d_cmd(&gpu3d_req_cap, sizeof(gpu3d_req_cap), 0, 0, gpu3d_resp_cap,
                   GPU3D_RESP_HDR_LEN + gpu3d_capset_maxsize, &rlen, &rtype);
    if (rc != 0) {
      uart_puts("[VIRGL3D] capset FAIL resp=");
      uart_print_hex(rtype);
      uart_puts(" len=");
      print_int((int)rlen);
      uart_puts("\n");
    } else {
      uint32_t n = rlen > GPU3D_RESP_HDR_LEN ? rlen - GPU3D_RESP_HDR_LEN : 0;
      if (n > gpu3d_capset_maxsize) n = gpu3d_capset_maxsize;
      gpu3d_copy(gpu3d_caps, gpu3d_resp_cap + GPU3D_RESP_HDR_LEN, n);
      gpu3d_caps_len = n;
      gpu3d_capset_version = ver;
      /* virgl_caps_v1.max_version is the first dword of the blob. */
      uint32_t blob_ver = 0;
      if (n >= 4) {
        const uint8_t* p = gpu3d_caps;
        blob_ver = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 24);
      }
      uart_puts("[VIRGL3D] capset id=");
      print_int((int)gpu3d_capset_id);
      uart_puts(" ver=");
      print_int((int)ver);
      uart_puts(" len=");
      print_int((int)n);
      uart_puts(" blob_maxver=");
      print_int((int)blob_ver);
      uart_puts(" resp=");
      uart_print_hex(rtype);
      uart_puts("\n");
      if (n >= 4 && blob_ver != 0) gpu3d_ready = 1;
    }
  }
  virtio_gpu_x64_unlock(fl);
  if (!gpu3d_ready) uart_puts("[VIRGL3D] INIT FAIL (2D path unaffected)\n");
  return gpu3d_ready ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* contexts                                                           */
/* ------------------------------------------------------------------ */

int virtio_gpu_3d_ctx_create(uint32_t capset_id, uint32_t capset_version,
                             const char* name, uint32_t* ctx_id) {
  if (!gpu3d_ready) return -ENODEV;
  if (capset_id != gpu3d_capset_id || capset_version == 0 ||
      capset_version > gpu3d_capset_maxver)
    return -EINVAL;

  uint64_t fl = virtio_gpu_x64_lock();
  struct gpu3d_ctx* slot = 0;
  for (int i = 0; i < GPU3D_MAX_CTX; i++)
    if (!gpu3d_ctxs[i].used) {
      slot = &gpu3d_ctxs[i];
      break;
    }
  if (!slot) {
    virtio_gpu_x64_unlock(fl);
    return -ENOSPC;
  }

  uint32_t id = gpu3d_next_ctx_id++;
  hdr_init(&gpu3d_req_ctx.hdr, VIRTIO_GPU_CMD_CTX_CREATE, 0, 0, id);
  gpu3d_zero(gpu3d_req_ctx.debug_name, sizeof(gpu3d_req_ctx.debug_name));
  uint32_t nlen = 0;
  if (name) {
    while (name[nlen] && nlen < sizeof(gpu3d_req_ctx.debug_name)) {
      gpu3d_req_ctx.debug_name[nlen] = name[nlen];
      nlen++;
    }
  }
  gpu3d_req_ctx.nlen = nlen;
  gpu3d_req_ctx.context_init = 0;

  uint32_t rlen = 0, rtype = 0;
  int rc = gpu3d_cmd(&gpu3d_req_ctx, sizeof(gpu3d_req_ctx), 0, 0, &gpu3d_resp,
                     GPU3D_RESP_HDR_LEN, &rlen, &rtype);
  uart_puts("[VIRGL3D] ctx_create ctx=");
  print_int((int)id);
  uart_puts(" name_len=");
  print_int((int)nlen);
  uart_puts(" resp=");
  uart_print_hex(rtype);
  uart_puts("\n");
  if (rc != 0) {
    virtio_gpu_x64_unlock(fl);
    return -EIO;
  }

  slot->used = 1;
  slot->id = id;
  slot->owner_pid = -1;
  slot->owner_fd = -1;
  slot->capset_id = capset_id;
  slot->capset_version = capset_version;
  for (uint32_t i = 0; i < sizeof(slot->name) - 1 && i < nlen; i++)
    slot->name[i] = gpu3d_req_ctx.debug_name[i];
  slot->name[nlen < sizeof(slot->name) ? nlen : sizeof(slot->name) - 1] = 0;
  if (ctx_id) *ctx_id = id;
  virtio_gpu_x64_unlock(fl);
  return 0;
}

int virtio_gpu_3d_ctx_destroy(uint32_t ctx_id) {
  if (!gpu3d_ready) return -ENODEV;
  uint64_t fl = virtio_gpu_x64_lock();
  struct gpu3d_ctx* c = gpu3d_find_ctx(ctx_id);
  if (!c) {
    virtio_gpu_x64_unlock(fl);
    return -EINVAL;
  }

  /* Detach + unref anything this context still owns, so a destroyed context
   * can never strand a host resource (the M1 leak row). */
  for (int i = 0; i < GPU3D_MAX_RES; i++) {
    struct gpu3d_res* r = &gpu3d_ress[i];
    if (!r->used || r->ctx_id != ctx_id) continue;
    if (r->ctx_attached) {
      hdr_init(&gpu3d_req_ctxres.hdr, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, 0, 0, ctx_id);
      gpu3d_req_ctxres.resource_id = r->id;
      gpu3d_req_ctxres.padding = 0;
      gpu3d_cmd(&gpu3d_req_ctxres, sizeof(gpu3d_req_ctxres), 0, 0, &gpu3d_resp,
                GPU3D_RESP_HDR_LEN, 0, 0);
      r->ctx_attached = 0;
    }
    hdr_init(&gpu3d_req_unref.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF, 0, 0, 0);
    gpu3d_req_unref.resource_id = r->id;
    gpu3d_req_unref.padding = 0;
    gpu3d_cmd(&gpu3d_req_unref, sizeof(gpu3d_req_unref), 0, 0, &gpu3d_resp,
              GPU3D_RESP_HDR_LEN, 0, 0);
    if (r->attached) {
      gpu3d_pool_free(r->page_start, r->page_count);
      gpu3d_pages_in_use -= r->page_count;
    }
    if (gpu3d_res_in_use > 0) gpu3d_res_in_use--;
    gpu3d_zero(r, sizeof(*r));
  }

  hdr_init(&gpu3d_req_unref.hdr, VIRTIO_GPU_CMD_CTX_DESTROY, 0, 0, ctx_id);
  uint32_t rlen = 0, rtype = 0;
  int rc = gpu3d_cmd(&gpu3d_req_unref, sizeof(struct virtio_gpu_ctrl_hdr), 0, 0,
                     &gpu3d_resp, GPU3D_RESP_HDR_LEN, &rlen, &rtype);
  uart_puts("[VIRGL3D] ctx_destroy ctx=");
  print_int((int)ctx_id);
  uart_puts(" resp=");
  uart_print_hex(rtype);
  uart_puts("\n");
  /* The demand row: what this client actually asked the ABI for.  It is the
   * evidence for the M3 limits (docs/rimworld-virgl-m3.md §1) -- a refusal
   * without these numbers would be a guess. */
  uart_puts("[VIRGL3D] stats res_in_use=");
  print_int(gpu3d_res_in_use);
  uart_puts(" peak_res=");
  print_int(gpu3d_res_peak);
  uart_puts(" pages_in_use=");
  print_int(gpu3d_pages_in_use);
  uart_puts(" peak_pages=");
  print_int(gpu3d_pages_peak);
  uart_puts(" peak_mib=");
  print_int(gpu3d_pages_peak / 256);
  uart_puts(" caps=");
  print_int(GPU3D_MAX_RES);
  uart_puts("/");
  print_int(GPU3D_PAGE_POOL);
  uart_puts("\n");
  gpu3d_zero(c, sizeof(*c));
  virtio_gpu_x64_unlock(fl);
  return rc;
}

int virtio_gpu_3d_ctx_attach(uint32_t ctx_id, uint32_t res_id) {
  if (!gpu3d_ready) return -ENODEV;
  uint64_t fl = virtio_gpu_x64_lock();
  struct gpu3d_ctx* c = gpu3d_find_ctx(ctx_id);
  struct gpu3d_res* r = gpu3d_find_res(res_id);
  if (!c || !r || !r->attached) {
    virtio_gpu_x64_unlock(fl);
    return -EINVAL;
  }
  hdr_init(&gpu3d_req_ctxres.hdr, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, 0, 0, ctx_id);
  gpu3d_req_ctxres.resource_id = res_id;
  gpu3d_req_ctxres.padding = 0;
  uint32_t rlen = 0, rtype = 0;
  int rc = gpu3d_cmd(&gpu3d_req_ctxres, sizeof(gpu3d_req_ctxres), 0, 0, &gpu3d_resp,
                     GPU3D_RESP_HDR_LEN, &rlen, &rtype);
  uart_puts("[VIRGL3D] ctx_attach ctx=");
  print_int((int)ctx_id);
  uart_puts(" res=");
  print_int((int)res_id);
  uart_puts(" resp=");
  uart_print_hex(rtype);
  uart_puts("\n");
  if (rc == 0) r->ctx_attached = 1;
  virtio_gpu_x64_unlock(fl);
  return rc;
}

/* ------------------------------------------------------------------ */
/* resources                                                          */
/* ------------------------------------------------------------------ */

int virtio_gpu_3d_res_create(uint32_t ctx_id, const struct virtio_gpu_3d_res* d,
                             uint32_t* res_id) {
  if (!gpu3d_ready) return -ENODEV;
  if (!d || d->width == 0 || d->height == 0 || d->depth == 0 ||
      d->array_size == 0)
    return -EINVAL;

  uint64_t fl = virtio_gpu_x64_lock();
  struct gpu3d_ctx* c = gpu3d_find_ctx(ctx_id);
  if (!c) {
    virtio_gpu_x64_unlock(fl);
    return -EINVAL;
  }
  struct gpu3d_res* slot = 0;
  for (int i = 0; i < GPU3D_MAX_RES; i++)
    if (!gpu3d_ress[i].used) {
      slot = &gpu3d_ress[i];
      break;
    }
  if (!slot) {
    virtio_gpu_x64_unlock(fl);
    /* The exact request, so a table-full refusal is explainable. */
    uart_puts("[VIRGL3D] res_create refused: table full (max ");
    print_int(GPU3D_MAX_RES);
    uart_puts(") target=");
    print_int((int)d->target);
    uart_puts(" fmt=");
    print_int((int)d->format);
    uart_puts(" ");
    print_int((int)d->width);
    uart_puts("x");
    print_int((int)d->height);
    uart_puts("x");
    print_int((int)d->depth);
    uart_puts(" array=");
    print_int((int)d->array_size);
    uart_puts(" bind=");
    uart_print_hex(d->bind);
    uart_puts("\n");
    return -ENOSPC;
  }

  uint32_t id = gpu3d_next_res_id++;
  hdr_init(&gpu3d_req_res3d.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, 0, 0, 0);
  gpu3d_req_res3d.resource_id = id;
  gpu3d_req_res3d.target = d->target;
  gpu3d_req_res3d.format = d->format;
  gpu3d_req_res3d.bind = d->bind;
  gpu3d_req_res3d.width = d->width;
  gpu3d_req_res3d.height = d->height;
  gpu3d_req_res3d.depth = d->depth;
  gpu3d_req_res3d.array_size = d->array_size;
  gpu3d_req_res3d.last_level = d->last_level;
  gpu3d_req_res3d.nr_samples = d->nr_samples;
  gpu3d_req_res3d.flags = d->flags;

  uint32_t rlen = 0, rtype = 0;
  int rc = gpu3d_cmd(&gpu3d_req_res3d, sizeof(gpu3d_req_res3d), 0, 0, &gpu3d_resp,
                     GPU3D_RESP_HDR_LEN, &rlen, &rtype);
  if (rc == 0) {
    gpu3d_res_in_use++;
    if (gpu3d_res_in_use > gpu3d_res_peak) gpu3d_res_peak = gpu3d_res_in_use;
  }
  if (gpu3d_trace_res_row()) {
  uart_puts("[VIRGL3D] res_create res=");
  print_int((int)id);
  uart_puts(" ");
  print_int((int)d->width);
  uart_puts("x");
  print_int((int)d->height);
  uart_puts(" fmt=");
  print_int((int)d->format);
  uart_puts(" bind=");
  uart_print_hex(d->bind);
  uart_puts(" resp=");
  uart_print_hex(rtype);
  uart_puts("\n");
  }
  if (rc != 0) {
    virtio_gpu_x64_unlock(fl);
    return -EIO;
  }

  gpu3d_zero(slot, sizeof(*slot));
  slot->used = 1;
  slot->id = id;
  slot->owner_pid = -1;
  slot->owner_fd = -1;
  slot->ctx_id = ctx_id;
  slot->target = d->target;
  slot->format = d->format;
  slot->bind = d->bind;
  slot->width = d->width;
  slot->height = d->height;
  slot->depth = d->depth;
  slot->array_size = d->array_size;
  slot->last_level = d->last_level;
  slot->nr_samples = d->nr_samples;
  slot->flags = d->flags;
  if (res_id) *res_id = id;
  virtio_gpu_x64_unlock(fl);
  return 0;
}

int virtio_gpu_3d_res_attach(uint32_t res_id, const uint64_t* phys, uint32_t nr_pages,
                             uint64_t len) {
  if (!gpu3d_ready) return -ENODEV;
  if (!phys || nr_pages == 0 || nr_pages > GPU3D_MAX_ATTACH || len == 0)
    return -EINVAL;
  if ((len + 4095) / 4096 != nr_pages) return -EINVAL;

  uint64_t fl = virtio_gpu_x64_lock();
  struct gpu3d_res* r = gpu3d_find_res(res_id);
  if (!r || r->attached) {
    virtio_gpu_x64_unlock(fl);
    return -EINVAL;
  }
  int start = gpu3d_pool_alloc((int)nr_pages);
  if (start < 0) {
    int free_pages = 0;
    for (int i = 0; i < GPU3D_PAGE_POOL; i++)
      if (!(gpu3d_pool_used[i >> 3] & (1u << (i & 7)))) free_pages++;
    /* Say the exact demand: a pool refusal is the one ABI failure the lane
     * must be able to explain without guessing. */
    uart_puts("[VIRGL3D] attach refused res=");
    print_int((int)res_id);
    uart_puts(" need_pages=");
    print_int((int)nr_pages);
    uart_puts(" free_pages=");
    print_int(free_pages);
    uart_puts(" pool_pages=");
    print_int(GPU3D_PAGE_POOL);
    uart_puts(" reason=no_contiguous_run\n");
    virtio_gpu_x64_unlock(fl);
    return -ENOSPC;
  }

  /* Build the sg list.  Each entry is at most one page; the last entry is
   * short when `len` does not end on a page boundary. */
  uint64_t left = len;
  for (uint32_t i = 0; i < nr_pages; i++) {
    uint64_t chunk = left > 4096 ? 4096 : left;
    gpu3d_pool[start + i] = phys[i];
    gpu3d_ents[i].addr = phys[i];
    gpu3d_ents[i].length = (uint32_t)chunk;
    gpu3d_ents[i].padding = 0;
    left -= chunk;
  }

  hdr_init(&gpu3d_req_attach.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, 0, 0, 0);
  gpu3d_req_attach.resource_id = res_id;
  gpu3d_req_attach.nr_entries = nr_pages;

  uint32_t rlen = 0, rtype = 0;
  int rc = gpu3d_cmd(&gpu3d_req_attach, sizeof(gpu3d_req_attach), gpu3d_ents,
                     (uint32_t)(nr_pages * sizeof(struct virtio_gpu_mem_entry)),
                     &gpu3d_resp, GPU3D_RESP_HDR_LEN, &rlen, &rtype);
  if (gpu3d_trace_res_row()) {
  uart_puts("[VIRGL3D] attach res=");
  print_int((int)res_id);
  uart_puts(" pages=");
  print_int((int)nr_pages);
  uart_puts(" len=");
  print_int((int)len);
  uart_puts(" resp=");
  uart_print_hex(rtype);
  uart_puts("\n");
  }
  if (rc != 0) {
    gpu3d_pool_free(start, (int)nr_pages);
    virtio_gpu_x64_unlock(fl);
    return -EIO;
  }
  r->page_start = start;
  r->page_count = (int)nr_pages;
  r->len = len;
  r->attached = 1;
  gpu3d_pages_in_use += (int)nr_pages;
  if (gpu3d_pages_in_use > gpu3d_pages_peak) gpu3d_pages_peak = gpu3d_pages_in_use;
  virtio_gpu_x64_unlock(fl);
  return 0;
}

int virtio_gpu_3d_res_unref(uint32_t res_id) {
  if (!gpu3d_ready) return -ENODEV;
  uint64_t fl = virtio_gpu_x64_lock();
  struct gpu3d_res* r = gpu3d_find_res(res_id);
  if (!r) {
    virtio_gpu_x64_unlock(fl);
    return -EINVAL;
  }

  hdr_init(&gpu3d_req_unref.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF, 0, 0, 0);
  gpu3d_req_unref.resource_id = res_id;
  gpu3d_req_unref.padding = 0;
  uint32_t rlen = 0, rtype = 0;
  int rc = gpu3d_cmd(&gpu3d_req_unref, sizeof(gpu3d_req_unref), 0, 0, &gpu3d_resp,
                     GPU3D_RESP_HDR_LEN, &rlen, &rtype);
  if (gpu3d_trace_res_row()) {
  uart_puts("[VIRGL3D] unref res=");
  print_int((int)res_id);
  uart_puts(" resp=");
  uart_print_hex(rtype);
  uart_puts("\n");
  }
  if (r->attached) gpu3d_pool_free(r->page_start, r->page_count);
  if (r->attached) gpu3d_pages_in_use -= r->page_count;
  if (gpu3d_res_in_use > 0) gpu3d_res_in_use--;
  gpu3d_zero(r, sizeof(*r));
  virtio_gpu_x64_unlock(fl);
  return rc;
}

/* ------------------------------------------------------------------ */
/* transfers, submits, fences                                          */
/* ------------------------------------------------------------------ */

int virtio_gpu_3d_transfer(uint32_t res_id, int from_host,
                           const struct virtio_gpu_box* box, uint32_t level,
                           uint32_t stride, uint32_t layer_stride, uint64_t offset) {
  if (!gpu3d_ready) return -ENODEV;
  if (!box) return -EINVAL;
  uint64_t fl = virtio_gpu_x64_lock();
  struct gpu3d_res* r = gpu3d_find_res(res_id);
  if (!r || !r->attached) {
    virtio_gpu_x64_unlock(fl);
    return -EINVAL;
  }
  hdr_init(&gpu3d_req_xfer.hdr,
           from_host ? VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D
                     : VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
           0, 0, 0);
  gpu3d_req_xfer.box = *box;
  gpu3d_req_xfer.offset = offset;
  gpu3d_req_xfer.resource_id = res_id;
  gpu3d_req_xfer.level = level;
  gpu3d_req_xfer.stride = stride;
  gpu3d_req_xfer.layer_stride = layer_stride;

  uint32_t rlen = 0, rtype = 0;
  int rc = gpu3d_cmd(&gpu3d_req_xfer, sizeof(gpu3d_req_xfer), 0, 0, &gpu3d_resp,
                     GPU3D_RESP_HDR_LEN, &rlen, &rtype);
  if (!gpu3d_quiet) {
    uart_puts(from_host ? "[VIRGL3D] transfer_from_host res="
                        : "[VIRGL3D] transfer_to_host res=");
    print_int((int)res_id);
    uart_puts(" box=");
    print_int((int)box->w);
    uart_puts("x");
    print_int((int)box->h);
    uart_puts("x");
    print_int((int)box->d);
    uart_puts(" resp=");
    uart_print_hex(rtype);
    uart_puts("\n");
  }
  virtio_gpu_x64_unlock(fl);
  return rc;
}

/* `fenced` is the M1 public behaviour (1).  The probe's measurement pass
 * also runs an unfenced variant: the round trip without a fence is what M3
 * will pay if it batches a whole frame into one submit, and the difference
 * between the two rows is exactly the fence-completion cost. */
static int gpu3d_submit_ex(uint32_t ctx_id, const void* cmds, uint32_t len,
                           int fenced, uint32_t* fence_id) {
  if (!gpu3d_ready) return -ENODEV;
  if (!cmds || len == 0 || len > GPU3D_SUBMIT_MAX || (len & 3u)) return -EINVAL;

  uint64_t fl = virtio_gpu_x64_lock();
  struct gpu3d_ctx* c = gpu3d_find_ctx(ctx_id);
  if (!c) {
    virtio_gpu_x64_unlock(fl);
    return -EINVAL;
  }

  uint32_t fid = gpu3d_fence_seq + 1;
  if (fenced) gpu3d_fence_seq = fid;
  struct virtio_gpu_cmd_submit* cmd = (struct virtio_gpu_cmd_submit*)gpu3d_submit_buf;
  hdr_init(&cmd->hdr, VIRTIO_GPU_CMD_SUBMIT_3D,
           fenced ? VIRTIO_GPU_FLAG_FENCE : 0, fenced ? fid : 0, ctx_id);
  cmd->size = len;
  cmd->padding = 0;
  gpu3d_copy(gpu3d_submit_buf + sizeof(*cmd), cmds, len);

  uint32_t rlen = 0, rtype = 0;
  int rc = gpu3d_cmd(gpu3d_submit_buf, (uint32_t)(sizeof(*cmd) + len), 0, 0,
                     &gpu3d_resp, GPU3D_RESP_HDR_LEN, &rlen, &rtype);
  struct virtio_gpu_ctrl_hdr* rh = (struct virtio_gpu_ctrl_hdr*)&gpu3d_resp;
  uint64_t echoed = rh->fence_id;
  if (!gpu3d_quiet) {
    uart_puts("[VIRGL3D] submit ctx=");
    print_int((int)ctx_id);
    uart_puts(" bytes=");
    print_int((int)len);
    uart_puts(" fence=");
    print_int(fenced ? (int)fid : 0); /* unfenced submits have no fence id */
    uart_puts(" echoed=");
    print_int((int)echoed);
    uart_puts(" resp=");
    uart_print_hex(rtype);
    uart_puts("\n");
  }
  if (fenced) {
    /* A fenced command's used-ring completion IS the host's completion.  A
     * failed submit still retires its id so a later WAIT cannot block; the
     * submit's return code is the status. */
    gpu3d_fence_done = fid;
    if (rc == 0 && fence_id) *fence_id = fid;
  }
  virtio_gpu_x64_unlock(fl);
  return rc;
}

int virtio_gpu_3d_submit(uint32_t ctx_id, const void* cmds, uint32_t len,
                         uint32_t* fence_id) {
  return gpu3d_submit_ex(ctx_id, cmds, len, 1, fence_id);
}

int virtio_gpu_3d_submit_unfenced(uint32_t ctx_id, const void* cmds, uint32_t len) {
  return gpu3d_submit_ex(ctx_id, cmds, len, 0, 0);
}

int virtio_gpu_3d_wait(uint32_t fence_id) {
  if (!gpu3d_ready) return -ENODEV;
  if (fence_id == 0 || fence_id > gpu3d_fence_seq) return -EINVAL;
  /* M1 submits are synchronous: taking the lock means any issued fence has
   * already been retired on the device. */
  uint64_t fl = virtio_gpu_x64_lock();
  int done = (fence_id <= gpu3d_fence_done);
  virtio_gpu_x64_unlock(fl);
  return done ? 0 : -EAGAIN;
}

int virtio_gpu_3d_flush(uint32_t res_id, const struct virtio_gpu_box* box) {
  if (!gpu3d_ready) return -ENODEV;
  if (!box) return -EINVAL;
  uint64_t fl = virtio_gpu_x64_lock();
  struct gpu3d_res* r = gpu3d_find_res(res_id);
  if (!r) {
    virtio_gpu_x64_unlock(fl);
    return -EINVAL;
  }
  uint32_t fid = gpu3d_fence_seq + 1;
  gpu3d_fence_seq = fid;
  hdr_init(&gpu3d_req_flush.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH,
           VIRTIO_GPU_FLAG_FENCE, fid, 0);
  gpu3d_req_flush.r.x = box->x;
  gpu3d_req_flush.r.y = box->y;
  gpu3d_req_flush.r.width = box->w;
  gpu3d_req_flush.r.height = box->h;
  gpu3d_req_flush.resource_id = res_id;
  gpu3d_req_flush.padding = 0;
  uint32_t rlen = 0, rtype = 0;
  int rc = gpu3d_cmd(&gpu3d_req_flush, sizeof(gpu3d_req_flush), 0, 0, &gpu3d_resp,
                     GPU3D_RESP_HDR_LEN, &rlen, &rtype);
  uart_puts("[VIRGL3D] flush res=");
  print_int((int)res_id);
  uart_puts(" fence=");
  print_int((int)fid);
  uart_puts(" resp=");
  uart_print_hex(rtype);
  uart_puts("\n");
  if (rc == 0) gpu3d_fence_done = fid;
  else gpu3d_fence_done = fid;
  virtio_gpu_x64_unlock(fl);
  return rc;
}

/* ------------------------------------------------------------------ */
/* M1 gate: kernel-only 3D clear + readback                           */
/* ------------------------------------------------------------------ */

static uint32_t gpu3d_probe_buf[GPU3D_PROBE_W * GPU3D_PROBE_H]
    __attribute__((aligned(4096)));

static int gpu3d_probe_clear_stream(uint32_t* w, uint32_t res_id) {
  /* VIRGL_CCMD_CLEAR_TEXTURE: header + res handle, level, box (x,y,z,w,h,d),
   * then four clear-value dwords.  12 payload dwords. */
  w[0] = (uint32_t)VIRGL_CCMD_CLEAR_TEXTURE | (0u << 8) |
         ((uint32_t)VIRGL_CLEAR_TEXTURE_SIZE << 16);
  w[1] = res_id;
  w[2] = 0; /* level */
  w[3] = 0; /* x */
  w[4] = 0; /* y */
  w[5] = 0; /* z */
  w[6] = GPU3D_PROBE_W;
  w[7] = GPU3D_PROBE_H;
  w[8] = 1; /* depth */
  w[9] = GPU3D_PROBE_RGBA; /* RGBA8: bytes R,G,B,A = 00 FF 00 FF */
  w[10] = 0;
  w[11] = 0;
  w[12] = 0;
  return 13 * 4; /* bytes */
}

void virtio_gpu_3d_probe(void) {
  if (!gpu3d_ready) return;

  struct virtio_gpu_3d_res d;
  gpu3d_zero(&d, sizeof(d));
  d.target = VIRGL_TEXTURE_TARGET_2D;
  d.format = VIRGL_FORMAT_R8G8B8A8_UNORM;
  d.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW;
  d.width = GPU3D_PROBE_W;
  d.height = GPU3D_PROBE_H;
  d.depth = 1;
  d.array_size = 1;
  d.last_level = 0;
  d.nr_samples = 0;
  d.flags = 0;

  uint32_t ctx = 0, res = 0, fence = 0;
  if (virtio_gpu_3d_ctx_create(gpu3d_capset_id, gpu3d_capset_version, "m1-probe",
                               &ctx) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=ctx_create\n");
    return;
  }
  if (virtio_gpu_3d_res_create(ctx, &d, &res) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=res_create\n");
    virtio_gpu_3d_ctx_destroy(ctx);
    return;
  }

  /* 16 KiB = 4 pages of kernel memory; identity (VA == phys). */
  const int npages = (int)(sizeof(gpu3d_probe_buf) / 4096);
  uint64_t phys[8];
  for (int i = 0; i < npages; i++)
    phys[i] = (uint64_t)(uintptr_t)(gpu3d_probe_buf + i * (4096 / 4));
  if (virtio_gpu_3d_res_attach(res, phys, (uint32_t)npages, sizeof(gpu3d_probe_buf)) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=attach\n");
    virtio_gpu_3d_res_unref(res);
    virtio_gpu_3d_ctx_destroy(ctx);
    return;
  }
  if (virtio_gpu_3d_ctx_attach(ctx, res) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=ctx_attach\n");
    virtio_gpu_3d_res_unref(res);
    virtio_gpu_3d_ctx_destroy(ctx);
    return;
  }

  /* Start from a known non-green image so "readback shows the clear" cannot
   * be satisfied by uninitialised memory. */
  for (uint32_t i = 0; i < GPU3D_PROBE_W * GPU3D_PROBE_H; i++) gpu3d_probe_buf[i] = 0;
  struct virtio_gpu_box full;
  full.x = 0; full.y = 0; full.z = 0;
  full.w = GPU3D_PROBE_W; full.h = GPU3D_PROBE_H; full.d = 1;
  if (virtio_gpu_3d_transfer(res, 0, &full, 0, GPU3D_PROBE_W * 4,
                             GPU3D_PROBE_W * GPU3D_PROBE_H * 4, 0) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=transfer_to_host\n");
    virtio_gpu_3d_res_unref(res);
    virtio_gpu_3d_ctx_destroy(ctx);
    return;
  }

  uint32_t stream[32];
  int slen = gpu3d_probe_clear_stream(stream, res);
  if (virtio_gpu_3d_submit(ctx, stream, (uint32_t)slen, &fence) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=submit\n");
    virtio_gpu_3d_res_unref(res);
    virtio_gpu_3d_ctx_destroy(ctx);
    return;
  }
  if (virtio_gpu_3d_wait(fence) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=fence_wait\n");
    virtio_gpu_3d_res_unref(res);
    virtio_gpu_3d_ctx_destroy(ctx);
    return;
  }
  uart_puts("[VIRGL3D] fence id=");
  print_int((int)fence);
  uart_puts(" retired=1\n");

  if (virtio_gpu_3d_transfer(res, 1, &full, 0, GPU3D_PROBE_W * 4,
                             GPU3D_PROBE_W * GPU3D_PROBE_H * 4, 0) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=transfer_from_host\n");
    virtio_gpu_3d_res_unref(res);
    virtio_gpu_3d_ctx_destroy(ctx);
    return;
  }

  uint32_t px00 = gpu3d_probe_buf[0];
  uint32_t px_n = gpu3d_probe_buf[GPU3D_PROBE_W * GPU3D_PROBE_H - 1];
  uint32_t px_mid = gpu3d_probe_buf[(GPU3D_PROBE_H / 2) * GPU3D_PROBE_W +
                                    GPU3D_PROBE_W / 2];
  int match = 0;
  for (uint32_t i = 0; i < GPU3D_PROBE_W * GPU3D_PROBE_H; i++)
    if (gpu3d_probe_buf[i] == GPU3D_PROBE_RGBA) match++;

  uart_puts("[VIRGL3D] readback px00=");
  uart_print_hex(px00);
  uart_puts(" px_mid=");
  uart_print_hex(px_mid);
  uart_puts(" px_last=");
  uart_print_hex(px_n);
  uart_puts(" expect=");
  uart_print_hex(GPU3D_PROBE_RGBA);
  uart_puts(" match=");
  print_int(match);
  uart_puts("/");
  print_int(GPU3D_PROBE_W * GPU3D_PROBE_H);
  uart_puts("\n");

#if VGPU3D_SELFTEST_TIMING
  /* M1's falsifier numbers (docs/rimworld-virgl-plan.md §8): the cost of one
   * submit + readback round trip, measured as a loop because the guest clock
   * is 1 ms-granular.  Same 64x64 resource, same fence path. */
  {
    uint64_t t0 = timer_get_ms();
    int ok = 1;
    gpu3d_quiet = 1;
    for (int i = 0; i < GPU3D_TIMING_ITERS && ok; i++) {
      if (virtio_gpu_3d_submit(ctx, stream, (uint32_t)slen, &fence) != 0) ok = 0;
      else if (virtio_gpu_3d_transfer(res, 1, &full, 0, GPU3D_PROBE_W * 4,
                                      GPU3D_PROBE_W * GPU3D_PROBE_H * 4, 0) != 0)
        ok = 0;
    }
    gpu3d_quiet = 0;
    uint64_t t1 = timer_get_ms();
    uart_puts("[VIRGL3D] timing submits=");
    print_int(GPU3D_TIMING_ITERS);
    if (t0 == 0 && t1 == 0) {
      /* Before the first PIT tick calibrates the TSC the clock reads 0;
       * the row stays honest instead of reporting a fake 0 us. */
      uart_puts(" clock=uncalibrated");
    } else {
      uint64_t dt = t1 >= t0 ? t1 - t0 : 0;
      uart_puts(" total_ms=");
      print_int((int)dt);
      uart_puts(" us_per_roundtrip=");
      print_int(dt ? (int)((dt * 1000) / GPU3D_TIMING_ITERS) : 0);
    }
    uart_puts(ok ? " ok=1\n" : " ok=0\n");

    /* Split the round trip: a fenced submit with no readback isolates the
     * fence/used-ring cost, and a readback with no submit isolates the
     * transfer cost.  M2/M3 need both numbers, not just their sum. */
    {
      uint64_t s0 = timer_get_ms();
      gpu3d_quiet = 1;
      int ok2 = 1;
      for (int i = 0; i < GPU3D_TIMING_ITERS && ok2; i++)
        if (virtio_gpu_3d_submit(ctx, stream, (uint32_t)slen, &fence) != 0) ok2 = 0;
      gpu3d_quiet = 0;
      uint64_t s1 = timer_get_ms();
      uart_puts("[VIRGL3D] timing_submit submits=");
      print_int(GPU3D_TIMING_ITERS);
      uart_puts(" total_ms=");
      print_int((int)(s1 >= s0 ? s1 - s0 : 0));
      uart_puts(" us_each=");
      print_int(s1 > s0 ? (int)(((s1 - s0) * 1000) / GPU3D_TIMING_ITERS) : 0);
      uart_puts(ok2 ? " ok=1\n" : " ok=0\n");
    }
    {
      uint64_t u0 = timer_get_ms();
      gpu3d_quiet = 1;
      int ok4 = 1;
      for (int i = 0; i < GPU3D_TIMING_ITERS && ok4; i++) {
        if (gpu3d_submit_ex(ctx, stream, (uint32_t)slen, 0, 0) != 0) ok4 = 0;
        else if (virtio_gpu_3d_transfer(res, 1, &full, 0, GPU3D_PROBE_W * 4,
                                        GPU3D_PROBE_W * GPU3D_PROBE_H * 4, 0) != 0)
          ok4 = 0;
      }
      gpu3d_quiet = 0;
      uint64_t u1 = timer_get_ms();
      uart_puts("[VIRGL3D] timing_unfenced submits=");
      print_int(GPU3D_TIMING_ITERS);
      uart_puts(" total_ms=");
      print_int((int)(u1 >= u0 ? u1 - u0 : 0));
      uart_puts(" us_per_roundtrip=");
      print_int(u1 > u0 ? (int)(((u1 - u0) * 1000) / GPU3D_TIMING_ITERS) : 0);
      uart_puts(ok4 ? " ok=1\n" : " ok=0\n");
    }
    /* Ordering proof for the cheap (unfenced) path: clear to opaque red,
     * read the resource straight back, and require red.  If QEMU reordered
     * the transfer before the submit, this reads green and fails. */
    {
      stream[9] = 0xFF0000FFu; /* RGBA8: R=FF G=00 B=00 A=FF */
      int orc = gpu3d_submit_ex(ctx, stream, (uint32_t)slen, 0, 0);
      if (orc == 0)
        orc = virtio_gpu_3d_transfer(res, 1, &full, 0, GPU3D_PROBE_W * 4,
                                     GPU3D_PROBE_W * GPU3D_PROBE_H * 4, 0);
      uint32_t red = gpu3d_probe_buf[0];
      uart_puts("[VIRGL3D] unfenced_readback px00=");
      uart_print_hex(red);
      uart_puts(" expect=0x00000000FF0000FF\n");
      if (orc != 0 || red != 0xFF0000FFu) {
        uart_puts("[VIRGL3D] RESULT FAIL step=unfenced_order\n");
        virtio_gpu_3d_res_unref(res);
        virtio_gpu_3d_ctx_destroy(ctx);
        return;
      }
      stream[9] = GPU3D_PROBE_RGBA; /* leave the resource green for the rest */
      gpu3d_submit_ex(ctx, stream, (uint32_t)slen, 0, 0);
    }
    {
      uint64_t r0 = timer_get_ms();
      gpu3d_quiet = 1;
      int ok3 = 1;
      for (int i = 0; i < GPU3D_TIMING_ITERS && ok3; i++)
        if (virtio_gpu_3d_transfer(res, 1, &full, 0, GPU3D_PROBE_W * 4,
                                   GPU3D_PROBE_W * GPU3D_PROBE_H * 4, 0) != 0)
          ok3 = 0;
      gpu3d_quiet = 0;
      uint64_t r1 = timer_get_ms();
      uart_puts("[VIRGL3D] timing_readback bytes=");
      print_int((int)sizeof(gpu3d_probe_buf));
      uart_puts(" total_ms=");
      print_int((int)(r1 >= r0 ? r1 - r0 : 0));
      uart_puts(" us_each=");
      print_int(r1 > r0 ? (int)(((r1 - r0) * 1000) / GPU3D_TIMING_ITERS) : 0);
      uart_puts(ok3 ? " ok=1\n" : " ok=0\n");
    }
  }
#endif

  /* Fenced RESOURCE_FLUSH: the completion path a 3D present will use in M5. */
  virtio_gpu_3d_flush(res, &full);

  if (virtio_gpu_3d_res_unref(res) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=unref\n");
    virtio_gpu_3d_ctx_destroy(ctx);
    return;
  }
  if (virtio_gpu_3d_ctx_destroy(ctx) != 0) {
    uart_puts("[VIRGL3D] RESULT FAIL step=ctx_destroy\n");
    return;
  }

  int pass = (match == GPU3D_PROBE_W * GPU3D_PROBE_H) && px00 == GPU3D_PROBE_RGBA;
  uart_puts(pass ? "[VIRGL3D] RESULT PASS\n" : "[VIRGL3D] RESULT FAIL step=pixels\n");
}

#endif /* __x86_64__ */
