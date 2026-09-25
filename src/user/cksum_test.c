/*
 * CKSUMTEST.BIN — in-OS end-to-end acceptance for the ported GNU cksum.
 *
 * Spawns CKSUM.BIN through the real spawn2/pipe path (crt0 main() ->
 * getopt_long -> open/read -> CRC-32 + length bytes -> our printf -> write)
 * and compares the captured stdout byte-for-byte against GNU textutils-2.1
 * cksum behavior: file operands, stdin with and without "-", multi-file
 * runs, empty / all-256-byte-value / 70 KB fixtures and the missing-file
 * exit code.  Every expected crc+length pair below was produced by the
 * textutils-2.1 reference binary (src/host/build_tu21_cksum_ref.sh) and
 * cross-checked against /usr/bin/cksum; the same values back the strict
 * host parity run in src/host/cksum_parity.sh.
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by cksum
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
    print_console("[CKSUMTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[CKSUMTEST] FAIL ");
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
    print_console("[CKSUMTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[CKSUMTEST] FAIL ");
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

/* Run "cksum <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_cksum_attempt(const char *args, const char *stdin_data,
                              char *out, size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[CKSUMTEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("CKSUM.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("CKSUM.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[CKSUMTEST] spawn CKSUM.BIN failed\n");
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
static void run_cksum(const char *args, const char *stdin_data, char *out,
                      size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_cksum_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[CKSUMTEST] cannot create ");
    print_console(path);
    print_console("\n");
    failures++;
    return;
  }
  write(fd, content, strlen(content));
  close(fd);
}

/* Write UNIT repeated REPS times to PATH (keeps the 70 KB fixture off the
   stack and out of a single string literal). */
static void write_pattern(const char *path, const char *unit, int reps) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  size_t len = strlen(unit);
  int i;

  if (fd < 0) {
    print_console("[CKSUMTEST] cannot create ");
    print_console(path);
    print_console("\n");
    failures++;
    return;
  }
  for (i = 0; i < reps; i++)
    write(fd, unit, len);
  close(fd);
}

int main(void) {
  char out[512];

  print_console("[CKSUMTEST] starting\n");

  /* Fixtures.  Every crc below is the POSIX CRC-32 + length of the exact
     bytes written here, taken from the textutils-2.1 reference binary. */
  write_file("/CKSEMPTY.TXT", "");                  /* 0 bytes   */
  write_file("/CKSA.TXT", "a");                     /* 1 byte, no newline */
  write_file("/CKSAN.TXT", "a\n");                  /* 2 bytes   */
  write_file("/CKSABC.TXT", "abc\n");               /* 4 bytes   */
  write_file("/CKSHELLO.TXT", "hello world\n");     /* 12 bytes  */
  {
    int fd = open("/CKSALL.TXT", O_WRONLY | O_CREAT | O_TRUNC);
    char bytes[256];
    int j;
    if (fd < 0) {
      print_console("[CKSUMTEST] cannot create /CKSALL.TXT\n");
      failures++;
    } else {
      for (j = 0; j < 256; j++)
        bytes[j] = (char)j;
      write(fd, bytes, 256);
      close(fd);
    }
  }
  write_pattern("/CKSPAT.TXT", "ABCD1234", 500);          /* 4000 bytes */
  write_pattern("/CKSBIG.TXT", "0123456789abcdef", 4375); /* 70000 bytes */

  run_cksum("/CKSEMPTY.TXT", "", out, sizeof out);
  check("empty file", out, "4294967295 0 /CKSEMPTY.TXT\n");

  run_cksum("/CKSA.TXT", "", out, sizeof out);
  check("one byte 'a'", out, "1220704766 1 /CKSA.TXT\n");

  run_cksum("/CKSAN.TXT", "", out, sizeof out);
  check("'a\\n' (2 bytes)", out, "2418082923 2 /CKSAN.TXT\n");

  run_cksum("/CKSABC.TXT", "", out, sizeof out);
  check("'abc\\n' (4 bytes)", out, "1112837078 4 /CKSABC.TXT\n");

  run_cksum("/CKSHELLO.TXT", "", out, sizeof out);
  check("'hello world\\n' (12 bytes)", out, "3733384285 12 /CKSHELLO.TXT\n");

  run_cksum("/CKSALL.TXT", "", out, sizeof out);
  check("all 256 byte values", out, "1313719201 256 /CKSALL.TXT\n");

  run_cksum("/CKSPAT.TXT", "", out, sizeof out);
  check("4000-byte pattern", out, "1980477897 4000 /CKSPAT.TXT\n");

  run_cksum("/CKSBIG.TXT", "", out, sizeof out);
  check("70000-byte pattern", out, "2844939061 70000 /CKSBIG.TXT\n");

  run_cksum("/CKSA.TXT /CKSABC.TXT", "", out, sizeof out);
  check("multi-file", out,
        "1220704766 1 /CKSA.TXT\n1112837078 4 /CKSABC.TXT\n");

  run_cksum("/CKSEMPTY.TXT /CKSHELLO.TXT", "", out, sizeof out);
  check("multi-file incl. empty", out,
        "4294967295 0 /CKSEMPTY.TXT\n3733384285 12 /CKSHELLO.TXT\n");

  run_cksum("", "abc\n", out, sizeof out);
  check("stdin, no operand", out, "1112837078 4\n");

  run_cksum("-", "a", out, sizeof out);
  check("stdin via '-'", out, "1220704766 1 -\n");

  run_cksum("", "", out, sizeof out);
  check("empty stdin", out, "4294967295 0\n");

  run_cksum("--version", "", out, sizeof out);
  check("--version", out, "cksum (textutils) 2.1\n");

  /* error paths: exit codes (stdout empty; diagnostics go to stderr) */
  run_cksum("/CKSNOPE.TXT", "", out, sizeof out);
  check_rc("missing file rc",
           WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_cksum("/CKSA.TXT /CKSNOPE.TXT", "", out, sizeof out);
  check_rc("missing among files rc",
           WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  if (failures == 0) {
    print_console("[CKSUMTEST] ALL PASSED (16 checks)\n");
    exit(0);
  }
  print_console("[CKSUMTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
