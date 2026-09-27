/*
 * libgen.c — POSIX dirname()/basename() for the HobbyOS sysroot.
 *
 * Used by ported GNU software (nano builds backup/lock file names with
 * them).  Both functions may modify their argument, per POSIX.
 */
#include <libgen.h>
#include <string.h>

#ifndef HOST_TEST

char *dirname(char *path) {
  static char dot[] = ".";
  char *slash;

  if (!path || !*path) return dot;

  /* Strip trailing slashes (but keep a lone "/"). */
  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/') len--;
  path[len] = '\0';

  slash = strrchr(path, '/');
  if (!slash) return dot;          /* no slash: current directory */
  if (slash == path) {
    path[1] = '\0';                /* "/foo" -> "/" */
    return path;
  }
  *slash = '\0';
  return path;
}

char *basename(char *path) {
  static char dot[] = ".";
  static char slash[] = "/";
  char *base;

  if (!path || !*path) return dot;

  /* Strip trailing slashes (but keep a lone "/"). */
  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/') len--;
  path[len] = '\0';

  base = strrchr(path, '/');
  if (!base) return path;          /* no slash: the whole name */
  if (base == path) return slash;  /* "/foo" -> "/" */
  return base + 1;
}

#endif /* !HOST_TEST */
