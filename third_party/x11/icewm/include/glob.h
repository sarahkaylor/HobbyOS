/* Lane-local <glob.h> for the HobbyOS IceWM build.  glob()/globfree() in
 * ../compat/hb_glob.c (opendir/readdir + fnmatch, per path component). */
#ifndef HB_ICEWM_GLOB_H
#define HB_ICEWM_GLOB_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  size_t gl_pathc;
  char **gl_pathv;
  size_t gl_offs;
  int gl_flags;
} glob_t;
#define GLOB_ERR      0x0001
#define GLOB_MARK     0x0002
#define GLOB_NOSORT   0x0004
#define GLOB_DOOFFS   0x0008
#define GLOB_NOCHECK  0x0010
#define GLOB_APPEND   0x0020
#define GLOB_NOESCAPE 0x0040
#define GLOB_PERIOD   0x0080
#define GLOB_TILDE    0x1000
#define GLOB_NOSPACE  1
#define GLOB_ABORTED  2
#define GLOB_NOMATCH  3
#define GLOB_NOSYS    4
int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob);
void globfree(glob_t *pglob);
#ifdef __cplusplus
}
#endif
#endif
