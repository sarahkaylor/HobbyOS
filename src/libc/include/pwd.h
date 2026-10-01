#ifndef HOBBYOS_PWD_H
#define HOBBYOS_PWD_H

/* HobbyOS P6.1 sysroot: pwd.h — passwd database.
 *
 * HobbyOS is single-user: getpwuid()/getpwnam() return one synthetic entry
 * (uid 0, gid 0, name "user", home "/") and NULL/ENOENT for anything else,
 * mirroring glibc's contract for unknown ids.  HOST_TEST builds defer to
 * the host's header. */

#include <sys/types.h> /* uid_t, gid_t */

#ifdef HOST_TEST
#include_next <pwd.h>
#else

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

  struct passwd {
    char *pw_name;   /* "user"            */
    char *pw_passwd; /* "x" (no shadow)   */
    uid_t pw_uid;    /* 0                 */
    gid_t pw_gid;    /* 0                 */
    char *pw_gecos;  /* full name         */
    char *pw_dir;    /* "/"               */
    char *pw_shell;  /* "/bin/sh"         */
  };

  struct passwd *getpwuid(uid_t uid);
  struct passwd *getpwnam(const char *name);
  int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
                 struct passwd **result);
  int getpwnam_r(const char *name, struct passwd *pwd, char *buf,
                 size_t buflen, struct passwd **result);
  void endpwent(void);
  void setpwent(void);
  struct passwd *getpwent(void);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_PWD_H */
