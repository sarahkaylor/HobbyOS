/*
 * HobbyOS Phase-2 sysroot: stat.c — stat/fstat/lseek stubs.
 *
 * The Phase-2 kernel exposes open/read/write/close/pipe/spawn but no
 * stat/lseek yet; portable code that needs them should degrade cleanly.
 * wc's lseek-size fast path (fstat + S_ISREG + lseek) therefore falls
 * back to the read-until-EOF loop, which yields identical counts.
 */
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

int stat(const char *path, struct stat *buf)
{
    (void)path;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

int fstat(int fd, struct stat *buf)
{
    (void)fd;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

off_t lseek(int fd, off_t offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    errno = ENOSYS;
    return -1;
}
