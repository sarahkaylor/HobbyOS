/*
 * HobbyOS sysroot: <selinux/selinux.h> — stub SELinux API.
 *
 * GNU sed 4.8 calls is_selinux_enabled()/lgetfilecon()/... unconditionally
 * (no HAVE_SELINUX guard in upstream execute.c), so the header must exist
 * for the port to compile.  The stubs always report SELinux as disabled,
 * which makes every caller take the no-SELinux path; the context helpers
 * exist only so the program links.
 */
#ifndef HOBBYOS_SELINUX_SELINUX_H
#define HOBBYOS_SELINUX_SELINUX_H 1

#ifdef __cplusplus
extern "C" {
#endif

  typedef char *security_context_t;

  int is_selinux_enabled(void);                                   /* 0 */
  int lgetfilecon(const char *path, security_context_t *con);     /* -1 */
  int getfscreatecon(security_context_t *con);                    /* -1 */
  int setfscreatecon(security_context_t con);                     /* -1 */
  void freecon(security_context_t con);                           /* no-op */

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SELINUX_SELINUX_H */
