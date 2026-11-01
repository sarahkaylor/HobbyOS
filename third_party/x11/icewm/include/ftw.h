/* Lane-local <ftw.h> for the HobbyOS IceWM build.  nftw() (../compat/hb_ftw.c)
 * walks with opendir/readdir; used by IceWM's theme installer only. */
#ifndef HB_ICEWM_FTW_H
#define HB_ICEWM_FTW_H
#include <sys/stat.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FTW_F   0
#define FTW_D   1
#define FTW_DNR 2
#define FTW_NS  3
#define FTW_SL  4
#define FTW_DP  5
#define FTW_SLN 6
#define FTW_PHYS  0x01
#define FTW_MOUNT 0x02
#define FTW_CHDIR 0x04
#define FTW_DEPTH 0x08
struct FTW { int base; int level; };
int nftw(const char *path, int (*fn)(const char *, const struct stat *, int, struct FTW *),
         int nfds, int flags);
#ifdef __cplusplus
}
#endif
#endif
