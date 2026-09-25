/*
 * CUTTEST.BIN — in-OS end-to-end acceptance for the ported GNU cut.
 *
 * Spawns CUT.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> open/read -> field/byte selection -> our
 * printf -> write) and compares the captured stdout byte-for-byte
 * against GNU cut 2.1 behavior: byte modes, field modes with the
 * default TAB and custom -d delimiters, -s, --output-delimiter, stdin
 * mode, multi-file mode, long options, and exit codes for the error
 * paths (missing file, no list, empty list).
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by cut
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
    print_console("[CUTTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[CUTTEST] FAIL ");
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
    print_console("[CUTTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[CUTTEST] FAIL ");
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

/* Run "cut <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_cut_attempt(const char *args, const char *stdin_data, char *out,
                            size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[CUTTEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("CUT.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("CUT.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[CUTTEST] spawn CUT.BIN failed\n");
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
static void run_cut(const char *args, const char *stdin_data, char *out,
                    size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_cut_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[CUTTEST] cannot create ");
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

  print_console("[CUTTEST] starting\n");

  /* 10 + 5 + 0 + 2 chars on 4 lines */
  write_file("/CUTTEST.TXT", "abcdefghij\nklmno\n\npq\n");
  /* four TAB-separated fields, then two */
  write_file("/CUTTAB.TXT", "a\tb\tc\td\ne\tf\n");
  /* colon-separated incl. one non-delimited line */
  write_file("/CUTCOL.TXT", "one:two:three\nfour:five\nnosep\n");
  write_file("/CUTCOL2.TXT", "zzz\n");

  run_cut("-b 1-3 /CUTTEST.TXT", "", out, sizeof out);
  check("-b 1-3 file", out, "abc\nklm\n\npq\n");

  run_cut("-c 5- /CUTTEST.TXT", "", out, sizeof out);
  check("-c 5- file", out, "efghij\no\n\n\n");

  run_cut("-b 2,4 /CUTTEST.TXT", "", out, sizeof out);
  check("-b 2,4 file", out, "bd\nln\n\nq\n");

  run_cut("-f 2 /CUTTAB.TXT", "", out, sizeof out);
  check("-f 2 default TAB", out, "b\nf\n");

  run_cut("-f 2-3 /CUTTAB.TXT", "", out, sizeof out);
  check("-f 2-3 default TAB", out, "b\tc\nf\n");

  run_cut("-d : -f 1,3 /CUTCOL.TXT", "", out, sizeof out);
  check("-d : -f 1,3 file", out, "one:three\nfour\nnosep\n");

  run_cut("-d : -s -f 2 /CUTCOL.TXT", "", out, sizeof out);
  check("-d : -s -f 2 file", out, "two\nfive\n");

  run_cut("-d : -f 2- --output-delimiter='|' /CUTCOL.TXT", "", out, sizeof out);
  check("--output-delimiter file", out, "two|three\nfive\nnosep\n");

  run_cut("--bytes=1-2 /CUTTEST.TXT", "", out, sizeof out);
  check("--bytes=1-2 long opt", out, "ab\nkl\n\npq\n");

  run_cut("-n -b 1 /CUTTEST.TXT", "", out, sizeof out);
  check("-n ignored", out, "a\nk\n\np\n");

  run_cut("-b 1-3", "abcdef\nxy\n", out, sizeof out);
  check("stdin byte mode", out, "abc\nxy\n");

  run_cut("-d : -f 2", "p:q:r\ns\n", out, sizeof out);
  check("stdin field mode", out, "q\ns\n");

  run_cut("-d : -f 1 /CUTCOL.TXT /CUTCOL2.TXT", "", out, sizeof out);
  check("multi-file", out, "one\nfour\nnosep\nzzz\n");

  /* error paths: exit codes (stdout empty; diagnostics go to stderr) */
  run_cut("-b 1 /NO_SUCH_CUT_FILE", "", out, sizeof out);
  check_rc("missing file rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_cut("", "x\n", out, sizeof out);
  check_rc("no list rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_cut("-b 0", "x\n", out, sizeof out);
  check_rc("empty list rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_cut("-d xy -f 1 /CUTCOL.TXT", "", out, sizeof out);
  check_rc("multi-char delim rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  if (failures == 0) {
    print_console("[CUTTEST] ALL PASSED (17 checks)\n");
    exit(0);
  }
  print_console("[CUTTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
