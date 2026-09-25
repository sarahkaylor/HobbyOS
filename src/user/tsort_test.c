/*
 * TSORTTEST.BIN — in-OS end-to-end acceptance for the ported GNU tsort.
 *
 * Spawns TSORT.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> fopen/getc -> readtoken -> our printf ->
 * write) and compares the captured stdout and stderr byte-for-byte
 * against GNU tsort 2.1 behavior: simple chains and diamond graphs from
 * FILE and stdin, the cycle diagnostic (stderr lists the loop members,
 * exit status 1, the sort still completes), an odd token count (2.1 has
 * no diagnostic for it — the dangling token sorts normally, while modern
 * coreutils added a fatal one), repeated edges, a self-loop, whitespace
 * variants, empty input, a missing file and the two-operand error path.
 *
 * The child's argv[0] is its binary name (crt0 takes it from
 * SYS_GETPROGNAME), so error() diagnostics carry a "TSORT.BIN: " prefix.
 * The test derives that prefix from the missing-file run and then
 * compares the cycle/probe stderr text exactly.
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

/* One slot per case: all runs first, then all checks, so the checks can
   use the program-name prefix the child reports.  */
enum {
  R_CHAIN_FILE,
  R_CHAIN_STDIN,
  R_CHAIN_DASH,
  R_DIAMOND_FILE,
  R_DIAMOND_STDIN,
  R_CYCLE_FILE,
  R_CYCLE_STDIN,
  R_ODD,
  R_WS,
  R_SELFLOOP,
  R_DUP,
  R_EMPTY,
  R_MISSING,
  R_TWOARGS,
  R_VERSION,
  R_HELP,
  R_N
};

static int failures = 0;

static char run_out[R_N][1024];
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
  print_console("[TSORTTEST] FAIL ");
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
  print_console("[TSORTTEST] PASS ");
  print_console(what);
  print_console("\n");
}

static int write_file(const char *path, const char *content) {
  int fd;
  size_t len = strlen(content), off = 0;

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

/* Spawn TSORT.BIN once with args, feed stdin_data, capture stdout, stderr
   and the exit status.  Returns 0 on a completed run, -1 when the spawn
   never succeeded. */
static int run_attempt(const char *args, const char *stdin_data, char *out,
                       size_t outsize, char *err, size_t errsize, int *status) {
  int in_p[2], out_p[2], err_p[2];
  int pid;
  size_t n = 0, en = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0 || pipe(err_p) != 0)
    return -1;
  pid = spawn2("TSORT.BIN", in_p[0], out_p[1], err_p[1], args);
  if (pid <= 0) {
    /* Process-table pressure from the concurrently running suite can make
       the first spawn fail (-1); retry briefly before giving up. */
    int tries = 0;
    while (pid <= 0 && tries < 100) {
      usleep(25000); /* 25 ms */
      pid = spawn2("TSORT.BIN", in_p[0], out_p[1], err_p[1], args);
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

  /* stderr first (the cycle diagnostic), then stdout: every case's output
     is far below the pipe capacity, so neither read can wedge. */
  close(err_p[1]);
  while (en + 1 < errsize) {
    int r = read(err_p[0], err + en, errsize - en - 1);
    if (r <= 0)
      break;
    en += (size_t)r;
  }
  close(err_p[0]);
  err[en] = '\0';

  close(out_p[1]);
  while (n + 1 < outsize) {
    int r = read(out_p[0], out + n, outsize - n - 1);
    if (r <= 0)
      break; /* EOF once the child exits and its pipe fd closes */
    n += (size_t)r;
  }
  close(out_p[0]);
  out[n] = '\0';

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
   non-empty output.  Error/empty-input cases accept empty output and only
   need the exit status, so they skip the retry. */
static void run_slot(int slot, const char *args, const char *stdin_data,
                     int expect_nonempty) {
  int attempt;

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
   compared against the reference build by src/host/tsort_parity.sh). */
static void check_run(int slot, const char *what, const char *want_out,
                      int want_rc, const char *want_err) {
  int bad = 0;

  if (!run_ok[slot]) {
    print_console("[TSORTTEST] FAIL ");
    print_console(what);
    print_console(" (spawn TSORT.BIN failed)\n");
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
    print_console("[TSORTTEST] FAIL ");
    print_console(what);
    print_console(" (exit status ");
    con_int(run_rc[slot]);
    print_console(" != ");
    con_int(want_rc);
    print_console(")\n");
    bad = 1;
  }
  if (want_err != NULL && strcmp(run_err[slot], want_err) != 0) {
    print_console("[TSORTTEST] FAIL ");
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
   non-NULL) instead of matching byte-for-byte. */
static void check_run_has(int slot, const char *what, const char *sub1,
                          const char *sub2, int want_rc) {
  int bad = 0;

  if (!run_ok[slot]) {
    print_console("[TSORTTEST] FAIL ");
    print_console(what);
    print_console(" (spawn TSORT.BIN failed)\n");
    failures++;
    return;
  }
  if (strstr(run_out[slot], sub1) == NULL ||
      (sub2 != NULL && strstr(run_out[slot], sub2) == NULL)) {
    fail_head(what);
    print_console("  stdout was: \"");
    print_preview(run_out[slot]);
    print_console("\"\n");
    bad = 1;
  }
  if (run_rc[slot] != want_rc) {
    print_console("[TSORTTEST] FAIL ");
    print_console(what);
    print_console(" (exit status ");
    con_int(run_rc[slot]);
    print_console(" != ");
    con_int(want_rc);
    print_console(")\n");
    bad = 1;
  }
  if (bad)
    failures++;
  else
    pass(what);
}

/* The diagnostics' leading "<program name>: " prefix, learned from the
   missing-file run (the child's argv[0] is TSORT.BIN either way). */
static const char *prog_prefix(void) {
  static char pfx[64];
  char *colon;
  size_t n;

  colon = strchr(run_err[R_MISSING], ':');
  if (colon == NULL)
    return "TSORT.BIN";
  n = (size_t)(colon - run_err[R_MISSING]);
  if (n >= sizeof pfx)
    n = sizeof pfx - 1;
  memcpy(pfx, run_err[R_MISSING], n);
  pfx[n] = '\0';
  return pfx;
}

int main(void) {
  static char want[512];
  const char *pfx;

  print_console("[TSORTTEST] starting\n");

  /* Fixtures (8.3-safe FAT names) */
  if (write_file("/TSTCH.TXT", "a b\nb c\nc d\n") != 0 ||
      write_file("/TSTDM.TXT", "a b\na c\nb d\nc d\n") != 0 ||
      write_file("/TSTCY.TXT", "a b\nb c\nc a\n") != 0 ||
      write_file("/TSTOD.TXT", "a b c\n") != 0 ||
      write_file("/TSTWS.TXT", "a\tb\n  b   c \n") != 0 ||
      write_file("/TSTSL.TXT", "k k\nk m\n") != 0 ||
      write_file("/TSTDP.TXT", "x y\nx y\n") != 0 ||
      write_file("/TSTEM.TXT", "") != 0) {
    print_console("[TSORTTEST] cannot write fixtures\n");
    exit(1);
  }

  run_slot(R_CHAIN_FILE, "/TSTCH.TXT", "", 1);
  run_slot(R_CHAIN_STDIN, "", "a b\nb c\nc d\n", 1);
  run_slot(R_CHAIN_DASH, "-", "a b\nb c\nc d\n", 1);
  run_slot(R_DIAMOND_FILE, "/TSTDM.TXT", "", 1);
  run_slot(R_DIAMOND_STDIN, "", "a b\na c\nb d\nc d\n", 1);
  run_slot(R_CYCLE_FILE, "/TSTCY.TXT", "", 1);
  run_slot(R_CYCLE_STDIN, "", "a b\nb c\nc a\n", 1);
  run_slot(R_ODD, "", "a b c\n", 1);
  run_slot(R_WS, "/TSTWS.TXT", "", 1);
  run_slot(R_SELFLOOP, "/TSTSL.TXT", "", 1);
  run_slot(R_DUP, "/TSTDP.TXT", "", 1);
  run_slot(R_EMPTY, "/TSTEM.TXT", "", 0);
  run_slot(R_MISSING, "/TSTMIS.TXT", "", 0);
  run_slot(R_TWOARGS, "/TSTCH.TXT /TSTDM.TXT", "", 0);
  run_slot(R_VERSION, "--version", "", 1);
  run_slot(R_HELP, "--help", "", 1);

  pfx = prog_prefix();

  check_run(R_CHAIN_FILE, "chain from FILE", "a\nb\nc\nd\n", 0, NULL);
  check_run(R_CHAIN_STDIN, "chain from stdin", "a\nb\nc\nd\n", 0, NULL);
  check_run(R_CHAIN_DASH, "chain from '-' operand", "a\nb\nc\nd\n", 0, NULL);
  check_run(R_DIAMOND_FILE, "diamond from FILE", "a\nc\nb\nd\n", 0, NULL);
  check_run(R_DIAMOND_STDIN, "diamond from stdin", "a\nc\nb\nd\n", 0, NULL);

  /* The input contains a loop: stderr names the file, lists the loop
     members and the exit status is 1; the sort still emits every node. */
  snprintf(want, sizeof want,
           "%s: /TSTCY.TXT: input contains a loop:\n"
           "%s: a\n%s: b\n%s: c\n",
           pfx, pfx, pfx, pfx);
  check_run(R_CYCLE_FILE, "cycle from FILE (stderr + rc 1)", "a\nb\nc\n", 1,
            want);
  snprintf(want, sizeof want,
           "%s: -: input contains a loop:\n"
           "%s: a\n%s: b\n%s: c\n",
           pfx, pfx, pfx, pfx);
  check_run(R_CYCLE_STDIN, "cycle from stdin names '-'", "a\nb\nc\n", 1, want);

  /* textutils-2.1 has no odd-token diagnostic (modern coreutils exits 1
     with "input contains an odd number of tokens"): the dangling token is
     just another node and stderr stays empty. */
  print_console("[TSORTTEST] note: odd token count -> 2.1 behavior (no "
                "diagnostic, rc 0)\n");
  check_run(R_ODD, "odd token count (2.1: no diagnostic)", "a\nc\nb\n", 0, "");

  check_run(R_WS, "whitespace/tab variants", "a\nb\nc\n", 0, NULL);
  check_run(R_SELFLOOP, "self-loop pair ignored", "k\nm\n", 0, NULL);
  check_run(R_DUP, "repeated edges", "x\ny\n", 0, NULL);
  check_run(R_EMPTY, "empty input", "", 0, NULL);

  snprintf(want, sizeof want, "%s: /TSTMIS.TXT: No such file or directory\n",
           pfx);
  check_run(R_MISSING, "missing file (rc 1 + stderr)", "", 1, want);
  snprintf(want, sizeof want,
           "%s: only one argument may be specified\n"
           "Try `%s --help' for more information.\n",
           pfx, pfx);
  check_run(R_TWOARGS, "two operands rejected (rc 1 + stderr)", "", 1, want);

  check_run(R_VERSION, "--version prints the 2.1 banner",
            "tsort (textutils) 2.1\n", 0, NULL);
  /* --help writes the usage text to stdout; the exact bytes are already
     compared against the reference build in src/host/tsort_parity.sh, so
     only the framing is checked here. */
  snprintf(want, sizeof want, "Usage: %s [OPTION] [FILE]\n", pfx);
  check_run_has(R_HELP, "--help prints the 2.1 usage", want,
                "Write totally ordered list consistent with the partial "
                "ordering in FILE.\n",
                0);

  if (failures == 0) {
    print_console("[TSORTTEST] ALL PASSED (16 checks)\n");
    exit(0);
  }
  print_console("[TSORTTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
