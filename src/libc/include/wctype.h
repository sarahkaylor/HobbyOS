/* HobbyOS sysroot: <wctype.h> — C-locale (byte-mode) wide character
 * classification.
 *
 * See <wchar.h>: HobbyOS is byte-only with glibc's C-locale behavior.
 * The classification functions here are the C locale's (ASCII case
 * mapping, ASCII printability), so GNU sources that consult them during
 * locale setup get glibc-identical answers.
 *
 * P3.2 (browser.md §6, "wide-char basics"): libc++'s <cwctype> re-exports
 * the complete C99 surface, so it is declared and implemented here
 * (ASCII classes; wctype()/wctrans() answer the standard class names —
 * see src/libc/src/wctype.c).
 *
 * HOST_TEST compiles src/libc/src/wctype.c renamed to hb_* so host tests
 * can race these functions against glibc's.
 */
#ifndef HOBBYOS_WCTYPE_H
#define HOBBYOS_WCTYPE_H 1

#include <wchar.h>
#include <locale.h> /* locale_t for the *_l variants */

#ifdef __cplusplus
extern "C" {
#endif

  typedef unsigned long int wctype_t;
  typedef const int *wctrans_t; /* non-NULL token; see wctype.c */

  int iswalnum(wint_t c);
  int iswalpha(wint_t c);
  int iswblank(wint_t c);
  int iswcntrl(wint_t c);
  int iswdigit(wint_t c);
  int iswgraph(wint_t c);
  int iswlower(wint_t c);
  int iswprint(wint_t c);
  int iswpunct(wint_t c);
  int iswspace(wint_t c);
  int iswupper(wint_t c);
  int iswxdigit(wint_t c);
  int iswctype(wint_t c, wctype_t desc);

  wctype_t wctype(const char *property);
  wctrans_t wctrans(const char *property);
  wint_t towlower(wint_t c);
  wint_t towupper(wint_t c);
  wint_t towctrans(wint_t c, wctrans_t desc);

  /* xlocale/P3.2 variants (C locale: the locale argument is ignored). */
  int iswalnum_l(wint_t c, locale_t loc);
  int iswalpha_l(wint_t c, locale_t loc);
  int iswblank_l(wint_t c, locale_t loc);
  int iswcntrl_l(wint_t c, locale_t loc);
  int iswdigit_l(wint_t c, locale_t loc);
  int iswgraph_l(wint_t c, locale_t loc);
  int iswlower_l(wint_t c, locale_t loc);
  int iswprint_l(wint_t c, locale_t loc);
  int iswpunct_l(wint_t c, locale_t loc);
  int iswspace_l(wint_t c, locale_t loc);
  int iswupper_l(wint_t c, locale_t loc);
  int iswxdigit_l(wint_t c, locale_t loc);
  int iswctype_l(wint_t c, wctype_t desc, locale_t loc);
  wint_t towlower_l(wint_t c, locale_t loc);
  wint_t towupper_l(wint_t c, locale_t loc);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_WCTYPE_H */
