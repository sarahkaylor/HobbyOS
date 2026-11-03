/* linux/types.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * Mesa's include/drm-uapi/drm.h (used by src/mesa/main/externalobjects.c for
 * the DMA-BUF import path) needs the __u8..__u64/s64 aliases.  HobbyOS has no
 * <linux/*.h>; the fixed-width types are the kernel ABI's, so these are exact.
 * The external-objects path is compiled but unreachable on softpipe (no DRM
 * device), which is why declarations are enough. */
#ifndef HOBBYOS_LINUX_TYPES_H
#define HOBBYOS_LINUX_TYPES_H 1

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;
typedef __u16    __le16;
typedef __u32    __le32;
typedef __u64    __le64;
typedef __u16    __be16;
typedef __u32    __be32;
typedef __u64    __be64;
typedef __u32    __kernel_dev_t;
typedef unsigned long __kernel_ulong_t;
typedef long     __kernel_long_t;
typedef int32_t  __kernel_pid_t;
typedef unsigned long __kernel_size_t;
typedef long     __kernel_ssize_t;

#endif
