/* HobbyOS sysroot: <wchar.h> — C-locale (byte-mode) wide-character surface.
 *
 * HobbyOS has a byte-only C locale: MB_CUR_MAX is 1 and every byte
 * 0x80..0xFF is an encoding error (EILSEQ), exactly like glibc's C
 * locale.  The wide functions below therefore have single-byte behavior.
 * GNU sources that probe the locale (GNU sed's mbcs/localeinfo) get
 * glibc-identical answers and pick their single-byte code paths.
 *
 * HOST_TEST compiles src/libc/src/wchar.c renamed to hb_* so host tests
 * can race these functions against glibc's.
 */
#ifndef HOBBYOS_WCHAR_H
#define HOBBYOS_WCHAR_H 1

#include <stddef.h>

typedef unsigned int wint_t;

#define WEOF ((wint_t)-1)

/* Opaque conversion state; C-locale conversions never leave the initial
 * (all-zero) state. */
typedef struct {
  int __count;
  unsigned int __value;
} mbstate_t;

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
int mbsinit(const mbstate_t *ps);
wint_t btowc(int c);
int wctob(wint_t c);
int wcwidth(wchar_t wc);

#endif /* HOBBYOS_WCHAR_H */
