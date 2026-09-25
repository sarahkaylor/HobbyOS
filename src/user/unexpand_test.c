/*
 * UNEXPTEST.BIN — in-OS end-to-end acceptance for the ported GNU unexpand.
 *
 * Spawns UNEXPAND.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> open/read -> space/tab column arithmetic ->
 * our printf -> write) and compares the captured stdout byte-for-byte
 * against GNU unexpand 2.1 behavior: the leading-blanks-only default
 * (including the 7/8/9-space tab-stop corners and a lone blank), -a, -t
 * with a single size and with an explicit tab-stop list, long options,
 * stdin mode, and the exit codes for the error paths (bad tab lists,
 * missing file).
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by unexpand
 * itself on the error paths (stderr) go to the console only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include "libc.h" /* pipe(), spawn2(), print_console() */

static int failures = 0;

/* tiny signed-int console printer (no libc/sysroot dependency) */
static void con_int(int v) {
  char b[16];
  int i = 15, neg = 0;
  if (v < 0) {
    neg = 1;
    v = (v == -2147483648) ? 2147483647 : -v;
  }
  b[i--] = 0;
  if (v == 0)
    b[i--] = '0';
  while (v > 0) {
    b[i--] = (char)('0' + v % 10);
    v /= 10;
  }
  if (neg)
    b[i--] = '-';
  print_console(&b[i + 1]);
}

static void check(const char *what, const char *got, const char *want) {
  if (strcmp(got, want) == 0) {
    print_console("[UNEXPTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[UNEXPTEST] FAIL ");
    print_console(what);
    print_console("\n  got:  [");
    print_console(got);
    print_console("]\n  want: [");
    print_console(want);
    print_console("]\n");
    failures++;
  }
}

static void check_rc(const char *what, int got, int want) {
  if (got == want) {
    print_console("[UNEXPTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[UNEXPTEST] FAIL ");
    print_console(what);
    print_console(" (rc ");
    con_int(got);
    print_console(" != ");
    con_int(want);
    print_console(")\n");
    failures++;
  }
}

static int last_status; /* raw waitpid status of the last run */

/* Run "unexpand <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_unexpand_attempt(const char *args, const char *stdin_data, char *out,
                                 size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[UNEXPTEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("UNEXPAND.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("UNEXPAND.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[UNEXPTEST] spawn UNEXPAND.BIN failed\n");
    failures++;
    return;
  }
  /* feed stdin, then let the child see EOF.  Retry the write briefly:
     under full-suite process-table pressure a spawn can race the pipe
     reader accounting, and an EAGAIN/PIPE error here must not turn
     into an empty-output failure. */
  close(in_p[0]);
  {
    size_t off = 0, len = strlen(stdin_data);
    int tries = 0;
    while (off < len && tries < 50) {
      ssize_t w = write(in_p[1], stdin_data + off, len - off);
      if (w > 0) {
        off += (size_t)w;
        tries = 0;
        continue;
      }
      if (w == 0)
        break;
      usleep(2000); /* 2 ms; wait for the reader to attach */
      tries++;
    }
  }
  close(in_p[1]);

  close(out_p[1]);
  while (n + 1 < outsize) {
    int r = read(out_p[0], out + n, outsize - n - 1);
    if (r <= 0)
      break; /* EOF once the child exits and its pipe fd closes */
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
    last_status = ws;
  }
}

/* Empty stdout despite a successful spawn is a scheduler-pressure flake:
   while the fork/stress tests hammer process creation in parallel, the
   child can be scheduled so late that its stdout pipe has already reported
   EOF.  Retry the whole interaction a few times for the cases that expect
   non-empty output; error-path cases (want_rc != 0) accept empty output
   and only need the exit status. */
static void run_unexpand(const char *args, const char *stdin_data, char *out,
                         size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_unexpand_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[UNEXPTEST] cannot create ");
    print_console(path);
    print_console("\n");
    failures++;
    return;
  }
  write(fd, content, strlen(content));
  close(fd);
}

int main(void) {
  char out[512];

  print_console("[UNEXPTEST] starting\n");

  /* leading runs of 8, 7, 9, 1, 2 and 3 blanks; two interior runs */
  write_file("/UNEXPD.TXT", "        x\n");
  write_file("/UNEXP7.TXT", "       x\n");
  write_file("/UNEXP9.TXT", "         x\n");
  write_file("/UNEXP1.TXT", " x\n");
  write_file("/UNEXP2.TXT", "  x\n");
  write_file("/UNEXP3.TXT", "   x\n");
  write_file("/UNEXPMID.TXT", "a       b\nab      c\n");

  /* default: only leading blanks, and only whole tab stops' worth */
  run_unexpand("/UNEXPD.TXT", "", out, sizeof out);
  check("default 8 blanks -> tab", out, "\tx\n");

  run_unexpand("/UNEXP7.TXT", "", out, sizeof out);
  check("default 7 blanks kept", out, "       x\n");

  run_unexpand("/UNEXP9.TXT", "", out, sizeof out);
  check("default 9 blanks -> tab+space", out, "\t x\n");

  run_unexpand("/UNEXP1.TXT", "", out, sizeof out);
  check("default lone blank kept", out, " x\n");

  run_unexpand("/UNEXPMID.TXT", "", out, sizeof out);
  check("default interior kept", out, "a       b\nab      c\n");

  /* -a: blanks anywhere on the line */
  run_unexpand("-a /UNEXPMID.TXT", "", out, sizeof out);
  check("-a interior runs -> tabs", out, "a\tb\nab\tc\n");

  run_unexpand("--all /UNEXPMID.TXT", "", out, sizeof out);
  check("--all long option", out, "a\tb\nab\tc\n");

  /* -t: single tab size, then an explicit tab-stop list */
  run_unexpand("-t 4 /UNEXPD.TXT", "", out, sizeof out);
  check("-t 4: 8 blanks -> 2 tabs", out, "\t\tx\n");

  run_unexpand("-t 4 /UNEXP2.TXT", "", out, sizeof out);
  check("-t 4: 2 blanks kept", out, "  x\n");

  run_unexpand("-t 2,4 /UNEXP3.TXT", "", out, sizeof out);
  check("-t 2,4: 3 blanks -> tab+space", out, "\t x\n");

  run_unexpand("-a -t 4 /UNEXPMID.TXT", "", out, sizeof out);
  check("-a -t 4 interior runs", out, "a\t\tb\nab\t\tc\n");

  /* stdin mode (no file argument) */
  run_unexpand("", "        y\n", out, sizeof out);
  check("stdin default 8 blanks", out, "\ty\n");

  run_unexpand("-a", "a       b\n", out, sizeof out);
  check("stdin -a interior run", out, "a\tb\n");

  /* error paths: exit codes (stdout empty; diagnostics go to stderr) */
  run_unexpand("-t 0 /UNEXPD.TXT", "", out, sizeof out);
  check_rc("-t 0 rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_unexpand("-t 4,2 /UNEXPD.TXT", "", out, sizeof out);
  check_rc("-t 4,2 rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_unexpand("-t 4 /NO_SUCH_UNEXPAND_FILE", "", out, sizeof out);
  check_rc("missing file rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  if (failures == 0) {
    print_console("[UNEXPTEST] ALL PASSED (16 checks)\n");
    exit(0);
  }
  print_console("[UNEXPTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
