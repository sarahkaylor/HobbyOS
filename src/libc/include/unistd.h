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
  /* P2.4 (S4): ftruncate is real for memfd fds (row 33; other file types
     return ENOTSUP). */
  int ftruncate(int fd, off_t length);
  int access(const char *path, int mode);
  int unlink(const char *path);
  int isatty(int fd);
  int getpagesize(void);
  unsigned int sleep(unsigned int seconds);
  int usleep(unsigned int useconds);

  /* P6.1 (browser.md section 6): file resize + sync.  ftruncate reaches
   * the kernel's FAT16 resize (row 33); every FAT16 write is synchronous
   * (no page cache), so fsync/fdatasync validate the fd and succeed. */
  int ftruncate(int fd, off_t length);
  int fsync(int fd);
  int fdatasync(int fd);

  /* P6.1: single-user identity — HobbyOS has no accounts, uid/gid 0. */
  unsigned int getuid(void);
  unsigned int geteuid(void);
  unsigned int getgid(void);
  unsigned int getegid(void);
  int getgroups(int size, unsigned int list[]);

  /* P6.1: sysconf() bits.  Names carry glibc's numbering; unsupported
   * names return -1/EINVAL like glibc. */
  long sysconf(int name);
#define _SC_ARG_MAX           0
#define _SC_CHILD_MAX         1
#define _SC_CLK_TCK           2
#define _SC_NGROUPS_MAX       3
#define _SC_OPEN_MAX          4
#define _SC_JOB_CONTROL       7
#define _SC_SAVED_IDS         8
#define _SC_VERSION           29
#define _SC_PAGESIZE          30
#define _SC_PAGE_SIZE         _SC_PAGESIZE
#define _SC_GETPW_R_SIZE_MAX  70
#define _SC_GETGR_R_SIZE_MAX  71
#define _SC_LOGIN_NAME_MAX    73
#define _SC_TTY_NAME_MAX      74
#define _SC_NPROCESSORS_CONF  83
#define _SC_NPROCESSORS_ONLN  84
#define _SC_ATEXIT_MAX        87
#define _SC_MONOTONIC_CLOCK   149
#define _SC_HOST_NAME_MAX     180

  /* Phase 3 (posix.md): process identity + exec. */
  int getpid(void);
  int getppid(void);
  int execv(const char *path, char *const argv[]);
  int execve(const char *path, char *const argv[], char *const envp[]);
  /* P6.3: bare-name search through $PATH ("/" when unset; see stdlib.c). */
  int execvp(const char *file, char *const argv[]);
  int execvpe(const char *file, char *const argv[], char *const envp[]);

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
