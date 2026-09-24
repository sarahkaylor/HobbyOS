/*
 * HEDTEST.BIN — in-OS acceptance for the ported GNU head (HEDGNU.BIN).
 *
 * The exhaustive byte-exact matrix (131 cases vs genuine GNU textutils-2.1
 * head) lives in the HOST parity harness (src/host/head_parity.sh +
 * build_tu21_head_ref.sh).  This in-OS test proves the device path: each
 * case spawns HEDGNU.BIN through the real spawn2/pipe machinery and
 * compares captured stdout byte-for-byte against the GNU textutils layout
 * for the representative option paths: -n, -c, old -Nc syntax, multi-file
 * headers, and stdin-pipe EOF (regression for the zero-length pipe_write
 * kernel bug that used to block empty writes forever).
 *
 * Kept to a handful of spawns (like WCTEST) so it coexists with the rest
 * of the boot suite's process-table pressure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "libc.h" /* pipe(), spawn2(), print_console() */

static int failures = 0;

static void con_int(long v) {
  char b[20];
  int i = 19, neg = 0;
  if (v < 0) { neg = 1; v = -v; }
  b[i--] = 0;
  if (v == 0) b[i--] = '0';
  while (v > 0) { b[i--] = (char)('0' + v % 10); v /= 10; }
  if (neg) b[i--] = '-';
  print_console(&b[i + 1]);
}

static void check(const char *what, const char *got, const char *want) {
  if (got && strcmp(got, want) == 0) {
    print_console("[HEDTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[HEDTEST] FAIL ");
    print_console(what);
    print_console("\n  got:  [");
    print_console(got ? got : "(null)");
    print_console("]\n  want: [");
    print_console(want);
    print_console("]\n");
    failures++;
  }
}

/* Run "head <args>" with the given stdin content; capture stdout. */
static void run_head_once(const char *args, const char *stdin_data, char *out,
                          size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  /* let any previous HEDGNU child exit and free its process slot */
  usleep(50000);

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[HEDTEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("HEDGNU.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    int tries = 0;
    while (pid <= 0 && tries < 200) {
      usleep(50000); /* 50 ms; up to ~10 s of patience */
      pid = spawn2("HEDGNU.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[HEDTEST] spawn HEDGNU.BIN failed\n");
    failures++;
    return;
  }
  close(in_p[0]);
  /* feed stdin, then let the child see EOF.  Retry the write briefly:
     under full-suite process-table pressure a spawn can race the pipe
     reader accounting, and an EAGAIN/PIPE error here must not turn
     into an empty-output failure. */
  {
    size_t off = 0, len = strlen(stdin_data);
    int tries = 0;
    while (off < len && tries < 50) {
      ssize_t w = write(in_p[1], stdin_data + off, len - off);
      if (w > 0) { off += (size_t)w; tries = 0; continue; }
      if (w == 0) break;
      usleep(2000); /* 2 ms; wait for the reader to attach */
      tries++;
    }
  }
  close(in_p[1]);

  close(out_p[1]);
  while (n + 1 < outsize) {
    int r = read(out_p[0], out + n, outsize - n - 1);
    if (r <= 0)
      break;
    n += (size_t)r;
  }
  close(out_p[0]);
  out[n] = '\0';
  /* Reap the child so its process slot is recycled for the next spawn
     (the child has exited by now: EOF on the stdout pipe).  Without
     this, every spawned child stays EXITED and piles up until the
     parent exits, starving later spawns of process-table slots. */
  if (pid > 0) {
    int ws = 0;
    waitpid(pid, &ws, 0);
  }
}

/* Empty stdout despite a live child is the same scheduler-pressure flake
   the spawn/write retries above absorb; retry the whole capture. */
static void run_head(const char *args, const char *stdin_data, char *out,
                     size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_head_once(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[HEDTEST] cannot create ");
    print_console(path);
    print_console("\n");
    failures++;
    return;
  }
  write(fd, content, strlen(content));
  close(fd);
}

int main(void) {
  char out[2048];

  print_console("[HEDTEST] starting\n");

  /* 12 lines, line 3 "gamma" is 5 bytes: exercises -n boundaries */
  write_file("/HEDTEST.TXT",
             "alpha\n"
             "beta\n"
             "gamma\n"
             "delta\n"
             "epsilon\n"
             "zeta\n"
             "eta\n"
             "theta\n"
             "iota\n"
             "kappa\n"
             "lambda\n"
             "mu\n");
  /* second file so multi-file header banners trigger */
  write_file("/HEDTEST2.TXT", "one\ntwo\nthree\n");

  /* -n 3 */
  run_head("-n 3 /HEDTEST.TXT", "", out, sizeof out);
  check("-n 3", out, "alpha\nbeta\ngamma\n");

  /* -c 6: six bytes (a,l,p,h,a,\n) */
  run_head("-c 6 /HEDTEST.TXT", "", out, sizeof out);
  check("-c 6", out, "alpha\n");

  /* old syntax -2c = first 2 bytes */
  run_head("-2c /HEDTEST.TXT", "", out, sizeof out);
  check("old -2c", out, "al");

  /* multi-file: header banners + default 10 lines each */
  run_head("/HEDTEST.TXT /HEDTEST2.TXT", "", out, sizeof out);
  check("multi-file headers", out,
        "==> /HEDTEST.TXT <==\n"
        "alpha\nbeta\ngamma\ndelta\nepsilon\nzeta\neta\ntheta\niota\nkappa\n"
        "\n"
        "==> /HEDTEST2.TXT <==\n"
        "one\ntwo\nthree\n");

  /* stdin-pipe case: content delivered through the spawn2 pipe, head
     must read it fully and see EOF (regression for pipe_write n==0) */
  run_head("-", "one\ntwo\nthree\n", out, sizeof out);
  check("stdin '-' with pipe data", out, "one\ntwo\nthree\n");

  if (failures == 0) {
    print_console("[HEDTEST] ALL PASSED (5 checks)\n");
    exit(0);
  }
  print_console("[HEDTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
