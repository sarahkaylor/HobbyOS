/* popen()/pclose()/mkdtemp() for the HobbyOS IceWM build.  popen runs
 * "/SH.BIN -c <command>" in a forked child over a pipe(); pclose waits. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int fork(void);

#define HB_POPEN_MAX 8
static struct { FILE *fp; int pid; } g_po[HB_POPEN_MAX];

FILE *popen(const char *cmd, const char *mode) {
  int fds[2];
  int rd = mode && mode[0] == 'r';
  if (!mode || (mode[0] != 'r' && mode[0] != 'w')) { errno = EINVAL; return NULL; }
  int slot = -1;
  for (int i = 0; i < HB_POPEN_MAX; i++) if (!g_po[i].fp) { slot = i; break; }
  if (slot < 0) { errno = EMFILE; return NULL; }
  if (pipe(fds) != 0) return NULL;
  int pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
  if (pid == 0) {
    if (rd) { dup2(fds[1], 1); } else { dup2(fds[0], 0); }
    close(fds[0]); close(fds[1]);
    char *argv[] = { (char *)"SH.BIN", (char *)"-c", (char *)cmd, NULL };
    execv("/SH.BIN", argv);
    _exit(127);
  }
  FILE *fp;
  if (rd) { close(fds[1]); fp = fdopen(fds[0], "r"); }
  else    { close(fds[0]); fp = fdopen(fds[1], "w"); }
  if (!fp) return NULL;
  g_po[slot].fp = fp;
  g_po[slot].pid = pid;
  return fp;
}

int pclose(FILE *fp) {
  for (int i = 0; i < HB_POPEN_MAX; i++) {
    if (g_po[i].fp == fp) {
      int pid = g_po[i].pid, st = 0;
      g_po[i].fp = NULL;
      fclose(fp);
      while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
      return st;
    }
  }
  errno = ECHILD;
  return -1;
}

char *mkdtemp(char *tmpl) {
  size_t n = tmpl ? strlen(tmpl) : 0;
  if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) { errno = EINVAL; return NULL; }
  static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  static unsigned seed;
  seed ^= (unsigned)getpid() * 2654435761u;
  for (int tries = 0; tries < 100; tries++) {
    for (int i = 0; i < 6; i++) {
      seed = seed * 1103515245u + 12345u;
      tmpl[n - 6 + i] = cs[(seed >> 16) % 36];
    }
    if (mkdir(tmpl, 0700) == 0) return tmpl;
    if (errno != EEXIST) return NULL;
  }
  return NULL;
}

/* system(): declared by the repo <stdlib.h> but not implemented there. */
int system(const char *cmd) {
  if (!cmd) return 1; /* a shell is available */
  int pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    char *argv[] = { (char *)"SH.BIN", (char *)"-c", (char *)cmd, NULL };
    execv("/SH.BIN", argv);
    _exit(127);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
  return st;
}
