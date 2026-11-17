/*
 * hbxcompat.c -- libc gaps the X server (and its libXfont2/libfontenc
 * deps) needs on HobbyOS, kept server-private (linked only into
 * XHOBBY.BIN) so the repo libc and the default build stay untouched.
 * Each symbol is weak: if the repo libc later grows a real one, that
 * definition wins and this file can shrink.
 */
#include <math.h>
#include <errno.h>
#include <stddef.h>

__attribute__((weak)) double hypot(double x, double y)
{
    double ax = fabs(x), ay = fabs(y), t;
    if (ax < ay) { t = ax; ax = ay; ay = t; }
    if (ax == 0.0)
        return 0.0;
    t = ay / ax;
    return ax * sqrt(1.0 + t * t);
}

/* fscanf(): the repo libc has sscanf/vsscanf but no stream scanf.  Line-
 * based approximation: one fgets() line per call, scanned with vsscanf.
 * Its only users here (libXfont2 fontfile/dirfile.c fonts.dir/fonts.alias
 * readers) scan exactly one record per line, which this matches; the
 * server normally runs with only the built-in fonts and never reaches it. */
#include <stdio.h>
#include <stdarg.h>
__attribute__((weak)) int fscanf(FILE *f, const char *fmt, ...)
{
    char line[4096];
    va_list ap;
    int r;
    if (!fgets(line, sizeof line, f))
        return EOF;
    va_start(ap, fmt);
    r = vsscanf(line, fmt, ap);
    va_end(ap);
    return r;
}

/* Process identity: HobbyOS has one identity (uid/gid 0) and no process
 * groups; os/utils.c's Popen()/System() drop privileges before exec and
 * os/osinit.c detaches into its own group -- all no-ops here. */
#include <sys/types.h>
#include <unistd.h>
__attribute__((weak)) int setuid(uid_t uid) { (void)uid; return 0; }
__attribute__((weak)) int setgid(gid_t gid) { (void)gid; return 0; }
__attribute__((weak)) int seteuid(uid_t uid) { (void)uid; return 0; }
__attribute__((weak)) int setegid(gid_t gid) { (void)gid; return 0; }
__attribute__((weak)) int setpgid(pid_t pid, pid_t pgid) { (void)pid; (void)pgid; return 0; }
__attribute__((weak)) pid_t getpgrp(void) { return getpid(); }

/* execl(): the list form over execv (the repo libc has execv/execve/execvp). */
__attribute__((weak)) int execl(const char *path, const char *arg, ...)
{
    const char *argv[64];
    int n = 0;
    va_list ap;
    argv[n++] = arg;
    va_start(ap, arg);
    while (arg && n < 63) {
        arg = va_arg(ap, const char *);
        argv[n++] = arg;
        if (!arg)
            break;
    }
    va_end(ap);
    argv[63] = NULL;
    return execv(path, (char *const *)argv);
}

/* acos()/cbrt(): missing from the repo libm (dix/ptrveloc.c, pixman). */
__attribute__((weak)) double acos(double x)
{
    if (x != x || x > 1.0 || x < -1.0)
        return (x - x) / (x - x);       /* NaN, as libm does */
    return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

__attribute__((weak)) double cbrt(double x)
{
    double y, ax = fabs(x);
    int i;
    if (ax == 0.0 || ax != ax || ax > 1.7e308)
        return x;
    y = pow(ax, 1.0 / 3.0);
    for (i = 0; i < 2; i++)             /* Newton: y -= (y^3 - a) / 3y^2 */
        y = y - (y * y * y - ax) / (3.0 * y * y);
    return x < 0 ? -y : y;
}

/* System V shared memory for MIT-SHM (lane C4-x3, docs/cp-x3.md): the
 * kernel's SYS_SHM row (src/include/syscall.h, 106) multiplexes the four
 * calls over the same segment table the compat class's shmget/shmat/shmdt/
 * shmctl use, so a Wine client's IPC_PRIVATE segment id is attachable here.
 * Native ABI: aarch64 x8 = number, x0..x3; x86_64 rax, rdi/rsi/rdx/r10. */
#include <sys/shm.h>
#define HB_SYS_SHM 106
static long hb_shm_sys(long op, long a1, long a2, long a3)
{
#ifdef __x86_64__
    long ret;
    register long rdi __asm__("rdi") = op;
    register long rsi __asm__("rsi") = a1;
    register long rdx __asm__("rdx") = a2;
    register long r10 __asm__("r10") = a3;
    __asm__ volatile("syscall\n"
                     : "=a"(ret)
                     : "a"((long) HB_SYS_SHM), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
#else
    register long x8 __asm__("x8") = HB_SYS_SHM;
    register long x0 __asm__("x0") = op;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0\n"
                     : "=r"(x0)
                     : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
#endif
}
static long hb_shm_ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int) -r;
        return -1;
    }
    return r;
}
__attribute__((weak)) int shmget(key_t key, size_t size, int shmflg)
{
    return (int) hb_shm_ret(hb_shm_sys(0, (long) key, (long) size, (long) shmflg));
}
__attribute__((weak)) void *shmat(int shmid, const void *shmaddr, int shmflg)
{
    long r = hb_shm_ret(hb_shm_sys(1, (long) shmid, (long) shmaddr, (long) shmflg));
    return (void *) r;          /* -1 == (void *) -1 on failure */
}
__attribute__((weak)) int shmdt(const void *shmaddr)
{
    return (int) hb_shm_ret(hb_shm_sys(2, (long) shmaddr, 0, 0));
}
__attribute__((weak)) int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
    return (int) hb_shm_ret(hb_shm_sys(3, (long) shmid, (long) cmd, (long) buf));
}
