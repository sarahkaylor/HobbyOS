/*
 * MD5SUMTEST.BIN — in-OS end-to-end acceptance for the ported GNU md5sum.
 *
 * Spawns MD5SUM.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> fopen/fread -> RFC 1321 engine -> our printf
 * -> write) and compares the captured stdout byte-for-byte against GNU
 * textutils-2.1 md5sum behavior: the RFC 1321 test vectors (empty, "abc",
 * "message digest"), multi-file order, -b's `*' marker, stdin mode,
 * --string, -c / --check pass, fail and --status (with the exit codes
 * those produce), the missing-file rc and the --version banner.  The
 * exhaustive byte-exact matrix (97 cases vs the genuine 2.1 build, plus a
 * loose race against the host's md5sum) lives in the host harness
 * src/host/md5sum_parity.sh; this test proves the device path with a
 * handful of spawns.
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by md5sum
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
    print_console("[MD5TEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[MD5TEST] FAIL ");
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
    print_console("[MD5TEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[MD5TEST] FAIL ");
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

/* Run "md5sum <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_md5sum_attempt(const char *args, const char *stdin_data, char *out,
                               size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[MD5TEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("MD5SUM.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("MD5SUM.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[MD5TEST] spawn MD5SUM.BIN failed\n");
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
   non-empty output; the --status and error-path cases accept empty output
   and only need the exit status. */
static void run_md5sum(const char *args, const char *stdin_data, char *out,
                       size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_md5sum_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[MD5TEST] cannot create ");
    print_console(path);
    print_console("\n");
    failures++;
    return;
  }
  write(fd, content, strlen(content));
  close(fd);
}

/* exit status of the last spawned md5sum, or -1 if it did not exit */
static int last_rc(void) {
  return WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1;
}

int main(void) {
  char out[512];

  print_console("[MD5TEST] starting\n");

  /* fixtures: the RFC 1321 vectors as files, plus self-written check files */
  write_file("/MD5EMPTY.TXT", "");
  write_file("/MD5ABC.TXT", "abc");
  write_file("/MD5MSG.TXT", "message digest");
  write_file("/MD5OK.CHK",
             "900150983cd24fb0d6963f7d28e17f72  /MD5ABC.TXT\n");
  write_file("/MD5BAD.CHK",
             "900150983cd24fb0d6963f7d28e17f73  /MD5ABC.TXT\n");

  /* 1 */
  run_md5sum("/MD5EMPTY.TXT", "", out, sizeof out);
  check("empty file digest", out,
        "d41d8cd98f00b204e9800998ecf8427e  /MD5EMPTY.TXT\n");

  /* 2 */
  run_md5sum("/MD5ABC.TXT", "", out, sizeof out);
  check("abc digest", out,
        "900150983cd24fb0d6963f7d28e17f72  /MD5ABC.TXT\n");

  /* 3 */
  run_md5sum("/MD5MSG.TXT", "", out, sizeof out);
  check("message digest", out,
        "f96b697d7cb7938d525a2f31aaf161d0  /MD5MSG.TXT\n");

  /* 4 */
  run_md5sum("/MD5ABC.TXT /MD5EMPTY.TXT /MD5MSG.TXT", "", out, sizeof out);
  check("multi-file order", out,
        "900150983cd24fb0d6963f7d28e17f72  /MD5ABC.TXT\n"
        "d41d8cd98f00b204e9800998ecf8427e  /MD5EMPTY.TXT\n"
        "f96b697d7cb7938d525a2f31aaf161d0  /MD5MSG.TXT\n");

  /* 5 */
  run_md5sum("-b /MD5ABC.TXT", "", out, sizeof out);
  check("-b binary marker", out,
        "900150983cd24fb0d6963f7d28e17f72 */MD5ABC.TXT\n");

  /* 6 */
  run_md5sum("-", "abc", out, sizeof out);
  check("stdin dash digest", out,
        "900150983cd24fb0d6963f7d28e17f72  -\n");

  /* 7 */
  run_md5sum("--string=abc", "", out, sizeof out);
  check("--string digest", out,
        "900150983cd24fb0d6963f7d28e17f72  \"abc\"\n");

  /* 8, 9 */
  run_md5sum("-c /MD5OK.CHK", "", out, sizeof out);
  check("-c pass output", out, "/MD5ABC.TXT: OK\n");
  check_rc("-c pass rc", last_rc(), 0);

  /* 10 */
  run_md5sum("--check /MD5OK.CHK", "", out, sizeof out);
  check("--check long option", out, "/MD5ABC.TXT: OK\n");

  /* 11, 12 */
  run_md5sum("-c /MD5BAD.CHK", "", out, sizeof out);
  check("-c fail output", out, "/MD5ABC.TXT: FAILED\n");
  check_rc("-c fail rc", last_rc(), 1);

  /* 13, 14 */
  run_md5sum("-c --status /MD5OK.CHK", "", out, sizeof out);
  check("-c --status quiet", out, "");
  check_rc("-c --status rc", last_rc(), 0);

  /* 15 */
  run_md5sum("/MD5NOPE.XXX", "", out, sizeof out);
  check_rc("missing file rc", last_rc(), 1);

  /* 16 */
  run_md5sum("--version", "", out, sizeof out);
  check("--version banner", out, "md5sum (textutils) 2.1\n");

  if (failures == 0) {
    print_console("[MD5TEST] ALL PASSED (16 checks)\n");
    exit(0);
  }
  print_console("[MD5TEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
