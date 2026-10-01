/*
 * HobbyOS P6.1 sysroot: pwd.c - passwd/group database for a single-user
 * system (see the sysroot pwd.h/grp.h contracts).
 *
 * No files, no NSS: getpwuid(0)/getpwnam("user") and getgrgid(0)/
 * getgrnam("root") return one synthetic entry; every other id/name is
 * NULL/ENOENT, exactly glibc's contract for an unknown account.  The _r
 * variants copy the name into the caller's buffer (ERANGE when it does
 * not fit) and point the struct's name at it, as POSIX requires.
 */
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>

#ifndef HOST_TEST

#define HB_USER_NAME  "user"
#define HB_GROUP_NAME "root"

static int hb_copy_name(char *dst, size_t dstlen, const char *src) {
  size_t n = strlen(src) + 1;

  if (n > dstlen) return -1;
  memcpy(dst, src, n);
  return 0;
}

/* ---- passwd ----------------------------------------------------------- */

struct passwd *getpwuid(uid_t uid) {
  static struct passwd pw = {
    (char *)HB_USER_NAME, (char *)"x", 0, 0, (char *)"HobbyOS user",
    (char *)"/", (char *)"/bin/sh"
  };

  if (uid != 0) {
    errno = ENOENT;
    return 0;
  }
  return &pw;
}

struct passwd *getpwnam(const char *name) {
  if (!name || strcmp(name, HB_USER_NAME) != 0) {
    errno = ENOENT;
    return 0;
  }
  return getpwuid(0);
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
  if (!pwd || !result) return EINVAL;
  *result = 0; /* POSIX: not-found is rc 0 with *result == NULL */
  if (uid != 0) return 0;
  if (hb_copy_name(buf, buflen, HB_USER_NAME) != 0) return ERANGE;
  pwd->pw_name = buf;
  pwd->pw_passwd = (char *)"x";
  pwd->pw_uid = 0;
  pwd->pw_gid = 0;
  pwd->pw_gecos = (char *)"HobbyOS user";
  pwd->pw_dir = (char *)"/";
  pwd->pw_shell = (char *)"/bin/sh";
  *result = pwd;
  return 0;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf,
               size_t buflen, struct passwd **result) {
  if (!name || strcmp(name, HB_USER_NAME) != 0) {
    if (result) *result = 0;
    return 0;
  }
  return getpwuid_r(0, pwd, buf, buflen, result);
}

void endpwent(void) {}
void setpwent(void) {}
struct passwd *getpwent(void) { return 0; }

/* ---- group ------------------------------------------------------------ */

struct group *getgrgid(gid_t gid) {
  static char *members[1] = { 0 };
  static struct group gr = {
    (char *)HB_GROUP_NAME, (char *)"x", 0, members
  };

  if (gid != 0) {
    errno = ENOENT;
    return 0;
  }
  return &gr;
}

struct group *getgrnam(const char *name) {
  if (!name || strcmp(name, HB_GROUP_NAME) != 0) {
    errno = ENOENT;
    return 0;
  }
  return getgrgid(0);
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result) {
  if (!grp || !result) return EINVAL;
  *result = 0;
  if (gid != 0) return 0;
  if (hb_copy_name(buf, buflen, HB_GROUP_NAME) != 0) return ERANGE;
  grp->gr_name = buf;
  grp->gr_passwd = (char *)"x";
  grp->gr_gid = 0;
  grp->gr_mem = 0;
  *result = grp;
  return 0;
}

int getgrnam_r(const char *name, struct group *grp, char *buf,
               size_t buflen, struct group **result) {
  if (!name || strcmp(name, HB_GROUP_NAME) != 0) {
    if (result) *result = 0;
    return 0;
  }
  return getgrgid_r(0, grp, buf, buflen, result);
}

void endgrent(void) {}
void setgrent(void) {}
struct group *getgrent(void) { return 0; }

#endif /* !HOST_TEST */
