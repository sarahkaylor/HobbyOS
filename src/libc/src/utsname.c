/*
 * HobbyOS libc: uname() / gethostname() (icewm.md M2, lane xlibs-core).
 *
 * The native track has no uname syscall (the Linux-compat identity lives in
 * the kernel's linux_uname.c for compat processes only).  Native programs
 * get a fixed identity: sysname "HobbyOS", nodename = $HOSTNAME when set
 * (else "hobbyos"), machine "aarch64" / "x86_64".
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef HOST_TEST
#define HB_NAME(name) hbl_##name
#else
#define HB_NAME(name) name
#endif

static const char *hb_nodename(void) {
  const char *h = getenv("HOSTNAME");
  return (h && *h) ? h : "hobbyos";
}

static void put_n(char *dst, size_t cap, const char *src) {
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}
#define put(field, src) put_n((field), sizeof(field), (src))

int HB_NAME(uname)(struct utsname *u) {
  if (!u) { errno = EFAULT; return -1; }
  memset(u, 0, sizeof *u);
  put(u->sysname, "HobbyOS");
  put(u->nodename, hb_nodename());
  put(u->release, "1.0");
  put(u->version, "HobbyOS native");
#if defined(__aarch64__)
  put(u->machine, "aarch64");
#elif defined(__x86_64__)
  put(u->machine, "x86_64");
#else
  put(u->machine, "unknown");
#endif
  return 0;
}

int HB_NAME(gethostname)(char *name, size_t len) {
  const char *h = hb_nodename();
  size_t n = strlen(h);
  if (!name) { errno = EFAULT; return -1; }
  if (n + 1 > len) {
    if (len) { memcpy(name, h, len); }
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(name, h, n + 1);
  return 0;
}
