/*
 * HobbyOS sysroot: SELinux stub implementations.
 *
 * See <selinux/selinux.h>: GNU sed 4.8 calls these unconditionally, so
 * they must link.  Every entry point reports "SELinux is off", and the
 * context helpers fail with ENOSYS — exactly the behavior a build with a
 * disabled libselinux observes.  No port program reaches the helpers at
 * runtime because is_selinux_enabled() returns 0.
 */
#include "errno.h"
#include "selinux/selinux.h"

int is_selinux_enabled(void) {
  return 0;
}

int lgetfilecon(const char *path, security_context_t *con) {
  (void)path;
  if (con)
    *con = 0;
  errno = ENOSYS;
  return -1;
}

int getfscreatecon(security_context_t *con) {
  if (con)
    *con = 0;
  errno = ENOSYS;
  return -1;
}

int setfscreatecon(security_context_t con) {
  (void)con;
  errno = ENOSYS;
  return -1;
}

void freecon(security_context_t con) {
  (void)con;
}
