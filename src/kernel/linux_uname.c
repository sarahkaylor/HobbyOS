/* src/kernel/linux_uname.c — GB-016 Linux-ish uname identity.
 *
 * SDL and the games sniff uname (sysname=="Linux", machine per arch) to pick
 * paths + codepaths, so a compat process's uname() must not present a foreign
 * identity.  See linux_uname.h.  Pure function, no syscall wiring here (that
 * rides GB-014's dispatch); host-buildable and T2-testable.
 */
#include "linux_uname.h"

#ifdef __x86_64__
#define HB_UNAME_MACHINE "x86_64"
#else
#define HB_UNAME_MACHINE "aarch64"
#endif

/* Byte-wise copy (kernel is freestanding; no libc strcpy). */
static void kcopy(char *dst, const char *src, int cap) {
  int i = 0;
  for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
  for (; i < cap; i++) dst[i] = '\0';
}

int linux_fill_utsname(linux_utsname_t *u) {
  if (!u) return -1;
  kcopy(u->sysname,    "Linux",      sizeof u->sysname);
  kcopy(u->nodename,   "hobbyos",    sizeof u->nodename);
  kcopy(u->release,    "6.1-hobbyos", sizeof u->release);
  kcopy(u->version,    "#1 HobbyOS", sizeof u->version);
  kcopy(u->machine,    HB_UNAME_MACHINE, sizeof u->machine);
  kcopy(u->domainname, "(none)",     sizeof u->domainname);
  return 0;
}
