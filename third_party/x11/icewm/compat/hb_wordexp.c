/* wordexp()/wordfree() for the HobbyOS IceWM build (lane-local;
 * ../include/wordexp.h).  A small POSIX-shell word splitter: blanks split
 * fields; '...' and "..." quote; backslash escapes; a leading unquoted ~
 * becomes $HOME; $NAME and ${NAME} expand (unquoted expansions are split on
 * blanks).  `...`, $(...) and $((...)) are refused with WRDE_CMDSUB (or
 * WRDE_SYNTAX without WRDE_NOCMD), unquoted | & ; < > ( ) { } and newline
 * with WRDE_BADCHAR -- exactly the cases IceWM then hands to the shell.
 * No globbing of the resulting fields. */
#include <wordexp.h>
#include <stdlib.h>
#include <string.h>

struct buf { char *p; size_t n, cap; int have; };

static int bput(struct buf *b, char c) {
  if (b->n + 2 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 64;
    char *np = (char *)realloc(b->p, nc);
    if (!np) return -1;
    b->p = np; b->cap = nc;
  }
  b->p[b->n++] = c; b->p[b->n] = 0; b->have = 1;
  return 0;
}

struct vec { char **v; size_t n, cap; };

static int vpush(struct vec *v, struct buf *b) {
  if (v->n + 2 > v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 8;
    char **nv = (char **)realloc(v->v, nc * sizeof(char *));
    if (!nv) return -1;
    v->v = nv; v->cap = nc;
  }
  if (!b->p && bput(b, 0) == 0) b->n = 0;
  if (!b->p) return -1;
  b->p[b->n] = 0;
  v->v[v->n++] = b->p;
  b->p = NULL; b->n = b->cap = 0; b->have = 0;
  return 0;
}

static int isname(char c, int first) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (!first && c >= '0' && c <= '9');
}

/* Expand $... at *pp; appends to b (splitting into v when !quoted). */
static int dollar(const char **pp, struct buf *b, struct vec *v, int quoted, int flags) {
  const char *p = *pp + 1;
  char name[256];
  size_t n = 0;
  if (*p == '(') return (flags & WRDE_NOCMD) ? WRDE_CMDSUB : WRDE_SYNTAX;
  if (*p == '{') {
    p++;
    while (*p && *p != '}' && n + 1 < sizeof name) name[n++] = *p++;
    if (*p != '}') return WRDE_SYNTAX;
    p++;
  } else if (isname(*p, 1)) {
    while (isname(*p, 0) && n + 1 < sizeof name) name[n++] = *p++;
  } else {
    if (bput(b, '$')) return WRDE_NOSPACE; /* lone '$' is literal */
    *pp = p;
    return 0;
  }
  name[n] = 0;
  *pp = p;
  const char *val = getenv(name);
  if (!val) {
    if (flags & WRDE_UNDEF) return WRDE_BADVAL;
    return 0;
  }
  for (; *val; val++) {
    if (!quoted && (*val == ' ' || *val == '\t' || *val == '\n')) {
      if (b->have && vpush(v, b)) return WRDE_NOSPACE;
      continue;
    }
    if (bput(b, *val)) return WRDE_NOSPACE;
  }
  return 0;
}

int wordexp(const char *s, wordexp_t *we, int flags) {
  struct vec v = {0, 0, 0};
  struct buf b = {0, 0, 0, 0};
  int err = 0, atstart = 1;
  const char *p = s;
  while (*p && !err) {
    char c = *p;
    if (c == ' ' || c == '\t') {
      if (b.have && vpush(&v, &b)) err = WRDE_NOSPACE;
      p++; atstart = 1;
      continue;
    }
    if (strchr("|&;<>(){}\n", c)) { err = WRDE_BADCHAR; break; }
    if (c == '`') { err = (flags & WRDE_NOCMD) ? WRDE_CMDSUB : WRDE_SYNTAX; break; }
    if (c == '~' && atstart && !b.have) {
      const char *h = getenv("HOME");
      p++;
      if (*p == 0 || *p == '/' || *p == ' ' || *p == '\t') {
        if (!h) h = "/";
        for (; *h; h++) if (bput(&b, *h)) { err = WRDE_NOSPACE; break; }
      } else if (bput(&b, '~')) err = WRDE_NOSPACE;
      atstart = 0;
      continue;
    }
    atstart = 0;
    if (c == '\\') {
      p++;
      if (!*p) break;
      if (bput(&b, *p++)) err = WRDE_NOSPACE;
      continue;
    }
    if (c == '\'') {
      p++;
      b.have = 1;
      while (*p && *p != '\'') if (bput(&b, *p++)) { err = WRDE_NOSPACE; break; }
      if (!*p && !err) err = WRDE_SYNTAX;
      else p++;
      continue;
    }
    if (c == '"') {
      p++;
      b.have = 1;
      while (*p && *p != '"' && !err) {
        if (*p == '\\' && p[1] && strchr("$`\"\\\n", p[1])) { p++; if (bput(&b, *p++)) err = WRDE_NOSPACE; continue; }
        if (*p == '`') { err = (flags & WRDE_NOCMD) ? WRDE_CMDSUB : WRDE_SYNTAX; break; }
        if (*p == '$') { err = dollar(&p, &b, &v, 1, flags); continue; }
        if (bput(&b, *p++)) err = WRDE_NOSPACE;
      }
      if (!err && *p != '"') err = WRDE_SYNTAX;
      else if (!err) p++;
      continue;
    }
    if (c == '$') { err = dollar(&p, &b, &v, 0, flags); continue; }
    if (bput(&b, c)) err = WRDE_NOSPACE;
    p++;
  }
  if (!err && b.have && vpush(&v, &b)) err = WRDE_NOSPACE;
  free(b.p);
  if (err) {
    for (size_t i = 0; i < v.n; i++) free(v.v[i]);
    free(v.v);
    return err;
  }
  size_t offs = (flags & WRDE_DOOFFS) ? we->we_offs : 0;
  size_t old = (flags & WRDE_APPEND) ? we->we_wordc : 0;
  char **base = (flags & WRDE_APPEND) ? we->we_wordv : NULL;
  if (flags & WRDE_REUSE) wordfree(we);
  char **nv = (char **)realloc(base, (offs + old + v.n + 1) * sizeof(char *));
  if (!nv) { for (size_t i = 0; i < v.n; i++) free(v.v[i]); free(v.v); return WRDE_NOSPACE; }
  if (!(flags & WRDE_APPEND)) for (size_t i = 0; i < offs; i++) nv[i] = NULL;
  for (size_t i = 0; i < v.n; i++) nv[offs + old + i] = v.v[i];
  nv[offs + old + v.n] = NULL;
  free(v.v);
  we->we_wordv = nv;
  we->we_wordc = old + v.n;
  if (!(flags & WRDE_DOOFFS)) we->we_offs = 0;
  return 0;
}

void wordfree(wordexp_t *we) {
  if (!we->we_wordv) return;
  for (size_t i = 0; i < we->we_wordc; i++) free(we->we_wordv[we->we_offs + i]);
  free(we->we_wordv);
  we->we_wordv = NULL;
  we->we_wordc = 0;
}
