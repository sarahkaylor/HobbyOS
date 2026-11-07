#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

// VirtIO GPU Feature Bits
#define VIRTIO_GPU_F_VIRGL 0           // Support for VirGL 3D hardware acceleration
#define VIRTIO_GPU_F_EDID 1            // EDID exposed as a response
#define VIRTIO_GPU_F_CONTEXT_INIT 4    // Context-init payload in CTX_CREATE

// VirtIO GPU Control Commands
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO       0x0100 // Get display resolution/status
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     0x0101 // Create a 2D resource in host
#define VIRTIO_GPU_CMD_RESOURCE_UNREF         0x0102 // Destroy a host resource
#define VIRTIO_GPU_CMD_SET_SCANOUT            0x0103 // Map resource to display scanout
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH         0x0104 // Flush host resource to display
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    0x0105 // Transfer guest buffer to host resource
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106 // Attach guest memory to resource
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107 // Detach guest memory from resource

/* M1 (docs/rimworld-virgl-plan.md §3.3): capset query + the 3D command set.
 * These are the Linux uapi opcodes; QEMU's virtio-gpu dispatches them straight
 * into virglrenderer once VIRTIO_GPU_F_VIRGL is negotiated. */
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108 // Capset id/version/size
#define VIRTIO_GPU_CMD_GET_CAPSET               0x0109 // Capset blob
#define VIRTIO_GPU_CMD_CTX_CREATE               0x0200 // Create a virgl context
#define VIRTIO_GPU_CMD_CTX_DESTROY              0x0201 // Destroy a virgl context
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE      0x0202 // Bind a resource into a context
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE      0x0203 // Unbind a resource from a context
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D       0x0204 // Create a 3D (virgl) resource
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D      0x0205 // Upload into a 3D resource
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D    0x0206 // Read a 3D resource back
#define VIRTIO_GPU_CMD_SUBMIT_3D                0x0207 // Submit a virgl command stream

// VirtIO GPU Response Types
#define VIRTIO_GPU_RESP_OK_NODATA             0x1100 // Command succeeded
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO       0x1101 // Display info returned
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO        0x1102 // GET_CAPSET_INFO answered
#define VIRTIO_GPU_RESP_OK_CAPSET             0x1103 // GET_CAPSET answered
#define VIRTIO_GPU_RESP_ERR_UNSPEC            0x1200 // Unspecified error
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY     0x1201 // Host allocation failed
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1205

/* ctrl_hdr.flags (M1 fence path): a command carrying this flag is not
 * completed (its used-ring entry is withheld) until the host GPU has
 * finished it, and the response echoes fence_id. */
#define VIRTIO_GPU_FLAG_FENCE (1u << 0)

// VirtIO GPU Color Formats
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM      1 // 32-bit BGRA format

// Rectangle definition for clipping and updates
/**
 * Rectangle definition for clipping and display updates.
 */
struct virtio_gpu_rect {
  uint32_t x;         /**< X-coordinate of top-left corner */
  uint32_t y;         /**< Y-coordinate of top-left corner */
  uint32_t width;     /**< Width of the rectangle */
  uint32_t height;    /**< Height of the rectangle */
};

// Standard header for all VirtIO GPU control commands
/**
 * Standard header for all VirtIO GPU control commands.
 */
struct virtio_gpu_ctrl_hdr {
  uint32_t type;     /**< Command type (VIRTIO_GPU_CMD_*) */
  uint32_t flags;    /**< Command flags */
  uint64_t fence_id; /**< Fence ID for synchronization */
  uint32_t ctx_id;   /**< Context ID */
  uint32_t padding;
};

// Command to create a 2D resource on the host
/**
 * Command to create a 2D resource on the host.
 */
struct virtio_gpu_resource_create_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id; /**< Unique ID for the resource */
  uint32_t format;      /**< Color format (VIRTIO_GPU_FORMAT_*) */
  uint32_t width;       /**< Resource width in pixels */
  uint32_t height;      /**< Resource height in pixels */
};

// Command to attach guest memory pages to a host resource
struct virtio_gpu_resource_attach_backing {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id; // Target resource ID
  uint32_t nr_entries;  // Number of memory entries (scatter-gather list)
};

// A single entry in the resource backing memory list
struct virtio_gpu_mem_entry {
  uint64_t addr;   // Guest physical address
  uint32_t length; // Length of the memory segment
  uint32_t padding;
};

// Command to link a resource to a display scanout
struct virtio_gpu_set_scanout {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r; // Display region
  uint32_t scanout_id;      // Display output ID
  uint32_t resource_id;     // Resource to display
};

// Command to transfer guest memory contents to a host resource
struct virtio_gpu_transfer_to_host_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r; // Source region in the guest buffer
  uint64_t offset;          // Destination offset in host resource
  uint32_t resource_id;     // Target resource ID
  uint32_t padding;
};

// Command to trigger a display update from a host resource
struct virtio_gpu_resource_flush {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r; // Region to flush to display
  uint32_t resource_id;     // Source resource ID
  uint32_t padding;
};

/**
 * Initializes the VirtIO GPU device, setup display info, and create the primary framebuffer.
 *
 * Returns:
 *   0 on success, -1 on failure.
 */
int virtio_gpu_init(void);

/* ------------------------------------------------------------------ */
/* M1 (docs/rimworld-virgl-plan.md): virtio-gpu 3D command layouts.    */
/* Wire structs only -- the command encoding is the Linux uapi.        */
/* ------------------------------------------------------------------ */

struct virtio_gpu_ctx_create {
  struct virtio_gpu_ctrl_hdr hdr; /* hdr.ctx_id = new context id */
  uint32_t nlen;                  /* debug_name length, <= 64 */
  uint32_t context_init;          /* 0 unless CONTEXT_INIT is negotiated */
  char debug_name[64];
};

struct virtio_gpu_ctx_destroy {
  struct virtio_gpu_ctrl_hdr hdr;
};

struct virtio_gpu_resource_unref {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
};

struct virtio_gpu_ctx_resource {
  struct virtio_gpu_ctrl_hdr hdr; /* hdr.ctx_id = context */
  uint32_t resource_id;
  uint32_t padding;
};

struct virtio_gpu_get_capset_info {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t capset_index; /* 0-based probe index; 0 is VIRGL */
  uint32_t padding;
};

struct virtio_gpu_resp_capset_info {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t capset_id;
  uint32_t capset_max_version;
  uint32_t capset_max_size;
  uint32_t padding;
};

struct virtio_gpu_get_capset {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t capset_id;
  uint32_t capset_version;
};

/* GET_CAPSET's response is hdr followed by capset_max_size bytes. */

struct virtio_gpu_box {
  uint32_t x, y, z;
  uint32_t w, h, d;
};

struct virtio_gpu_resource_create_3d {
  struct virtio_gpu_ctrl_hdr hdr; /* hdr.ctx_id unused by the host */
  uint32_t resource_id;
  uint32_t target;   /* virgl texture target (2 = 2D) */
  uint32_t format;   /* virgl format enum */
  uint32_t bind;     /* virgl bind flags */
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t array_size;
  uint32_t last_level;
  uint32_t nr_samples;
  uint32_t flags;
};

struct virtio_gpu_transfer_host_3d {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_box box;
  uint64_t offset;
  uint32_t resource_id;
  uint32_t level;
  uint32_t stride;
  uint32_t layer_stride;
};

/* SUBMIT_3D: this struct followed immediately by `size` bytes of virgl
 * command stream (one contiguous range, per the uapi). */
struct virtio_gpu_cmd_submit {
  struct virtio_gpu_ctrl_hdr hdr; /* hdr.ctx_id = target context */
  uint32_t size;
  uint32_t padding;
};

/* Kernel-internal description of a 3D resource (no wire header). */
struct virtio_gpu_3d_res {
  uint32_t target;
  uint32_t format;
  uint32_t bind;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t array_size;
  uint32_t last_level;
  uint32_t nr_samples;
  uint32_t flags;
};

/**
 * Flushes the kernel-side framebuffer contents to the host display device.
 */
void virtio_gpu_flush(void);

/**
 * GX (docs/graphics-accel.md): damage-rect present.  Presents the listed
 * screen-space rects; after it returns, every covered pixel must equal a
 * full virtio_gpu_flush() result (implementations may expand/merge rects
 * internally; overlaps/order are meaningless; count == 0 is a no-op).
 * Freeze default: the full-flush fallback -- G1/G2 replace the internals
 * with the real rect command paths (ARM virtio-mmio / x64 virtio-pci+BGA).
 */
struct virtio_gpu_xrect { int32_t x, y, w, h; };
void virtio_gpu_flush_rects(const struct virtio_gpu_xrect *rects, int count);
/* SEC-7 (security-plan.md #7): syscall-boundary batch validation.  Returns
 * 1 if every rect is non-degenerate and fully inside the framebuffer and
 * the summed area is within the per-call cap; 0 means reject (-EINVAL). */
int virtio_gpu_flush_args_ok(const struct virtio_gpu_xrect *rects, int count);

/**
 * Returns the base address of the kernel's memory-mapped framebuffer.
 */
uint32_t* virtio_gpu_get_framebuffer(void);

/* ------------------------------------------------------------------ */
/* M1: kernel 3D transport (x64).  virtio_gpu_3d.c owns this surface;  */
/* it is only reachable once VIRTIO_GPU_F_VIRGL was negotiated, and    */
/* every entry point fails cleanly (-ENODEV / -EINVAL) otherwise.      */
/* ------------------------------------------------------------------ */
#ifdef __x86_64__
/* M1 gate switch (main.c + virtio_gpu_3d.c): the boot-time 3D clear +
 * readback probe.  1 = run it whenever VIRTIO_GPU_F_VIRGL was negotiated. */
#ifndef VGPU3D_SELFTEST
#define VGPU3D_SELFTEST 1
#endif

/* Control-queue transaction with an explicit response buffer and the
 * device's response type/length reported back (GET_CAPSET needs both). */
int virtio_gpu_x64_transact_ex(const void* req, uint32_t req_len,
                               const void* data, uint32_t data_len,
                               void* resp, uint32_t resp_cap,
                               uint32_t* resp_len, uint32_t* resp_type);
/* Serializes 3D work against the 2D present path (same gpu_lock). */
uint64_t virtio_gpu_x64_lock(void);
void virtio_gpu_x64_unlock(uint64_t flags);

int virtio_gpu_3d_available(void);
/* Bring-up: capset discovery, run once after the 2D resource exists.  A
 * failure leaves gpu3d unavailable and the 2D desktop untouched. */
int virtio_gpu_3d_init(void);
/* M1 gate: the kernel-only 3D clear + readback probe ([VIRGL3D] rows). */
void virtio_gpu_3d_probe(void);
/* Capset discovery (VIRGL = id 1).  Returns 0 on success. */
int virtio_gpu_3d_capset_info(uint32_t* id, uint32_t* max_version, uint32_t* max_size);
int virtio_gpu_3d_capset(uint32_t id, uint32_t version, void* out, uint32_t cap,
                         uint32_t* size_out);
int virtio_gpu_3d_capset_cached(const void** blob, uint32_t* size_out,
                                uint32_t* id_out, uint32_t* version_out);
/* Contexts.  `ctx_id` is kernel-allocated, owner checked at the ABI layer. */
int virtio_gpu_3d_ctx_create(uint32_t capset_id, uint32_t capset_version,
                             const char* name, uint32_t* ctx_id);
int virtio_gpu_3d_ctx_destroy(uint32_t ctx_id);
int virtio_gpu_3d_ctx_attach(uint32_t ctx_id, uint32_t res_id);
/* Resources: create, attach a guest-physical page list, unref. */
int virtio_gpu_3d_res_create(uint32_t ctx_id, const struct virtio_gpu_3d_res* d,
                             uint32_t* res_id);
int virtio_gpu_3d_res_attach(uint32_t res_id, const uint64_t* phys, uint32_t nr_pages,
                             uint64_t len);
int virtio_gpu_3d_res_unref(uint32_t res_id);
/* Submits are fenced and synchronous in M1: on return the host has retired
 * `fence_id`, so a following TRANSFER_FROM_HOST_3D sees the commands' effect. */
int virtio_gpu_3d_submit(uint32_t ctx_id, const void* cmds, uint32_t len,
                         uint32_t* fence_id);
/* M1 measurement path: same submit without VIRTIO_GPU_FLAG_FENCE.  Returns
 * when the device accepted the stream; QEMU still executes the context's
 * commands in order, so a following TRANSFER_FROM_HOST_3D observes it (the
 * probe asserts this).  Measured 100x cheaper than the fenced form because a
 * fenced command waits for QEMU's virgl poll; M2/M3 should batch a frame into
 * one submit and fence once, if at all. */
int virtio_gpu_3d_submit_unfenced(uint32_t ctx_id, const void* cmds, uint32_t len);
int virtio_gpu_3d_wait(uint32_t fence_id);
int virtio_gpu_3d_transfer(uint32_t res_id, int from_host,
                           const struct virtio_gpu_box* box, uint32_t level,
                           uint32_t stride, uint32_t layer_stride, uint64_t offset);
int virtio_gpu_3d_flush(uint32_t res_id, const struct virtio_gpu_box* box);
#endif /* __x86_64__ */

#endif // VIRTIO_GPU_H
