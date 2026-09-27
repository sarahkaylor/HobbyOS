#ifndef HOBBYOS_LIBGEN_H
#define HOBBYOS_LIBGEN_H

/* HobbyOS sysroot: <libgen.h> — POSIX pathname decomposition.
 *
 * dirname()/basename() follow POSIX.1-2008 (stripping trailing slashes,
 * "." for empty names, "/" for the root).  They may modify their argument,
 * exactly like POSIX says.
 *
 * Host (HOST_TEST): defer to the real <libgen.h> via include_next.
 */

#ifdef HOST_TEST
#include_next <libgen.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

  char *dirname(char *path);
  char *basename(char *path);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_LIBGEN_H */
