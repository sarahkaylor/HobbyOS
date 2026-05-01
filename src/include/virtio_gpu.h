#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

// VirtIO GPU Feature Bits
#define VIRTIO_GPU_F_VIRGL 0 // Support for VirGL 3D hardware acceleration

// VirtIO GPU Control Commands
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO       0x0100 // Get display resolution/status
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     0x0101 // Create a 2D resource in host
#define VIRTIO_GPU_CMD_RESOURCE_UNREF         0x0102 // Destroy a host resource
#define VIRTIO_GPU_CMD_SET_SCANOUT            0x0103 // Map resource to display scanout
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH         0x0104 // Flush host resource to display
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    0x0105 // Transfer guest buffer to host resource
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106 // Attach guest memory to resource
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107 // Detach guest memory from resource

// VirtIO GPU Response Types
#define VIRTIO_GPU_RESP_OK_NODATA             0x1100 // Command succeeded
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO       0x1101 // Display info returned
#define VIRTIO_GPU_RESP_ERR_UNSPEC            0x1200 // Unspecified error

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

/**
 * Flushes the kernel-side framebuffer contents to the host display device.
 */
void virtio_gpu_flush(void);

/**
 * Returns the base address of the kernel's memory-mapped framebuffer.
 */
uint32_t* virtio_gpu_get_framebuffer(void);

#endif // VIRTIO_GPU_H
