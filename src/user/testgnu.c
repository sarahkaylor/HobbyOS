/* test -- evaluate a conditional expression (GNU coreutils parity, loose).
   Fresh port for the HobbyOS userland; also usable as `[` (auto-detect
   argv[0] ending in ']' convention is N/A here: programs run as TEST.BIN,
   so `[` is the shell builtin; this binary implements the test(1) grammar).

   Supports: unary  -e -f -d -r -w -x -s -L -n -z
             binary  = != (strings), -eq -ne -lt -le -gt -ge (ints),
                     -ef
             ops     ! , -a , -o and ( expr ) / ( expr )   (no -a/-o here;
             POSIX says use && ||, but loose parity keeps -a/-o)
             default 1 arg  -> nonempty; 2 args -> unary/!/-n/-z;
             3 args -> binary or ! unary; 4+ -> parenthesis handling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int eval_and(int *pos, int end, char **argv, int depth);

static int do_eval(int *pos, int end, char **argv, int depth);
static const char *arg_at(int *pos, int end, char **argv) {
  if (*pos >= end) return NULL;
  return argv[(*pos)++];
}

static int file_test(const char *op, const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  if (strcmp(op, "-e") == 0) return 1;
  if (strcmp(op, "-f") == 0) return S_ISREG(st.st_mode);
  if (strcmp(op, "-d") == 0) return S_ISDIR(st.st_mode);
  if (strcmp(op, "-L") == 0) return S_ISLNK(st.st_mode);
  if (strcmp(op, "-r") == 0) return (access(path, R_OK) == 0);
  if (strcmp(op, "-w") == 0) return (access(path, W_OK) == 0);
  if (strcmp(op, "-x") == 0) return (access(path, X_OK) == 0);
  if (strcmp(op, "-s") == 0) return (st.st_size > 0);
  return 0;
}

static int is_number(const char *s) {
  if (!s || !*s) return 0;
  if (*s == '-' || *s == '+') s++;
  if (!*s) return 0;
  while (*s) {
    if (*s < '0' || *s > '9') return 0;
    s++;
  }
  return 1;
}

/* evaluate a top-level OR list (-o). */
static int eval_or(int *pos, int end, char **argv, int depth) {
  int l = eval_and(pos, end, argv, depth);
  for (;;) {
    const char *op = arg_at(pos, end, argv);
    if (!op) return l;
    if (strcmp(op, "-o") != 0) { (*pos)--; return l; }
    int r = eval_and(pos, end, argv, depth);
    l = l || r;
  }
}

static int eval_and(int *pos, int end, char **argv, int depth) {
  int l = do_eval(pos, end, argv, depth);
  for (;;) {
    const char *op = arg_at(pos, end, argv);
    if (!op) return l;
    if (strcmp(op, "-a") != 0) { (*pos)--; return l; }
    int r = do_eval(pos, end, argv, depth);
    l = l && r;
  }
}

static int do_eval(int *pos, int end, char **argv, int depth) {
  const char *a = arg_at(pos, end, argv);
  if (!a) return 0;
  if (strcmp(a, "!") == 0) {
    int v = do_eval(pos, end, argv, depth);
    return !v;
  }
  if (strcmp(a, "(") == 0) {
    int v = eval_or(pos, end, argv, depth + 1);
    const char *c = arg_at(pos, end, argv);
    if (c && strcmp(c, ")") == 0) return v;
    (*pos)--; /* tolerate missing ')' */
    return v;
  }
  if (strcmp(a, ")") == 0 && depth > 0) {
    (*pos)--;
    return 0;
  }

  /* unary */
  if (a[0] == '-' && a[1] != '\0' && a[2] == '\0') {
    const char *ol = a + 1;
    const char *operand = arg_at(pos, end, argv);
    if (!operand) return 0;
    if (ol[0] == 'n') return (operand[0] != '\0');
    if (ol[0] == 'z') return (operand[0] == '\0');
    if (ol[0] == 'e' || ol[0] == 'f' || ol[0] == 'd' || ol[0] == 'r' ||
        ol[0] == 'w' || ol[0] == 'x' || ol[0] == 's')
      return file_test(a, operand);
    if (ol[0] == 'L') return file_test(a, operand);
    /* unknown unary: treat whole thing as a string test */
    return 1;
  }

  /* binary (need at least 2 more tokens) */
  if (*pos >= end) return (a[0] != '\0'); /* single operand: nonempty */
  const char *op = arg_at(pos, end, argv);
  const char *b = arg_at(pos, end, argv);
  if (!b) {
    (*pos)--; /* only one more token: `a op` invalid -> treat a as truthy */
    (*pos)--;
    return (a[0] != '\0');
  }
  if (strcmp(op, "=") == 0) return (strcmp(a, b) == 0);
  if (strcmp(op, "!=") == 0) return (strcmp(a, b) != 0);
  if (strcmp(op, "-eq") == 0 ||
      strcmp(op, "-ne") == 0 || strcmp(op, "-lt") == 0 ||
      strcmp(op, "-le") == 0 || strcmp(op, "-gt") == 0 ||
      strcmp(op, "-ge") == 0) {
    long x = atol(a), y = atol(b);
    if (strcmp(op, "-eq") == 0) return x == y;
    if (strcmp(op, "-ne") == 0) return x != y;
    if (strcmp(op, "-lt") == 0) return x < y;
    if (strcmp(op, "-le") == 0) return x <= y;
    if (strcmp(op, "-gt") == 0) return x > y;
    return x >= y;
  }
  if (strcmp(op, "-ef") == 0) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 0;
    return (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino);
  }
  if (strcmp(op, "<") == 0) return (strcmp(a, b) < 0);
  if (strcmp(op, ">") == 0) return (strcmp(a, b) > 0);
  /* bad syntax: a op b with unknown op -> syntax error, exit 2 */
  return -1;
}

int main(int argc, char **argv) {
  int end = argc; /* skip argv[0] */
  int pos = 1;
  int r;
  if (argc == 1) return 1; /* test with no args = false */
  r = eval_or(&pos, end, argv, 0);
  if (r == -1) {
    fprintf(stderr, "test: syntax error\n");
    return 2;
  }
  return r ? 0 : 1;
}
