/*
 * SEDPROBE.BIN — focused in-OS diagnostics for SED.BIN, file-based.
 *
 * Writes its findings to PROBE.LOG (plus one OUT<n> file per probe holding
 * the raw captured bytes) because the UART console interleaves and drops
 * lines under load.  Read the files back out of the disk image with mcopy
 * after the run.
 *
 * Probes 7 and 8 sleep before draining the pipe: if a spawn ever results in
 * two live SED.BIN processes, the byte count doubles and both children's
 * writes appear in one buffer.
 *
 * Runs before the rest of the test suite in test mode, when the process
 * table and physical-block pool are still idle.
 */
#define _GNU_SOURCE 1 /* expose the GNU regex interface sed's pipeline uses */
#include <fcntl.h>
#include <regex.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "libc.h"

/* tiny signed-int printer */
static void con_int(int v) {
  char b[16];
  int i = 15, neg = 0;
  if (v < 0) {
    neg = 1;
    v = (v == -2147483648) ? 2147483647 : -v;
  }
  b[i--] = 0;
  if (v == 0) b[i--] = '0';
  while (v > 0) {
    b[i--] = (char)('0' + v % 10);
    v /= 10;
  }
  if (neg) b[i--] = '-';
  print_console(&b[i + 1]);
}

static char *append_int(char *p, int v) {
  char b[16];
  int i = 15, neg = 0;
  if (v < 0) {
    neg = 1;
    v = -v;
  }
  b[i--] = 0;
  if (v == 0) b[i--] = '0';
  while (v > 0) {
    b[i--] = (char)('0' + v % 10);
    v /= 10;
  }
  if (neg) b[i--] = '-';
  while (b[i + 1]) *p++ = b[++i];
  return p;
}

static void log_line(const char *text) {
  int fd = open("PROBE.LOG", O_WRONLY | O_CREAT);
  if (fd >= 0) {
    lseek(fd, 0, 2 /* SEEK_END */);
    write(fd, text, strlen(text));
    write(fd, "\n", 1);
    close(fd);
  }
}

static void dump_file(const char *path, const char *data, size_t n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd >= 0) {
    if (n) write(fd, data, n);
    close(fd);
  }
}

static int write_file(const char *path, const char *data) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  size_t len = strlen(data), off = 0;
  if (fd < 0) return -1;
  while (off < len) {
    ssize_t w = write(fd, data + off, len - off);
    if (w <= 0) {
      close(fd);
      return -1;
    }
    off += (size_t)w;
  }
  close(fd);
  return 0;
}

/*
 * Run SED.BIN once.  stdout AND stderr are merged into the capture so a
 * silent failure stays visible verbatim.  If sleep_ms > 0, wait that long
 * after closing our stdin before draining the pipe, so any duplicate live
 * child has time to write its copy too.
 */
static void probe(int num, const char *label, const char *args,
                  const char *stdin_data, int sleep_ms) {
  int in_p[2], out_p[2], pid, ws = 0;
  char out[4096];
  char line[256];
  char *p;
  char outn[8];
  size_t n = 0;

  print_console("[SEDPROBE]");
  con_int(num);
  print_console(" ");
  print_console(label);
  print_console("\n");
  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    log_line("pipe fail");
    return;
  }
  pid = spawn2("SED.BIN", in_p[0], out_p[1], out_p[1], args);
  close(in_p[0]);
  if (pid <= 0) {
    close(in_p[1]);
    close(out_p[0]);
    close(out_p[1]);
    p = line;
    memcpy(p, "P", 1);
    p++;
    p = append_int(p, num);
    memcpy(p, " spawn failed", 13);
    p += 13;
    *p = 0;
    log_line(line);
    return;
  }
  if (stdin_data && stdin_data[0]) {
    size_t off = 0, len = strlen(stdin_data);
    while (off < len) {
      ssize_t w = write(in_p[1], stdin_data + off, len - off);
      if (w <= 0) break;
      off += (size_t)w;
    }
  }
  close(in_p[1]);
  close(out_p[1]);
  if (sleep_ms > 0) usleep((unsigned)sleep_ms * 1000u);
  while (n + 1 < sizeof out) {
    int r = read(out_p[0], out + n, sizeof out - n - 1);
    if (r <= 0) break;
    n += (size_t)r;
  }
  close(out_p[0]);
  out[n] = 0;
  waitpid(pid, &ws, 0);

  p = line;
  memcpy(p, "P", 1);
  p++;
  p = append_int(p, num);
  memcpy(p, " pid=", 5);
  p += 5;
  p = append_int(p, pid);
  memcpy(p, " status=", 8);
  p += 8;
  p = append_int(p, ws);
  memcpy(p, " len=", 5);
  p += 5;
  p = append_int(p, (int)n);
  *p = 0;
  log_line(line);

  memcpy(outn, "OUT0\0\0\0", 7);
  outn[3] = (char)('0' + num);
  dump_file(outn, out, n);
}

/*
 * Replicate sed's compile_regex_1 call sequence exactly for a plain
 * s/alpha/ALPHA/ substitution (POSIX basic syntax, no subexpressions
 * needed, no icase, no newline flag), then match with re_search the way
 * sed's execute.c does.  This isolates whether the raw GNU regex
 * interface fails where the POSIX regcomp path (exercised by REGTEST)
 * succeeds.  A malloc soak afterwards catches heap corruption that only
 * shows up after a few hundred mixed allocations.
 */
static void regex_probe(void) {
  char line[256];
  char *p;
  struct re_pattern_buffer pat;
  const char *err;
  reg_syntax_t syntax;
  int r, i, fails;
  regex_t posix_re;

  memset(&pat, 0, sizeof pat);
  syntax = RE_SYNTAX_POSIX_BASIC;
  syntax = (syntax & ~RE_DOT_NOT_NULL) | RE_NO_POSIX_BACKTRACKING | RE_NO_SUB;
  re_set_syntax(syntax);
  pat.fastmap = (char *)malloc(1 << (sizeof(char) * 8));
  err = re_compile_pattern("alpha", 5, &pat);

  p = line;
  memcpy(p, "REGEX compile err=", 18);
  p += 18;
  if (err) {
    memcpy(p, err, strlen(err));
    p += strlen(err);
  } else {
    memcpy(p, "(null)", 6);
    p += 6;
  }
  memcpy(p, " allocated=", 11);
  p += 11;
  p = append_int(p, (int)pat.allocated);
  memcpy(p, " used=", 6);
  p += 6;
  p = append_int(p, (int)pat.used);
  memcpy(p, " nsub=", 6);
  p += 6;
  p = append_int(p, (int)pat.re_nsub);
  *p = 0;
  log_line(line);

  r = re_search(&pat, "alpha beta", 10, 0, 10, NULL);
  p = line;
  memcpy(p, "REGEX search(NULL regs) r=", 26);
  p += 26;
  p = append_int(p, r);
  *p = 0;
  log_line(line);

  /* Second search with a caller-provided register array, the way sed's
     match_regex passes one when subexpressions are needed.  Config A
     mimics the pattern as compiled with needed_sub>0 (no RE_NO_SUB);
     config B keeps RE_NO_SUB to reproduce the suspected unset-pmatch
     behaviour. */
  {
    struct re_pattern_buffer patB;
    struct re_registers rr;
    reg_syntax_t synB;

    memset(&patB, 0, sizeof patB);
    synB = RE_SYNTAX_POSIX_BASIC;
    synB = (synB & ~RE_DOT_NOT_NULL) | RE_NO_POSIX_BACKTRACKING | RE_NO_SUB;
    re_set_syntax(synB);
    patB.fastmap = malloc(1 << (sizeof(char) * 8));
    err = re_compile_pattern("alpha", 5, &patB);
    memset(&rr, 0, sizeof rr);
    patB.regs_allocated = REGS_REALLOCATE;
    r = re_search(&patB, "alpha beta", 10, 0, 10, &rr);
    p = line;
    memcpy(p, "REGEX B(no_sub) r=", 18);
    p += 18;
    p = append_int(p, r);
    memcpy(p, " nr=", 4);
    p += 4;
    p = append_int(p, (int)rr.num_regs);
    if (rr.start) {
      memcpy(p, " s0=", 4);
      p += 4;
      p = append_int(p, (int)rr.start[0]);
      memcpy(p, " e0=", 4);
      p += 4;
      p = append_int(p, (int)rr.end[0]);
    }
    *p = 0;
    log_line(line);

    /* Config C: sub-capable pattern, but the match spans the ENTIRE
       buffer, exactly like sed's line buffer ("alpha", len 5). */
    memset(&patB, 0, sizeof patB);
    synB = RE_SYNTAX_POSIX_BASIC;
    synB = (synB & ~RE_DOT_NOT_NULL) | RE_NO_POSIX_BACKTRACKING;
    re_set_syntax(synB);
    patB.fastmap = malloc(1 << (sizeof(char) * 8));
    err = re_compile_pattern("alpha", 5, &patB);
    memset(&rr, 0, sizeof rr);
    patB.regs_allocated = REGS_REALLOCATE;
    r = re_search(&patB, "alpha", 5, 0, 5, &rr);
    p = line;
    memcpy(p, "REGEX C(whole) r=", 17);
    p += 17;
    p = append_int(p, r);
    memcpy(p, " nr=", 4);
    p += 4;
    p = append_int(p, (int)rr.num_regs);
    if (rr.start) {
      memcpy(p, " s0=", 4);
      p += 4;
      p = append_int(p, (int)rr.start[0]);
      memcpy(p, " e0=", 4);
      p += 4;
      p = append_int(p, (int)rr.end[0]);
    }
    *p = 0;
    log_line(line);

    /* Config A: same pattern but WITHOUT RE_NO_SUB (needed_sub > 0). */
    memset(&patB, 0, sizeof patB);
    synB = RE_SYNTAX_POSIX_BASIC;
    synB = (synB & ~RE_DOT_NOT_NULL) | RE_NO_POSIX_BACKTRACKING;
    re_set_syntax(synB);
    patB.fastmap = malloc(1 << (sizeof(char) * 8));
    err = re_compile_pattern("alpha", 5, &patB);
    memset(&rr, 0, sizeof rr);
    patB.regs_allocated = REGS_REALLOCATE;
    r = re_search(&patB, "alpha beta", 10, 0, 10, &rr);
    p = line;
    memcpy(p, "REGEX A(sub) r=", 15);
    p += 15;
    p = append_int(p, r);
    memcpy(p, " nr=", 4);
    p += 4;
    p = append_int(p, (int)rr.num_regs);
    if (rr.start) {
      memcpy(p, " s0=", 4);
      p += 4;
      p = append_int(p, (int)rr.start[0]);
      memcpy(p, " e0=", 4);
      p += 4;
      p = append_int(p, (int)rr.end[0]);
    }
    *p = 0;
    log_line(line);
  }

  /* POSIX path for contrast (this is what REGTEST exercises). */
  memset(&posix_re, 0, sizeof posix_re);
  r = regcomp(&posix_re, "alpha", 0);
  p = line;
  memcpy(p, "POSIX regcomp r=", 16);
  p += 16;
  p = append_int(p, r);
  *p = 0;
  log_line(line);
  if (r == 0) regfree(&posix_re);

  /* malloc soak: sed-like mixed sizes, write to each block to catch
     invalid pointers that the kernel would reject on a syscall. */
  fails = 0;
  for (i = 0; i < 500; i++) {
    int size = 16 + (i * 53) % 4000;
    char *q = (char *)malloc((size_t)size);
    if (!q) {
      fails++;
      continue;
    }
    memset(q, (char)i, (size_t)size);
    free(q);
  }
  p = line;
  memcpy(p, "SOAK fail=", 10);
  p += 10;
  p = append_int(p, fails);
  *p = 0;
  log_line(line);
}

int main(void) {
  print_console("[SEDPROBE] SED.BIN in-OS diagnostics (file-based)\n");
  dump_file("PROBE.LOG", "", 0); /* fresh log */

  if (write_file("SEDT.IN", "alpha\nbeta\ngamma\ndelta\n") != 0)
    log_line("fixture write FAILED");

  regex_probe();

  probe(0, "p FILE", "p SEDT.IN", "", 0);
  probe(1, "-n p FILE", "-n p SEDT.IN", "", 0);
  probe(2, "s/// FILE", "s/alpha/ALPHA/ SEDT.IN", "", 0);
  probe(3, "s/// STDIN", "s/alpha/ALPHA/", "alpha\nbeta\n", 0);
  probe(4, "1s/// FILE", "1s/alpha/ALPHA/ SEDT.IN", "", 0);
  probe(5, "y FILE", "y/abc/ABC/ SEDT.IN", "", 0);
  probe(6, "p FILE (sleep 2s)", "p SEDT.IN", "", 2000);
  probe(7, "s/// FILE (sleep 2s)", "s/alpha/ALPHA/ SEDT.IN", "", 2000);
  probe(8, "s backref", "s/\\(al\\)\\(ph\\)/\\2\\1/ SEDT.IN", "", 0);

  log_line("[SEDPROBE] done");
  print_console("[SEDPROBE] done\n");
  return 0;
}
