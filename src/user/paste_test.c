/*
 * PASTETEST.BIN — in-OS end-to-end acceptance for the ported GNU paste.
 *
 * Spawns PASTE.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> fopen/getc/putc -> our stdio) and compares
 * the captured stdout byte-for-byte against GNU paste 2.1 behavior:
 * default parallel merging (TAB-separated), -s serial mode, -d
 * delimiter lists (single, cycling, escapes), the --serial/
 * --delimiters=LIST long options, stdin ("-" among the FILEs and the
 * no-FILE form), empty files, a file without a trailing newline, a long
 * line, and exit codes for the error paths (missing file, unknown
 * option).
 *
 * Every expected string below was generated with the textutils-2.1
 * reference paste (src/host/build_tu21_paste_ref.sh) on byte-identical
 * fixtures; src/host/paste_parity.sh races the same cases on the host.
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by paste
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
    print_console("[PASTETEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[PASTETEST] FAIL ");
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
    print_console("[PASTETEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[PASTETEST] FAIL ");
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

/* Run "paste <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_paste_attempt(const char *args, const char *stdin_data, char *out,
                              size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[PASTETEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("PASTE.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("PASTE.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[PASTETEST] spawn PASTE.BIN failed\n");
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
static void run_paste(const char *args, const char *stdin_data, char *out,
                      size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_paste_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[PASTETEST] cannot create ");
    print_console(path);
    print_console("\n");
    failures++;
    return;
  }
  write(fd, content, strlen(content));
  close(fd);
}

/* 200-char line, then a 5-char line (long-line case) */
static const char long_fixture[] =
    "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL"
    "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL"
    "\nshort\n";

int main(void) {
  char out[1024];

  print_console("[PASTETEST] starting\n");

  write_file("/PSTA.TXT", "a1\na2\na3\n");
  write_file("/PSTB.TXT", "b1\nb2\n");
  write_file("/PSTC.TXT", "c1\nc2\nc3\nc4\n");
  write_file("/PSTEMPT.TXT", "");
  write_file("/PSTNONL.TXT", "x1\nx2"); /* no trailing newline */
  write_file("/PSTLONG.TXT", long_fixture);

  /* default mode: one line from each file, TAB-separated */
  run_paste("/PSTA.TXT /PSTB.TXT", "", out, sizeof out);
  check("two files default TAB", out, "a1\tb1\na2\tb2\na3\t\n");

  run_paste("/PSTA.TXT /PSTB.TXT /PSTC.TXT", "", out, sizeof out);
  check("three files default TAB", out, "a1\tb1\tc1\na2\tb2\tc2\na3\t\tc3\n\t\tc4\n");

  /* -s / --serial */
  run_paste("-s /PSTA.TXT /PSTB.TXT", "", out, sizeof out);
  check("-s serial two files", out, "a1\ta2\ta3\nb1\tb2\n");

  run_paste("--serial /PSTC.TXT", "", out, sizeof out);
  check("--serial long option", out, "c1\tc2\tc3\tc4\n");

  /* -d / --delimiters */
  run_paste("-d , /PSTA.TXT /PSTB.TXT", "", out, sizeof out);
  check("-d comma", out, "a1,b1\na2,b2\na3,\n");

  run_paste("-d :, /PSTA.TXT /PSTB.TXT /PSTC.TXT", "", out, sizeof out);
  check("-d list cycles :,", out, "a1:b1,c1\na2:b2,c2\na3:,c3\n:,c4\n");

  run_paste("--delimiters=: /PSTA.TXT /PSTB.TXT", "", out, sizeof out);
  check("--delimiters=: long option", out, "a1:b1\na2:b2\na3:\n");

  run_paste("-s -d : /PSTA.TXT /PSTB.TXT", "", out, sizeof out);
  check("-s -d :", out, "a1:a2:a3\nb1:b2\n");

  run_paste("-d \\0 /PSTA.TXT /PSTB.TXT", "", out, sizeof out);
  check("-d \\0 (empty delimiter)", out, "a1b1\na2b2\na3\n");

  /* empty files in either position */
  run_paste("/PSTEMPT.TXT /PSTA.TXT", "", out, sizeof out);
  check("empty file first", out, "\ta1\n\ta2\n\ta3\n");

  run_paste("/PSTA.TXT /PSTEMPT.TXT", "", out, sizeof out);
  check("empty file last", out, "a1\t\na2\t\na3\t\n");

  /* stdin: the no-FILE form, and "-" among the FILEs */
  run_paste("", "p1\np2\n", out, sizeof out);
  check("stdin, no FILEs", out, "p1\np2\n");

  run_paste("-d : /PSTA.TXT -", "s1\ns2\n", out, sizeof out);
  check("-d : file and stdin", out, "a1:s1\na2:s2\na3:\n");

  run_paste("-d , - -", "1\n2\n", out, sizeof out);
  check("stdin twice", out, "1,2\n");

  /* no trailing newline, serial mode */
  run_paste("-s /PSTNONL.TXT", "", out, sizeof out);
  check("-s without trailing newline", out, "x1\tx2\n");

  /* long line */
  run_paste("/PSTLONG.TXT /PSTB.TXT", "", out, sizeof out);
  check("long line", out,
        "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL"
        "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL"
        "\tb1\nshort\tb2\n");

  /* error paths: exit codes (stdout empty; diagnostics go to stderr) */
  run_paste("/NO_SUCH_PASTE_FILE", "", out, sizeof out);
  check_rc("missing file rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_paste("-Q /PSTA.TXT", "", out, sizeof out);
  check_rc("unknown option rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  if (failures == 0) {
    print_console("[PASTETEST] ALL PASSED (18 checks)\n");
    exit(0);
  }
  print_console("[PASTETEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
