/*
 * HobbyOS Phase-2 sysroot: error.h — GNU error() convenience routine.
 * Implementation in src/libc/src/error.c.  On the host the include
 * resolves to glibc's <error.h> when built there; here we match glibc's
 * calling convention: error(status, errnum, fmt, ...).
 */
#ifndef __HB_ERROR_H
#define __HB_ERROR_H

#include <stdarg.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Program name used by error() (set it from argv[0] in main). */
extern char *program_name;

void error(int status, int errnum, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __HB_ERROR_H */
