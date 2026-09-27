#ifndef HOBBYOS_STRINGS_H
#define HOBBYOS_STRINGS_H

/* HobbyOS sysroot: <strings.h> — the BSD/POSIX byte-string names that live
 * outside <string.h> (strcasecmp family, ffs).  The implementations are in
 * string.c; this header exists because ported GNU code includes it
 * directly (nano, coreutils' case-insensitive paths).
 *
 * Host (HOST_TEST): defer to the real <strings.h> via include_next.
 */

#ifdef HOST_TEST
#include_next <strings.h>
#else

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

  int strcasecmp(const char *s1, const char *s2);
  int strncasecmp(const char *s1, const char *s2, size_t n);

  /* Bit-scan (POSIX). */
  int ffs(int i);
  int ffsl(long i);
  int ffsll(long long i);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_STRINGS_H */
