#ifndef HOBBYOS_LIMITS_H
#define HOBBYOS_LIMITS_H

/* HobbyOS Phase-1 sysroot: limits.h wrapper over the compiler's builtin
 * (CHAR_BIT, LONG_MAX, etc. are target-correct there). */
#include_next <limits.h>

/* M4/L5: POSIX path limits.  HobbyOS's libc had none, but every ported
 * program sizes path buffers with PATH_MAX (Mesa: fossilize_db.c,
 * os_file_notify.c, u_cpu_detect.c).  4096 is the Linux value; the VFS's own
 * limit is smaller, so a buffer sized by this can never be overrun. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#endif /* HOBBYOS_LIMITS_H */
