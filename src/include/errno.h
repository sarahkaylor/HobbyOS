#ifndef ERRNO_H
#define ERRNO_H

/*
 * Error numbers, Linux-compatible values.
 *
 * The kernel returns -errno from POSIX-shaped syscalls (see syscall.h); the
 * userland libc converts a negative result into the global `errno` and
 * returns -1. Keeping Linux's numbering lets host-side tests compare errno
 * against glibc directly. The kernel includes this header for the constants
 * only; the `extern int errno` declaration is inert there.
 *
 * Userland storage is a plain global. Processes are single-threaded today,
 * which makes `int errno` correct; it must move to per-thread storage
 * (TLS) if pthreads are ever introduced.
 */

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENXIO        6
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define ENOTBLK     15
#define EBUSY       16
#define EEXIST      17
#define EXDEV       18
#define ENODEV      19
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define ENOTSUP     95
#define EMFILE      24
#define ENOTTY      25
#define ETXTBSY     26
#define EFBIG       27
#define ENOSPC      28
#define ESPIPE      29
#define EROFS       30
#define EMLINK      31
#define EPIPE       32
#define EDOM        33
#define ERANGE      34
#define ENAMETOOLONG 36
#define ENOSYS      38
#define ENOTEMPTY   39
#define ELOOP       40
#define EILSEQ      84
#define EOPNOTSUPP  95 /* Linux value; acl-errno-valid.c */ /* Linux value; used by the byte-mode wide-char layer */

/* Networking / socket errnos (Linux values; Phase F1, browser.md A.1a). */
#define ENOTSOCK      88
#define ENOPROTOOPT   92
#define EADDRINUSE    98
#define ENETUNREACH  101
#define ECONNRESET   104
#define ETIMEDOUT    110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EINPROGRESS  115
#define EPROTONOSUPPORT 93
#define EAFNOSUPPORT    97
#define EWOULDBLOCK  EAGAIN

#ifndef HOST_TEST
extern int errno;
#else
/* Host tests link against glibc, where `errno` is a TLS macro, not a
 * variable. Some host rules compile with -Isrc/include, which shadows
 * glibc's <errno.h> with this header; #include_next escapes back to
 * glibc's so the TLS relocation is used (a non-TLS `int errno` would
 * mismatch libc.so.6's .tbss and fail the link). Same numbering either
 * way. */
#include_next <errno.h>
#endif

#endif /* ERRNO_H */
