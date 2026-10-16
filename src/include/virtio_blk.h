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

#endif // VIRTIO_BLK_H
