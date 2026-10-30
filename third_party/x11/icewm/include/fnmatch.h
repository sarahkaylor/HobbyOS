/* Lane-local <fnmatch.h> for the HobbyOS IceWM build (third_party/x11/icewm).
 * The repo libc has no fnmatch(); implementation in ../compat/hb_fnmatch.c. */
#ifndef HB_ICEWM_FNMATCH_H
#define HB_ICEWM_FNMATCH_H
#ifdef __cplusplus
extern "C" {
#endif
#define FNM_NOMATCH     1
#define FNM_NOESCAPE    0x02
#define FNM_PATHNAME    0x01
#define FNM_PERIOD      0x04
#define FNM_LEADING_DIR 0x08
#define FNM_CASEFOLD    0x10
#define FNM_FILE_NAME   FNM_PATHNAME
int fnmatch(const char *pattern, const char *string, int flags);
#ifdef __cplusplus
}
#endif
#endif
