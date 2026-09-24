/*
 * HobbyOS sysroot: <sys/param.h> — MIN/MAX helpers.
 *
 * gnulib's minmax.h includes this header for MIN/MAX before supplying its
 * own definitions, so the core macros must be present.  (No MAXPATHLEN /
 * device-macro baggage: ports that need those include different headers.)
 */
#ifndef HOBBYOS_SYS_PARAM_H
#define HOBBYOS_SYS_PARAM_H 1

#undef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#undef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif /* HOBBYOS_SYS_PARAM_H */
