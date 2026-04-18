#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>

// Standard initialization of the VirtIO Block Device 
int virtio_blk_init(void);

// Read one or more 512-byte sectors from the disk
int virtio_blk_read_sector(uint64_t sector, void* buf, uint32_t count);

// Write one or more 512-byte sectors to the disk
int virtio_blk_write_sector(uint64_t sector, const void* buf, uint32_t count);

#endif
