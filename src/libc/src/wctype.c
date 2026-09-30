/* HobbyOS libc: wctype.c — C-locale wide classification and case maps.
 *
 * The C locale's answers, matching glibc:
 *  - towlower/towupper change only ASCII letters;
 *  - iswprint covers 0x20..0x7E;
 *  - iswblank covers space and tab.
 *
 * P3.2 adds the full isw* family, the wctype()/iswctype() name lookup and
 * the wctrans()/towctrans() transforms (browser.md §6 — libc++'s <cwctype>
 * re-exports these, and the C locale is all HobbyOS has).
 *
 * HOST_TEST compiles this translation unit renamed to hb_* so host tests
 * can race it against glibc on the same inputs.
 */
#ifdef HOST_TEST
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#else
#include "wctype.h"
#include "string.h"
#endif

#ifdef HOST_TEST
#define iswprint hb_iswprint
#define iswblank hb_iswblank
#define iswalpha hb_iswalpha
#define iswalnum hb_iswalnum
#define iswcntrl hb_iswcntrl
#define iswdigit hb_iswdigit
#define iswgraph hb_iswgraph
#define iswlower hb_iswlower
#define iswpunct hb_iswpunct
#define iswspace hb_iswspace
#define iswupper hb_iswupper
#define iswxdigit hb_iswxdigit
#define wctype hb_wctype
#define iswctype hb_iswctype
#define wctrans hb_wctrans
#define towctrans hb_towctrans
#define towlower hb_towlower
#define towupper hb_towupper
#endif

int iswprint(wint_t c) {
  return c >= 0x20 && c < 0x7f;
}

int iswblank(wint_t c) {
  return c == ' ' || c == '\t';
}

static int w_isalpha(wint_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int w_isdigit(wint_t c) { return c >= '0' && c <= '9'; }

static int w_isxdigit(wint_t c) {
  return w_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int w_isspace(wint_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

int iswalpha(wint_t c) { return w_isalpha(c); }
int iswalnum(wint_t c) { return w_isalpha(c) || w_isdigit(c); }
int iswcntrl(wint_t c) { return c < 0x20 || c == 0x7f; }
int iswdigit(wint_t c) { return w_isdigit(c); }
int iswgraph(wint_t c) { return c > 0x20 && c < 0x7f; }
int iswlower(wint_t c) { return c >= 'a' && c <= 'z'; }
int iswpunct(wint_t c) { return iswgraph(c) && !iswalnum(c); }
int iswspace(wint_t c) { return w_isspace(c); }
int iswupper(wint_t c) { return c >= 'A' && c <= 'Z'; }
int iswxdigit(wint_t c) { return w_isxdigit(c); }

wint_t towlower(wint_t c) {
  if (c >= 'A' && c <= 'Z')
    return c + ('a' - 'A');
  return c;
}

wint_t towupper(wint_t c) {
  if (c >= 'a' && c <= 'z')
    return c - ('a' - 'A');
  return c;
}

/* ---------------- P3.2: wctype()/wctrans() ---------------------------- */

/* Class ids: small integers handed out as wctype_t tokens.  0 is the
 * "no such class" answer, as in glibc. */
enum {
  WC_ALNUM = 1,
  WC_ALPHA,
  WC_BLANK,
  WC_CNTRL,
  WC_DIGIT,
  WC_GRAPH,
  WC_LOWER,
  WC_PRINT,
  WC_PUNCT,
  WC_SPACE,
  WC_UPPER,
  WC_XDIGIT
};

struct wctype_pair {
  const char *name;
  unsigned long id;
};

static const struct wctype_pair wctype_names[] = { {"alnum", WC_ALNUM}, {"alpha", WC_ALPHA}, {"blank", WC_BLANK}, {"cntrl", WC_CNTRL}, {"digit", WC_DIGIT}, {"graph", WC_GRAPH}, {"lower", WC_LOWER}, {"print", WC_PRINT}, {"punct", WC_PUNCT}, {"space", WC_SPACE}, {"upper", WC_UPPER}, {"xdigit", WC_XDIGIT},
};

wctype_t wctype(const char *name) {
  unsigned long i;
  for (i = 0; i < sizeof(wctype_names) / sizeof(wctype_names[0]); i++) {
    if (strcmp(name, wctype_names[i].name) == 0)
      return wctype_names[i].id;
  }
  return 0;
}

int iswctype(wint_t c, wctype_t type) {
  switch (type) {
  case WC_ALNUM:
    return iswalnum(c);
  case WC_ALPHA:
    return iswalpha(c);
  case WC_BLANK:
    return iswblank(c);
  case WC_CNTRL:
    return iswcntrl(c);
  case WC_DIGIT:
    return iswdigit(c);
  case WC_GRAPH:
    return iswgraph(c);
  case WC_LOWER:
    return iswlower(c);
  case WC_PRINT:
    return iswprint(c);
  case WC_PUNCT:
    return iswpunct(c);
  case WC_SPACE:
    return iswspace(c);
  case WC_UPPER:
    return iswupper(c);
  case WC_XDIGIT:
    return iswxdigit(c);
  default:
    return 0;
  }
}

/* wctrans_t is `const int *` in <wctype.h>; the token points at one of
 * these constants. */
static const int wctrans_tolower_id = 1;
static const int wctrans_toupper_id = 2;

wctrans_t wctrans(const char *name) {
  if (strcmp(name, "tolower") == 0)
    return &wctrans_tolower_id;
  if (strcmp(name, "toupper") == 0)
    return &wctrans_toupper_id;
  return NULL;
}

wint_t towctrans(wint_t c, wctrans_t desc) {
  if (desc == &wctrans_tolower_id)
    return towlower(c);
  if (desc == &wctrans_toupper_id)
    return towupper(c);
  return c;
}
