/*
 * EXPANDTEST.BIN — in-OS end-to-end acceptance for the ported GNU expand.
 *
 * Spawns EXPAND.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> fopen/getc -> column tracking -> putchar ->
 * our printf -> write) and compares the captured stdout byte-for-byte
 * against GNU expand 2.1 behavior: the default 8-column tabbing, -t/--tabs
 * single sizes and explicit tab-stop lists (including the column past the
 * last stop, which becomes a single space), -i/--initial, stdin (no FILE
 * and the "-" operand), a FILE operand, multi-file mode, blank lines and
 * missing final newline, and the exit codes of the tab-list error paths
 * (non-numeric list, zero size) and a missing input file.
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by expand
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
    print_console("[EXPANDTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[EXPANDTEST] FAIL ");
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
    print_console("[EXPANDTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[EXPANDTEST] FAIL ");
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

/* Run "expand <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_expand_attempt(const char *args, const char *stdin_data,
                               char *out, size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[EXPANDTEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("EXPAND.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("EXPAND.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[EXPANDTEST] spawn EXPAND.BIN failed\n");
    failures++;
    return;
  }
  /* feed stdin, then let the child see EOF.  Retry the write briefly:
     under full-suite process-table pressure a spawn can race the pipe
     reader accounting, and an EAGAIN/PIPE error here must not turn
     into an empty-output failure.  Never write zero bytes: a zero-length
     pipe write blocks the caller forever. */
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
static void run_expand(const char *args, const char *stdin_data, char *out,
                       size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_expand_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[EXPANDTEST] cannot create ");
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

  print_console("[EXPANDTEST] starting\n");

  /* tab at column 1, default stop 8 -> 7 spaces */
  write_file("/EXPTAB.TXT", "a\tb\n");
  /* tabs at every column, for the explicit tab-stop list */
  write_file("/EXPLIST.TXT", "a\tb\tc\td\te\n");
  /* leading tab plus a tab after text, for -i */
  write_file("/EXPMIX.TXT", "\tlead\nx\tmid\n");
  /* blank lines and no final newline */
  write_file("/EXPBLNK.TXT", "\n\nab\tc");

  run_expand("/EXPTAB.TXT", "", out, sizeof out);
  check("default tabs (8) on file", out, "a       b\n");

  run_expand("-t 4 /EXPTAB.TXT", "", out, sizeof out);
  check("-t 4 file", out, "a   b\n");

  run_expand("-t 1 /EXPTAB.TXT", "", out, sizeof out);
  check("-t 1 file", out, "a b\n");

  run_expand("-t 4,8 /EXPLIST.TXT", "", out, sizeof out);
  check("-t 4,8 list + past last stop", out, "a   b   c d e\n");

  run_expand("-t 4 /EXPMIX.TXT", "", out, sizeof out);
  check("-t 4 mid-line tabs", out, "    lead\nx   mid\n");

  run_expand("-i /EXPMIX.TXT", "", out, sizeof out);
  check("-i keeps tabs after text", out, "        lead\nx\tmid\n");

  run_expand("-i -t 4 /EXPMIX.TXT", "", out, sizeof out);
  check("-i -t 4 file", out, "    lead\nx\tmid\n");

  run_expand("--tabs=4 /EXPTAB.TXT", "", out, sizeof out);
  check("--tabs=4 long option", out, "a   b\n");

  run_expand("--initial /EXPMIX.TXT", "", out, sizeof out);
  check("--initial long option", out, "        lead\nx\tmid\n");

  run_expand("-t 4 /EXPBLNK.TXT", "", out, sizeof out);
  check("-t 4 blank lines, no final EOL", out, "\n\nab  c");

  run_expand("-t 4 /EXPTAB.TXT /EXPLIST.TXT", "", out, sizeof out);
  check("multi-file", out, "a   b\na   b   c d e\n");

  run_expand("", "a\tb\n", out, sizeof out);
  check("stdin default tabs", out, "a       b\n");

  run_expand("-t 4 -", "s\tt1\n", out, sizeof out);
  check("stdin via - operand", out, "s   t1\n");

  /* error paths: exit codes (stdout empty; diagnostics go to stderr) */
  run_expand("-t abc /EXPTAB.TXT", "", out, sizeof out);
  check_rc("-t abc rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1,
           1);

  run_expand("-t 0 /EXPTAB.TXT", "", out, sizeof out);
  check_rc("-t 0 rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_expand("-t 4 /EXPMISS.TXT", "", out, sizeof out);
  check_rc("missing file rc",
           WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  if (failures == 0) {
    print_console("[EXPANDTEST] ALL PASSED (16 checks)\n");
    exit(0);
  }
  print_console("[EXPANDTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
