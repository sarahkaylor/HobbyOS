#ifndef HOBBYOS_UNISTD_H
#define HOBBYOS_UNISTD_H

/* HobbyOS Phase-2 sysroot: unistd.h — the fd-based POSIX I/O surface.
 * read/write/close/open are legacy SYS_* wrappers; lseek/access/dup/
 * ftruncate land with the Phase-2 syscalls (currently declared, with
 * stubs that return -1/errno until the kernel side lands). */

#include <stddef.h>
#include <sys/types.h> /* ssize_t via the sysroot */

#ifdef __cplusplus
extern "C" {
#endif

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

  /* POSIX.1-2008 realtime timers are provided (clock_gettime, nanosleep,
   * CLOCK_MONOTONIC; see time.h), so advertise them like glibc does.
   * libc++'s <chrono> selects its CLOCK_MONOTONIC steady_clock branch via
   * "#if _POSIX_TIMERS > 0" (see libcxx/src/chrono.cpp). */
#define _POSIX_TIMERS 200809L
#define _POSIX_MONOTONIC_CLOCK 200809L

  /* POSIX seek whence values (also in stdio.h) */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

  ssize_t read(int fd, void *buf, size_t count);
  ssize_t write(int fd, const void *buf, size_t count);
  int close(int fd);
  int open(const char *path, int flags, ...); /* mode is unused today */
  off_t lseek(int fd, off_t offset, int whence);
  int dup(int oldfd);
  int dup2(int oldfd, int newfd);
  int access(const char *path, int mode);
  int unlink(const char *path);
  int isatty(int fd);
  int getpagesize(void);
  unsigned int sleep(unsigned int seconds);
  int usleep(unsigned int useconds);

  /* Phase 3 (posix.md): process identity + exec. */
  int getpid(void);
  int getppid(void);
  int execv(const char *path, char *const argv[]);
  int execve(const char *path, char *const argv[], char *const envp[]);

  /* Phase 4 (posix.md): heap break control. */
  void _exit(int status) __attribute__((noreturn)); /* POSIX: exit w/o atexit */

  int brk(void *addr);         /* 0 on success, -1 (ENOMEM) if out of range */
  void *sbrk(intptr_t delta);  /* old break on success, (void *)-1 on error */

  /* Path target of a symbolic link (no symlinks exist here, so this
   * always fails with EINVAL, like Linux on a non-link path). */
  long readlink(const char *path, char *buf, unsigned long bufsiz);

  /* Working directory (implemented in user/libc.c over SYS_GETCWD/SYS_CHDIR;
   * declared here so sysroot-built programs can use them). */
  char *getcwd(char *buf, size_t size);
  int chdir(const char *path);

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_UNISTD_H */
