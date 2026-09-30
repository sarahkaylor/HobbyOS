/* HobbyOS libc: xlocale.c — locale_t objects + POSIX-2008 *_l functions.
 *
 * P3.2 (browser.md §6): libc++'s locale layer calls the xlocale surface
 * (newlocale/uselocale/freelocale + the *_l wrappers).  HobbyOS has exactly
 * one locale — "C" — so every locale object is a stateless token, every
 * wrapper ignores its locale_t argument, and uselocale() only tracks the
 * current token for round-tripping (the C locale's behavior is identical
 * whichever token is active; POSIX' per-thread current-locale semantics
 * are therefore trivially satisfied).
 *
 * The token is a heap-allocated struct so newlocale/duplocale/freelocale
 * have correct lifetimes; LC_GLOBAL_LOCALE ((locale_t)-1) is never
 * allocated or freed.
 *
 * HOST_TEST compiles this translation unit renamed to hb_* so host tests
 * can race the wrappers against glibc's *_l on the C locale.
 */
#ifdef HOST_TEST
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#else
#include "ctype.h"
#include "errno.h"
#include "locale.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"
#include "wchar.h"
#include "wctype.h"
#endif

#ifdef HOST_TEST
#define newlocale hb_newlocale
#define duplocale hb_duplocale
#define freelocale hb_freelocale
#define uselocale hb_uselocale
#define strtol_l hb_strtol_l
#define strtoul_l hb_strtoul_l
#define strtoll_l hb_strtoll_l
#define strtoull_l hb_strtoull_l
#define isdigit_l hb_isdigit_l
#define isxdigit_l hb_isxdigit_l
#define strcoll_l hb_strcoll_l
#define strxfrm_l hb_strxfrm_l
#define toupper_l hb_toupper_l
#define tolower_l hb_tolower_l
#define iswctype_l hb_iswctype_l
#define wcscoll_l hb_wcscoll_l
#define wcsxfrm_l hb_wcsxfrm_l
#define iswspace_l hb_iswspace_l
#define iswprint_l hb_iswprint_l
#define iswcntrl_l hb_iswcntrl_l
#define iswupper_l hb_iswupper_l
#define iswlower_l hb_iswlower_l
#define iswalpha_l hb_iswalpha_l
#define iswblank_l hb_iswblank_l
#define iswdigit_l hb_iswdigit_l
#define iswpunct_l hb_iswpunct_l
#define iswxdigit_l hb_iswxdigit_l
#define towupper_l hb_towupper_l
#define towlower_l hb_towlower_l
#endif

struct __hb_locale {
  unsigned long __category_mask;
};

static struct __hb_locale c_locale_object = {LC_ALL_MASK};

/* Current locale token for uselocale(); NULL means "global". */
static locale_t current_locale;

static int locale_name_ok(const char *name) {
  return name != NULL &&
         (name[0] == '\0' || strcmp(name, "C") == 0 ||
          strcmp(name, "POSIX") == 0);
}

locale_t newlocale(int category_mask, const char *locale, locale_t base) {
  struct __hb_locale *loc;

  if ((unsigned int)category_mask & ~(unsigned int)LC_ALL_MASK) {
    errno = EINVAL;
    return NULL;
  }
  if (!locale_name_ok(locale)) {
    errno = ENOENT;
    return NULL;
  }
  if (base == LC_GLOBAL_LOCALE && category_mask == 0) {
    errno = EINVAL;
    return NULL;
  }

  loc = malloc(sizeof(*loc));
  if (loc == NULL) {
    errno = ENOMEM;
    return NULL;
  }
  loc->__category_mask = (unsigned long)category_mask;
  return loc;
}

locale_t duplocale(locale_t loc) {
  struct __hb_locale *copy;

  if (loc == LC_GLOBAL_LOCALE)
    loc = &c_locale_object;
  if (loc == NULL) {
    errno = EINVAL;
    return NULL;
  }
  copy = malloc(sizeof(*copy));
  if (copy == NULL) {
    errno = ENOMEM;
    return NULL;
  }
#ifdef HOST_TEST
  /* glibc's locale_t points at a different struct type; a byte copy of
   * our (tiny) object is all this implementation ever reads back. */
  memcpy(copy, loc, sizeof(*copy));
#else
  *copy = *loc;
#endif
  return copy;
}

void freelocale(locale_t loc) {
  if (loc != NULL && loc != LC_GLOBAL_LOCALE)
    free(loc);
}

locale_t uselocale(locale_t newloc) {
  locale_t previous = current_locale != NULL ? current_locale : LC_GLOBAL_LOCALE;

  if (newloc != NULL) {
    if (newloc == LC_GLOBAL_LOCALE)
      current_locale = NULL;
    else
      current_locale = newloc;
  }
  return previous;
}

/* ---------------- integer strto*_l ---------------- */

long strtol_l(const char *nptr, char **endptr, int base, locale_t loc) {
  (void)loc;
  return strtol(nptr, endptr, base);
}

unsigned long strtoul_l(const char *nptr, char **endptr, int base,
                        locale_t loc) {
  (void)loc;
  return strtoul(nptr, endptr, base);
}

long long strtoll_l(const char *nptr, char **endptr, int base, locale_t loc) {
  (void)loc;
  return strtoll(nptr, endptr, base);
}

unsigned long long strtoull_l(const char *nptr, char **endptr, int base,
                              locale_t loc) {
  (void)loc;
  return strtoull(nptr, endptr, base);
}

/* ---------------- ctype / string ---------------- */

int strcoll_l(const char *s1, const char *s2, locale_t loc) {
  (void)loc;
  return strcoll(s1, s2);
}

size_t strxfrm_l(char *dst, const char *src, size_t n, locale_t loc) {
  (void)loc;
  return strxfrm(dst, src, n);
}

int isdigit_l(int c, locale_t loc) {
  (void)loc;
  return isdigit(c);
}

int isxdigit_l(int c, locale_t loc) {
  (void)loc;
  return isxdigit(c);
}

int toupper_l(int c, locale_t loc) {
  (void)loc;
  return toupper(c);
}

int tolower_l(int c, locale_t loc) {
  (void)loc;
  return tolower(c);
}

/* ---------------- wide classification / collation ---------------- */

int wcscoll_l(const wchar_t *s1, const wchar_t *s2, locale_t loc) {
  (void)loc;
  return wcscoll(s1, s2);
}

size_t wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, locale_t loc) {
  (void)loc;
  return wcsxfrm(dst, src, n);
}

int iswctype_l(wint_t c, wctype_t type, locale_t loc) {
  (void)loc;
  return iswctype(c, type);
}

int iswspace_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswspace(c);
}

int iswprint_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswprint(c);
}

int iswcntrl_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswcntrl(c);
}

int iswupper_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswupper(c);
}

int iswlower_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswlower(c);
}

int iswalpha_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswalpha(c);
}

int iswblank_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswblank(c);
}

int iswdigit_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswdigit(c);
}

int iswpunct_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswpunct(c);
}

int iswxdigit_l(wint_t c, locale_t loc) {
  (void)loc;
  return iswxdigit(c);
}

wint_t towupper_l(wint_t c, locale_t loc) {
  (void)loc;
  return towupper(c);
}

wint_t towlower_l(wint_t c, locale_t loc) {
  (void)loc;
  return towlower(c);
}
