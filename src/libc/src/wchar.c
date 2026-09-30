/* HobbyOS libc: wchar.c — C-locale (byte-mode) wchar.h functions.
 *
 * Semantics are the C locale's, matching glibc byte-for-byte:
 *  - bytes 0x00..0x7F are valid single-byte characters;
 *  - bytes 0x80..0xFF are encoding errors (EILSEQ);
 *  - the conversion state is stateless (always initial after return).
 *
 * P3.2 adds the wide-string surface libc++'s <cwchar> re-exports
 * (browser.md §6): the wcs and wmem families, the mbs and wcs conversion
 * wide-to-number conversions (implemented over the narrow strto* on an
 * ASCII fast-path buffer — the C locale's only admissible encodings).
 *
 * HOST_TEST compiles this translation unit renamed to hb_* so host
 * tests can race it against glibc on the same inputs.
 */
#ifdef HOST_TEST
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <wchar.h>
#else
#include "wchar.h"
#include "errno.h"
#include "stdio.h" /* EOF for wctob() */
#include "stdlib.h" /* strto* for the wcsto* fast path */
#include "string.h"
#endif

#ifdef HOST_TEST
#define mbrtowc hb_mbrtowc
#define mbrlen hb_mbrlen
#define wcrtomb hb_wcrtomb
#define mbsinit hb_mbsinit
#define btowc hb_btowc
#define wctob hb_wctob
#define wcwidth hb_wcwidth
#define wcslen hb_wcslen
#define wcscmp hb_wcscmp
#define wcsncmp hb_wcsncmp
#define wcscpy hb_wcscpy
#define wcsncpy hb_wcsncpy
#define wcscat hb_wcscat
#define wcsncat hb_wcsncat
#define wcschr hb_wcschr
#define wcsrchr hb_wcsrchr
#define wcsstr hb_wcsstr
#define wcspbrk hb_wcspbrk
#define wcsspn hb_wcsspn
#define wcscspn hb_wcscspn
#define wcstok hb_wcstok
#define wcscoll hb_wcscoll
#define wcsxfrm hb_wcsxfrm
#define wmemcpy hb_wmemcpy
#define wmemmove hb_wmemmove
#define wmemset hb_wmemset
#define wmemcmp hb_wmemcmp
#define wmemchr hb_wmemchr
#define mbsrtowcs hb_mbsrtowcs
#define mbsnrtowcs hb_mbsnrtowcs
#define wcsrtombs hb_wcsrtombs
#define wcsnrtombs hb_wcsnrtombs
#define mbtowc hb_mbtowc
#define wctomb hb_wctomb
#define mblen hb_mblen
#define mbstowcs hb_mbstowcs
#define wcstombs hb_wcstombs
#define swprintf hb_swprintf
#define vswprintf hb_vswprintf
#define wcstod hb_wcstod
#define wcstof hb_wcstof
#define wcstold hb_wcstold

/* The renames rewrite intra-file call sites too: everything called before
 * its definition needs an hb_* prototype.  Plain names here -- the macros
 * do the renaming. */
size_t wcslen(const wchar_t *s);
int wcscmp(const wchar_t *s1, const wchar_t *s2);
int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
size_t wcsspn(const wchar_t *s, const wchar_t *accept);
size_t wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr);
int wcscoll(const wchar_t *s1, const wchar_t *s2);
size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *dst, wchar_t c, size_t n);
int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
#endif

static int state_initial(const mbstate_t *ps) {
#ifdef HOST_TEST
  /* glibc's mbstate_t is a different (union-holding) type; the initial
   * state is the all-zero object there too. */
  static const mbstate_t zero_state;
  return memcmp(ps, &zero_state, sizeof(*ps)) == 0;
#else
  return ps->__count == 0 && ps->__value == 0;
#endif
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
  static mbstate_t internal; /* used when ps == NULL, like glibc */
  unsigned char c;

  if (ps == NULL)
    ps = &internal;

  /* Testing call: mbrtowc(NULL, "", 1, ps).  The C locale is stateless,
   * so the answer is always a completed NUL character. */
  if (s == NULL)
    return 0;

  if (n == 0)
    return (size_t)-2; /* would need more bytes */

  c = (unsigned char)*s;

  if (c == 0) {
    if (pwc != NULL)
      *pwc = 0;
    return 0;
  }
  if (c < 0x80) {
    if (pwc != NULL)
      *pwc = (wchar_t)c;
    return 1;
  }

  errno = EILSEQ;
  return (size_t)-1;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
  return mbrtowc(NULL, s, n, ps);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) {
  static mbstate_t internal;

  if (ps == NULL)
    ps = &internal;

  if (s == NULL) {
    /* Restore-to-initial call; trivially stateful-free. */
#ifdef HOST_TEST
    memset(ps, 0, sizeof(*ps));
#else
    ps->__count = 0;
    ps->__value = 0;
#endif
    return 1;
  }

  if (wc == 0) {
    s[0] = '\0';
    return 1;
  }
  if (wc > 0 && wc < 0x80) {
    s[0] = (char)wc;
    return 1;
  }

  errno = EILSEQ;
  return (size_t)-1;
}

int mbsinit(const mbstate_t *ps) {
  return ps == NULL || state_initial(ps);
}

wint_t btowc(int c) {
  if (c == EOF)
    return WEOF;
  if ((unsigned int)c < 0x80)
    return (wint_t)c;
  return WEOF;
}

int wctob(wint_t c) {
  if ((unsigned long)c < 0x80)
    return (int)c;
  return EOF;
}

int wcwidth(wchar_t wc) {
  /* C locale: printable ASCII is one column; everything else is not
   * printable. */
  if (wc == 0)
    return 0;
  if (wc >= 0x20 && wc < 0x7f)
    return 1;
  return -1;
}

/* ---------------- P3.2: wide strings ---------------- */

size_t wcslen(const wchar_t *s) {
  const wchar_t *p = s;
  while (*p != 0)
    p++;
  return (size_t)(p - s);
}

int wcscmp(const wchar_t *s1, const wchar_t *s2) {
  while (*s1 != 0 && *s1 == *s2) {
    s1++;
    s2++;
  }
  if (*s1 == *s2)
    return 0;
  return *s1 < *s2 ? -1 : 1;
}

int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
  while (n > 0 && *s1 != 0 && *s1 == *s2) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  if (*s1 == *s2)
    return 0;
  return *s1 < *s2 ? -1 : 1;
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src) {
  wchar_t *r = dst;
  while ((*dst++ = *src++) != 0)
    ;
  return r;
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n) {
  wchar_t *r = dst;
  while (n > 0 && *src != 0) {
    *dst++ = *src++;
    n--;
  }
  while (n > 0) {
    *dst++ = 0;
    n--;
  }
  return r;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src) {
  wchar_t *r = dst;
  while (*dst != 0)
    dst++;
  while ((*dst++ = *src++) != 0)
    ;
  return r;
}

wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n) {
  wchar_t *r = dst;
  while (*dst != 0)
    dst++;
  while (n > 0 && *src != 0) {
    *dst++ = *src++;
    n--;
  }
  *dst = 0;
  return r;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
  while (*s != 0 && *s != c)
    s++;
  return *s == c ? (wchar_t *)s : NULL;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
  const wchar_t *last = NULL;
  while (*s != 0) {
    if (*s == c)
      last = s;
    s++;
  }
  return c == 0 ? (wchar_t *)s : (wchar_t *)last;
}

size_t wcsspn(const wchar_t *s, const wchar_t *accept) {
  const wchar_t *p = s;
  while (*p != 0 && wcschr(accept, *p) != NULL)
    p++;
  return (size_t)(p - s);
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject) {
  const wchar_t *p = s;
  while (*p != 0 && wcschr(reject, *p) == NULL)
    p++;
  return (size_t)(p - s);
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept) {
  while (*s != 0) {
    if (wcschr(accept, *s) != NULL)
      return (wchar_t *)s;
    s++;
  }
  return NULL;
}

wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle) {
  if (*needle == 0)
    return (wchar_t *)haystack;
  while (*haystack != 0) {
    size_t i = 0;
    while (needle[i] != 0 && haystack[i] == needle[i])
      i++;
    if (needle[i] == 0)
      return (wchar_t *)haystack;
    haystack++;
  }
  return NULL;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr) {
  wchar_t *p;

  if (s == NULL)
    s = *saveptr;
  if (s == NULL)
    return NULL;

  s += wcsspn(s, delim);
  if (*s == 0) {
    *saveptr = s;
    return NULL;
  }
  p = s + wcscspn(s, delim);
  if (*p != 0) {
    *p = 0;
    *saveptr = p + 1;
  } else {
    *saveptr = p;
  }
  return s;
}

/* The C locale's collation is the wide code point order; glibc's C-locale
 * wcscoll is wcscmp. */
int wcscoll(const wchar_t *s1, const wchar_t *s2) {
  return wcscmp(s1, s2);
}

/* wcsxfrm: identity transform in the C locale.  Copy semantics match
 * glibc's strxfrm/wcsxfrm: min(n, len) wide chars are written; the NUL is
 * only stored when it fits (n > len); the full needed length is returned. */
size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n) {
  size_t len = wcslen(src);
  if (n > 0) {
    size_t copy = len < n ? len : n;
    wmemcpy(dst, src, copy);
    if (n > len)
      dst[len] = 0;
  }
  return len;
}

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n) {
  size_t i;
  for (i = 0; i < n; i++)
    dst[i] = src[i];
  return dst;
}

wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n) {
  size_t i;
  if (dst < src) {
    for (i = 0; i < n; i++)
      dst[i] = src[i];
  } else if (dst > src) {
    for (i = n; i > 0; i--)
      dst[i - 1] = src[i - 1];
  }
  return dst;
}

wchar_t *wmemset(wchar_t *dst, wchar_t c, size_t n) {
  size_t i;
  for (i = 0; i < n; i++)
    dst[i] = c;
  return dst;
}

int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (s1[i] != s2[i])
      return s1[i] < s2[i] ? -1 : 1;
  }
  return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] == c)
      return (wchar_t *)(s + i);
  }
  return NULL;
}

/* ---------------- P3.2: wide <-> multibyte conversion loops ------------ */

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps) {
  const char *s = *src;
  size_t n = 0;

  (void)ps; /* the C locale is stateless: ps is never consulted */

  while (dst == NULL || n < len) {
    unsigned char c = (unsigned char)*s;
    if (c == 0) {
      if (dst != NULL) {
        dst[n] = 0;
        *src = NULL;
      }
      return n;
    }
    if (c >= 0x80) {
      errno = EILSEQ;
      if (dst != NULL)
        *src = s;
      return (size_t)-1;
    }
    if (dst != NULL)
      dst[n] = (wchar_t)c;
    n++;
    s++;
  }
  *src = s;
  return n;
}

size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nmc, size_t len,
                  mbstate_t *ps) {
  const char *s = *src;
  size_t n = 0;

  (void)ps; /* the C locale is stateless */

  while ((dst == NULL || n < len) && nmc > 0) {
    unsigned char c = (unsigned char)*s;
    if (c == 0) {
      if (dst != NULL) {
        dst[n] = 0;
        *src = NULL;
      }
      return n;
    }
    if (c >= 0x80) {
      errno = EILSEQ;
      if (dst != NULL)
        *src = s;
      return (size_t)-1;
    }
    if (dst != NULL)
      dst[n] = (wchar_t)c;
    n++;
    s++;
    nmc--;
  }
  if (dst != NULL)
    *src = s;
  return n;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps) {
  const wchar_t *s = *src;
  size_t n = 0;

  (void)ps; /* the C locale is stateless */

  while (dst == NULL || n < len) {
    wchar_t wc = *s;
    if (wc == 0) {
      if (dst != NULL) {
        dst[n] = 0;
        *src = NULL;
      }
      return n;
    }
    if (wc < 0 || wc >= 0x80) {
      errno = EILSEQ;
      if (dst != NULL)
        *src = s;
      return (size_t)-1;
    }
    if (dst != NULL)
      dst[n] = (char)wc;
    n++;
    s++;
  }
  *src = s;
  return n;
}

size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
                  mbstate_t *ps) {
  const wchar_t *s = *src;
  size_t n = 0;

  (void)ps; /* the C locale is stateless */

  while ((dst == NULL || n < len) && nwc > 0) {
    wchar_t wc = *s;
    if (wc == 0) {
      if (dst != NULL) {
        dst[n] = 0;
        *src = NULL;
      }
      return n;
    }
    if (wc < 0 || wc >= 0x80) {
      errno = EILSEQ;
      if (dst != NULL)
        *src = s;
      return (size_t)-1;
    }
    if (dst != NULL)
      dst[n] = (char)wc;
    n++;
    s++;
    nwc--;
  }
  if (dst != NULL)
    *src = s;
  return n;
}

int mbtowc(wchar_t *pwc, const char *s, size_t n) {
  if (s == NULL)
    return 0; /* stateless */
  if (n == 0)
    return -2; /* glibc: at least one byte must be supplied */
  if (*s == 0) {
    if (pwc != NULL)
      *pwc = 0;
    return 0;
  }
  if ((unsigned char)*s < 0x80) {
    if (pwc != NULL)
      *pwc = (wchar_t)(unsigned char)*s;
    return 1;
  }
  errno = EILSEQ;
  return -1;
}

int mblen(const char *s, size_t n) { return mbtowc(NULL, s, n); }

int wctomb(char *s, wchar_t wc) {
  if (s == NULL)
    return 0; /* stateless */
  if (wc == 0) {
    s[0] = '\0';
    return 1;
  }
  if (wc > 0 && wc < 0x80) {
    s[0] = (char)wc;
    return 1;
  }
  errno = EILSEQ;
  return -1;
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t n) {
  size_t k = 0;

  while (dst == NULL || k < n) {
    unsigned char c = (unsigned char)*src;
    if (c == 0) {
      if (dst != NULL)
        dst[k] = 0;
      return k;
    }
    if (c >= 0x80) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    if (dst != NULL)
      dst[k] = (wchar_t)c;
    k++;
    src++;
  }
  return k;
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n) {
  size_t k = 0;

  while (dst == NULL || k < n) {
    wchar_t wc = *src;
    if (wc == 0) {
      if (dst != NULL)
        dst[k] = 0;
      return k;
    }
    if (wc < 0 || wc >= 0x80) {
      errno = EILSEQ;
      return (size_t)-1;
    }
    if (dst != NULL)
      dst[k] = (char)wc;
    k++;
    src++;
  }
  return k;
}

/* ---------------- P3.2: wide numeric conversions ---------------------- */

/* The C locale admits only ASCII, so a narrow parse of the widened token is
 * byte-identical.  Tokens longer than the buffer are truncated (a >200
 * character numeric token is far outside any real use; glibc caps its own
 * work at similar lengths). */
#define WIDE_PARSE_BUF 200

static size_t wide_to_narrow(char *buf, size_t bufsz, const wchar_t *s) {
  size_t i = 0;
  while (i + 1 < bufsz && s[i] != 0 && s[i] > 0 && s[i] < 0x80) {
    buf[i] = (char)s[i];
    i++;
  }
  buf[i] = '\0';
  return i;
}

#define DEFINE_WCSTOX(name, base, type)                                        \
  type name(const wchar_t *nptr, wchar_t **endptr, int base_) {                \
    char buf[WIDE_PARSE_BUF];                                                  \
    char *nend;                                                                \
    type v;                                                                    \
    size_t used = wide_to_narrow(buf, sizeof(buf), nptr);                      \
    if (used == 0) {                                                           \
      /* glibc leaves errno alone when nothing is converted. */                \
      if (endptr != NULL)                                                      \
        *endptr = (wchar_t *)nptr;                                             \
      return 0;                                                                \
    }                                                                          \
    v = base(buf, &nend, base_);                                               \
    if (endptr != NULL)                                                        \
      *endptr = (wchar_t *)(nptr + (nend - buf));                              \
    return v;                                                                  \
  }

DEFINE_WCSTOX(wcstol, strtol, long)
DEFINE_WCSTOX(wcstoul, strtoul, unsigned long)
DEFINE_WCSTOX(wcstoll, strtoll, long long)
DEFINE_WCSTOX(wcstoull, strtoull, unsigned long long)

double wcstod(const wchar_t *nptr, wchar_t **endptr) {
  char buf[WIDE_PARSE_BUF];
  char *nend;
  double v;
  size_t used = wide_to_narrow(buf, sizeof(buf), nptr);

  if (used == 0) {
    /* glibc leaves errno alone when nothing is converted. */
    if (endptr != NULL)
      *endptr = (wchar_t *)nptr;
    return 0.0;
  }
  v = strtod(buf, &nend);
  if (endptr != NULL)
    *endptr = (wchar_t *)(nptr + (nend - buf));
  return v;
}

float wcstof(const wchar_t *nptr, wchar_t **endptr) {
  return (float)wcstod(nptr, endptr);
}

long double wcstold(const wchar_t *nptr, wchar_t **endptr) {
  char buf[WIDE_PARSE_BUF];
  char *nend;
  long double v;
  size_t used = wide_to_narrow(buf, sizeof(buf), nptr);

  if (used == 0) {
    /* glibc leaves errno alone when nothing is converted. */
    if (endptr != NULL)
      *endptr = (wchar_t *)nptr;
    return 0.0L;
  }
  v = strtold(buf, &nend);
  if (endptr != NULL)
    *endptr = (wchar_t *)(nptr + (nend - buf));
  return v;
}
/* ------------------------------------------------------- wide printf family
 *
 * vswprintf/swprintf (C99 7.29.2.1 subset) for the byte-mode C locale.
 *
 * libc++'s src/string.cpp calls swprintf() for to_wstring(), so the port
 * needs a wide formatter.  Each directive is materialized into an
 * equivalent narrow directive -- flags, field width and precision are
 * copied with '*' replaced by the consumed argument's decimal value -- the
 * argument is taken once with the type the directive names, and the value
 * is rendered with the narrow snprintf().  Wide-string conversions (%ls,
 * %lc) are converted with wcstombs()/wcrtomb() first; their directive is
 * then rendered as narrow %s/%c.
 *
 * The numeric conversions therefore inherit the narrow formatter's
 * behavior.  That includes its current absence of %f/%e/%g/%a support:
 * %L... float specs drop the 'L' and pass a double, and the float
 * conversions will produce correct output only once the narrow formatter
 * grows float support (they render through it either way).
 *
 * Return value follows the C99/POSIX compromise libc++ expects (its
 * as_string() loop resizes until `used <= available`): the number of wide
 * characters the full conversion needs, with at most n-1 characters plus
 * the terminating NUL stored; -1 on an encoding error.
 */

typedef struct {
  wchar_t *p;
  size_t len;
  size_t cap;
} sw_out_t;

static int sw_reserve(sw_out_t *o, size_t extra) {
  size_t need = o->len + extra;
  if (need <= o->cap)
    return 0;
  {
    size_t cap = o->cap ? o->cap : 128;
    wchar_t *np;
    while (cap < need)
      cap *= 2;
    np = (wchar_t *)realloc(o->p, cap * sizeof(wchar_t));
    if (!np)
      return -1;
    o->p = np;
    o->cap = cap;
  }
  return 0;
}

static int sw_putc(sw_out_t *o, wchar_t c) {
  if (sw_reserve(o, 1) < 0)
    return -1;
  o->p[o->len++] = c;
  return 0;
}

/* Append the narrow rendering of one directive.  `piece` names the narrow
 * conversion (e.g. "-8.3lld"); the argument has already been consumed. */
static int sw_emit_narrow(sw_out_t *o, const char *piece, ...) {
  va_list ap;
  va_list ap2;
  int need;
  char *buf;
  int i;

  va_start(ap, piece);
  va_copy(ap2, ap);
  need = vsnprintf(NULL, 0, piece, ap2);
  va_end(ap2);
  if (need < 0) {
    va_end(ap);
    return -1;
  }
  buf = (char *)malloc((size_t)need + 1);
  if (!buf) {
    va_end(ap);
    return -1;
  }
  vsnprintf(buf, (size_t)need + 1, piece, ap);
  va_end(ap);

  for (i = 0; i < need; i++) {
    unsigned char b = (unsigned char)buf[i];
    if (b > 0x7F) { /* cannot happen from our own buffers, but be safe:
                       a byte that is not a valid C-locale character */
      free(buf);
      errno = EILSEQ;
      return -1;
    }
    if (sw_putc(o, (wchar_t)b) < 0) {
      free(buf);
      return -1;
    }
  }
  free(buf);
  return 0;
}

/* Field text (flags/width/precision) of a materialized directive: the
 * wide c/s conversions are rendered straight into the wide buffer and need
 * the numbers, not a narrow round trip. */
static void sw_parse_field(const char *piece, int *minus, int *width,
                           int *prec, int *has_prec) {
  const char *p = piece + 1; /* skip '%' */
  *minus = 0;
  *width = 0;
  *prec = 0;
  *has_prec = 0;
  while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
    if (*p == '-')
      *minus = 1;
    p++;
  }
  while (*p >= '0' && *p <= '9') {
    *width = *width * 10 + (*p - '0');
    p++;
  }
  if (*p == '.') {
    p++;
    *has_prec = 1;
    while (*p >= '0' && *p <= '9') {
      *prec = *prec * 10 + (*p - '0');
      p++;
    }
  }
}

static int sw_pad(sw_out_t *o, int count) {
  while (count-- > 0) {
    if (sw_putc(o, L' ') < 0)
      return -1;
  }
  return 0;
}

int vswprintf(wchar_t *ws, size_t n, const wchar_t *fmt, va_list ap) {
  sw_out_t o;
  int rc = -1;

  o.p = NULL;
  o.len = 0;
  o.cap = 0;

  if (ws == NULL && n > 0)
    return -1;

  while (*fmt) {
    char piece[96];
    size_t pi = 0;

    if (*fmt != L'%') {
      if (sw_putc(&o, *fmt++) < 0)
        goto done;
      continue;
    }

    fmt++; /* past '%' */
    if (*fmt == L'%') {
      if (sw_putc(&o, L'%') < 0)
        goto done;
      fmt++;
      continue;
    }

    /* ---- collect the directive ------------------------------------- */
    piece[pi++] = '%';
    {
      int flags = 0;
      if (*fmt == L'-' || *fmt == L'+' || *fmt == L' ' || *fmt == L'#' ||
          *fmt == L'0') {
        while (*fmt == L'-' || *fmt == L'+' || *fmt == L' ' || *fmt == L'#' ||
               *fmt == L'0') {
          wchar_t f = *fmt++;
          if (pi < sizeof(piece) - 24)
            piece[pi++] = (char)f;
          flags = 1;
        }
      }
      (void)flags;

      /* field width ('*' materialized) */
      if (*fmt == L'*') {
        int w = va_arg(ap, int);
        fmt++;
        if (w < 0) { /* negative width: left-justify with |w| */
          if (pi < sizeof(piece) - 24)
            piece[pi++] = '-';
          w = -w;
        }
        pi += (size_t)snprintf(piece + pi, sizeof(piece) - pi, "%d", w);
      } else {
        while (*fmt >= L'0' && *fmt <= L'9') {
          if (pi < sizeof(piece) - 24)
            piece[pi++] = (char)*fmt;
          fmt++;
        }
      }

      /* precision */
      if (*fmt == L'.') {
        fmt++;
        if (*fmt == L'*') {
          int p = va_arg(ap, int);
          fmt++;
          if (p >= 0)
            pi += (size_t)snprintf(piece + pi, sizeof(piece) - pi, ".%d", p);
          /* negative precision: same as omitted */
        } else {
          if (pi < sizeof(piece) - 24)
            piece[pi++] = '.';
          while (*fmt >= L'0' && *fmt <= L'9') {
            if (pi < sizeof(piece) - 24)
              piece[pi++] = (char)*fmt;
            fmt++;
          }
        }
      }
    }

    /* ---- length modifier ------------------------------------------- */
    {
      int len = 0;   /* 0 none, 1 h, 2 hh, 3 l, 4 ll, 5 j, 6 z, 7 t, 8 L */
      int is_wide = 0;
      if (*fmt == L'h') {
        fmt++;
        len = 1;
        if (*fmt == L'h') { len = 2; fmt++; }
      } else if (*fmt == L'l') {
        fmt++;
        len = 3;
        if (*fmt == L'l') { len = 4; fmt++; }
      } else if (*fmt == L'j') { len = 5; fmt++; }
      else if (*fmt == L'z') { len = 6; fmt++; }
      else if (*fmt == L't') { len = 7; fmt++; }
      else if (*fmt == L'L') { len = 8; fmt++; }

      /* wide c/s: %lc / %ls */
      if (len == 3 && (*fmt == L'c' || *fmt == L's'))
        is_wide = 1;

      if (*fmt == L'\0')
        goto done; /* dangling '%' */

      if (*fmt > 0x7F) { /* directive byte not encodable in the C locale */
        errno = EILSEQ;
        goto done;
      }

      /* length letters are copied verbatim except the wide c/s (which
       * turn into narrow c/s after argument conversion) and 'L' (which
       * the narrow formatter does not know: dropped, double passed). */
      if (is_wide) {
        len = 0; /* converted below */
      } else if (len == 3 || len == 4) {
        if (pi < sizeof(piece) - 8)
          piece[pi++] = 'l';
        if (len == 4 && pi < sizeof(piece) - 8)
          piece[pi++] = 'l';
      } else if (len == 1 || len == 2) {
        if (pi < sizeof(piece) - 8)
          piece[pi++] = 'h';
        if (len == 2 && pi < sizeof(piece) - 8)
          piece[pi++] = 'h';
      } else if (len == 5) {
        if (pi < sizeof(piece) - 8)
          piece[pi++] = 'j';
      } else if (len == 6) {
        if (pi < sizeof(piece) - 8)
          piece[pi++] = 'z';
      } else if (len == 7) {
        if (pi < sizeof(piece) - 8)
          piece[pi++] = 't';
      }

      /* Terminate the materialized directive text: the wide c/s branches
       * render straight into the wide buffer and never append the spec
       * byte, so sw_parse_field() must not read the stack bytes past the
       * field text (they are the previous directive's leftovers). */
      piece[pi] = '\0';

      {
        wchar_t spec = *fmt++;
        switch (spec) {
        case L'd':
        case L'i': {
            int r;
            piece[pi++] = (char)spec;
            piece[pi] = '\0';
            if (len == 4)
              r = sw_emit_narrow(&o, piece, va_arg(ap, long long));
            else if (len == 3 || len == 5 || len == 6 || len == 7)
              r = sw_emit_narrow(&o, piece, va_arg(ap, long));
            else
              r = sw_emit_narrow(&o, piece, va_arg(ap, int));
            if (r < 0)
              goto done;
            break;
          }
        case L'u':
        case L'o':
        case L'x':
        case L'X': {
            int r;
            piece[pi++] = (char)spec;
            piece[pi] = '\0';
            if (len == 4)
              r = sw_emit_narrow(&o, piece, va_arg(ap, unsigned long long));
            else if (len == 3 || len == 5 || len == 6 || len == 7)
              r = sw_emit_narrow(&o, piece, va_arg(ap, unsigned long));
            else
              r = sw_emit_narrow(&o, piece, va_arg(ap, unsigned int));
            if (r < 0)
              goto done;
            break;
          }
        case L'c': {
            int cv = va_arg(ap, int);
            if (is_wide) {
              /* Wide output needs no multibyte conversion: glibc stores the
               * wide char as-is (probe: %lc of U+2014 succeeds, r=1).  The
               * zero flag is ignored for %c (glibc: %05lc pads with
               * spaces). */
              int fm, w, pr, hp;
              sw_parse_field(piece, &fm, &w, &pr, &hp);
              if (w > 1 && !fm && sw_pad(&o, w - 1) < 0)
                goto done;
              if (sw_putc(&o, (wchar_t)cv) < 0)
                goto done;
              if (w > 1 && fm && sw_pad(&o, w - 1) < 0)
                goto done;
            } else {
              piece[pi++] = 'c';
              piece[pi] = '\0';
              if (sw_emit_narrow(&o, piece, cv) < 0)
                goto done;
            }
            break;
          }
        case L's': {
            if (is_wide) {
              wchar_t *wv = va_arg(ap, wchar_t *);
              const wchar_t *src = wv ? wv : L"(null)";
              size_t slen = wcslen(src);
              size_t disp = slen;
              size_t i;
              int fm, w, pr, hp;
              int pad;
              sw_parse_field(piece, &fm, &w, &pr, &hp);
              if (hp && (size_t)pr < slen)
                disp = (size_t)pr; /* precision = wide characters */
              pad = (w > (int)disp) ? w - (int)disp : 0;
              if (pad > 0 && !fm && sw_pad(&o, pad) < 0)
                goto done;
              for (i = 0; i < disp; i++) {
                if (sw_putc(&o, src[i]) < 0)
                  goto done;
              }
              if (pad > 0 && fm && sw_pad(&o, pad) < 0)
                goto done;
            } else {
              const char *sv = va_arg(ap, const char *);
              piece[pi++] = 's';
              piece[pi] = '\0';
              if (sw_emit_narrow(&o, piece, sv) < 0)
                goto done;
            }
            break;
          }
        case L'p': {
            piece[pi++] = 'p';
            piece[pi] = '\0';
            if (sw_emit_narrow(&o, piece, va_arg(ap, void *)) < 0)
              goto done;
            break;
          }
        case L'n': {
            long long cnt = (long long)o.len;
            if (len == 4)
              *(long long *)va_arg(ap, void *) = cnt;
            else if (len == 3 || len == 5 || len == 6 || len == 7)
              *(long *)va_arg(ap, void *) = (long)cnt;
            else
              *(int *)va_arg(ap, void *) = (int)cnt;
            break;
          }
        case L'a': case L'A': case L'e': case L'E':
        case L'f': case L'F': case L'g': case L'G': {
            /* 'L' is not in the narrow formatter: render as double */
            piece[pi++] = (char)spec;
            piece[pi] = '\0';
            if (len == 8) {
              if (sw_emit_narrow(&o, piece, (double)va_arg(ap, long double)) < 0)
                goto done;
            } else {
              if (sw_emit_narrow(&o, piece, va_arg(ap, double)) < 0)
                goto done;
            }
            break;
          }
        default:
          /* unknown conversion: undefined behavior; stop */
          goto done;
        }
      }
    }
  }

  rc = (int)o.len;
  if ((size_t)rc >= n) /* glibc: -1 when the buffer is too small (n counts
                          the terminating NUL; n == 0 always fails) */
    rc = -1;

done:
  if (ws != NULL) {
    size_t stored = (size_t)rc < n ? (size_t)rc : (n > 0 ? n - 1 : 0);
    size_t i;
    for (i = 0; i < stored && i < o.len; i++)
      ws[i] = o.p[i];
    if (n > 0)
      ws[stored] = L'\0';
  }
  free(o.p);
  return rc;
}

int swprintf(wchar_t *ws, size_t n, const wchar_t *fmt, ...) {
  va_list ap;
  int r;

  va_start(ap, fmt);
  r = vswprintf(ws, n, fmt, ap);
  va_end(ap);
  return r;
}
