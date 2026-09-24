/* HobbyOS libc: wctype.c — C-locale wide classification and case maps.
 *
 * The C locale's answers, matching glibc:
 *  - towlower/towupper change only ASCII letters;
 *  - iswprint covers 0x20..0x7E;
 *  - iswblank covers space and tab.
 *
 * HOST_TEST compiles this translation unit renamed to hb_* so host tests
 * can race it against glibc on the same inputs.
 */
#ifdef HOST_TEST
#include <wchar.h>
#else
#include "wctype.h"
#endif

#ifdef HOST_TEST
#define iswprint hb_iswprint
#define iswblank hb_iswblank
#define towlower hb_towlower
#define towupper hb_towupper
#endif

int iswprint(wint_t c) {
  return c >= 0x20 && c < 0x7f;
}

int iswblank(wint_t c) {
  return c == ' ' || c == '\t';
}

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
