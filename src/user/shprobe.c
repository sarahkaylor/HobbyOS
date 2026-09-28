/* SHPROBE.BIN — on-OS concurrency stress for the shell protocol.
 * Spawns shells and drives each through help + cd /home, dumping the
 * exact buffer as shell_test's read_until would see it.  Confirms
 * whether the cd-prompt failure reproduces outside the SHTEST harness
 * and whether pipe writer accounting survives concurrency.
 * TEMPORARY diagnostic — remove when the shell protocol tests are green.
 */
#include "libc.h"
#include <fcntl.h>

extern int main(int argc, char **argv);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  main(0, 0);
  exit(0);
}
#endif

static void say(const char *s) { print_console(s); }

static void saydec(long v) {
  char t[12]; int i = 0; long x = v;
  char rev[10]; int j = 0;
  if (x == 0) rev[j++] = '0';
  while (x > 0 && j < 9) { rev[j++] = '0' + (x % 10); x /= 10; }
  while (j > 0) t[i++] = rev[--j];
  t[i] = 0;
  say(t);
}

/* read up to the pattern like shell_test's read_until */
static int rd_until(int fd, char *buf, int max_len, const char *pattern) {
  int len = 0;
  while (len < max_len - 1) {
    char c;
    int r = read(fd, &c, 1);
    if (r <= 0) break;
    buf[len++] = c;
    buf[len] = '\0';
    int pl = 0;
    while (pattern[pl]) pl++;
    if (len >= pl) {
      int m = 1;
      for (int i = 0; i < pl; i++)
        if (buf[len - pl + i] != pattern[i]) { m = 0; break; }
      if (m) return len;
    }
  }
  return len;
}

static void dump(const char *tag, char *buf, int len) {
  say(tag);
  say(" len=");
  saydec(len);
  say(" >>> ");
  print_console(buf);
  say(" <<<\n");
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  char buf[1024];
  int n;
  say("[SHPROBE] concurrency protocol stress\n");

  /* spawn three shells at once so the whole boot burst is simulated */
#define NSH 3
  int in[NSH][2], out[NSH][2];
  int pids[NSH];
  for (int s = 0; s < NSH; s++) {
    if (pipe(in[s]) != 0 || pipe(out[s]) != 0) { say("[SHPROBE] pipe FAILED\n"); return 1; }
    pids[s] = spawn2("SH.BIN", in[s][0], out[s][1], -1, 0);
    say("[SHPROBE]   spawned shell ");
    saydec(s);
    say(" pid=");
    saydec(pids[s]);
    say("\n");
  }

  for (int s = 0; s < NSH; s++) {
    n = rd_until(out[s][0], buf, sizeof buf, "$ ");
    dump("[SHPROBE] 1 init:", buf, n);
  }
  for (int s = 0; s < NSH; s++) {
    write(in[s][1], "help\n", 5);
  }
  for (int s = 0; s < NSH; s++) {
    n = rd_until(out[s][0], buf, sizeof buf, "$ ");
    dump("[SHPROBE] 1 help:", buf, n);
  }
  for (int s = 0; s < NSH; s++) {
    write(in[s][1], "cd /home\n", 9);
  }
  for (int s = 0; s < NSH; s++) {
    n = rd_until(out[s][0], buf, sizeof buf, "$ ");
    dump("[SHPROBE] 1 cd  :", buf, n);
  }

  /* second round with fresh pipe pairs, still all concurrent */
  for (int s = 0; s < NSH; s++) {
    int wst = 0;
    for (int j = 0; j < 2000; j++)
      if (waitpid(pids[s], &wst, 0) == pids[s]) break;
    close(in[s][0]); close(in[s][1]); close(out[s][0]); close(out[s][1]);
  }
  for (int s = 0; s < NSH; s++) {
    if (pipe(in[s]) != 0 || pipe(out[s]) != 0) return 1;
    pids[s] = spawn2("SH.BIN", in[s][0], out[s][1], -1, 0);
  }
  for (int s = 0; s < NSH; s++) {
    n = rd_until(out[s][0], buf, sizeof buf, "$ ");
    dump("[SHPROBE] 2 init:", buf, n);
    write(in[s][1], "help\ncd /home\n", 14);
    n = rd_until(out[s][0], buf, sizeof buf, "$ ");
    dump("[SHPROBE] 2 help:", buf, n);
    write(in[s][1], "cd /home\n", 9);
    n = rd_until(out[s][0], buf, sizeof buf, "$ ");
    dump("[SHPROBE] 2 cd  :", buf, n);
    close(in[s][1]);
    int wst = 0;
    for (int j = 0; j < 2000; j++)
      if (waitpid(pids[s], &wst, 0) == pids[s]) break;
  }

  say("[SHPROBE] done\n");
  return 0;
}
