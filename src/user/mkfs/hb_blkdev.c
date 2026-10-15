/*
 * hb_blkdev.c — HobbyOS block-device geometry stubs for the dosfstools
 * mkfs.fat port.
 *
 * First-party glue (W2-MKFS).  Upstream links blkdev/blkdev.c +
 * blkdev/linux_version.c (ioctl/sysfs plumbing) for mkfs.fat; the port
 * replaces them with these failing stubs, per the L4 audit's ioctl() ->
 * -1/ENOTTY prescription (docs/selfhost/buildtools-audit.md §5.2).  For a
 * plain image file device_info.c's get_device_info() returns before any of
 * these is reached (S_ISREG short-circuit), so they are dead-but-linked at
 * runtime; they only exist so the link stays identical to upstream's shape
 * and the transcribed device_info.c compiles unmodified (outside the
 * __linux__ block).
 */

#include <sys/types.h>
#include <errno.h>

#include "blkdev.h"

int is_blkdev(int fd) {
  (void)fd;
  return 0;
}

off_t blkdev_find_size(int fd) {
  (void)fd;
  errno = ENOTTY;
  return -1;
}

int blkdev_get_size(int fd, unsigned long long *bytes) {
  (void)fd;
  (void)bytes;
  errno = ENOTTY;
  return -1;
}

int blkdev_get_sectors(int fd, unsigned long long *sectors) {
  (void)fd;
  (void)sectors;
  errno = ENOTTY;
  return -1;
}

int blkdev_get_sector_size(int fd, int *sector_size) {
  (void)fd;
  (void)sector_size;
  errno = ENOTTY;
  return -1;
}

int blkdev_is_misaligned(int fd) {
  (void)fd;
  return 0;
}

int blkdev_get_physector_size(int fd, int *sector_size) {
  (void)fd;
  (void)sector_size;
  errno = ENOTTY;
  return -1;
}

int blkdev_is_cdrom(int fd) {
  (void)fd;
  return 0;
}

int blkdev_get_geometry(int fd, unsigned int *h, unsigned int *s) {
  (void)fd;
  (void)h;
  (void)s;
  errno = ENOTTY;
  return -1;
}

int blkdev_get_start(int fd, dev_t rdev, unsigned long long *s) {
  (void)fd;
  (void)rdev;
  (void)s;
  errno = ENOTTY;
  return -1;
}

const char *blkdev_scsi_type_to_name(int type) {
  (void)type;
  return "unknown";
}
