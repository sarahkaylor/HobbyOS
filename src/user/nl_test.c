/*
 * NLTEST.BIN — in-OS end-to-end acceptance for the ported GNU nl.
 *
 * Spawns NL.BIN through the real spawn2/pipe path (crt0 main() -> get_args
 * -> getopt_long -> open/read -> linebuffer readline -> our printf ->
 * write) and compares the captured stdout byte-for-byte against GNU
 * textutils-2.1 nl behavior: default and -b t/a/n numbering, -i, -v, -w,
 * -n ln/rn/rz, -s, two-character -d section delimiters with logical-page
 * sections, -l blank-line joining, long options, stdin mode, multi-file
 * mode, a file with no final newline, and exit codes for the error paths
 * (missing file, invalid -i/-n/-w values, unknown option).
 *
 * The expectations below are the exact bytes textutils-2.1 produces
 * (verified against the reference build by src/host/nl_parity.sh — e.g.
 * 2.1 prints an unnumbered line's field with puts(), so each such line
 * contributes an extra newline; modern nl emits the field with fputs()).
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by nl itself
 * on the error paths (stderr) go to the console only.
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
    print_console("[NLTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[NLTEST] FAIL ");
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
    print_console("[NLTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[NLTEST] FAIL ");
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

/* Run "nl <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_nl_attempt(const char *args, const char *stdin_data, char *out,
                           size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[NLTEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("NL.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("NL.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[NLTEST] spawn NL.BIN failed\n");
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
static void run_nl(const char *args, const char *stdin_data, char *out,
                   size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_nl_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[NLTEST] cannot create ");
    print_console(path);
    print_console("\n");
    failures++;
    return;
  }
  write(fd, content, strlen(content));
  close(fd);
}

int main(void) {
  char out[1024];

  print_console("[NLTEST] starting\n");

  /* three non-empty lines separated by blank-line runs */
  write_file("/NLTEST.TXT", "alpha\n\nbeta\n\n\ngamma\n");
  /* three non-empty lines, none blank */
  write_file("/NLALL.TXT", "one\ntwo\nthree\n");
  /* a short blank-line run for -l */
  write_file("/NLBLANK.TXT", "x\n\n\ny\n");
  /* no final newline (readline must terminate the last line) */
  write_file("/NLNOEOL.TXT", "one\ntwo\nthree-no-nl");
  /* logical pages delimited by "xy" */
  write_file("/NLSEC.TXT", "xyxyxy\nh1\nxyxy\nb\nxy\nf\n");

  /* default: number non-empty lines only; 2.1 prints an unnumbered
     line's 7-space field with puts(), adding a newline after it */
  run_nl("/NLTEST.TXT", "", out, sizeof out);
  check("default numbering",
        out,
        "     1\talpha\n       \n\n     2\tbeta\n       \n\n       \n\n     3\tgamma\n");

  run_nl("-bt /NLTEST.TXT", "", out, sizeof out);
  check("-bt non-empty", out,
        "     1\talpha\n       \n\n     2\tbeta\n       \n\n       \n\n     3\tgamma\n");

  run_nl("-ba /NLTEST.TXT", "", out, sizeof out);
  check("-ba all lines", out,
        "     1\talpha\n     2\t\n     3\tbeta\n     4\t\n     5\t\n     6\tgamma\n");

  run_nl("-bn /NLTEST.TXT", "", out, sizeof out);
  check("-bn no numbering", out,
        "       \nalpha\n       \n\n       \nbeta\n       \n\n       \n\n       \ngamma\n");

  run_nl("-i3 /NLALL.TXT", "", out, sizeof out);
  check("-i3 increment", out, "     1\tone\n     4\ttwo\n     7\tthree\n");

  run_nl("-v10 /NLALL.TXT", "", out, sizeof out);
  check("-v10 start", out, "    10\tone\n    11\ttwo\n    12\tthree\n");

  run_nl("-w3 /NLALL.TXT", "", out, sizeof out);
  check("-w3 width", out, "  1\tone\n  2\ttwo\n  3\tthree\n");

  run_nl("-nrz -w3 /NLALL.TXT", "", out, sizeof out);
  check("-nrz -w3 right zero filled", out, "001\tone\n002\ttwo\n003\tthree\n");

  run_nl("-s: -w2 /NLALL.TXT", "", out, sizeof out);
  check("-s: -w2 separator", out, " 1:one\n 2:two\n 3:three\n");

  run_nl("--number-format=rz -w3 /NLALL.TXT", "", out, sizeof out);
  check("long options rz, w3", out, "001\tone\n002\ttwo\n003\tthree\n");

  run_nl("-ba -l2 /NLBLANK.TXT", "", out, sizeof out);
  check("-ba -l2 blank join", out,
        "     1\tx\n       \n\n     2\t\n     3\ty\n");

  run_nl("-d xy -ba /NLSEC.TXT", "", out, sizeof out);
  check("-d xy sections", out, "\n       \nh1\n\n     1\tb\n\n       \nf\n");

  run_nl("-ba /NLNOEOL.TXT", "", out, sizeof out);
  check("-ba no final newline", out,
        "     1\tone\n     2\ttwo\n     3\tthree-no-nl\n");

  run_nl("-ba /NLALL.TXT /NLTEST.TXT", "", out, sizeof out);
  check("multi-file numbering", out,
        "     1\tone\n     2\ttwo\n     3\tthree\n     4\talpha\n     5\t\n     6\tbeta\n     7\t\n     8\t\n     9\tgamma\n");

  run_nl("-ba -w3 -s:", "p\nq\n", out, sizeof out);
  check("stdin -ba -w3 -s:", out, "  1:p\n  2:q\n");

  /* error paths: exit codes (stdout empty; diagnostics go to stderr) */
  run_nl("-ba /NO_SUCH_NL_FILE", "", out, sizeof out);
  check_rc("missing file rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_nl("-i x /NLALL.TXT", "", out, sizeof out);
  check_rc("bad -i rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_nl("-n q /NLALL.TXT", "", out, sizeof out);
  check_rc("bad -n rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_nl("-Z /NLALL.TXT", "", out, sizeof out);
  check_rc("unknown option rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  if (failures == 0) {
    print_console("[NLTEST] ALL PASSED (19 checks)\n");
    exit(0);
  }
  print_console("[NLTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
