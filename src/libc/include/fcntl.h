#ifndef HOBBYOS_FCNTL_H
#define HOBBYOS_FCNTL_H

/* HobbyOS Phase-2 sysroot: fcntl.h — O_* open flags + fcntl commands.
 * The kernel honors O_CREAT, O_EXCL and O_TRUNC in SYS_OPEN (missing
 * files without O_CREAT fail ENOENT, POSIX-style); O_APPEND is still
 * accepted but treated as plain write.
 *
 * P5 (D3.2): fcntl(F_GETFD/F_SETFD) read/write FD_CLOEXEC (bit 0) in the
 * process's fd mask; fds with the bit set are closed at exec.
 *
 * P6.1 (browser.md section 6): advisory record locks — F_GETLK/F_SETLK/
 * F_SETLKW over the LP64 struct flock below, plus flock(2) built on them
 * (SQLite's lock surface).  The syscall argument is a pointer to struct
 * flock, which is exactly why fcntl() is variadic here (like glibc). */

#include <sys/types.h> /* off_t, pid_t */

#define F_GETFD   1
#define F_SETFD   2
#define FD_CLOEXEC 1

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

  /* P4 (docs/browser/p4-ipc-design.md §6.4): fcntl() commands + the fd flag
   * bits.  F_GETFL/F_SETFL/O_NONBLOCK repeat the values in libc.h (identical
   * replacement lists, so both headers coexist). */
#define F_GETFL    3
#define F_SETFL    4
#define O_NONBLOCK 0x800

  /* P6.1: advisory record locks (Linux numbering; the kernel mirrors these
   * in src/include/fs.h).  F_SETLKW is blocking in libc: it retries the
   * non-blocking kernel command with short sleeps (bounded, see libc.c). */
#define F_RDLCK  0
#define F_WRLCK  1
#define F_UNLCK  2
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7

  /* LP64 layout (32 bytes, 8-byte natural alignment): short, short, off_t,
   * off_t, pid_t.  The kernel parses this byte-wise, and the trap layer
   * requires the pointer to carry the struct's natural 8-byte alignment. */
  struct flock {
    short l_type;    /* F_RDLCK / F_WRLCK / F_UNLCK                    */
    short l_whence;  /* SEEK_SET / SEEK_CUR / SEEK_END                 */
    off_t l_start;   /* region start                                   */
    off_t l_len;     /* region length; 0 = to EOF                      */
    pid_t l_pid;     /* F_GETLK out: pid holding a conflicting lock    */
  };

  /* flock(2) flags; implemented over the record locks above (whole file). */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

  /* fcntl() itself (the libc.a wrapper covers every fd type; variadic like
   * glibc so pointer arguments — struct flock* — pass uncast). */
  int fcntl(int fd, int cmd, ...);

  /* Advisory whole-file lock (flock(2) subset).  LOCK_NB picks the
   * non-blocking form; without it the call blocks (bounded retry). */
  int flock(int fd, int op);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_FCNTL_H */
