/* Small POSIX/BSD gap fillers for the HobbyOS IceWM build. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int fork(void);

/* No timezone database (local time == UTC). */
void tzset(void) {}

/* No load average source on HobbyOS. */
int getloadavg(double loadavg[], int nelem) {
  (void)loadavg; (void)nelem;
  errno = ENOSYS;
  return -1;
}

char *getlogin(void) {
  char *u = getenv("USER");
  return u && *u ? u : (char *)"user";
}

/* Single priority level: accepted, no effect. */
int getpriority(int which, int who) { (void)which; (void)who; errno = 0; return 0; }
int setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }
int nice(int inc) { (void)inc; errno = 0; return 0; }

void *memmem(const void *hay, size_t hl, const void *needle, size_t nl) {
  const unsigned char *h = (const unsigned char *)hay, *n = (const unsigned char *)needle;
  if (nl == 0) return (void *)hay;
  if (hl < nl) return NULL;
  for (size_t i = 0; i + nl <= hl; i++)
    if (h[i] == n[0] && memcmp(h + i, n, nl) == 0) return (void *)(h + i);
  return NULL;
}

long fpathconf(int fd, int name) {
  (void)fd;
  switch (name) {
  case 3: return 255;   /* _PC_NAME_MAX */
  case 4: return 4096;  /* _PC_PATH_MAX */
  case 5: return 4096;  /* _PC_PIPE_BUF */
  default: errno = EINVAL; return -1;
  }
}
long pathconf(const char *path, int name) { (void)path; return fpathconf(-1, name); }

/* BSD daemon(): background via fork; the parent exits. */
int daemon(int nochdir, int noclose) {
  (void)noclose;
  int pid = fork();
  if (pid < 0) return -1;
  if (pid > 0) _exit(0);
  setsid();
  if (!nochdir) chdir("/");
  return 0;
}
