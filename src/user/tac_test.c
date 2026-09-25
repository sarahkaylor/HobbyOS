/*
 * TACTEST.BIN — in-OS end-to-end acceptance for the ported GNU tac.
 *
 * Spawns TAC.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> open/getc -> regex -> our printf -> write)
 * and compares the captured stdout and stderr against the behavior the
 * host parity suite pinned to textutils-2.1.  Every expected string below
 * was generated with the parity-verified host build, including the 2.1
 * quirks (a missing trailing newline joins the last two records -
 * "one\ntwo" reverses to "twoone\n"; -b attaches the separator to the
 * start of the record it precedes).
 *
 * Inputs given on stdin arrive through spawn2's pipe, which is not a
 * regular file: tac then routes them through its temp-file path in /tmp,
 * so the R_STDIN/R_DASH cases double as the /tmp plumbing test.
 *
 * All status output goes to the console (print_console), matching the
 * rest of the test programs — stdout is a pipe owned by this harness and
 * is not echoed to the serial log.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "libc.h" /* pipe(), spawn2(), print_console() */

enum {
  R_BASIC,
  R_STDIN,
  R_DASH,
  R_BEFORE,
  R_BEFORENONL,
  R_SEP,
  R_BEFSEP,
  R_REGEX,
  R_NONL,
  R_EMPTY,
  R_ONELINE,
  R_MULTI,
  R_MISSING,
  R_EMPTYSEP,
  R_BIG,
  R_VERSION,
  R_HELP,
  R_N
};

static int failures = 0;

static char run_out[R_N][12000];
static char run_err[R_N][512];
static int run_rc[R_N];
static int run_ok[R_N];

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

static void fail_head(const char *what) {
  print_console("[TACTEST] FAIL ");
  print_console(what);
  print_console("\n");
  failures++;
}

/* Show a string with visible escapes so byte differences are readable. */
static void print_preview(const char *s) {
  char b[8];
  const char *p;
  int n = 0;
  for (p = s; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '\n') {
      print_console("\\n");
    } else if (c == '\t') {
      print_console("\\t");
    } else if (c == '\r') {
      print_console("\\r");
    } else if (c >= 0x20 && c < 0x7f) {
      b[0] = (char)c;
      b[1] = 0;
      print_console(b);
    } else {
      static const char hex[] = "0123456789abcdef";
      b[0] = '\\';
      b[1] = 'x';
      b[2] = hex[(c >> 4) & 0xf];
      b[3] = hex[c & 0xf];
      b[4] = 0;
      print_console(b);
    }
    if (++n >= 96) {
      print_console("...");
      break;
    }
  }
}

static void pass(const char *what) {
  print_console("[TACTEST] PASS ");
  print_console(what);
  print_console("\n");
}

static int write_file(const char *path, const char *content, size_t len) {
  int fd;
  size_t off = 0;

  /* FAT16 has no truncate: drop any stale file first so a shorter rewrite
     cannot leave old bytes past the new end.  */
  unlink(path);
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0)
    return -1;
  while (off < len) {
    ssize_t w = write(fd, content + off, len - off);
    if (w <= 0) {
      close(fd);
      return -1;
    }
    off += (size_t)w;
  }
  close(fd);
  return 0;
}

/* Spawn TAC.BIN once with args, feed stdin_data, capture stdout, stderr
   and the exit status.  Returns 0 on a completed run, -1 when the spawn
   never succeeded. */
static int run_attempt(const char *args, const char *stdin_data, char *out,
                       size_t outsize, char *err, size_t errsize, int *status) {
  int in_p[2], out_p[2], err_p[2];
  int pid;
  size_t n = 0, en = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0 || pipe(err_p) != 0)
    return -1;
  pid = spawn2("TAC.BIN", in_p[0], out_p[1], err_p[1], args);
  if (pid <= 0) {
    /* Process-table pressure from the concurrently running suite can make
       the first spawn fail (-1); retry briefly before giving up. */
    int tries = 0;
    while (pid <= 0 && tries < 100) {
      usleep(25000); /* 25 ms */
      pid = spawn2("TAC.BIN", in_p[0], out_p[1], err_p[1], args);
      tries++;
    }
  }
  if (pid <= 0) {
    close(in_p[0]);
    close(in_p[1]);
    close(out_p[0]);
    close(out_p[1]);
    close(err_p[0]);
    close(err_p[1]);
    return -1;
  }
  /* feed stdin, then let the child see EOF (no write at all for empty
     input: a zero-length pipe write must never be issued) */
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

  /* Drain stdout first, then stderr.  The pipe holds only PIPE_SIZE (512
     bytes), and a case whose output exceeds that (the 10KB file case)
     blocks the child mid-write until someone reads; reading stderr first
     would deadlock — the child waits for stdout space while the parent
     waits for stderr EOF.  stdout EOF only arrives once the child has
     exited, so the stderr read after it cannot wedge either. */
  close(out_p[1]);
  while (n + 1 < outsize) {
    int r = read(out_p[0], out + n, outsize - n - 1);
    if (r <= 0)
      break; /* EOF once the child exits and its pipe fd closes */
    n += (size_t)r;
  }
  close(out_p[0]);
  out[n] = '\0';

  close(err_p[1]);
  while (en + 1 < errsize) {
    int r = read(err_p[0], err + en, errsize - en - 1);
    if (r <= 0)
      break;
    en += (size_t)r;
  }
  close(err_p[0]);
  err[en] = '\0';

  /* Reap the child so its process slot is recycled for the next spawn. */
  {
    int ws = 0;
    waitpid(pid, &ws, 0);
    *status = ws;
  }
  return 0;
}

/* Empty stdout despite a successful spawn is a scheduler-pressure flake:
   retry the whole interaction a few times for the cases that expect
   non-empty output.  Error cases accept empty output and only need the
   exit status, so they skip the retry. */
static void run_slot(int slot, const char *args, const char *stdin_data,
                     int expect_nonempty) {
  int attempt;

  /* Progress marker: with the suite running many programs concurrently,
     a hang in this test is otherwise invisible (its verdict prints only
     after all cases).  The marker names the case being started. */
  print_console("[TACTEST] run: ");
  print_console(args[0] ? args : "(stdin)");
  print_console("\n");

  for (attempt = 0; attempt < 3; attempt++) {
    int status = 0;
    run_out[slot][0] = '\0';
    run_err[slot][0] = '\0';
    if (run_attempt(args, stdin_data, run_out[slot], sizeof run_out[slot],
                    run_err[slot], sizeof run_err[slot], &status) != 0) {
      if (attempt + 1 < 3) {
        usleep(50000); /* 50 ms; let other programs exit and free slots */
        continue;
      }
      run_ok[slot] = 0;
      run_rc[slot] = -1;
      return;
    }
    run_ok[slot] = 1;
    run_rc[slot] = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (!expect_nonempty || run_out[slot][0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

/* Assert one run's captured stdout + exit status (+ stderr when
   want_err != NULL: empty string means "must be empty").  want_out == NULL
   skips the stdout comparison (used for --help, whose bytes are already
   compared against the reference build by src/host/tac_parity.sh). */
static void check_run(int slot, const char *what, const char *want_out,
                      int want_rc, const char *want_err) {
  int bad = 0;

  if (!run_ok[slot]) {
    print_console("[TACTEST] FAIL ");
    print_console(what);
    print_console(" (spawn TAC.BIN failed)\n");
    failures++;
    return;
  }
  if (want_out != NULL && strcmp(run_out[slot], want_out) != 0) {
    fail_head(what);
    print_console("  stdout got:  \"");
    print_preview(run_out[slot]);
    print_console("\"\n  stdout want: \"");
    print_preview(want_out);
    print_console("\"\n");
    bad = 1;
  }
  if (run_rc[slot] != want_rc) {
    print_console("[TACTEST] FAIL ");
    print_console(what);
    print_console(" (exit status ");
    con_int(run_rc[slot]);
    print_console(" != ");
    con_int(want_rc);
    print_console(")\n");
    bad = 1;
  }
  if (want_err != NULL && strcmp(run_err[slot], want_err) != 0) {
    print_console("[TACTEST] FAIL ");
    print_console(what);
    print_console(" (stderr)\n  stderr got:  \"");
    print_preview(run_err[slot]);
    print_console("\"\n  stderr want: \"");
    print_preview(want_err);
    print_console("\"\n");
    bad = 1;
  }
  if (bad)
    failures++;
  else
    pass(what);
}

/* Same as check_run, but the stdout must contain SUB1 (and SUB2 when
   non-NULL) instead of matching byte-for-byte, and the stderr must
   contain ERRSUB when non-NULL. */
static void check_run_has(int slot, const char *what, const char *sub1,
                          const char *sub2, int want_rc, const char *errsub) {
  int bad = 0;

  if (!run_ok[slot]) {
    print_console("[TACTEST] FAIL ");
    print_console(what);
    print_console(" (spawn TAC.BIN failed)\n");
    failures++;
    return;
  }
  if (sub1 != NULL && strstr(run_out[slot], sub1) == NULL) {
    fail_head(what);
    print_console("  stdout missing \"");
    print_preview(sub1);
    print_console("\"; got: \"");
    print_preview(run_out[slot]);
    print_console("\"\n");
    bad = 1;
  }
  if (sub2 != NULL && strstr(run_out[slot], sub2) == NULL) {
    fail_head(what);
    print_console("  stdout missing \"");
    print_preview(sub2);
    print_console("\"\n");
    bad = 1;
  }
  if (run_rc[slot] != want_rc) {
    print_console("[TACTEST] FAIL ");
    print_console(what);
    print_console(" (exit status ");
    con_int(run_rc[slot]);
    print_console(" != ");
    con_int(want_rc);
    print_console(")\n");
    bad = 1;
  }
  if (errsub != NULL && strstr(run_err[slot], errsub) == NULL) {
    fail_head(what);
    print_console("  stderr missing \"");
    print_preview(errsub);
    print_console("\"; got: \"");
    print_preview(run_err[slot]);
    print_console("\"\n");
    bad = 1;
  }
  if (bad)
    failures++;
  else
    pass(what);
}

int main(void) {
  /* Fixtures.  The 100-line big file crosses the 8192-byte read-size
     boundary in tac's seeked path, so block-straddling records are
     exercised on-device too.  */
  static char bigbuf[11000];
  int i;

  if (write_file("/TACA.TXT", "one\ntwo\nthree\n", 14) != 0 ||
      write_file("/TACB.TXT", "one\ntwo", 7) != 0 ||
      write_file("/TACC.TXT", "", 0) != 0 ||
      write_file("/TACD.TXT", "wXXvXXu", 7) != 0 ||
      write_file("/TACF.TXT", "solo\n", 5) != 0 ||
      write_file("/TACG.TXT", "x1y22z333*", 10) != 0) {
    print_console("[TACTEST] FAIL fixture write\n");
    return 1;
  }
  /* 100 numbered lines of 100 bytes each = 10000 bytes > 8192, so the
     seeked path steps through its read-size boundary on-device.  */
  {
    size_t off = 0;
    for (i = 0; i < 100; i++) {
      int k;
      bigbuf[off++] = 'L';
      bigbuf[off++] = (char)('0' + (i / 100) % 10);
      bigbuf[off++] = (char)('0' + (i / 10) % 10);
      bigbuf[off++] = (char)('0' + i % 10);
      for (k = 4; k < 99; k++)
        bigbuf[off++] = 'x';
      bigbuf[off++] = '\n';
    }
    if (write_file("/TACE.TXT", bigbuf, off) != 0) {
      print_console("[TACTEST] FAIL big fixture write\n");
      return 1;
    }
  }

  /* --- runs ---------------------------------------------------------- */
  run_slot(R_BASIC, "/TACA.TXT", "", 1);
  run_slot(R_STDIN, "", "alpha\nbeta\ngamma\n", 1);
  run_slot(R_DASH, "-", "x\ny\n", 1);
  run_slot(R_BEFORE, "-b /TACA.TXT", "", 1);
  run_slot(R_BEFORENONL, "-b /TACB.TXT", "", 1);
  run_slot(R_SEP, "-s XX /TACD.TXT", "", 1);
  run_slot(R_BEFSEP, "-b -s XX /TACD.TXT", "", 1);
  run_slot(R_REGEX, "-r -s [0-9] /TACG.TXT", "", 1);
  run_slot(R_NONL, "/TACB.TXT", "", 1);
  run_slot(R_EMPTY, "/TACC.TXT", "", 0);
  run_slot(R_ONELINE, "/TACF.TXT", "", 1);
  run_slot(R_MULTI, "/TACA.TXT /TACF.TXT", "", 1);
  run_slot(R_MISSING, "/TACNOPE.TXT", "", 0);
  run_slot(R_EMPTYSEP, "--separator= /TACA.TXT", "", 0);
  run_slot(R_BIG, "/TACE.TXT", "", 1);
  run_slot(R_VERSION, "--version", "", 1);
  run_slot(R_HELP, "--help", "", 1);

  /* --- checks -------------------------------------------------------- */
  check_run(R_BASIC, "reverse 3-line file", "three\ntwo\none\n", 0, "");
  check_run(R_STDIN, "reverse stdin (temp-file path)",
            "gamma\nbeta\nalpha\n", 0, "");
  check_run(R_DASH, "reverse stdin via '-' operand", "y\nx\n", 0, "");
  check_run(R_BEFORE, "-b on 3-line file", "\n\nthree\ntwoone", 0, "");
  check_run(R_BEFORENONL, "-b with missing trailing newline", "\ntwoone", 0,
            "");
  check_run(R_SEP, "-s XX", "uvXXwXX", 0, "");
  check_run(R_BEFSEP, "-b -s XX", "XXuXXvw", 0, "");
  check_run(R_REGEX, "-r -s [0-9]", "*33z32y2x1", 0, "");
  check_run(R_NONL, "missing trailing newline joins records", "twoone\n", 0,
            "");
  check_run(R_EMPTY, "empty file", "", 0, "");
  check_run(R_ONELINE, "single line", "solo\n", 0, "");
  check_run(R_MULTI, "two file operands", "three\ntwo\none\nsolo\n", 0, "");
  check_run_has(R_MISSING, "missing file", NULL, NULL, 1,
                "No such file or directory");
  check_run_has(R_EMPTYSEP, "empty separator", NULL, NULL, 1,
                "separator cannot be empty");
  check_run_has(R_BIG, "10 KB file crosses read-size boundary", "L099",
                "L000", 0, "");
  check_run(R_VERSION, "--version", "tac (textutils) 2.1\n", 0, "");
  check_run_has(R_HELP, "--help usage text",
                "Usage: TAC.BIN [OPTION]... [FILE]...",
                "Write each FILE to standard output, last line first.", 0,
                "");

  if (failures == 0) {
    print_console("[TACTEST] all ");
    con_int(R_N);
    print_console(" cases passed\n");
    return 0;
  }
  print_console("[TACTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  return 1;
}
