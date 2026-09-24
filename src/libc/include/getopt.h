/*
 * HobbyOS Phase-2 sysroot: getopt.h — POSIX getopt + GNU getopt_long.
 * Implementation in src/libc/src/getopt.c (host-testable, raced against
 * glibc's getopt_long in src/host/libc_getopt_test.c).
 *
 * Under HOST_TEST the functions are renamed hb_* (same pattern as the
 * read/write/open/close ho_* mapping) so a host binary can use OUR parser
 * while everything else calls glibc.
 */
#ifndef __HB_GETOPT_H
#define __HB_GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

  struct option {
    const char *name;
    /* has_arg can also be ':' or ';' to indicate an option argument
       that is optional. */
    int has_arg;
    int *flag;
    int val;
  };

#define no_argument 0
#define required_argument 1
#define optional_argument 2

#ifdef HOST_TEST
#define getopt hb_getopt
#define getopt_long hb_getopt_long
#define optarg hb_optarg
#define optind hb_optind
#define optopt hb_optopt
#define opterr hb_opterr
#define optreset hb_optreset
#endif

  extern char *optarg;
  extern int optind, opterr, optopt, optreset;

  int getopt(int argc, char *const argv[], const char *optstring);
  int getopt_long(int argc, char *const argv[], const char *optstring,
                  const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif /* __HB_GETOPT_H */
