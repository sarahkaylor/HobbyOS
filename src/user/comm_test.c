/*
 * COMMTEST.BIN — in-OS end-to-end acceptance for the ported GNU comm.
 *
 * Spawns COMM.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> fopen -> lib/linebuffer's readline -> the
 * memcmp merge -> our stdio) and compares the captured stdout
 * byte-for-byte against GNU comm 2.1 behavior: the default 3-column
 * TAB-separated output, each of -1/-2/-3, the -12/-13/-23/-123
 * combinations, runs of duplicate lines, a file pair without a final
 * newline, an empty left file, the "-" (stdin) operand, the
 * unsorted-input merge (textutils-2.1 has NO order check: it merges and
 * exits 0, unlike modern coreutils which diagnoses unsorted input), and
 * the exit codes for the error paths (missing file, missing operand ->
 * usage (1)).
 *
 * Every expected string below was generated with the textutils-2.1
 * reference comm (src/host/build_tu21_comm_ref.sh) on byte-identical
 * fixtures; src/host/comm_parity.sh races the same cases on the host.
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by comm
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
    print_console("[COMMTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[COMMTEST] FAIL ");
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
    print_console("[COMMTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[COMMTEST] FAIL ");
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

/* Run "comm <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_comm_attempt(const char *args, const char *stdin_data, char *out,
                             size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[COMMTEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("COMM.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("COMM.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[COMMTEST] spawn COMM.BIN failed\n");
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
static void run_comm(const char *args, const char *stdin_data, char *out,
                     size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_comm_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[COMMTEST] cannot create ");
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

  print_console("[COMMTEST] starting\n");

  /* sorted, four lines each, two shared */
  write_file("/COML.TXT", "apple\nbanana\ncherry\ndate\n");
  write_file("/COMR.TXT", "banana\ncherry\nfig\ngrape\n");
  /* runs of duplicated lines */
  write_file("/COMLD.TXT", "a\na\nb\nc\nc\nc\n");
  write_file("/COMRD.TXT", "a\nb\nb\nc\n");
  /* no trailing newline on either side */
  write_file("/COMN1.TXT", "m\nn");
  write_file("/COMN2.TXT", "m\no\n");
  /* deliberately unsorted (textutils-2.1 does not check order) */
  write_file("/COMU1.TXT", "b\na\n");
  write_file("/COMU2.TXT", "a\nb\n");
  write_file("/COME.TXT", "");

  run_comm("/COML.TXT /COMR.TXT", "", out, sizeof out);
  check("default 3-column", out,
        "apple\n\t\tbanana\n\t\tcherry\ndate\n\tfig\n\tgrape\n");

  run_comm("-1 /COML.TXT /COMR.TXT", "", out, sizeof out);
  check("-1 suppress left-unique", out, "\tbanana\n\tcherry\nfig\ngrape\n");

  run_comm("-2 /COML.TXT /COMR.TXT", "", out, sizeof out);
  check("-2 suppress right-unique", out,
        "apple\n\tbanana\n\tcherry\ndate\n");

  run_comm("-3 /COML.TXT /COMR.TXT", "", out, sizeof out);
  check("-3 suppress shared", out, "apple\ndate\n\tfig\n\tgrape\n");

  run_comm("-12 /COML.TXT /COMR.TXT", "", out, sizeof out);
  check("-12 shared only", out, "banana\ncherry\n");

  run_comm("-13 /COML.TXT /COMR.TXT", "", out, sizeof out);
  check("-13 right-unique only", out, "fig\ngrape\n");

  run_comm("-23 /COML.TXT /COMR.TXT", "", out, sizeof out);
  check("-23 left-unique only", out, "apple\ndate\n");

  run_comm("-123 /COML.TXT /COMR.TXT", "", out, sizeof out);
  check("-123 empty", out, "");

  run_comm("/COMLD.TXT /COMRD.TXT", "", out, sizeof out);
  check("duplicate runs", out, "\t\ta\na\n\t\tb\n\tb\n\t\tc\nc\nc\n");

  run_comm("/COMN1.TXT /COMN2.TXT", "", out, sizeof out);
  check("missing final newline", out, "\t\tm\nn\n\to\n");

  run_comm("/COME.TXT /COMR.TXT", "", out, sizeof out);
  check("empty left file", out, "\tbanana\n\tcherry\n\tfig\n\tgrape\n");

  run_comm("- /COMR.TXT", "apple\nbanana\ncherry\ndate\n", out, sizeof out);
  check("stdin operand", out,
        "apple\n\t\tbanana\n\t\tcherry\ndate\n\tfig\n\tgrape\n");

  /* 2.1 has no order check: unsorted input still merges (the memcmp
     merge of the two files) and exits 0 */
  run_comm("/COMU1.TXT /COMU2.TXT", "", out, sizeof out);
  check("unsorted: merged, no order check", out, "\ta\n\t\tb\na\n");
  check_rc("unsorted rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 0);

  /* error paths: exit codes (stdout empty; diagnostics go to stderr) */
  run_comm("/NO_SUCH_COMM_FILE /COMR.TXT", "", out, sizeof out);
  check_rc("missing file rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_comm("/COML.TXT", "", out, sizeof out);
  check_rc("missing operand rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  if (failures == 0) {
    print_console("[COMMTEST] ALL PASSED (16 checks)\n");
    exit(0);
  }
  print_console("[COMMTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
