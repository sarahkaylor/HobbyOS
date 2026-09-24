/* HobbyOS sysroot: <wctype.h> — C-locale (byte-mode) wide character
 * classification.
 *
 * See <wchar.h>: HobbyOS is byte-only with glibc's C-locale behavior.
 * The classification functions here are the C locale's (ASCII case
 * mapping, ASCII printability), so GNU sources that consult them during
 * locale setup get glibc-identical answers.
 *
 * HOST_TEST compiles src/libc/src/wctype.c renamed to hb_* so host tests
 * can race these functions against glibc's.
 */
#ifndef HOBBYOS_WCTYPE_H
#define HOBBYOS_WCTYPE_H 1

#include <wchar.h>

typedef unsigned long int wctype_t;

int iswprint(wint_t c);
int iswblank(wint_t c);
wint_t towlower(wint_t c);
wint_t towupper(wint_t c);

#endif /* HOBBYOS_WCTYPE_H */
