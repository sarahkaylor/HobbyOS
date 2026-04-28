#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>

// Initialize the VirtIO Block Device using the MMIO transport.
// Performs the VirtIO handshake and configures the request virtqueue.
// Returns 0 on success, or -1 if the device is not found or fails to init.
int virtio_blk_init(void);

// Read data from the block device into a memory buffer.
// sector: The starting 512-byte sector index on the disk.
// buf: The destination memory buffer.
// count: The number of sectors to read.
// Returns 0 on success, or -1 on failure.
int virtio_blk_read_sector(uint64_t sector, void* buf, uint32_t count);

// Write data from a memory buffer to the block device.
// sector: The starting 512-byte sector index on the disk.
// buf: The source memory buffer.
// count: The number of sectors to write.
// Returns 0 on success, or -1 on failure.
int virtio_blk_write_sector(uint64_t sector, const void* buf, uint32_t count);

#endif // VIRTIO_BLK_H
