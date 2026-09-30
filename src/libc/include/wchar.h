/* HobbyOS sysroot: <wchar.h> — C-locale (byte-mode) wide-character surface.
 *
 * HobbyOS has a byte-only C locale: MB_CUR_MAX is 1 and every byte
 * 0x80..0xFF is an encoding error (EILSEQ), exactly like glibc's C
 * locale.  The wide functions below therefore have single-byte behavior.
 * GNU sources that probe the locale (GNU sed's mbcs/localeinfo) get
 * glibc-identical answers and pick their single-byte code paths.
 *
 * P3.2 (browser.md §6, "wide-char basics"): libc++'s <wchar.h>/<cwchar>
 * re-export the *complete* C99 wide surface, so this header declares it.
 * The wide string/memory/conversion functions are implemented in
 * src/libc/src/wchar.c (host-parity tested); the wide stdio family
 * (fgetwc.., wprintf..) and wcsftime are declared for source
 * compatibility but not implemented yet — referencing them fails at
 * link time with the usual missing-symbol error (documented in
 * third_party/libcxx-21.1.8/README.md).
 *
 * HOST_TEST compiles src/libc/src/wchar.c renamed to hb_* so host tests
 * can race these functions against glibc's.
 */
#ifndef HOBBYOS_WCHAR_H
#define HOBBYOS_WCHAR_H 1

#include <stddef.h>
#include <stdarg.h> /* va_list for the v*wprintf/v*wscanf declarations */
#include <locale.h> /* locale_t for the *_l variants */
#include <stdio.h> /* FILE for the wide stdio declarations */

#ifdef __cplusplus
extern "C" {
#endif

  typedef unsigned int wint_t;

#define WEOF ((wint_t)-1)

  /* Opaque conversion state; C-locale conversions never leave the initial
   * (all-zero) state. */
  typedef struct {
    int __count;
    unsigned int __value;
  } mbstate_t;

  /* ---- conversions (byte-mode C locale) ---- */
  size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
  size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
  size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
  size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
  size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);
  size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len,
                    mbstate_t *ps);
  size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
                    mbstate_t *ps);
  int mbsinit(const mbstate_t *ps);
  wint_t btowc(int c);
  int wctob(wint_t c);
  int mbtowc(wchar_t *pwc, const char *s, size_t n);
  int wctomb(char *s, wchar_t wc);
  size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
  size_t wcstombs(char *dst, const wchar_t *src, size_t n);
  int wcwidth(wchar_t wc);

  /* ---- wide strings ---- */
  size_t wcslen(const wchar_t *s);
  wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
  wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
  wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
  wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n);
  int wcscmp(const wchar_t *s1, const wchar_t *s2);
  int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n);
  int wcscoll(const wchar_t *s1, const wchar_t *s2);
  size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n);
  wchar_t *wcschr(const wchar_t *s, wchar_t c);
  wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
  size_t wcsspn(const wchar_t *s, const wchar_t *accept);
  size_t wcscspn(const wchar_t *s, const wchar_t *reject);
  wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
  wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
  wchar_t *wcstok(wchar_t *str, const wchar_t *delim, wchar_t **saveptr);
  wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
  wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n);
  wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n);
  wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);
  int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n);

  /* ---- wide numeric conversions (C-locale byte mode) ---- */
  long wcstol(const wchar_t *nptr, wchar_t **endptr, int base);
  unsigned long wcstoul(const wchar_t *nptr, wchar_t **endptr, int base);
  long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base);
  unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr, int base);
  double wcstod(const wchar_t *nptr, wchar_t **endptr);
  float wcstof(const wchar_t *nptr, wchar_t **endptr);
  long double wcstold(const wchar_t *nptr, wchar_t **endptr);

  /* ---- wide time ---- */
  struct tm; /* defined by <time.h>; declared for source compatibility */
  size_t wcsftime(wchar_t *s, size_t maxsize, const wchar_t *format,
                  const struct tm *timeptr);

  /* xlocale/P3.2 variants (C locale: the locale argument is ignored). */
  int wcscoll_l(const wchar_t *s1, const wchar_t *s2, locale_t loc);
  size_t wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, locale_t loc);

  /* ---- wide stdio (declarations only: see file header) ---- */
  int fwide(FILE *stream, int mode);
  wint_t fgetwc(FILE *stream);
  wchar_t *fgetws(wchar_t *s, int n, FILE *stream);
  wint_t fputwc(wchar_t wc, FILE *stream);
  int fputws(const wchar_t *s, FILE *stream);
  wint_t getwc(FILE *stream);
  wint_t getwchar(void);
  wint_t putwc(wchar_t wc, FILE *stream);
  wint_t putwchar(wchar_t wc);
  wint_t ungetwc(wint_t wc, FILE *stream);
  int fwprintf(FILE *stream, const wchar_t *format, ...);
  int swprintf(wchar_t *s, size_t n, const wchar_t *format, ...);
  int wprintf(const wchar_t *format, ...);
  int fwscanf(FILE *stream, const wchar_t *format, ...);
  int swscanf(const wchar_t *s, const wchar_t *format, ...);
  int wscanf(const wchar_t *format, ...);
  int vfwprintf(FILE *stream, const wchar_t *format, va_list ap);
  int vswprintf(wchar_t *s, size_t n, const wchar_t *format,
                va_list ap);
  int vwprintf(const wchar_t *format, va_list ap);
  int vfwscanf(FILE *stream, const wchar_t *format, va_list ap);
  int vswscanf(const wchar_t *s, const wchar_t *format, va_list ap);
  int vwscanf(const wchar_t *format, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_WCHAR_H */
