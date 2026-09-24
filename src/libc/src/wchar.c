/* HobbyOS libc: wchar.c — C-locale (byte-mode) wchar.h functions.
 *
 * Semantics are the C locale's, matching glibc byte-for-byte:
 *  - bytes 0x00..0x7F are valid single-byte characters;
 *  - bytes 0x80..0xFF are encoding errors (EILSEQ);
 *  - the conversion state is stateless (always initial after return).
 *
 * HOST_TEST compiles this translation unit renamed to hb_* so host
 * tests can race it against glibc on the same inputs.
 */
#ifdef HOST_TEST
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#else
#include "wchar.h"
#include "errno.h"
#include "stdio.h" /* EOF for wctob() */
#endif

#ifdef HOST_TEST
#define mbrtowc hb_mbrtowc
#define mbrlen hb_mbrlen
#define wcrtomb hb_wcrtomb
#define mbsinit hb_mbsinit
#define btowc hb_btowc
#define wctob hb_wctob
#define wcwidth hb_wcwidth
#endif

static int state_initial(const mbstate_t *ps) {
  return ps->__count == 0 && ps->__value == 0;
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
    ps->__count = 0;
    ps->__value = 0;
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
