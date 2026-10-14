/*
 * No copyright is claimed.  This code is in the public domain; do with
 * it what you wish.
 *
 * Written by Karel Zak <kzak@redhat.com>
 *
 * HobbyOS port (W2-MKFS): trimmed transcription of upstream blkdev/blkdev.h
 * for the mkfs.fat link set — the ioctl()/sysfs block-device plumbing is
 * replaced by the failing stubs in hb_blkdev.c (L4 audit §5.2: ioctl ->
 * -1/ENOTTY; image files never reach it).  Prototypes kept for exactly the
 * functions the transcribed device_info.c references, plus the header-wide
 * ones so the surface stays upstream-shaped.
 */
#ifndef BLKDEV_H
#define BLKDEV_H

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#define DEFAULT_SECTOR_SIZE       512

/* are we working with block device? */
int is_blkdev(int fd);

/* Determine size in bytes */
off_t blkdev_find_size (int fd);

/* get size in bytes */
int blkdev_get_size(int fd, unsigned long long *bytes);

/* get 512-byte sector count */
int blkdev_get_sectors(int fd, unsigned long long *sectors);

/* get hardware sector size */
int blkdev_get_sector_size(int fd, int *sector_size);

/* specifies whether or not the device is misaligned */
int blkdev_is_misaligned(int fd);

/* get physical block device size */
int blkdev_get_physector_size(int fd, int *sector_size);

/* is the device cdrom capable? */
int blkdev_is_cdrom(int fd);

/* get device's geometry - legacy */
int blkdev_get_geometry(int fd, unsigned int *h, unsigned int *s);

/* get partition devices start offset */
int blkdev_get_start(int fd, dev_t rdev, unsigned long long *s);

/* convert scsi type code to name */
const char *blkdev_scsi_type_to_name(int type);

#endif /* BLKDEV_H */
