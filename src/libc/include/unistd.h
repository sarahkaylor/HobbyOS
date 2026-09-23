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
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int useconds);

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_UNISTD_H */
