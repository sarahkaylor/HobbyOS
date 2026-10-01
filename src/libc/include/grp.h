#ifndef HOBBYOS_GRP_H
#define HOBBYOS_GRP_H

/* HobbyOS P6.1 sysroot: grp.h — group database.
 *
 * Single-user counterpart of pwd.h: one synthetic group (gid 0, name
 * "root"), NULL/ENOENT for unknown ids.  HOST_TEST builds defer to the
 * host's header. */

#include <sys/types.h> /* gid_t */

#ifdef HOST_TEST
#include_next <grp.h>
#else

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

  struct group {
    char *gr_name;   /* "root" */
    char *gr_passwd; /* "x"    */
    gid_t gr_gid;    /* 0      */
    char **gr_mem;   /* NULL-terminated member list (empty) */
  };

  struct group *getgrgid(gid_t gid);
  struct group *getgrnam(const char *name);
  int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
                 struct group **result);
  int getgrnam_r(const char *name, struct group *grp, char *buf,
                 size_t buflen, struct group **result);
  void endgrent(void);
  void setgrent(void);
  struct group *getgrent(void);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_GRP_H */
