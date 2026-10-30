/* glob()/globfree() for the HobbyOS IceWM build (lane-local; ../include/glob.h).
 * Walks the pattern one path component at a time with opendir/readdir and
 * fnmatch(FNM_PERIOD).  Supports GLOB_MARK, GLOB_NOCHECK, GLOB_NOSORT,
 * GLOB_NOESCAPE, GLOB_APPEND, GLOB_DOOFFS, GLOB_TILDE (~ = $HOME), GLOB_ERR. */
#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct acc { char **v; size_t n, cap; };

static int acc_add(struct acc *a, const char *s, size_t extra) {
  if (a->n + 1 >= a->cap) {
    size_t nc = a->cap ? a->cap * 2 : 16;
    char **nv = (char **)realloc(a->v, nc * sizeof(char *));
    if (!nv) return -1;
    a->v = nv; a->cap = nc;
  }
  size_t l = strlen(s);
  char *d = (char *)malloc(l + extra + 1);
  if (!d) return -1;
  memcpy(d, s, l + 1);
  a->v[a->n++] = d;
  return 0;
}

static int has_magic(const char *s, size_t n, int noesc) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && !noesc) { i++; continue; }
    if (s[i] == '*' || s[i] == '?' || s[i] == '[') return 1;
  }
  return 0;
}

static void unescape(char *s) {
  char *d = s;
  for (; *s; s++) { if (*s == '\\' && s[1]) s++; *d++ = *s; }
  *d = 0;
}

/* base: path so far ("" or ends without '/'); rest: remaining pattern. */
static int walk(const char *base, const char *rest, int flags,
                int (*errfunc)(const char *, int), struct acc *out) {
  while (*rest == '/') rest++;
  if (!*rest) return acc_add(out, base[0] ? base : "/", 1);
  const char *slash = strchr(rest, '/');
  size_t clen = slash ? (size_t)(slash - rest) : strlen(rest);
  char comp[1024];
  if (clen >= sizeof comp) return 0;
  memcpy(comp, rest, clen); comp[clen] = 0;
  const char *next = slash ? slash : "";
  size_t bl = strlen(base);
  if (!has_magic(comp, clen, flags & GLOB_NOESCAPE)) {
    if (!(flags & GLOB_NOESCAPE)) unescape(comp);
    char *p = (char *)malloc(bl + strlen(comp) + 2);
    if (!p) return -1;
    strcpy(p, base); strcat(p, "/"); strcat(p, comp);
    struct stat st;
    int r = 0;
    if (stat(p, &st) == 0) {
      if (!*next) r = acc_add(out, p, 1);
      else if (S_ISDIR(st.st_mode)) r = walk(p, next, flags, errfunc, out);
    }
    free(p);
    return r;
  }
  const char *dname = bl ? base : "/";
  DIR *d = opendir(dname);
  if (!d) {
    if ((errfunc && errfunc(dname, errno)) || (flags & GLOB_ERR)) return -2;
    return 0;
  }
  struct dirent *e;
  int r = 0;
  int fl = FNM_PERIOD | ((flags & GLOB_NOESCAPE) ? FNM_NOESCAPE : 0);
  while (r == 0 && (e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (fnmatch(comp, e->d_name, fl) != 0) continue;
    char *p = (char *)malloc(bl + strlen(e->d_name) + 2);
    if (!p) { r = -1; break; }
    strcpy(p, base); strcat(p, "/"); strcat(p, e->d_name);
    if (!*next) {
      r = acc_add(out, p, 1);
    } else {
      struct stat st;
      if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) r = walk(p, next, flags, errfunc, out);
    }
    free(p);
  }
  closedir(d);
  return r;
}

static int cmpstr(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g) {
  struct acc out = {0, 0, 0};
  char *pat = NULL;
  const char *pp = pattern;
  if ((flags & GLOB_TILDE) && pattern[0] == '~' && (pattern[1] == 0 || pattern[1] == '/')) {
    const char *h = getenv("HOME");
    if (!h) h = "/";
    pat = (char *)malloc(strlen(h) + strlen(pattern) + 1);
    if (!pat) return GLOB_NOSPACE;
    strcpy(pat, h); strcat(pat, pattern + 1);
    pp = pat;
  }
  int r;
  if (pp[0] == '/') {
    r = walk("", pp, flags, errfunc, &out);
  } else {
    /* relative: walk from "." and strip the "./" prefix afterwards */
    r = walk(".", pp, flags, errfunc, &out);
    for (size_t i = 0; i < out.n; i++)
      if (out.v[i][0] == '.' && out.v[i][1] == '/')
        memmove(out.v[i], out.v[i] + 2, strlen(out.v[i] + 2) + 1);
  }
  if (r == -1) { free(pat); return GLOB_NOSPACE; }
  if (r == -2) { free(pat); for (size_t i = 0; i < out.n; i++) free(out.v[i]); free(out.v); return GLOB_ABORTED; }
  if (out.n == 0 && (flags & GLOB_NOCHECK)) {
    if (acc_add(&out, pattern, 1)) { free(pat); return GLOB_NOSPACE; }
  } else if (flags & GLOB_MARK) {
    for (size_t i = 0; i < out.n; i++) {
      struct stat st;
      size_t l = strlen(out.v[i]);
      if (l && out.v[i][l - 1] != '/' && stat(out.v[i], &st) == 0 && S_ISDIR(st.st_mode)) {
        out.v[i][l] = '/'; out.v[i][l + 1] = 0;
      }
    }
  }
  free(pat);
  if (out.n == 0) { free(out.v); return GLOB_NOMATCH; }
  if (!(flags & GLOB_NOSORT)) qsort(out.v, out.n, sizeof(char *), cmpstr);

  size_t offs = (flags & GLOB_DOOFFS) ? g->gl_offs : 0;
  size_t old = (flags & GLOB_APPEND) ? g->gl_pathc : 0;
  char **base = (flags & GLOB_APPEND) ? g->gl_pathv : NULL;
  char **nv = (char **)realloc(base, (offs + old + out.n + 1) * sizeof(char *));
  if (!nv) { for (size_t i = 0; i < out.n; i++) free(out.v[i]); free(out.v); return GLOB_NOSPACE; }
  if (!(flags & GLOB_APPEND)) for (size_t i = 0; i < offs; i++) nv[i] = NULL;
  for (size_t i = 0; i < out.n; i++) nv[offs + old + i] = out.v[i];
  nv[offs + old + out.n] = NULL;
  free(out.v);
  g->gl_pathv = nv;
  g->gl_pathc = old + out.n;
  if (!(flags & GLOB_DOOFFS)) g->gl_offs = 0;
  return 0;
}

void globfree(glob_t *g) {
  if (!g->gl_pathv) return;
  for (size_t i = 0; i < g->gl_pathc; i++) free(g->gl_pathv[g->gl_offs + i]);
  free(g->gl_pathv);
  g->gl_pathv = NULL;
  g->gl_pathc = 0;
}
