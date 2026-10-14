/* src/user/selfhost/selfhost.c — SELFHOST.BIN: in-OS acceptance build driver.
 *
 * HobbyOS self-hosting program, wave 2 (harness lane; docs/selfhost/
 * acceptance.md §5).  The driver runs the in-OS build stages (S0..S3:
 * check / user / kernel / disk) by executing the commands listed in a
 * command script, and relays every [SELFHOST] phase marker to the serial
 * console so the host verify loop (tools/run_selfhost_build_test.py) can
 * verdict the stages from the serial capture (§8.4: marker substrings +
 * zero FAIL tokens — never exit codes).
 *
 * Script format (one stage per line, '|'-separated; '#' and blank lines
 * are skipped):
 *
 *     <phase>|<tool>|<args...>
 * e.g.  s0-check|MAKE.BIN|-f Makefile.hobbyos check
 *       s1-user|MAKE.BIN|-f Makefile.hobbyos user
 *       s2-kernel|MAKE.BIN|-f Makefile.hobbyos kernel
 *       s3-disk|MAKE.BIN|-f Makefile.hobbyos disk
 *
 * <phase> is the acceptance Sx-name the markers use ("s0-check" -> the
 * "phase=s0-check-start/fail/done" markers).  <tool> is the tool's 8.3
 * binary name; it is resolved (uppercased) against /TOOLS, then /BOOT, then
 * /.  The future in-OS toolchain drops the make binary into /TOOLS; until
 * then every stage cleanly fails at the first missing tool with rc=127
 * instead of hanging — the honest wave-2 behavior with no in-OS make/clang.
 *
 * Markers printed (acceptance §1 vocabulary):
 *   [SELFHOST] DRIVER-UP script=<path>
 *   [SELFHOST] phase=<phase>-start
 *   [SELFHOST] phase=<phase>-done rc=<n>            (tool ran, relay done)
 *   [SELFHOST] phase=<phase>-fail tool=<t> reason=tool-missing rc=127
 *   [SELFHOST] phase=<phase>-fail reason=<why> rc=<n>
 *   [SELFHOST] ABORT stage=<phase> missing-tool=<t>
 *   [SELFHOST] ABORT reason=script-missing
 *   [SELFHOST] FAILED-AT stage=<phase>
 *   [SELFHOST] ACCEPTANCE-COMPLETE stages=<n>       <- the only completion
 * The same lines are mirrored, best-effort, into /SRC/OUT.LOG (evidence
 * recovery, acceptance §5.4).
 *
 * Exit codes (diagnostic only — verdicts live in the markers):
 *   0 = ACCEPTANCE-COMPLETE; 1 = clean staged failure (missing tool or a
 *   tool reported failure); 2 = script missing/unreadable; 3 = usage error.
 */
#include "libc.h"

#define SCRIPT_MAX 16384
#define LINE_MAX 384
#define RELAY_CHUNK 192
#define STAGES_MAX 64

static int log_fd = -1;

/* emit(): one line to the serial console, mirrored into OUT.LOG (best
 * effort).  The whole line is built first and written with a single
 * write so a marker never interleaves mid-token with another core's
 * console output. */
static void emit(const char *s) {
  print_console(s);
  if (log_fd >= 0) {
    write(log_fd, s, (int)strlen(s));
  }
}

static void mark(const char *a, const char *b, const char *c) {
  char buf[LINE_MAX];
  size_t n = 0;
  const char *parts[3];
  int i;
  parts[0] = a;
  parts[1] = b;
  parts[2] = c;
  for (i = 0; i < 3; i++) {
    const char *p = parts[i];
    if (!p) {
      continue;
    }
    while (*p && n < sizeof(buf) - 1) {
      buf[n++] = *p++;
    }
  }
  buf[n] = '\0';
  emit(buf);
}

/* mark_num(): mark(a, <decimal v>, b) with v rendered inline. */
static void mark_num(const char *a, long v, const char *b) {
  char num[24];
  int i = sizeof(num) - 1;
  int neg = 0;
  num[i] = '\0';
  if (v < 0) {
    neg = 1;
    v = -v;
  }
  if (v == 0) {
    num[--i] = '0';
  }
  while (v > 0 && i > 0) {
    num[--i] = (char)('0' + (v % 10));
    v /= 10;
  }
  if (neg && i > 0) {
    num[--i] = '-';
  }
  mark(a, &num[i], b);
}

/* Uppercase the ASCII letters of s in place. */
static void upcase(char *s) {
  while (*s) {
    if (*s >= 'a' && *s <= 'z') {
      *s = (char)(*s - 'a' + 'A');
    }
    s++;
  }
}

/* Resolve <tool> (8.3 binary name, e.g. "MAKE.BIN") to a full path that
 * exists.  Uppercases it and forces a .BIN extension when the name has
 * none, then probes /TOOLS/, /BOOT/ and / in that order.  Uses access()
 * (a stat-based probe — read-only, never creates): a missing tool must NOT
 * be created by the probe, unlike open()'s find-or-create.  Returns 1 and
 * fills out[] on success, 0 otherwise. */
static int resolve_tool(const char *tool, char *out, size_t cap) {
  static const char *dirs[3] = { "/TOOLS/", "/BOOT/", "/" };
  char name[32];
  size_t i = 0;
  size_t len;
  const char *p;
  int d;
  p = tool;
  while (*p) {
    if (*p == '/') {
      i = 0; /* keep only the last path component */
    } else if (i < sizeof(name) - 1) {
      name[i++] = *p;
    }
    p++;
  }
  name[i] = '\0';
  upcase(name);
  len = strlen(name);
  if (len < 5 || name[len - 4] != '.' || len > 12) {
    /* no ".EXT" or a long stem: force the 8.3 form NAME.BIN */
    if (len > 8) {
      len = 8;
    }
    name[len] = '\0';
    strcat(name, ".BIN");
  }
  for (d = 0; d < 3; d++) {
    if (strlen(dirs[d]) + strlen(name) + 1 > cap) {
      continue;
    }
    strcpy(out, dirs[d]);
    strcat(out, name);
    if (access(out, 0) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Run one stage.  Returns 0 on success, 1 on a clean staged failure. */
static int run_stage(const char *phase, const char *tool, const char *args) {
  char path[64];
  int fds[2];
  int pid;
  int status = 0;
  int n;
  char chunk[RELAY_CHUNK + 1];

  mark("[SELFHOST] phase=", phase, "-start\n");

  if (!resolve_tool(tool, path, sizeof(path))) {
    /* First missing tool -> clean, deterministic staged failure (never a
     * hang): wave-2 reality until the in-OS toolchain exists. */
    mark("[SELFHOST] phase=", phase, "-fail tool=");
    emit(tool);
    emit(" reason=tool-missing rc=127\n");
    mark("[SELFHOST] ABORT stage=", phase, " missing-tool=");
    emit(tool);
    emit("\n");
    return 1;
  }

  if (pipe(fds) != 0) {
    mark("[SELFHOST] phase=", phase, "-fail reason=pipe-error\n");
    return 1;
  }
  pid = spawn2(path, 0, fds[1], fds[1], args);
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    mark("[SELFHOST] phase=", phase, "-fail reason=spawn-error\n");
    return 1;
  }
  /* Relay the tool's stdout/stderr to the serial console.  The tool's own
   * [SELFHOST] markers (echoed by Makefile.hobbyos recipes) land on serial
   * through here.  EOF arrives when the tool exits. */
  for (;;) {
    n = read(fds[0], chunk, RELAY_CHUNK);
    if (n <= 0) {
      break;
    }
    chunk[n] = '\0';
    emit(chunk);
  }
  close(fds[0]);
  waitpid(pid, &status, 0);
  if (status != 0) {
    mark("[SELFHOST] phase=", phase, "-fail reason=tool-rc");
    mark_num(" rc=", status, "\n");
    return 1;
  }
  mark("[SELFHOST] phase=", phase, "-done rc=0\n");
  return 0;
}

/* Parse one stage line "phase|tool|args" (mutates line).  Returns 0 on a
 * valid line, -1 on a blank/comment line. */
static int parse_line(char *line, char **phase, char **tool, char **args) {
  char *pipe1;
  char *pipe2;
  while (*line == ' ' || *line == '\t') {
    line++;
  }
  if (*line == '\0' || *line == '#') {
    return -1;
  }
  pipe1 = strchr(line, '|');
  if (!pipe1) {
    return -1; /* no field separator: not a stage line */
  }
  *pipe1 = '\0';
  *phase = line;
  pipe2 = strchr(pipe1 + 1, '|');
  if (pipe2) {
    *pipe2 = '\0';
    *tool = pipe1 + 1;
    *args = pipe2 + 1;
  } else {
    *tool = pipe1 + 1;
    *args = "";
  }
  if (**phase == '\0' || **tool == '\0') {
    return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *script = (argc >= 2 && argv[1]) ? argv[1] : "/SRC/SELFHOST.RUN";
  char *data;
  char *cur;
  char *save;
  char *line;
  char *phase;
  char *tool;
  char *args;
  int fd;
  int n;
  int total = 0;
  int stages = 0;
  const char *failed_phase = NULL;

  emit("[SELFHOST] DRIVER-UP script=");
  emit(script);
  emit("\n");

  /* Best-effort evidence log.  unlink() first: FAT has no truncate, so an
   * old, longer OUT.LOG would keep stale tail bytes (pitfall #10). */
  unlink("/SRC/OUT.LOG");
  log_fd = open("/SRC/OUT.LOG", 0);
  if (log_fd < 0) {
    log_fd = -1;
  }

  fd = open(script, 0);
  if (fd < 0) {
    mark("[SELFHOST] SCRIPT-MISSING path=", script, "\n");
    emit("[SELFHOST] ABORT reason=script-missing\n");
    return 2;
  }
  data = (char *)malloc(SCRIPT_MAX + 1);
  if (!data) {
    emit("[SELFHOST] ABORT reason=oom\n");
    return 3;
  }
  for (;;) {
    n = read(fd, data + total, SCRIPT_MAX - total);
    if (n <= 0 || total >= SCRIPT_MAX) {
      break;
    }
    total += n;
  }
  close(fd);
  data[total] = '\0';

  /* Execute the stages in order; stop at the first staged failure. */
  cur = data;
  while ((line = strtok_r(cur, "\n", &save)) != NULL) {
    cur = NULL;
    if (stages >= STAGES_MAX) {
      emit("[SELFHOST] ABORT reason=too-many-stages\n");
      return 3;
    }
    if (parse_line(line, &phase, &tool, &args) != 0) {
      continue;
    }
    stages++;
    if (run_stage(phase, tool, args) != 0) {
      failed_phase = phase;
      break;
    }
  }

  if (failed_phase) {
    mark("[SELFHOST] FAILED-AT stage=", failed_phase, "\n");
    return 1;
  }
  if (stages == 0) {
    emit("[SELFHOST] ABORT reason=empty-script\n");
    return 3;
  }
  mark("[SELFHOST] ACCEPTANCE-COMPLETE stages=", NULL, NULL);
  mark_num(NULL, (long)stages, "\n");
  return 0;
}
