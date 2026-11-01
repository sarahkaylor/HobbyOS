/* execl()/execlp() for the HobbyOS IceWM build: collect the variadic
 * argument list and hand it to the libc's execv()/execvp(). */
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>

#define HB_EXEC_MAXARGS 64

static int collect(const char *arg, va_list ap, char **argv) {
  int n = 0;
  argv[n++] = (char *)arg;
  while (arg && n < HB_EXEC_MAXARGS) {
    arg = va_arg(ap, const char *);
    argv[n++] = (char *)arg;
  }
  argv[HB_EXEC_MAXARGS] = NULL;
  return n;
}

int execl(const char *path, const char *arg, ...) {
  char *argv[HB_EXEC_MAXARGS + 1];
  va_list ap;
  va_start(ap, arg);
  collect(arg, ap, argv);
  va_end(ap);
  return execv(path, argv);
}

int execlp(const char *file, const char *arg, ...) {
  char *argv[HB_EXEC_MAXARGS + 1];
  va_list ap;
  va_start(ap, arg);
  collect(arg, ap, argv);
  va_end(ap);
  return execvp(file, argv);
}
