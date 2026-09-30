/* HobbyOS sysroot: <locale.h> — categories; only the C locale exists.
 *
 * Category values match glibc's so host comparisons stay honest.  P3.2
 * (browser.md §6): the xlocale surface (locale_t + LC_*_MASK + the
 * newlocale/freelocale/uselocale trio) is what libc++'s locale machinery
 * requires; HobbyOS has exactly one locale ("C"/"POSIX"), so the locale
 * handle is an opaque tag and every *_l function ignores it — see
 * src/libc/src/xlocale.c.  lconv/localeconv report the C locale's
 * formatting facts (glibc's C-locale values).
 */
#ifndef HOBBYOS_LOCALE_H
#define HOBBYOS_LOCALE_H 1

#define LC_CTYPE 0
#define LC_NUMERIC 1
#define LC_TIME 2
#define LC_COLLATE 3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL 6

/* glibc-compatible mask spelling ((1 << category)). */
#define LC_CTYPE_MASK (1 << LC_CTYPE)
#define LC_NUMERIC_MASK (1 << LC_NUMERIC)
#define LC_TIME_MASK (1 << LC_TIME)
#define LC_COLLATE_MASK (1 << LC_COLLATE)
#define LC_MONETARY_MASK (1 << LC_MONETARY)
#define LC_MESSAGES_MASK (1 << LC_MESSAGES)
#define LC_ALL_MASK                                                            \
  (LC_CTYPE_MASK | LC_NUMERIC_MASK | LC_TIME_MASK | LC_COLLATE_MASK |          \
   LC_MONETARY_MASK | LC_MESSAGES_MASK)

#ifdef __cplusplus
extern "C" {
#endif

  /* Opaque C-locale handle (P3.2).  Only the single built-in "C" locale
   * exists, so every non-NULL handle behaves identically. */
  struct __hb_locale;
  typedef struct __hb_locale *locale_t;

  char *setlocale(int category, const char *locale);

  /* xlocale (P3.2): "C" is always available; any other name fails (NULL).
   * The handle is an opaque tag allocated once (bound to LC_GLOBAL_LOCALE
   * semantics: uselocale's per-thread slot is documented in xlocale.c). */
  locale_t newlocale(int category_mask, const char *locale, locale_t base);
  locale_t duplocale(locale_t loc);
  void freelocale(locale_t loc);
  locale_t uselocale(locale_t newloc);
#define LC_GLOBAL_LOCALE ((locale_t)-1)

  /* Numeric/monetary formatting facts of the C locale (glibc values). */
  struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
  };

  struct lconv *localeconv(void);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_LOCALE_H */
