/* expr -- evaluate an expression (GNU coreutils parity, loose).
   Fresh port for the HobbyOS userland.  Understands the POSIX operator
   set with left-to-right precedence:
     |   &   = != < <= > >=   + -   * / %
   plus the string functions length / substr / index / match / : (regexp).
   Integers are 64-bit; division/modulo by zero print an error and exit 2. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <errno.h>

static int g_pos = 1; /* next argv index to consume */
static int g_argc;
static char **g_argv;
static int g_out_of_operands = 0;

static const char *next_arg(void) {
  if (g_pos >= g_argc) { g_out_of_operands = 1; return NULL; }
  return g_argv[g_pos++];
}

static int is_number(const char *s) {
  if (!s || !*s) return 0;
  const char *p = s;
  if (*p == '-' || *p == '+') p++;
  if (!*p) return 0;
  while (*p) {
    if (*p < '0' || *p > '9') return 0;
    p++;
  }
  return 1;
}

/* parses "left OP right" style binary expressions with the POSIX
   precedence levels.  Returns the string result (malloc'd or static). */
static char *expr_or(void);
static char *expr_and(void);
static char *expr_cmp(void);
static char *expr_add(void);
static char *expr_mul(void);

/* lower precedence: '|' */
static char *expr_or(void) {
  char *l = expr_and();
  if (!l) return NULL;
  for (;;) {
    const char *op = next_arg();
    if (!op || strcmp(op, "|") != 0) {
      if (op) g_pos--;
      return l;
    }
    char *r = expr_and();
    if (!r) { free(l); return NULL; }
    long a = atol(l), b = atol(r);
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", (a || b) ? 1L : 0L);
    free(l); free(r);
    l = strdup(buf);
  }
}

/* '&' */
static char *expr_and(void) {
  char *l = expr_cmp();
  if (!l) return NULL;
  for (;;) {
    const char *op = next_arg();
    if (!op || strcmp(op, "&") != 0) {
      if (op) g_pos--;
      return l;
    }
    char *r = expr_cmp();
    if (!r) { free(l); return NULL; }
    long a = atol(l), b = atol(r);
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", (a && b) ? 1L : 0L);
    free(l); free(r);
    l = strdup(buf);
  }
}

/* string/integer comparisons: = != < <= > >= */
static char *expr_cmp(void) {
  char *l = expr_add();
  if (!l) return NULL;
  for (;;) {
    const char *op = next_arg();
    if (!op) { return l; }
    if (strcmp(op, "=") != 0 && strcmp(op, "!=") != 0 &&
        strcmp(op, "<") != 0 && strcmp(op, "<=") != 0 &&
        strcmp(op, ">") != 0 && strcmp(op, ">=") != 0) {
      g_pos--;
      return l;
    }
    char *r = expr_add();
    if (!r) { free(l); return NULL; }
    int numeric = is_number(l) && is_number(r);
    long res;
    if (numeric) {
      long a = atol(l), b = atol(r);
      if (strcmp(op, "=") == 0) res = (a == b);
      else if (strcmp(op, "!=") == 0) res = (a != b);
      else if (strcmp(op, "<") == 0) res = (a < b);
      else if (strcmp(op, "<=") == 0) res = (a <= b);
      else if (strcmp(op, ">") == 0) res = (a > b);
      else res = (a >= b);
    } else {
      int c = strcmp(l, r);
      if (strcmp(op, "=") == 0) res = (c == 0);
      else if (strcmp(op, "!=") == 0) res = (c != 0);
      else if (strcmp(op, "<") == 0) res = (c < 0);
      else if (strcmp(op, "<=") == 0) res = (c <= 0);
      else if (strcmp(op, ">") == 0) res = (c > 0);
      else res = (c >= 0);
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", res);
    free(l); free(r);
    l = strdup(buf);
  }
}

/* + - */
static char *expr_add(void) {
  char *l = expr_mul();
  if (!l) return NULL;
  for (;;) {
    const char *op = next_arg();
    if (!op) return l;
    if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0) {
      g_pos--;
      return l;
    }
    char *r = expr_mul();
    if (!r) { free(l); return NULL; }
    long a = atol(l), b = atol(r);
    char buf[32];
    if (strcmp(op, "+") == 0) snprintf(buf, sizeof buf, "%ld", a + b);
    else snprintf(buf, sizeof buf, "%ld", a - b);
    free(l); free(r);
    l = strdup(buf);
  }
}

/* * / % (highest) */
static char *expr_mul(void) {
  /* operand: a string function, a ":" regexp match, or a literal/number */
  const char *a = next_arg();
  if (!a) return NULL;
  char *v = NULL;
  if (strcmp(a, "length") == 0) {
    const char *s = next_arg();
    if (!s) return NULL;
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)strlen(s));
    v = strdup(buf);
  } else if (strcmp(a, "index") == 0) {
    const char *s = next_arg();
    const char *chars = next_arg();
    if (!s || !chars) return NULL;
    long pos = 0;
    for (const char *p = s; *p; p++) {
      if (strchr(chars, *p)) { pos = (long)(p - s) + 1; break; }
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", pos);
    v = strdup(buf);
  } else if (strcmp(a, "substr") == 0) {
    const char *s = next_arg();
    const char *p1 = next_arg();
    const char *p2 = next_arg();
    if (!s || !p1 || !p2) return NULL;
    long start = atol(p1), len = atol(p2);
    if (start < 1) start = 1;
    if (len < 0) len = 0;
    size_t sl = strlen(s);
    char *out = (char *)malloc((size_t)len + 1);
    size_t i;
    for (i = 0; i < (size_t)len && (size_t)(start - 1 + i) < sl; i++)
      out[i] = s[start - 1 + i];
    out[i] = 0;
    v = strdup(out);
    free(out);
  } else if (strcmp(a, "match") == 0 || strcmp(a, ":") == 0) {
    /* regexp match: prints length of match, 0 if no match */
    const char *s = next_arg();
    const char *re = next_arg();
    if (!s || !re) return NULL;
    regmatch_t m;
    regex_t rx;
    int rc = regcomp(&rx, re, REG_EXTENDED);
    if (rc != 0) {
      regfree(&rx);
      char buf[32];
      snprintf(buf, sizeof buf, "expr: invalid regular expression: %s", re);
      fprintf(stderr, "%s\n", buf);
      return NULL;
    }
    long mm = 0;
    if (regexec(&rx, s, 1, &m, 0) == 0) mm = (long)(m.rm_eo - m.rm_so);
    regfree(&rx);
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", mm);
    v = strdup(buf);
  } else {
    v = strdup(a);
    /* if the previous token was an operator, this is an operand — fine */
  }
  if (!v) return NULL;

  for (;;) {
    const char *op = next_arg();
    if (!op) return v;
    if (strcmp(op, ":") == 0) {
      /* expr STR : REGEX  -- regexp match, prints match length */
      const char *re = next_arg();
      if (!re) { free(v); return NULL; }
      regmatch_t m;
      regex_t rx;
      int rc = regcomp(&rx, re, REG_EXTENDED);
      long mm = 0;
      if (rc == 0) {
        if (regexec(&rx, v, 1, &m, 0) == 0) mm = (long)(m.rm_eo - m.rm_so);
        regfree(&rx);
      }
      char buf[32];
      snprintf(buf, sizeof buf, "%ld", mm);
      free(v);
      v = strdup(buf);
      continue;
    }
    if (strcmp(op, "*") != 0 && strcmp(op, "/") != 0 && strcmp(op, "%") != 0) {
      g_pos--;
      return v;
    }
    char *r = expr_mul();
    if (!r) { free(v); return NULL; }
    long a2 = atol(v), b2 = atol(r);
    if ((strcmp(op, "/") == 0 || strcmp(op, "%") == 0) && b2 == 0) {
      fprintf(stderr, "expr: division by zero\n");
      free(v); free(r);
      return NULL;
    }
    char buf[32];
    if (strcmp(op, "*") == 0) snprintf(buf, sizeof buf, "%ld", a2 * b2);
    else if (strcmp(op, "/") == 0) snprintf(buf, sizeof buf, "%ld", a2 / b2);
    else snprintf(buf, sizeof buf, "%ld", a2 - (a2 / b2) * b2);
    free(v); free(r);
    v = strdup(buf);
  }
}

int main(int argc, char **argv) {
  g_argc = argc;
  g_argv = argv;
  if (argc < 2) {
    fprintf(stderr, "expr: missing operand\n");
    return 2;
  }
  char *res = expr_or();
  if (!res) {
    if (g_out_of_operands)
      fprintf(stderr, "expr: syntax error\n");
    return 2;
  }
  /* trailing tokens? */
  if (g_pos < g_argc) {
    fprintf(stderr, "expr: syntax error\n");
    free(res);
    return 2;
  }
  fputs(res, stdout);
  fputc('\n', stdout);
  /* POSIX: `expr` exits 1 when the result is the empty string or 0 */
  int rc = (strcmp(res, "0") == 0 || res[0] == '\0') ? 1 : 0;
  free(res);
  return rc;
}
