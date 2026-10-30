/* fnmatch() for the HobbyOS IceWM build (lane-local; see ../include/fnmatch.h).
 * POSIX shell patterns: * ? [set] [!set] [^set] ranges, backslash escapes,
 * FNM_NOESCAPE / FNM_PATHNAME / FNM_PERIOD / FNM_LEADING_DIR / FNM_CASEFOLD.
 * Character classes ([:alpha:] ...) are supported for the common names. */
#include <fnmatch.h>
#include <ctype.h>
#include <string.h>

static int fold(int c, int flags) {
  return (flags & FNM_CASEFOLD) ? tolower(c) : c;
}

static int class_match(const char *name, size_t len, int c) {
#define CL(n, f) if (len == sizeof(n) - 1 && memcmp(name, n, len) == 0) return f(c) != 0
  CL("alpha", isalpha); CL("digit", isdigit); CL("alnum", isalnum);
  CL("upper", isupper); CL("lower", islower); CL("space", isspace);
  CL("punct", ispunct); CL("print", isprint); CL("graph", isgraph);
  CL("cntrl", iscntrl); CL("xdigit", isxdigit);
  if (len == 5 && memcmp(name, "blank", 5) == 0) return c == ' ' || c == '\t';
#undef CL
  return 0;
}

/* Matches one bracket expression at *pp against c; advances *pp past it.
 * Returns 1 match, 0 no match, -1 malformed (treat '[' literally). */
static int bracket(const char **pp, int c, int flags) {
  const char *p = *pp + 1;
  int neg = 0, ok = 0;
  if (*p == '!' || *p == '^') { neg = 1; p++; }
  const char *start = p;
  while (*p && (*p != ']' || p == start)) {
    if (*p == '[' && p[1] == ':') {
      const char *e = strstr(p + 2, ":]");
      if (e) { if (class_match(p + 2, (size_t)(e - p - 2), c)) ok = 1; p = e + 2; continue; }
    }
    int lo = (unsigned char)*p;
    if (lo == '\\' && !(flags & FNM_NOESCAPE) && p[1]) lo = (unsigned char)*++p;
    p++;
    if (*p == '-' && p[1] && p[1] != ']') {
      int hi = (unsigned char)p[1];
      p += 2;
      if (hi == '\\' && !(flags & FNM_NOESCAPE) && *p) hi = (unsigned char)*p++;
      if (fold(c, flags) >= fold(lo, flags) && fold(c, flags) <= fold(hi, flags)) ok = 1;
    } else if (fold(lo, flags) == fold(c, flags)) {
      ok = 1;
    }
  }
  if (*p != ']') return -1;
  *pp = p + 1;
  return ok ^ neg;
}

static int match(const char *p, const char *s, const char *sbeg, int flags) {
  for (;;) {
    int c = (unsigned char)*s;
    switch (*p) {
    case '\0':
      if (c == '\0') return 0;
      if ((flags & FNM_LEADING_DIR) && c == '/') return 0;
      return FNM_NOMATCH;
    case '?':
      if (c == '\0') return FNM_NOMATCH;
      if (c == '/' && (flags & FNM_PATHNAME)) return FNM_NOMATCH;
      if (c == '.' && (flags & FNM_PERIOD) &&
          (s == sbeg || ((flags & FNM_PATHNAME) && s[-1] == '/')))
        return FNM_NOMATCH;
      p++; s++;
      break;
    case '*': {
      while (*p == '*') p++;
      if (c == '.' && (flags & FNM_PERIOD) &&
          (s == sbeg || ((flags & FNM_PATHNAME) && s[-1] == '/')))
        return FNM_NOMATCH;
      if (*p == '\0') {
        if (flags & FNM_PATHNAME)
          return ((flags & FNM_LEADING_DIR) || !strchr(s, '/')) ? 0 : FNM_NOMATCH;
        return 0;
      }
      for (const char *t = s;; t++) {
        if (match(p, t, sbeg, flags & ~FNM_PERIOD) == 0) return 0;
        if (*t == '\0') return FNM_NOMATCH;
        if (*t == '/' && (flags & FNM_PATHNAME)) return FNM_NOMATCH;
      }
    }
    case '[': {
      if (c == '\0') return FNM_NOMATCH;
      if (c == '/' && (flags & FNM_PATHNAME)) return FNM_NOMATCH;
      if (c == '.' && (flags & FNM_PERIOD) &&
          (s == sbeg || ((flags & FNM_PATHNAME) && s[-1] == '/')))
        return FNM_NOMATCH;
      const char *q = p;
      int r = bracket(&q, c, flags);
      if (r < 0) { /* literal '[' */
        if (c != '[') return FNM_NOMATCH;
        p++; s++;
        break;
      }
      if (!r) return FNM_NOMATCH;
      p = q; s++;
      break;
    }
    case '\\':
      if (!(flags & FNM_NOESCAPE) && p[1]) p++;
      /* fall through */
    default:
      if (fold((unsigned char)*p, flags) != fold(c, flags)) return FNM_NOMATCH;
      p++; s++;
      break;
    }
  }
}

int fnmatch(const char *pattern, const char *string, int flags) {
  return match(pattern, string, string, flags);
}
