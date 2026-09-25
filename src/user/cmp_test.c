/* cmp_test.c — on-device acceptance test for the HobbyOS port of GNU
   diffutils-2.8.1 cmp.

   Spawns CMP.BIN through the real spawn2/pipe path (crt0 main() ->
   sys_spawn_worker -> loader -> scheduler) with prepared fixtures on the
   FAT16 image, and checks stdout, stderr and the exit status for the
   documented behaviours: identical files, differing files (char/line
   report, -b, -l, -s), EOF handling, --ignore-initial, --bytes, stdin
   operands and the --help/--version text.

   The host build (Makefile cmp_host) races this same code against a
   diffutils-2.8.1 reference build in src/host/cmp_parity.sh; byte-exact
   output is the bar there.  Here the checks are the same assertions,
   run on the real kernel. */

#include "libc.h"
#include <fcntl.h>
#include <sys/wait.h>

enum {
  R_IDENT = 0,
  R_DIFF,
  R_DIFF_B,
  R_DIFF_L,
  R_DIFF_LB,
  R_QUIET,
  R_EOF,
  R_EMPTY_VS,
  R_MISSING,
  R_N_LIMIT,
  R_N_NONE,
  R_IGNORE,
  R_STDIN,
  R_VERSION,
  R_HELP,
  R_N
};

static int failures = 0;

static char run_out[R_N][2048];
static char run_err[R_N][512];
static int run_rc[R_N];
static int run_ok[R_N];

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
  print_console("[CMPTEST] FAIL ");
  print_console(what);
  print_console("\n");
  failures++;
}

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
  print_console("[CMPTEST] PASS ");
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

/* Spawn CMP.BIN once with args, capture stdout, stderr and exit status.
   Returns 0 on a completed run, -1 when the spawn never succeeded. */
static int run_attempt(const char *args, char *out, size_t outsize, char *err,
                       size_t errsize, int *status) {
  int in_p[2], out_p[2], err_p[2];
  int pid;
  size_t n = 0, en = 0;

  if (pipe(in_p) != 0 || pipe(out_p) != 0 || pipe(err_p) != 0)
    return -1;
  pid = spawn2("CMP.BIN", in_p[0], out_p[1], err_p[1], args);
  if (pid <= 0) {
    int tries = 0;
    while (pid <= 0 && tries < 100) {
      usleep(25000); /* 25 ms: the concurrent suite can fill the table */
      pid = spawn2("CMP.BIN", in_p[0], out_p[1], err_p[1], args);
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
  close(in_p[0]);
  /* Empty stdin: a "-" operand sees immediate EOF instead of blocking. */
  close(in_p[1]);
  close(out_p[1]);
  close(err_p[1]);

  /* Drain stdout first, then stderr.  Kernel pipes hold only 512 bytes;
     a child blocked mid-write on a full stdout pipe would never finish if
     the parent waited for stderr EOF first.  */
  while (n + 1 < outsize) {
    int r = read(out_p[0], out + n, outsize - n - 1);
    if (r <= 0)
      break; /* EOF once the child exits and its fd closes */
    n += (size_t)r;
  }
  close(out_p[0]);
  out[n] = '\0';

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
    int w;
    int guard = 0;
    while ((w = waitpid(pid, &ws, 0)) != pid && guard < 1000) {
      usleep(1000);
      guard++;
    }
    *status = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
  }
  return 0;
}

static void run_slot(int slot, const char *args) {
  int attempt;

  /* Progress marker: with the suite running many programs concurrently a
     silent case is otherwise indistinguishable from a kernel hang.  */
  print_console("[CMPTEST] run: ");
  if (args[0] == '\0')
    print_console("(no args)");
  else
    print_console(args);
  print_console("\n");

  for (attempt = 0; attempt < 3; attempt++) {
    run_out[slot][0] = '\0';
    run_err[slot][0] = '\0';
    run_ok[slot] = run_attempt(args, run_out[slot], sizeof run_out[slot],
                               run_err[slot], sizeof run_err[slot],
                               &run_rc[slot]);
    if (run_ok[slot] == 0)
      return;
    usleep(50000);
  }
}

static void check_run(int slot, const char *what, const char *want_out,
                      int want_rc) {
  if (!run_ok[slot]) {
    fail_head(what);
    print_console("  (spawn CMP.BIN failed)\n");
    return;
  }
  if (want_out != NULL && strcmp(run_out[slot], want_out) != 0) {
    fail_head(what);
    print_console("  stdout: got \"");
    print_preview(run_out[slot]);
    print_console("\" want \"");
    print_preview(want_out);
    print_console("\"\n");
    return;
  }
  if (run_rc[slot] != want_rc) {
    fail_head(what);
    print_console("  exit status ");
    con_int(run_rc[slot]);
    print_console(" != ");
    con_int(want_rc);
    print_console("\n");
    return;
  }
  pass(what);
}

static void check_run_has(int slot, const char *what, const char *sub,
                          int want_rc) {
  if (!run_ok[slot]) {
    fail_head(what);
    print_console("  (spawn CMP.BIN failed)\n");
    return;
  }
  if (sub != NULL && strstr(run_out[slot], sub) == NULL) {
    fail_head(what);
    print_console("  stdout missing \"");
    print_preview(sub);
    print_console("\"; got \"");
    print_preview(run_out[slot]);
    print_console("\"\n");
    return;
  }
  if (run_rc[slot] != want_rc) {
    fail_head(what);
    print_console("  exit status ");
    con_int(run_rc[slot]);
    print_console(" != ");
    con_int(want_rc);
    print_console("\n");
    return;
  }
  pass(what);
}

static void check_run_err_has(int slot, const char *what, const char *sub,
                              int want_rc) {
  if (!run_ok[slot]) {
    fail_head(what);
    print_console("  (spawn CMP.BIN failed)\n");
    return;
  }
  if (sub != NULL && strstr(run_err[slot], sub) == NULL) {
    fail_head(what);
    print_console("  stderr missing \"");
    print_preview(sub);
    print_console("\"; got \"");
    print_preview(run_err[slot]);
    print_console("\"\n");
    return;
  }
  if (run_rc[slot] != want_rc) {
    fail_head(what);
    print_console("  exit status ");
    con_int(run_rc[slot]);
    print_console(" != ");
    con_int(want_rc);
    print_console("\n");
    return;
  }
  pass(what);
}

int main(void) {
  /* Fixtures.  Names fit FAT16 8.3. */
  if (write_file("/CAPA.TXT", "hello\nworld\n", 12) != 0 ||
      write_file("/CAPB.TXT", "hello\nworxd\n", 12) != 0 ||
      write_file("/CAPC.TXT", "hello\nworld\n", 12) != 0 ||
      write_file("/CAPE.TXT", "", 0) != 0 ||
      write_file("/CAPN.TXT", "hello\nworld", 11) != 0 ||
      write_file("/CAPS.TXT", "zzz\nhello\nworld\n", 16) != 0) {
    print_console("[CMPTEST] FAIL fixture setup\n");
    failures++;
  }

  print_console("[CMPTEST] fixtures written\n");

  run_slot(R_IDENT, "/CAPA.TXT /CAPC.TXT");
  run_slot(R_DIFF, "/CAPA.TXT /CAPB.TXT");
  run_slot(R_DIFF_B, "-b /CAPA.TXT /CAPB.TXT");
  run_slot(R_DIFF_L, "-l /CAPA.TXT /CAPB.TXT");
  run_slot(R_DIFF_LB, "-l -b /CAPA.TXT /CAPB.TXT");
  run_slot(R_QUIET, "-s /CAPA.TXT /CAPB.TXT");
  run_slot(R_EOF, "/CAPA.TXT /CAPN.TXT");
  run_slot(R_EMPTY_VS, "/CAPE.TXT /CAPA.TXT");
  run_slot(R_MISSING, "/NOPE.TXT /CAPA.TXT");
  run_slot(R_N_LIMIT, "-n 5 /CAPA.TXT /CAPB.TXT");
  run_slot(R_N_NONE, "-n 4 /CAPA.TXT /CAPB.TXT");
  run_slot(R_IGNORE, "-i 4:0 /CAPS.TXT /CAPA.TXT");
  run_slot(R_STDIN, "- /CAPA.TXT");
  run_slot(R_VERSION, "--version");
  run_slot(R_HELP, "--help");

  check_run(R_IDENT, "identical files", "", 0);
  check_run_has(R_DIFF, "differing files report", "differ: char 10, line 2", 1);
  check_run_has(R_DIFF_B, "-b quotes differing bytes", "is 154", 1);
  check_run_has(R_DIFF_L, "-l lists all differing bytes", "10 154 170", 1);
  check_run_has(R_DIFF_LB, "-l -b lists quoted bytes", "154 l", 1);
  check_run(R_QUIET, "-s prints nothing", "", 1);
  check_run(R_EOF, "EOF on shorter file",
            "", 1);
  check_run_err_has(R_EOF, "EOF message on stderr", "cmp: EOF on /CAPN.TXT", 1);
  check_run(R_EMPTY_VS, "empty vs nonempty", "", 1);
  check_run_err_has(R_EMPTY_VS, "empty EOF message", "cmp: EOF on /CAPE.TXT", 1);
  check_run_err_has(R_MISSING, "missing file diagnostic",
                    "No such file or directory", 2);
  check_run(R_N_LIMIT, "-n 5 within identical prefix", "", 0);
  check_run(R_N_NONE, "-n 4 hits difference", "", 1);
  check_run(R_IGNORE, "-i 4:0 skips prefix", "", 0);
  check_run(R_STDIN, "- operand reads empty stdin", "", 1);
  check_run_err_has(R_STDIN, "- stdin EOF diagnostic", "cmp: EOF on -", 1);
  check_run(R_VERSION, "--version", "cmp (GNU diffutils) 2.8.1\n", 0);
  check_run_has(R_HELP, "--help usage text",
                "Usage: CMP.BIN [OPTION]... FILE1 [FILE2 [SKIP1 [SKIP2]]]",
                0);

  if (failures == 0) {
    print_console("[CMPTEST] all ");
    con_int(R_N);
    print_console(" cases passed\n");
  } else {
    print_console("[CMPTEST] FAILURES: ");
    con_int(failures);
    print_console("\n");
  }
  return failures == 0 ? 0 : 1;
}
