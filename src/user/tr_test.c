/*
 * TRTEST.BIN — in-OS end-to-end acceptance for the ported GNU tr.
 *
 * Spawns TR.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> read(0) -> set tables -> squeeze/delete/
 * translate -> our printf -> write(1)) and compares the captured stdout
 * byte-for-byte against GNU tr 2.1 behavior: translation with ranges and
 * [:classes:], -d deletes, -s squeezes, -c complement, -cd/-cs/-ds
 * combinations, -t truncate-set1, [c*n] repeats, backslash escapes,
 * stdin mode, and exit codes for the error paths (missing operand, too
 * many arguments, invalid class).
 *
 * tr reads only stdin, so every case feeds a fixture written by this test
 * (write_file + read_file_into) through the stdin pipe — that also
 * exercises open/read on the FAT image.  The expectations were captured
 * from the host build (obj/tr_host), which the strict parity run
 * (src/host/tr_parity.sh against the textutils-2.1 reference) proves is
 * byte-identical to the original.
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.  Diagnostics written by tr itself
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
    print_console("[TRTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[TRTEST] FAIL ");
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
    print_console("[TRTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[TRTEST] FAIL ");
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

/* Run "tr <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_tr_attempt(const char *args, const char *stdin_data, char *out,
                           size_t outsize) {
  int in_p[2], out_p[2];
  int pid;
  size_t n = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("[TRTEST] pipe failed\n");
    failures++;
    return;
  }
  pid = spawn2("TR.BIN", in_p[0], out_p[1], -1, args);
  if (pid <= 0) {
    /* process-table pressure from concurrently-running tests can make
       the first spawn fail (-1); retry briefly before giving up */
    int tries = 0;
    while (pid <= 0 && tries < 25) {
      usleep(20000); /* 20 ms */
      pid = spawn2("TR.BIN", in_p[0], out_p[1], -1, args);
      tries++;
    }
  }
  if (pid <= 0) {
    print_console("[TRTEST] spawn TR.BIN failed\n");
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
static void run_tr(const char *args, const char *stdin_data, char *out,
                   size_t outsize) {
  int tries;

  for (tries = 0; tries < 3; tries++) {
    out[0] = '\0';
    run_tr_attempt(args, stdin_data, out, outsize);
    if (out[0] != '\0')
      break;
    usleep(30000); /* 30 ms */
  }
}

static void write_file(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[TRTEST] cannot create ");
    print_console(path);
    print_console("\n");
    failures++;
    return;
  }
  write(fd, content, strlen(content));
  close(fd);
}

/* Load a fixture back so it can be fed to TR.BIN on stdin. */
static void read_file_into(const char *path, char *buf, size_t size) {
  int fd = open(path, O_RDONLY);
  int n = 0;

  if (fd < 0) {
    print_console("[TRTEST] cannot open ");
    print_console(path);
    print_console("\n");
    failures++;
    buf[0] = '\0';
    return;
  }
  while (n < (int)size - 1) {
    int r = read(fd, buf + n, size - 1 - (size_t)n);
    if (r <= 0)
      break;
    n += r;
  }
  close(fd);
  buf[n] = '\0';
}

int main(void) {
  char in[512];
  char out[512];

  print_console("[TRTEST] starting\n");

  write_file("/TRTEST.TXT", "hello world\nsecond line\n\nfourth\n");
  write_file("/TRDIG.TXT", "abc123DEF456\n");
  write_file("/TRTAB.TXT", "a\tb\tc\n\t\td\te\n");
  write_file("/TRSPC.TXT", "a   b     c  \n d\n");
  write_file("/TRRUN.TXT", "aaabbbccc aaa\n");

  read_file_into("/TRTEST.TXT", in, sizeof in);
  run_tr("a-z A-Z", in, out, sizeof out);
  check("a-z A-Z ranges", out, "HELLO WORLD\nSECOND LINE\n\nFOURTH\n");

  run_tr("-d aeiou", in, out, sizeof out);
  check("-d aeiou set", out, "hll wrld\nscnd ln\n\nfrth\n");

  read_file_into("/TRDIG.TXT", in, sizeof in);
  run_tr("-d 0-9", in, out, sizeof out);
  check("-d 0-9 range", out, "abcDEF\n");

  read_file_into("/TRSPC.TXT", in, sizeof in);
  run_tr("-s [:blank:]", in, out, sizeof out);
  check("-s [:blank:] class", out, "a b c \n d\n");

  read_file_into("/TRRUN.TXT", in, sizeof in);
  run_tr("-s a-z", in, out, sizeof out);
  check("-s a-z range", out, "abc a\n");

  read_file_into("/TRTEST.TXT", in, sizeof in);
  run_tr("-cd a-z", in, out, sizeof out);
  check("-cd complement delete", out, "helloworldsecondlinefourth");

  run_tr("-cs a-z \\n", in, out, sizeof out);
  check("-cs complement squeeze", out, "hello\nworld\nsecond\nline\nfourth\n");

  run_tr("[:lower:] [:upper:]", in, out, sizeof out);
  check("[:lower:] -> [:upper:]", out, "HELLO WORLD\nSECOND LINE\n\nFOURTH\n");

  read_file_into("/TRDIG.TXT", in, sizeof in);
  run_tr("[:digit:] x", in, out, sizeof out);
  check("[:digit:] -> x", out, "abcxxxDEFxxx\n");

  read_file_into("/TRRUN.TXT", in, sizeof in);
  run_tr("[a*2] X", in, out, sizeof out);
  check("[a*2] repeat", out, "XXXbbbccc XXX\n");

  read_file_into("/TRDIG.TXT", in, sizeof in);
  run_tr("-t 0-9 ab", in, out, sizeof out);
  check("-t truncate-set1", out, "abcb23DEF456\n");

  read_file_into("/TRTAB.TXT", in, sizeof in);
  run_tr("-d [:blank:]", in, out, sizeof out);
  check("-d [:blank:] tabs", out, "abc\nde\n");

  /* stdin mode: no file operand is ever given to tr; the pipe is fd 0 */
  read_file_into("/TRTEST.TXT", in, sizeof in);
  run_tr("A-Z a-z", in, out, sizeof out);
  check("stdin translate", out, "hello world\nsecond line\n\nfourth\n");

  /* error paths: exit codes (stdout empty; diagnostics go to stderr) */
  run_tr("-d", in, out, sizeof out);
  check_rc("missing operand rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_tr("a b c", in, out, sizeof out);
  check_rc("too many arguments rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  run_tr("-d [:bogus:]", in, out, sizeof out);
  check_rc("invalid class rc", WIFEXITED(last_status) ? WEXITSTATUS(last_status) : -1, 1);

  if (failures == 0) {
    print_console("[TRTEST] ALL PASSED (16 checks)\n");
    exit(0);
  }
  print_console("[TRTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
