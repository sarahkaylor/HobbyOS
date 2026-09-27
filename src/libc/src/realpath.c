/*
 * realpath.c — POSIX pathname canonicalization for the HobbyOS sysroot.
 *
 * The VFS has no symbolic links and a single namespace, so canonicalizing
 * is purely textual: make the path absolute (against getcwd), collapse
 * duplicate slashes and "." components, and resolve ".." by popping the
 * previous component (a ".." at the root stays at the root).
 *
 * Unlike glibc, no existence check is performed: the result is the
 * canonical *name*, which is what ported tools use it for (nano computes
 * the full path of the file being edited before it exists).
 */
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "malloc.h"

#ifndef HOST_TEST

char *realpath(const char *path, char *resolved) {
  if (!path || !*path) {
    errno = ENOENT;
    return NULL;
  }

  char cwd[1024];
  const char *base = path;
  if (path[0] != '/') {
    if (!getcwd(cwd, sizeof cwd)) return NULL;
    base = NULL;   /* relative: combine below */
  }

  size_t need = strlen(cwd) + strlen(path) + 2;
  char *work = resolved ? NULL : malloc(need);
  char *out = work ? work : resolved;
  if (!out) {
    errno = ENOMEM;
    return NULL;
  }

  /* Build the raw absolute path in `out`. */
  if (base) {
    strcpy(out, path);
  } else {
    strcpy(out, cwd);
    size_t l = strlen(out);
    if (l == 0 || out[l - 1] != '/') { out[l] = '/'; out[l + 1] = '\0'; }
    strcat(out, path);
  }

  /* Normalize in place: walk the components, writing a rebuilt path. */
  char *rd = out;
  char *wr = out;
  if (*rd == '/') *wr++ = '/';
  while (*rd) {
    while (*rd == '/') rd++;
    if (!*rd) break;
    char *comp = rd;
    while (*rd && *rd != '/') rd++;
    size_t clen = (size_t)(rd - comp);

    if (clen == 1 && comp[0] == '.') continue;
    if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
      /* Pop the previous component (never above the root). */
      while (wr > out + 1 && wr[-1] != '/') wr--;
      if (wr > out + 1 && wr[-1] == '/') wr--;
      while (wr > out + 1 && wr[-1] == '/') wr--;
      continue;
    }
    if (wr > out && wr[-1] != '/') *wr++ = '/';
    memcpy(wr, comp, clen);
    wr += clen;
  }
  if (wr == out) *wr++ = (out[0] == '/' ? '/' : '.');
  *wr = '\0';

  return out;
}

#endif /* !HOST_TEST */
