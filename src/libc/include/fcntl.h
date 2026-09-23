#ifndef HOBBYOS_FCNTL_H
#define HOBBYOS_FCNTL_H

/* HobbyOS Phase-2 sysroot: fcntl.h — O_* open flags.
 * O_RDONLY=0 keeps the legacy SYS_OPEN behavior; the kernel's flag
 * handling (O_TRUNC/O_APPEND/O_CREAT wiring) lands with Phase-2 syscalls. */

#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_APPEND 8
#define O_CREAT  0x40   /* 0100 octal POSIX value */
#define O_TRUNC  0x200  /* 01000 octal POSIX value */
#define O_EXCL   0x80   /* 0200 octal POSIX value */

/* HobbyOS does not track permissions yet: access modes beyond these are
 * accepted and ignored, so ported code that passes O_BINARY or similar
 * still compiles and runs. */

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_FCNTL_H */
