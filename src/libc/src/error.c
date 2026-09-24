/*
 * HobbyOS Phase-2 sysroot: error.c — GNU error() convenience routine.
 *
 * Prints "program_name: <fmt>[ : <strerror(errnum)>]\n" to stderr and, if
 * status is nonzero, exits with it — matching glibc's error(3).
 */
#include "error.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

char *program_name = NULL;

void error(int status, int errnum, const char *fmt, ...) {
  va_list ap;
  FILE *out = stderr;

  if (program_name && *program_name)
    fprintf(out, "%s: ", program_name);
  if (fmt && *fmt) {
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
  }
  if (errnum != 0)
    fprintf(out, ": %s", strerror(errnum));
  fputc('\n', out);
  fflush(out);

  if (status != 0)
    exit(status);
}
