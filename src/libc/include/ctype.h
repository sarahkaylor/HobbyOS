#ifndef HOBBYOS_CTYPE_H
#define HOBBYOS_CTYPE_H

#include <locale.h> /* locale_t for the *_l variants */

#ifdef __cplusplus
extern "C" {
#endif

  /* HobbyOS Phase-1 libc: ctype.h — full classification and conversion set,
   * C locale, driven by a char-class table in src/libc/src/ctype.c. Inputs
   * are interpreted as in glibc: c in [-128, 255] is looked up as
   * (unsigned char)c (EOF=-1 passes through in tolower/toupper); anything
   * else is undefined (returns 0). */


  int isalnum(int c);
  int isalpha(int c);
  int isascii(int c);
  int isblank(int c);
  int iscntrl(int c);
  int isdigit(int c);
  int isgraph(int c);
  int islower(int c);
  int isprint(int c);
  int ispunct(int c);
  int isspace(int c);
  int isupper(int c);
  int isxdigit(int c);
  int tolower(int c);
  int toupper(int c);

  /* xlocale/P3.2 variants (C locale: the locale argument is ignored; see
   * src/libc/src/xlocale.c).  libc++'s <ctype.h> layer requires
   * isdigit_l/isxdigit_l at header-parse time. */
  int isalnum_l(int c, locale_t loc);
  int isalpha_l(int c, locale_t loc);
  int isblank_l(int c, locale_t loc);
  int iscntrl_l(int c, locale_t loc);
  int isdigit_l(int c, locale_t loc);
  int isgraph_l(int c, locale_t loc);
  int islower_l(int c, locale_t loc);
  int isprint_l(int c, locale_t loc);
  int ispunct_l(int c, locale_t loc);
  int isspace_l(int c, locale_t loc);
  int isupper_l(int c, locale_t loc);
  int isxdigit_l(int c, locale_t loc);
  int tolower_l(int c, locale_t loc);
  int toupper_l(int c, locale_t loc);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_CTYPE_H */
