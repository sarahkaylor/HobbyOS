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
