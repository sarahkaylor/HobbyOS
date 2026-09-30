/* HobbyOS libc: locale.c — setlocale() for the C-only environment.
 *
 * HobbyOS ships exactly one locale: "C".  setlocale() accepts the
 * portable names for it ("", "C", "POSIX") and reports any other locale
 * as unavailable (NULL, ENOENT), which is also what glibc does for
 * locales that are not installed.  GNU tools therefore run their
 * C-locale code paths and nl_langinfo(CODESET) stays byte-exact with
 * glibc's "ANSI_X3.4-1968".
 *
 * HOST_TEST compiles this translation unit renamed to hb_* so host tests
 * can race it against glibc.
 */
#ifdef HOST_TEST
#include <errno.h>
#include <limits.h>
#include <string.h>
#else
#include "locale.h"
#include "errno.h"
#include "limits.h"
#include "string.h"
#endif

#ifdef HOST_TEST
#define setlocale hb_setlocale
#endif

char *setlocale(int category, const char *locale) {
  (void)category; /* there is only LC_ALL == "C" */

  if (locale == NULL)
    return (char *)"C";

  if (locale[0] == '\0' || strcmp(locale, "C") == 0 ||
      strcmp(locale, "POSIX") == 0)
    return (char *)"C";

  errno = ENOENT;
  return NULL;
}

/* lconv for the C locale.  Field-for-field glibc's values (host test
 * races localeconv against glibc's): only decimal_point is non-empty and
 * the numeric singles are CHAR_MAX ("not available"). */
static struct lconv c_lconv = {
  ".", "", "", "", "", "", "", "", "", "",
  CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
  CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
};

struct lconv *localeconv(void) { return &c_lconv; }
