#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>

/**
 * Raw device reads/writes used by the block cache (docs/blk-cache.md):
 * the arch-specific drivers were renamed to *_raw and the PUBLIC
 * virtio_blk_read_sector()/virtio_blk_write_sector() now route through
 * blk_cache.c when it is ready.  The cache fills/invalidates through
 * these; raw keep the exact legacy semantics (including the x64
 * sector-0 write guard).
 */
int virtio_blk_read_raw(uint64_t sector, void* buf, uint32_t count);
int virtio_blk_write_raw(uint64_t sector, const void* buf, uint32_t count);

/**
 * Initializes the VirtIO Block Device using the MMIO transport.
 * Performs the VirtIO handshake and configures the request virtqueue.
 *
 * Returns:
 *   0 on success, or -1 if the device is not found or fails to initialize.
 */
int virtio_blk_init(void);

/**
 * Reads data from the block device into a memory buffer.
 *
 * Parameters:
 *   sector - The starting 512-byte sector index on the disk.
 *   buf    - The destination memory buffer.
 *   count  - The number of sectors to read.
 *
 * Returns:
 *   0 on success, or -1 on failure.
 */
int virtio_blk_read_sector(uint64_t sector, void* buf, uint32_t count);

/**
 * Writes data from a memory buffer to the block device.
 *
 * Parameters:
 *   sector - The starting 512-byte sector index on the disk.
 *   buf    - The source memory buffer.
 *   count  - The number of sectors to write.
 *
 * Returns:
 *   0 on success, or -1 on failure.
 */
int virtio_blk_write_sector(uint64_t sector, const void* buf, uint32_t count);

/**
 * L67: periodic (5 s) controller-state row.
 *
 * Prints one line with the NVMe registers that a reset changes (CC, CSTS,
 * CAP, VS), the MMIO mapping that carries the doorbell (cpu_pd7[480]), and
 * the driver's request/devoff/reset counters.  It exists so a run yields a
 * TIMELINE of device state changes instead of only the post-mortem of a
 * wait that had already gone wrong: the wedge rows fire when a request is
 * stuck, this row says when the controller changed state, which is what
 * lets it be correlated with what the guest was doing.
 *
 * No-op on architectures without the NVMe path, and before the driver is
 * initialised.
 */
void virtio_blk_diag_row(void);

/**
 * L67: put the controller back if a host-side reset left it disabled between
 * requests.  Returns 1 when repaired, 0 when already healthy, -1 on failure.
 */
int virtio_blk_health(void);

#endif // VIRTIO_BLK_H
