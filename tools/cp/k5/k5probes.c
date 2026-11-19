/* tools/cp/k5/k5probes.c - C1-k5 native launcher for C8-game's P8 kernel
 * probes (docs/cp-k5.md §8; probes by lane C8-game, src/user/cp/p8*.c).
 *
 * The only wave member of the boot tools/cp/k5/run-k5probes.sh stages.  No
 * NFS: the probe ELFs sit on the FAT disk.  It runs, in order:
 *
 *   /P8ENV.ELF               E0 (what a native spawner hands a compat
 *                            process), then the probe execve()s itself with
 *                            40 entries / 1560 bytes: E1/E2
 *   /P8ENV.ELF spawned40     spawn_ex with the same 40 entries: E3/E4 (the
 *                            native -> compat path that dropped every byte
 *                            past 512 and every entry past 24 on base5)
 *   /P8FD.ELF 600            fd headroom with nothing else running
 *   /P8SCM.ELF               SCM_RIGHTS into a full table: S1-S3
 *   /P8KILL.ELF              SIGKILL to a group whose leader exited: K1/K2
 *
 * Every step is bracketed by "[K5P] begin <tag>" / "[K5P] end <tag> status=";
 * a step that outlives its deadline is SIGKILLed and polled for 10 s (K1's
 * own failure mode must not wedge the launcher).
 */
#include <fcntl.h>
#include <errno.h>
#include "libc.h"

static void pc(const char *s) { print_console(s); }
static void pd(long v) {
  char b[24]; int n = 0; unsigned long u;
  if (v < 0) { b[n++] = '-'; u = (unsigned long)(-(v + 1)) + 1UL; } else u = (unsigned long)v;
  char t[24]; int m = 0;
  if (u == 0) t[m++] = '0';
  while (u) { t[m++] = (char)('0' + u % 10); u /= 10; }
  while (m) b[n++] = t[--m];
  b[n] = 0; pc(b);
}
static uint64_t now_ms(void) { return (uint64_t)sysinfo(1, 0, 0); }

static char *envp0[] = { (char *)"HOME=/", (char *)"LANG=C", 0 };

static int run(const char *tag, const char *path, char *const argv[], char *const env[], int secs) {
  pc("[K5P] begin "); pc(tag); pc("\n");
  uint64_t t0 = now_ms();
  int pid = spawn_ex(path, argv, env, 0, 0);
  if (pid < 0) {
    pc("[K5P] end "); pc(tag); pc(" status=spawn-failed rc="); pd(pid); pc(" errno="); pd(errno); pc("\n");
    return -1;
  }
  int status = -1, wp = 0;
  for (int s = 0; s < secs * 4; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    usleep(250000);
  }
  if (wp == 0) {
    pc("[K5P] timeout "); pc(tag); pc(": SIGKILL pid="); pd(pid); pc("\n");
    kill(pid, 9);
    for (int s = 0; s < 40 && wp == 0; s++) { wp = waitpid(pid, &status, 1); usleep(250000); }
    pc("[K5P] end "); pc(tag); pc(" status=timeout reaped="); pd(wp != 0); pc("\n");
    return -2;
  }
  pc("[K5P] end "); pc(tag); pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
  return status;
}

int main(void) {
  pc("[K5P] k5probes launcher (C1-k5; probes by C8-game)\n");
  char *a_env[] = { (char *)"/P8ENV.ELF", 0 };
  run("envprobe", "/P8ENV.ELF", a_env, envp0, 60);

  /* C8-game's p8launch builds the same 40 entries: "Exx=" + 34 digits */
  static char st[40][48];
  static char *big[41];
  for (int i = 0; i < 40; i++) {
    st[i][0] = 'E'; st[i][1] = (char)('0' + i / 10); st[i][2] = (char)('0' + i % 10); st[i][3] = '=';
    for (int k = 0; k < 34; k++) st[i][4 + k] = (char)(k < 32 ? '0' : '0' + (k == 32 ? i / 10 : i % 10));
    st[i][38] = 0;
    big[i] = st[i];
  }
  big[40] = 0;
  char *a_env40[] = { (char *)"/P8ENV.ELF", (char *)"spawned40", 0 };
  run("envprobe40", "/P8ENV.ELF", a_env40, big, 60);

  /* E2BIG: an env that does not fit the 2048-byte blob is refused, not cut */
  static char huge[80][48];
  static char *hugep[81];
  for (int i = 0; i < 80; i++) {
    for (int k = 0; k < 38; k++) huge[i][k] = (char)('A' + (i + k) % 26);
    huge[i][1] = '='; huge[i][38] = 0;
    hugep[i] = huge[i];
  }
  hugep[80] = 0;
  pc("[K5P] begin e2big : spawn_ex with 80 entries / 3120 bytes\n");
  {
    int pid = spawn_ex("/P8ENV.ELF", a_env40, hugep, 0, 0);
    int ok = pid < 0 && errno == 7; /* E2BIG */
    if (pid >= 0) { int s; waitpid(pid, &s, 0); }
    pc("[K5P] E5 spawn_ex env > HO_ENV_LEN: rc="); pd(pid); pc(" errno="); pd(errno);
    pc(ok ? " -> PASS\n" : " -> FAIL\n");
  }

  char *a_fd[] = { (char *)"/P8FD.ELF", (char *)"600", 0 };
  run("fdcount", "/P8FD.ELF", a_fd, envp0, 60);
  char *a_scm[] = { (char *)"/P8SCM.ELF", 0 };
  run("scmprobe", "/P8SCM.ELF", a_scm, envp0, 60);
  char *a_kill[] = { (char *)"/P8KILL.ELF", 0 };
  run("killprobe", "/P8KILL.ELF", a_kill, envp0, 60);
  pc("[K5P] done\n");
  return 0;
}
