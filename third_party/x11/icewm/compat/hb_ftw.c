/* nftw() for the HobbyOS IceWM build (see ../include/ftw.h).  No symlinks on
 * this VFS, so FTW_PHYS is the only behaviour; FTW_CHDIR is ignored. */
#include <ftw.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

typedef int (*hb_ftw_fn)(const char *, const struct stat *, int, struct FTW *);

static int walk(char *path, size_t len, size_t cap, hb_ftw_fn fn, int flags, int level) {
  struct stat st;
  struct FTW f;
  const char *slash = strrchr(path, '/');
  f.base = slash ? (int)(slash - path + 1) : 0;
  f.level = level;
  if (stat(path, &st) != 0) return fn(path, &st, FTW_NS, &f);
  if (!S_ISDIR(st.st_mode)) return fn(path, &st, FTW_F, &f);
  int r;
  if (!(flags & FTW_DEPTH) && (r = fn(path, &st, FTW_D, &f)) != 0) return r;
  DIR *d = opendir(path);
  if (!d) return fn(path, &st, FTW_DNR, &f);
  struct dirent *e;
  r = 0;
  while (r == 0 && (e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    size_t nl = strlen(e->d_name);
    if (len + 1 + nl + 1 > cap) continue;
    path[len] = '/';
    memcpy(path + len + 1, e->d_name, nl + 1);
    r = walk(path, len + 1 + nl, cap, fn, flags, level + 1);
    path[len] = 0;
  }
  closedir(d);
  if (r == 0 && (flags & FTW_DEPTH)) r = fn(path, &st, FTW_DP, &f);
  return r;
}

int nftw(const char *p, hb_ftw_fn fn, int nfds, int flags) {
  (void)nfds;
  size_t cap = 4096;
  char *buf = (char *)malloc(cap);
  if (!buf) return -1;
  size_t l = strlen(p);
  if (l >= cap) { free(buf); return -1; }
  memcpy(buf, p, l + 1);
  while (l > 1 && buf[l - 1] == '/') buf[--l] = 0;
  int r = walk(buf, l, cap, fn, flags, 0);
  free(buf);
  return r;
}
