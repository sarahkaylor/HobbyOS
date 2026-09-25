/*
 * FOLDTEST.BIN — in-OS end-to-end acceptance for the ported GNU fold.
 *
 * Spawns FOLD.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> open/read -> column arithmetic -> our
 * printf -> write) and compares the captured stdout byte-for-byte
 * against GNU fold 2.1 behavior: wrapping at small and default widths,
 * -b byte counting, -s breaking at blanks, tabs spanning the fold
 * column, high-bit bytes, exactly-at-the-limit lines, a file without a
 * trailing newline, the obsolete -N spelling, stdin mode, multi-file
 * mode, and exit codes for the error paths.
 *
 * Every golden string below is the actual stdout of the textutils-2.1
 * fold built by src/host/build_tu21_fold_ref.sh for the same input (see
 * the comments), so these are reference values, not hand arithmetic.
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by fold
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
    print_console("[FOLDTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[FOLDTEST] FAIL ");
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
    print_console("[FOLDTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[FOLDTEST] FAIL ");
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

/* Run "fold <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_fold_attempt(const char *args, const char *stdin_data, char *out,
                             size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[FOLDTEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("FOLD.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("FOLD.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[FOLDTEST] spawn FOLD.BIN failed\n");
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
static void run_fold(const char *args, const char *stdin_data, char *out,
                     size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_fold_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[FOLDTEST] cannot create ");
    print_console(path);
    print_console("\n");
    failures++;
    return;
  }
  write(fd, content, strlen(content));
  close(fd);
}

/* /FOLD80.TXT: an exactly-80-column line, an 81-column one, a 79-column
   one — the default width only ever splits the middle one.  /FOLDLONG.TXT:
   200 'L's on one line, no blank anywhere, so -w splits it hard and -s
   falls back to the same hard split. */
static void make_fixtures(void) {
  char buf[512];
  int i, n = 0;

  for (i = 0; i < 80; i++)
    buf[n++] = 'x';
  buf[n++] = '\n';
  for (i = 0; i < 81; i++)
    buf[n++] = 'y';
  buf[n++] = '\n';
  for (i = 0; i < 79; i++)
    buf[n++] = 'z';
  buf[n++] = '\n';
  buf[n] = '\0';
  write_file("/FOLD80.TXT", buf);

  n = 0;
  for (i = 0; i < 200; i++)
    buf[n++] = 'L';
  buf[n++] = '\n';
  buf[n] = '\0';
  write_file("/FOLDLONG.TXT", buf);
}

int main(void) {
  char out[512];

  print_console("[FOLDTEST] starting\n");

  /* "hello world" (11), "second line" (11), empty, "fourth" (6) */
  write_file("/FOLDTEST.TXT", "hello world\nsecond line\n\nfourth\n");
  /* spaces at every plausible break point */
  write_file("/FOLDSP.TXT", "alpha beta gamma delta epsilon\n");
  /* TABs: column 8 exactly when -w 8 is used */
  write_file("/FOLDTAB.TXT", "ab\tcd\nef\tgh\n");
  /* 16 columns decimal-then-hex, no blank */
  write_file("/FOLDA.TXT", "0123456789abcdef\n");
  write_file("/FOLDB.TXT", "xyz\n");
  /* no trailing newline: the last output line must not gain one */
  write_file("/FOLDNOE.TXT", "tail");
  write_file("/FOLDEMPTY.TXT", "");
  /* high-bit bytes (latin-1 e9 ff, an invalid UTF-8 c3 28, then "ab") */
  write_file("/FOLDHI.TXT", "\351\377\303\050" "ab\n");
  make_fixtures();

  run_fold("-w 5 /FOLDTEST.TXT", "", out, sizeof out);
  check("-w 5 file", out,
        "hello\n worl\nd\nsecon\nd lin\ne\n\nfourt\nh\n");

  run_fold("-w 1 /FOLDTEST.TXT", "", out, sizeof out);
  check("-w 1 file", out,
        "h\ne\nl\nl\no\n \nw\no\nr\nl\nd\ns\ne\nc\no\nn\nd\n \nl\ni\n"
        "n\ne\n\nf\no\nu\nr\nt\nh\n");

  run_fold("/FOLD80.TXT", "", out, sizeof out);
  check("default 80", out,
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxx\nyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
        "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\ny\nzzzzzzzzzz"
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
        "zzzzzzzzzz\n");

  run_fold("-w 40 /FOLDLONG.TXT", "", out, sizeof out);
  check("-w 40 long", out,
        "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL\nLLLLLLLLLLLLLLLLL"
        "LLLLLLLLLLLLLLLLLLLLLLL\nLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL"
        "LLLLLL\nLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL\nLLLLLLLLL"
        "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL\n");

  run_fold("-s -w 30 /FOLDLONG.TXT", "", out, sizeof out);
  check("-s -w 30 long", out,
        "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLL\nLLLLLLLLLLLLLLLLLLLLLLLLLLL"
        "LLL\nLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL\nLLLLLLLLLLLLLLLLLLLLLL"
        "LLLLLLLL\nLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL\nLLLLLLLLLLLLLLLLL"
        "LLLLLLLLLLLLL\nLLLLLLLLLLLLLLLLLLLL\n");

  /* -s on a line whose last word would overflow: the blank stays at the
     end of the line, the tail starts the next one */
  run_fold("-s -w 6 /FOLDSP.TXT", "", out, sizeof out);
  check("-s -w 6 spaces", out, "alpha \nbeta \ngamma \ndelta \nepsilo\nn\n");

  run_fold("-s -w 10 /FOLDSP.TXT", "", out, sizeof out);
  check("-s -w 10 spaces", out, "alpha \nbeta \ngamma \ndelta \nepsilon\n");

  run_fold("-w 2 /FOLDNOE.TXT", "", out, sizeof out);
  check("-w 2 no-eol", out, "ta\nil");

  /* -b counts bytes, not columns: the TAB is one byte here */
  run_fold("-b -w 4 /FOLDTAB.TXT", "", out, sizeof out);
  check("-b -w 4 tabs", out, "ab\tc\nd\nef\tg\nh\n");

  /* without -b the TAB advances to column 8, so it ends the line when
     column 9 would exceed an 8-column width */
  run_fold("-w 8 /FOLDTAB.TXT", "", out, sizeof out);
  check("-w 8 tab col", out, "ab\t\ncd\nef\t\ngh\n");

  /* high-bit bytes are single columns in the C locale */
  run_fold("-w 3 /FOLDHI.TXT", "", out, sizeof out);
  check("-w 3 highbit", out, "\351\377\303\n" "(ab\n");

  run_fold("-w 6", "abcdefghij\n", out, sizeof out);
  check("stdin -w 6", out, "abcdef\nghij\n");

  /* The obsolete -N spelling: HobbyOS publishes no _POSIX2_VERSION, so
     2.1's compile-time default (0) accepts it silently — the same as the
     2.1 reference run with _POSIX2_VERSION=199901. */
  run_fold("-10 /FOLDA.TXT", "", out, sizeof out);
  check("obsolete -10", out, "0123456789\nabcdef\n");

  run_fold("-w 10 /FOLDA.TXT /FOLDB.TXT", "", out, sizeof out);
  check("multi-file", out, "0123456789\nabcdef\nxyz\n");

  run_fold("-w 5 /FOLDEMPTY.TXT", "", out, sizeof out);
  check("empty file", out, "");

  /* error paths: exit codes (stdout empty; diagnostics go to stderr) */
  run_fold("-w abc /FOLDTEST.TXT", "", out, sizeof out);
  check_rc("-w abc rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_fold("-w 0 /FOLDTEST.TXT", "", out, sizeof out);
  check_rc("-w 0 rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_fold("-w 5 /NO_SUCH_FOLD_FILE", "", out, sizeof out);
  check_rc("missing file rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  if (failures == 0) {
    print_console("[FOLDTEST] ALL PASSED (18 checks)\n");
    exit(0);
  }
  print_console("[FOLDTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
