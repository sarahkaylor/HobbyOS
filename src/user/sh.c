/* src/user/sh.c - HobbyOS shell
 *
 * A POSIX-style scripting shell for HobbyOS.  Supports the constructs a
 * Linux base-install user actually scripts against:
 *
 *   - word splitting, quotes ('...' and "..."), backslash escapes, #comments
 *   - variable expansion: $VAR, ${VAR}, ${VAR:-def}, ${VAR:=def}, $?,
 *     $#, $$, $0..$9, ${10..}, $@, $*
 *   - command substitution: $(...) and `...`
 *   - control flow: if/then/elif/else/fi, while/until/do/done,
 *     for VAR in .../do/done, case/;;/esac, { list; }, ( subshell )
 *   - pipelines (N stages), && / || / ; operators, background & and
 *     the `wait` builtin, redirections > >> < 2> 2>>
 *   - functions: name() { ... }, globbing of * ? [..]
 *   - builtins: cd pwd echo printf exit export unset set shift read test [
 *     true false : umask wait help clear.  External commands resolve by
 *     the shell's 8.3 uppercased name (cwd first, then /) and run through
 *     fork + execv so quoted arguments survive exactly.
 *
 * Interactive mode speaks the desktop protocol exactly like the original
 * shell (prompt "user@hobbyos:<cwd>$ ", per-keystroke echo, ESC sequences
 * swallowed) so the console and the boot-test wave keep working.
 *
 * Parsing model: a tokenizer produces words that PRESERVE their quoting
 * (single/double/backslash markers are kept in the token text; a `quoted`
 * flag says whether the word used any quoting).  A recursive-descent
 * parser builds a small AST; the executor walks it.  Expansion is done at
 * execution time by a quote-aware scanner.
 *
 * This is original HobbyOS code (no GNU sources); the `test` builtin
 * implements the POSIX shell-conditional grammar.
 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include "libc.h"
#ifdef HOST_TEST
#include <unistd.h>
#include <sys/syscall.h>
#include <dirent.h>
extern unsigned int umask(unsigned int); /* avoid sys/stat.h (mkdir clash) */
#endif

#define SH_MAX_LINE     2048
#define SH_MAX_TOKENS   1024
#define SH_MAX_WORDS    64
#define SH_MAX_REDIRS   8
#define SH_MAX_VARS     96
#define SH_MAX_FUNCS    32
#define SH_MAX_BG       16
#define SH_MAX_CAPTURE  8192
#define SH_CAPTURE_DEPTH 6

/* ------------------------------------------------------------------ */
/* small local string helpers (freestanding discipline)                */
/* ------------------------------------------------------------------ */

static int s_len(const char *s) {
  int n = 0;
  while (s && s[n]) n++;
  return n;
}

static char *s_dup(const char *s) {
  if (!s) return 0;
  int n = s_len(s);
  char *d = (char *)malloc((size_t)n + 1);
  if (!d) return d;
  for (int i = 0; i <= n; i++) d[i] = s[i];
  return d;
}

static int s_eq(const char *a, const char *b) {
  if (!a || !b) return a == b;
  while (*a && *b && *a == *b) { a++; b++; }
  return *a == *b;
}

static char *s_cat(const char *a, const char *b) {
  int na = s_len(a), nb = s_len(b);
  char *d = (char *)malloc((size_t)na + nb + 1);
  if (!d) return d;
  for (int i = 0; i < na; i++) d[i] = a[i];
  for (int i = 0; i < nb; i++) d[na + i] = b[i];
  d[na + nb] = 0;
  return d;
}

static int itoa10(int v, char *buf, int cap) {
  char tmp[16];
  int i = 0;
  int neg = 0;
  if (v < 0) { neg = 1; }
  unsigned int u = (unsigned int)(neg ? -v : v);
  if (u == 0) tmp[i++] = '0';
  while (u > 0 && i < 15) { tmp[i++] = (char)('0' + (u % 10)); u /= 10; }
  int n = 0;
  if (neg && n < cap - 1) buf[n++] = '-';
  while (i > 0 && n < cap - 1) buf[n++] = tmp[--i];
  buf[n] = 0;
  return n;
}

static void out1(const char *s) { write(1, s, s_len(s)); }
static void out2(const char *s) { write(2, s, s_len(s)); }

/* ------------------------------------------------------------------ */
/* variables / positional parameters / functions                       */
/* ------------------------------------------------------------------ */

struct shell_var {
  char *name;
  char *val;
  int exported;
};

static struct shell_var g_vars[SH_MAX_VARS];
static int g_nvars = 0;
static int g_laststatus = 0;
static int g_argv0_set = 0;
static char g_argv0_buf[32] = "sh";

/* positional parameters ($1..$n, ${10}..); $0 is the shell's own name. */
static char **g_pos = 0;   /* 0-indexed = $1.. */
static int g_npos = 0;

struct node; /* forward */

struct func_def {
  char *name;
  struct node *body;
  int defined;
};

static struct func_def g_funcs[SH_MAX_FUNCS];
static int g_func_n = 0;

static struct shell_var *var_find(const char *name) {
  for (int i = 0; i < g_nvars; i++) {
    if (s_eq(g_vars[i].name, name)) return &g_vars[i];
  }
  return 0;
}

static void var_set_val(const char *name, int namelen, const char *val,
                        int exported);
static void var_set(const char *name, const char *val, int exported) {
  var_set_val(name, s_len(name), val, exported);
}

/* var_set with an explicit name length (name need not be NUL-terminated) */
static void var_set_val(const char *name, int namelen, const char *val,
                        int exported) {
  char nbuf[96];
  int nl = namelen;
  if (nl > 95) nl = 95;
  for (int i = 0; i < nl; i++) nbuf[i] = name[i];
  nbuf[nl] = 0;
  struct shell_var *v = var_find(nbuf);
  if (!v) {
    if (g_nvars >= SH_MAX_VARS) return;
    v = &g_vars[g_nvars++];
    v->name = s_dup(nbuf);
    v->val = 0;
    v->exported = 0;
  }
  if (v->val) free(v->val);
  v->val = s_dup(val ? val : "");
  if (exported) v->exported = 1;
}

static const char *var_get(const char *name) {
  struct shell_var *v = var_find(name);
  if (v) return v->val ? v->val : "";
  return 0;
}

static void var_unset(const char *name) {
  for (int i = 0; i < g_nvars; i++) {
    if (s_eq(g_vars[i].name, name)) {
      if (g_vars[i].val) free(g_vars[i].val);
      if (g_vars[i].name) free(g_vars[i].name);
      for (int j = i; j < g_nvars - 1; j++) g_vars[j] = g_vars[j + 1];
      g_nvars--;
      return;
    }
  }
}

static void pos_set_all(char **argv, int n) {
  for (int i = 0; i < g_npos; i++) {
    if (g_pos && g_pos[i]) free(g_pos[i]);
  }
  if (g_pos) free(g_pos);
  g_pos = 0;
  g_npos = 0;
  if (n <= 0) return;
  g_pos = (char **)malloc((size_t)n * sizeof(char *));
  if (!g_pos) return;
  for (int i = 0; i < n; i++) g_pos[i] = s_dup(argv[i] ? argv[i] : "");
  g_npos = n;
}

static void pos_shift(int k) {
  if (k >= g_npos) {
    pos_set_all(0, 0);
    return;
  }
  int nn = g_npos - k;
  char **np = (char **)malloc((size_t)nn * sizeof(char *));
  if (!np) {
    pos_set_all(0, 0);
    return;
  }
  for (int i = 0; i < nn; i++) np[i] = s_dup(g_pos[i + k]);
  for (int i = 0; i < g_npos; i++) {
    if (g_pos[i]) free(g_pos[i]);
  }
  free(g_pos);
  g_pos = np;
  g_npos = nn;
}

static const char *pos_get(int idx) {
  if (idx < 1 || idx > g_npos) return "";
  return g_pos[idx - 1] ? g_pos[idx - 1] : "";
}

/* ------------------------------------------------------------------ */
/* tokenizer                                                           */
/* ------------------------------------------------------------------ */

enum {
  T_WORD, T_SEMI, T_DSEMI, T_AND, T_OR, T_PIPE, T_BG,
  T_LT, T_GT, T_GTGT, T_ERRGT, T_ERRGTGT, T_DUPGT, T_ERRDUP,
  T_LP, T_RP, T_NL, T_EOF
};

struct token {
  int type;
  char *text;
  int quoted;
};

static struct token g_tok[SH_MAX_TOKENS];
static int g_ntok = 0;
static int g_tokpos = 0;

static void tok_reset(void) {
  g_ntok = 0;
  g_tokpos = 0;
}

static int is_opchar(char c) {
  switch (c) {
  case ';': case '&': case '|': case '>': case '<': case '(': case ')':
    return 1;
  default:
    return 0;
  }
}

/* Append a command-substitution region verbatim to the word buffer,
 * consuming balanced parentheses for $(...) or a backquoted region. */
static int scan_cmdsubst(const char *text, int *ip, char *buf, int *bp, int cap) {
  int i = *ip;
  /* text[i] is '$' followed by '(' or a backtick */
  if (text[i] == '$') {
    int dbp = (text[i + 2] == '('); /* $(( ... )) double-paren closer */
    if (*bp < cap - 1) buf[(*bp)++] = '$';
    if (*bp < cap - 1) buf[(*bp)++] = '(';
    i += 2;
    int depth = 1;
    while (text[i] && depth > 0) {
      char c = text[i];
      if (c == '\'' || c == '"') {
        /* skip a quoted region without counting parens inside it */
        char q = c;
        if (*bp < cap - 1) buf[(*bp)++] = c;
        i++;
        while (text[i] && text[i] != q) {
          if (*bp < cap - 1) buf[(*bp)++] = text[i];
          i++;
        }
        if (text[i] == q) {
          if (*bp < cap - 1) buf[(*bp)++] = q;
          i++;
        }
        continue;
      }
      if (c == '$' && text[i + 1] == '(') depth++;
      else if (c == ')' && depth == 1 && dbp && text[i + 1] == ')') {
        /* closing "))" of an arithmetic expansion */
        if (*bp < cap - 1) buf[(*bp)++] = c;
        if (text[i + 1] == ')') {
          if (*bp < cap - 1) buf[(*bp)++] = ')';
          i += 2;
        }
        depth = 0;
        break;
      } else if (c == ')') depth--;
      if (*bp < cap - 1) buf[(*bp)++] = c;
      i++;
    }
    if (depth > 0) return -1; /* unterminated */
  } else {
    /* backtick */
    if (*bp < cap - 1) buf[(*bp)++] = '`';
    i++;
    while (text[i] && text[i] != '`') {
      if (*bp < cap - 1) buf[(*bp)++] = text[i];
      i++;
    }
    if (text[i] == '`') {
      if (*bp < cap - 1) buf[(*bp)++] = '`';
      i++;
    } else {
      return -1;
    }
  }
  *ip = i;
  return 0;
}

/* returns 0 on success, -1 on unterminated quote/backslash or overflow */
static int sh_tokenize(const char *text) {
  int i = 0;
  g_ntok = 0;
  while (text[i]) {
    char c = text[i];

    if (c == '#') {
      while (text[i] && text[i] != '\n') i++;
      continue;
    }
    if (c == '\n') {
      if (g_ntok < SH_MAX_TOKENS) {
        g_tok[g_ntok].type = T_NL;
        g_tok[g_ntok].text = 0;
        g_tok[g_ntok].quoted = 0;
        g_ntok++;
      }
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    if (c == ';') {
      g_tok[g_ntok].type = (text[i + 1] == ';') ? T_DSEMI : T_SEMI;
      i += (text[i + 1] == ';') ? 2 : 1;
      g_tok[g_ntok].text = 0;
      g_tok[g_ntok].quoted = 0;
      g_ntok++;
      continue;
    }
    if (c == '&') {
      g_tok[g_ntok].type = (text[i + 1] == '&') ? T_AND : T_BG;
      i += (text[i + 1] == '&') ? 2 : 1;
      g_tok[g_ntok].text = 0;
      g_tok[g_ntok].quoted = 0;
      g_ntok++;
      continue;
    }
    if (c == '|') {
      g_tok[g_ntok].type = (text[i + 1] == '|') ? T_OR : T_PIPE;
      i += (text[i + 1] == '|') ? 2 : 1;
      g_tok[g_ntok].text = 0;
      g_tok[g_ntok].quoted = 0;
      g_ntok++;
      continue;
    }
    if (c == '>') {
      if (text[i + 1] == '&') {
        g_tok[g_ntok].type = T_DUPGT; /* >&N : dup fd 1 from N */
        i += 2;
        g_tok[g_ntok].text = 0;
        g_tok[g_ntok].quoted = 0;
        g_ntok++;
        continue;
      }
      g_tok[g_ntok].type = (text[i + 1] == '>') ? T_GTGT : T_GT;
      i += (text[i + 1] == '>') ? 2 : 1;
      g_tok[g_ntok].text = 0;
      g_tok[g_ntok].quoted = 0;
      g_ntok++;
      continue;
    }
    if (c == '<') {
      if (text[i + 1] == '<') {
        /* heredoc: unsupported.  Swallow past the delimiter line so the
           parse fails cleanly rather than eating the script. */
        i += 2;
        while (text[i] && text[i] != '\n') i++;
        continue;
      }
      g_tok[g_ntok].type = T_LT;
      i++;
      g_tok[g_ntok].text = 0;
      g_tok[g_ntok].quoted = 0;
      g_ntok++;
      continue;
    }
    if (c == '(') {
      g_tok[g_ntok].type = T_LP;
      i++;
      g_tok[g_ntok].text = 0;
      g_tok[g_ntok].quoted = 0;
      g_ntok++;
      continue;
    }
    if (c == ')') {
      g_tok[g_ntok].type = T_RP;
      i++;
      g_tok[g_ntok].text = 0;
      g_tok[g_ntok].quoted = 0;
      g_ntok++;
      continue;
    }

    if (c == '2' && text[i + 1] == '>') {
      if (text[i + 2] == '&') {
        g_tok[g_ntok].type = T_ERRDUP; /* 2>&N : dup fd 2 from N */
        i += 3;
        g_tok[g_ntok].text = 0;
        g_tok[g_ntok].quoted = 0;
        g_ntok++;
        continue;
      }
      g_tok[g_ntok].type = (text[i + 2] == '>') ? T_ERRGTGT : T_ERRGT;
      i += (text[i + 2] == '>') ? 3 : 2;
      g_tok[g_ntok].text = 0;
      g_tok[g_ntok].quoted = 0;
      g_ntok++;
      continue;
    }

    /* a word: preserve quoting markers; expand later */
    {
      char buf[SH_MAX_LINE];
      int b = 0;
      int quoted = 0;
      for (;;) {
        c = text[i];
        if (c == 0) break;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            is_opchar(c)) {
          break;
        }
        if (c == '\'') {
          quoted = 1;
          if (b < SH_MAX_LINE - 1) buf[b++] = '\'';
          i++;
          int closed = 0;
          while (text[i]) {
            if (text[i] == '\'') {
              if (b < SH_MAX_LINE - 1) buf[b++] = '\'';
              i++;
              closed = 1;
              break;
            }
            if (b < SH_MAX_LINE - 1) buf[b++] = text[i];
            i++;
          }
          if (!closed) return -1;
        } else if (c == '"') {
          quoted = 1;
          if (b < SH_MAX_LINE - 1) buf[b++] = '"';
          i++;
          int closed = 0;
          while (text[i]) {
            if (text[i] == '"') {
              if (b < SH_MAX_LINE - 1) buf[b++] = '"';
              i++;
              closed = 1;
              break;
            }
            if (text[i] == '\\' && (text[i + 1] == '"' || text[i + 1] == '$' ||
                                    text[i + 1] == '`' || text[i + 1] == '\\')) {
              if (b < SH_MAX_LINE - 1) buf[b++] = '\\';
              if (b < SH_MAX_LINE - 1) buf[b++] = text[i + 1];
              i += 2;
            } else if (text[i] == '$' && text[i + 1] == '(') {
              if (scan_cmdsubst(text, &i, buf, &b, SH_MAX_LINE) != 0) return -1;
            } else if (text[i] == '`') {
              if (scan_cmdsubst(text, &i, buf, &b, SH_MAX_LINE) != 0) return -1;
            } else {
              if (b < SH_MAX_LINE - 1) buf[b++] = text[i];
              i++;
            }
          }
          if (!closed) return -1;
        } else if (c == '\\') {
          quoted = 1;
          if (!text[i + 1]) return -1;
          if (text[i + 1] == '\n') {
            i += 2; /* line continuation */
            continue;
          }
          if (b < SH_MAX_LINE - 1) buf[b++] = '\\';
          if (b < SH_MAX_LINE - 1) buf[b++] = text[i + 1];
          i += 2;
        } else if (c == '$' && text[i + 1] == '(') {
          if (scan_cmdsubst(text, &i, buf, &b, SH_MAX_LINE) != 0) return -1;
        } else if (c == '`') {
          if (scan_cmdsubst(text, &i, buf, &b, SH_MAX_LINE) != 0) return -1;
        } else {
          if (b < SH_MAX_LINE - 1) buf[b++] = c;
          i++;
        }
      }
      buf[b] = 0;
      if (b == 0 && !quoted) {
        /* nothing (should not happen) */
      }
      if (b > 0 || quoted) {
        if (g_ntok >= SH_MAX_TOKENS) return -1;
        g_tok[g_ntok].type = T_WORD;
        g_tok[g_ntok].text = s_dup(buf);
        g_tok[g_ntok].quoted = quoted;
        g_ntok++;
      } else {
        /* empty word at end (e.g. trailing backslash): drop */
        if (g_ntok < SH_MAX_TOKENS) {
        }
      }
    }
  }
  if (g_ntok >= SH_MAX_TOKENS) return -1;
  g_tok[g_ntok].type = T_EOF;
  g_tok[g_ntok].text = 0;
  g_tok[g_ntok].quoted = 0;
  g_ntok++;
  return 0;
}

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */

enum {
  N_SIMPLE, N_LIST, N_IF, N_WHILE, N_FOR, N_CASE, N_FUNC, N_BLOCK, N_SUBSH
};

struct redir {
  int fd;    /* 0, 1, 2 */
  int mode;  /* 0 = >, 1 = >>, 2 = < */
  char *file;
};

struct casepat {
  char **pats;
  int npats;
  struct node *body;
};

struct node {
  int kind;
  /* N_SIMPLE */
  char **words;
  int *wq;             /* parallel array: word had quoting */
  int nwords;
  struct redir redirs[SH_MAX_REDIRS];
  int nredirs;
  int bg;              /* trailing & on this list element */
  int negate;          /* ! prefix on the pipeline */
  /* N_LIST: op = ';', 'a' (&&), 'o' (||), '|' */
  int op;
  struct node *left;
  struct node *right;
  /* N_IF */
  struct node *cond;
  struct node *thenn;
  struct node *elsee;
  /* N_WHILE */
  int until;
  struct node *body;
  /* N_FOR */
  char *var;
  char **init_words;
  int *init_wq;
  int ninit;
  /* N_CASE */
  char *case_word;
  struct casepat *pats;
  int npats;
  /* N_FUNC */
  char *fname;
};

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */

static int peek_is_word(const char *w) {
  struct token *t = &g_tok[g_tokpos];
  return t->type == T_WORD && t->text && s_eq(t->text, w);
}

static int peek_type(int type) {
  return g_tok[g_tokpos].type == type;
}

static struct token *next_tok(void) {
  if (g_tokpos < g_ntok) return &g_tok[g_tokpos++];
  return &g_tok[g_ntok - 1];
}

static void skip_nl(void) {
  while (g_tok[g_tokpos].type == T_NL) g_tokpos++;
}

static struct node *parse_andor(void);
static struct node *parse_compound(void);
static struct node *parse_list(void);

static struct node *node_new(int kind) {
  struct node *n = (struct node *)malloc(sizeof *n);
  if (!n) return 0;
  n->kind = kind;
  n->words = 0;
  n->wq = 0;
  n->nwords = 0;
  for (int i = 0; i < SH_MAX_REDIRS; i++) {
    n->redirs[i].fd = 0;
    n->redirs[i].mode = 0;
    n->redirs[i].file = 0;
  }
  n->nredirs = 0;
  n->bg = 0;
  n->negate = 0;
  n->op = 0;
  n->left = 0;
  n->right = 0;
  n->cond = 0;
  n->thenn = 0;
  n->elsee = 0;
  n->until = 0;
  n->body = 0;
  n->var = 0;
  n->init_words = 0;
  n->init_wq = 0;
  n->ninit = 0;
  n->case_word = 0;
  n->pats = 0;
  n->npats = 0;
  n->fname = 0;
  return n;
}

static int add_simple_word(struct node *n, const char *w, int quoted, int wq) {
  if (n->nwords >= SH_MAX_WORDS) return -1;
  char *d = s_dup(w);
  if (!d) return -1;
  char **nw = (char **)malloc((size_t)(n->nwords + 1) * sizeof(char *));
  int *nq = (int *)malloc((size_t)(n->nwords + 1) * sizeof(int));
  if (!nw || !nq) {
    if (nw) free(nw);
    if (nq) free(nq);
    free(d);
    return -1;
  }
  for (int i = 0; i < n->nwords; i++) {
    nw[i] = n->words[i];
    nq[i] = n->wq[i];
  }
  if (n->words) free(n->words);
  if (n->wq) free(n->wq);
  nw[n->nwords] = d;
  nq[n->nwords] = wq;
  n->words = nw;
  n->wq = nq;
  n->nwords++;
  (void)quoted;
  return 0;
}

static int add_simple_redir(struct node *n, int fd, int mode, const char *file) {
  if (n->nredirs >= SH_MAX_REDIRS) return -1;
  char *d = s_dup(file);
  if (!d) return -1;
  n->redirs[n->nredirs].fd = fd;
  n->redirs[n->nredirs].mode = mode;
  n->redirs[n->nredirs].file = d;
  n->nredirs++;
  return 0;
}

static struct node *parse_simple(void) {
#ifdef SH_TRACE
  out2("P-SIMPLE\n");
#endif

  struct node *n = node_new(N_SIMPLE);
  if (!n) return 0;
  for (;;) {
    struct token *t = &g_tok[g_tokpos];
    if (t->type == T_WORD) {
      const char *w = t->text ? t->text : "";
      if (n->nwords == 0 &&
          (s_eq(w, "if") || s_eq(w, "while") || s_eq(w, "until") ||
           s_eq(w, "for") || s_eq(w, "case") || s_eq(w, "{"))) {
        if (n->words) free(n->words);
        if (n->wq) free(n->wq);
        free(n);
        return parse_compound();
      }
      if (n->nwords == 0 &&
          (s_eq(w, "then") || s_eq(w, "else") || s_eq(w, "elif") ||
           s_eq(w, "fi") || s_eq(w, "do") || s_eq(w, "done") ||
           s_eq(w, "esac") || s_eq(w, "}"))) {
        return n;
      }
      g_tokpos++;
      if (add_simple_word(n, t->text, t->quoted, t->quoted) != 0) {
        free(n);
        return 0;
      }
    } else if (t->type == T_GT || t->type == T_GTGT || t->type == T_LT ||
               t->type == T_ERRGT || t->type == T_ERRGTGT ||
               t->type == T_DUPGT || t->type == T_ERRDUP) {
      int fd = 1, mode = 0;
      switch (t->type) {
      case T_GT: fd = 1; mode = 0; break;
      case T_GTGT: fd = 1; mode = 1; break;
      case T_LT: fd = 0; mode = 2; break;
      case T_ERRGT: fd = 2; mode = 0; break;
      case T_ERRGTGT: fd = 2; mode = 1; break;
      case T_DUPGT: fd = 1; mode = 3; break;
      case T_ERRDUP: fd = 2; mode = 3; break;
      }
      g_tokpos++;
      struct token *f = &g_tok[g_tokpos];
      if (f->type != T_WORD) {
        free(n);
        return 0;
      }
      g_tokpos++;
      if (add_simple_redir(n, fd, mode, f->text) != 0) {
        free(n);
        return 0;
      }
    } else {
      break;
    }
  }
  if (n->nwords == 0 && n->nredirs == 0) {
    /* no command tokens consumed: never hand up a zero-progress node,
       or the caller would spin on the same token forever */
    free(n);
    return 0;
  }
  return n;
}

static struct node *parse_compound(void) {
#ifdef SH_TRACE
  {
    char nb[8];
    out2("P-COMPOUND tok=");
    itoa10(g_tok[g_tokpos].type, nb, sizeof nb);
    out2(nb);
    out2(":");
    out2(g_tok[g_tokpos].text ? g_tok[g_tokpos].text : "?");
    out2("\n");
  }
#endif

  struct token *t = &g_tok[g_tokpos];

  if (t->type == T_LP) {
    g_tokpos++;
    struct node *n = node_new(N_SUBSH);
    struct node *body = parse_list();
    if (!body) {
      free(n);
      return 0;
    }
    skip_nl();
    if (peek_type(T_SEMI)) g_tokpos++;
    skip_nl();
    if (peek_type(T_RP)) g_tokpos++;
    n->body = body;
    return n;
  }

  if (t->type == T_WORD && t->text) {
    const char *w = t->text;

    if (s_eq(w, "if")) {
      g_tokpos++;
      struct node *n = node_new(N_IF);
      struct node *cond = parse_andor();
      if (!cond) {
        free(n);
        return 0;
      }
      skip_nl();
      if (peek_type(T_SEMI)) g_tokpos++;
      skip_nl();
      if (!peek_is_word("then")) {
        free(n);
        return 0;
      }
      g_tokpos++;
      skip_nl();
      struct node *thenn = parse_list();
      if (!thenn) {
        free(n);
        return 0;
      }
      n->cond = cond;
      n->thenn = thenn;
      /* elif/else chain folded into nested N_IFs in .elsee */
      for (;;) {
        skip_nl();
        if (peek_type(T_SEMI)) g_tokpos++;
        skip_nl();
        if (peek_is_word("elif")) {
          g_tokpos++;
          struct node *ec = parse_andor();
          if (!ec) {
            free(n);
            return 0;
          }
          skip_nl();
          if (peek_type(T_SEMI)) g_tokpos++;
          skip_nl();
          if (!peek_is_word("then")) {
            free(n);
            return 0;
          }
          g_tokpos++;
          skip_nl();
          struct node *et = parse_list();
          if (!et) {
            free(n);
            return 0;
          }
          struct node *inner = node_new(N_IF);
          inner->cond = ec;
          inner->thenn = et;
          /* the outer else becomes this inner's else */
          inner->elsee = n->elsee;
          n->elsee = inner;
        } else if (peek_is_word("else")) {
          g_tokpos++;
          skip_nl();
          struct node *es = parse_list();
          if (!es) {
            free(n);
            return 0;
          }
          if (n->elsee) {
            /* an elif already nested an N_IF in our elsee: the final
               else belongs to the deepest nested if */
            struct node *last = n;
            while (last->elsee && last->elsee->kind == N_IF)
              last = last->elsee;
            if (last->elsee) {
              free(n);
              return 0;
            }
            last->elsee = es;
          } else {
            n->elsee = es;
          }
        } else {
          break;
        }
      }
      skip_nl();
      if (peek_type(T_SEMI)) g_tokpos++;
      skip_nl();
      if (!peek_is_word("fi")) {
        free(n);
        return 0;
      }
      g_tokpos++;
      return n;
    }

    if (s_eq(w, "while") || s_eq(w, "until")) {
      int until = s_eq(w, "until");
      g_tokpos++;
      struct node *n = node_new(N_WHILE);
      n->until = until;
      struct node *cond = parse_andor();
      if (!cond) {
        free(n);
        return 0;
      }
      skip_nl();
      if (peek_type(T_SEMI)) g_tokpos++;
      skip_nl();
      if (!peek_is_word("do")) {
        free(n);
        return 0;
      }
      g_tokpos++;
      skip_nl();
      struct node *body = parse_list();
      if (!body) {
        free(n);
        return 0;
      }
      n->cond = cond;
      n->body = body;
      skip_nl();
      if (peek_type(T_SEMI)) g_tokpos++;
      skip_nl();
      if (!peek_is_word("done")) {
        free(n);
        return 0;
      }
      g_tokpos++;
      return n;
    }

    if (s_eq(w, "for")) {
      g_tokpos++;
      struct token *v = &g_tok[g_tokpos];
      if (v->type != T_WORD) return 0;
      g_tokpos++;
      struct node *n = node_new(N_FOR);
      n->var = s_dup(v->text);
      skip_nl();
      if (peek_is_word("in")) {
        g_tokpos++;
        while (g_tok[g_tokpos].type == T_WORD) {
          struct token *wd = next_tok();
          char **nw = (char **)malloc((size_t)(n->ninit + 1) * sizeof(char *));
          int *nq = (int *)malloc((size_t)(n->ninit + 1) * sizeof(int));
          for (int i = 0; i < n->ninit; i++) nw[i] = n->init_words[i];
          for (int i = 0; i < n->ninit; i++) nq[i] = n->init_wq[i];
          if (n->init_words) free(n->init_words);
          if (n->init_wq) free(n->init_wq);
          nw[n->ninit] = s_dup(wd->text);
          nq[n->ninit] = wd->quoted;
          n->init_words = nw;
          n->init_wq = nq;
          n->ninit++;
        }
      }
      skip_nl();
      if (peek_type(T_SEMI)) g_tokpos++;
      skip_nl();
      if (!peek_is_word("do")) {
        free(n);
        return 0;
      }
      g_tokpos++;
      skip_nl();
      struct node *body = parse_list();
      if (!body) {
        free(n);
        return 0;
      }
      n->body = body;
      skip_nl();
      if (peek_type(T_SEMI)) g_tokpos++;
      skip_nl();
      if (!peek_is_word("done")) {
        free(n);
        return 0;
      }
      g_tokpos++;
      return n;
    }

    if (s_eq(w, "case")) {
      g_tokpos++;
      struct token *cw = &g_tok[g_tokpos];
      if (cw->type != T_WORD) return 0;
      g_tokpos++;
      struct node *n = node_new(N_CASE);
      n->case_word = s_dup(cw->text);
      skip_nl();
      if (!peek_is_word("in")) {
        free(n);
        return 0;
      }
      g_tokpos++;
      skip_nl();
      for (;;) {
        skip_nl();
        if (peek_is_word("esac")) {
          g_tokpos++;
          break;
        }
        struct casepat cp;
        cp.pats = 0;
        cp.npats = 0;
        while (g_tok[g_tokpos].type == T_WORD && !peek_is_word("esac")) {
          if (peek_is_word(")")) break;
          struct token *pt = next_tok();
          char **np = (char **)malloc((size_t)(cp.npats + 1) * sizeof(char *));
          for (int i = 0; i < cp.npats; i++) np[i] = cp.pats[i];
          if (cp.pats) free(cp.pats);
          np[cp.npats] = s_dup(pt->text);
          cp.pats = np;
          cp.npats++;
          while (peek_type(T_PIPE)) {
            g_tokpos++;
            struct token *pt2 = next_tok();
            if (pt2->type != T_WORD) break;
            char **np2 = (char **)malloc(
              (size_t)(cp.npats + 1) * sizeof(char *));
            for (int i = 0; i < cp.npats; i++) np2[i] = cp.pats[i];
            if (cp.pats) free(cp.pats);
            np2[cp.npats] = s_dup(pt2->text);
            cp.pats = np2;
            cp.npats++;
          }
        }
        if (peek_type(T_RP)) g_tokpos++;
        skip_nl();
        struct node *body = parse_list();
        if (!body) {
          free(n);
          return 0;
        }
        cp.body = body;
        struct casepat *np = (struct casepat *)malloc(
          (size_t)(n->npats + 1) * sizeof(struct casepat));
        for (int i = 0; i < n->npats; i++) np[i] = n->pats[i];
        if (n->pats) free(n->pats);
        np[n->npats] = cp;
        n->pats = np;
        n->npats++;
        skip_nl();
        if (peek_type(T_DSEMI)) {
          g_tokpos++;
        } else if (peek_is_word("esac")) {
          g_tokpos++;
          return n;
        }
      }
      return n;
    }

    if (s_eq(w, "{")) {
      g_tokpos++;
      struct node *n = node_new(N_BLOCK);
      struct node *body = parse_list();
      if (!body) {
        free(n);
        return 0;
      }
      skip_nl();
      if (peek_type(T_SEMI)) g_tokpos++;
      skip_nl();
      if (peek_is_word("}")) g_tokpos++;
      n->body = body;
      return n;
    }

    /* function definition: name () { list; }  */
    if (g_tokpos + 2 < g_ntok &&
        g_tok[g_tokpos + 1].type == T_LP &&
        g_tok[g_tokpos + 2].type == T_RP) {
      struct node *n = node_new(N_FUNC);
      n->fname = s_dup(w);
      g_tokpos += 3;
      skip_nl();
      /* function body is a single compound command (usually { list; });
         parse the brace group as one unit so a trailing "; cmd" after the
         closing brace is NOT absorbed into the body */
      struct node *body = parse_andor();
      if (!body) {
        free(n);
        return 0;
      }
      skip_nl();
      if (peek_type(T_SEMI)) g_tokpos++;
      skip_nl();
      if (peek_is_word("}")) g_tokpos++;
      n->body = body;
      return n;
    }
  }
  return parse_simple();
}

static struct node *parse_pipeline(void) {
#ifdef SH_TRACE
  out2("P-PIPE\n");
#endif

  int negate = 0;
  if (peek_is_word("!")) {
    g_tokpos++;
    skip_nl();
    negate = 1;
  }
  struct node *first = parse_compound();
  if (!first) return 0;
  first->negate = negate;
  struct node *cur = first;
  while (peek_type(T_PIPE)) {
    g_tokpos++;
    skip_nl();
    struct node *r = parse_compound();
    if (!r) return 0;
    struct node *list = node_new(N_LIST);
    if (!list) return 0;
    list->op = '|';
    list->left = cur;
    list->right = r;
    cur = list;
  }
  return cur;
}

static struct node *parse_andor(void) {
#ifdef SH_TRACE
  out2("P-ANDOR\n");
#endif

  struct node *l = parse_pipeline();
  if (!l) return 0;
  for (;;) {
    if (peek_type(T_AND)) {
      g_tokpos++;
      struct node *r = parse_pipeline();
      if (!r) return 0;
      struct node *x = node_new(N_LIST);
      x->op = 'a';
      x->left = l;
      x->right = r;
      l = x;
    } else if (peek_type(T_OR)) {
      g_tokpos++;
      struct node *r = parse_pipeline();
      if (!r) return 0;
      struct node *x = node_new(N_LIST);
      x->op = 'o';
      x->left = l;
      x->right = r;
      l = x;
    } else {
      break;
    }
  }
  return l;
}

static struct node *parse_list(void) {
#ifdef SH_TRACE
  {
    char nb[8];
    out2("P-LIST tok=");
    itoa10(g_tok[g_tokpos].type, nb, sizeof nb);
    out2(nb);
    out2(":");
    out2(g_tok[g_tokpos].text ? g_tok[g_tokpos].text : "?");
    out2("\n");
  }
#endif

  skip_nl();
  if (peek_type(T_EOF) || peek_is_word("fi") || peek_is_word("done") ||
      peek_is_word("esac") || peek_is_word("}") || peek_is_word("then") ||
      peek_is_word("do") || peek_is_word("else") || peek_is_word("elif")) {
    return 0;
  }
  struct node *l = parse_andor();
  if (!l) return 0;
  for (;;) {
    int op = 0;
    int bg = 0;
    if (peek_type(T_SEMI)) {
      op = ';';
      g_tokpos++;
    } else if (peek_type(T_BG)) {
      op = ';';
      bg = 1;
      g_tokpos++;
      l->bg = 1;
    } else if (peek_type(T_NL)) {
      op = ';';
      g_tokpos++;
    } else {
      break;
    }
    if (peek_type(T_EOF) || peek_is_word("fi") || peek_is_word("done") ||
        peek_is_word("esac") || peek_is_word("}") || peek_is_word("then") ||
        peek_is_word("do") || peek_is_word("else") || peek_is_word("elif")) {
      break;
    }
    skip_nl();
    if (peek_type(T_EOF) || peek_is_word("fi") || peek_is_word("done") ||
        peek_is_word("esac") || peek_is_word("}")) {
      break;
    }
    struct node *r = parse_andor();
    if (!r) break;
    struct node *x = node_new(N_LIST);
    x->op = op;
    x->left = l;
    x->right = r;
    l = x;
    l->bg = bg;
  }
  return l;
}

static struct node *parse_program(void) {
#ifdef SH_TRACE
  out2("P-PROGRAM\n");
#endif

  skip_nl();
  if (peek_type(T_EOF)) return 0;
  struct node *l = parse_list();
  if (!l) return 0;
  for (;;) {
    skip_nl();
    if (peek_type(T_EOF)) break;
    struct node *r = parse_list();
    if (!r) break;
    struct node *x = node_new(N_LIST);
    x->op = ';';
    x->left = l;
    x->right = r;
    l = x;
  }
  return l;
}

/* ------------------------------------------------------------------ */
/* expansion: quote-aware scanner                                      */
/* ------------------------------------------------------------------ */

struct fields {
  char **words;
  int nwords;
  int cap;
  char *gbl; /* parallel: 1 = globbable (came from unquoted text) */
};

static void fields_init(struct fields *f) {
  f->words = 0;
  f->nwords = 0;
  f->cap = 0;
  f->gbl = 0;
}

static void fields_free(struct fields *f) {
  for (int i = 0; i < f->nwords; i++) {
    if (f->words && f->words[i]) free(f->words[i]);
  }
  if (f->words) free(f->words);
  if (f->gbl) free(f->gbl);
  f->words = 0;
  f->gbl = 0;
  f->nwords = 0;
  f->cap = 0;
}

static int fields_add(struct fields *f, const char *s, int globbable) {
  if (f->nwords >= f->cap) {
    int nc = f->cap ? f->cap * 2 : 8;
    char **nw = (char **)malloc((size_t)nc * sizeof(char *));
    char *ng = (char *)malloc((size_t)nc * sizeof(char));
    if (!nw || !ng) {
      if (nw) free(nw);
      if (ng) free(ng);
      return -1;
    }
    for (int i = 0; i < f->nwords; i++) {
      nw[i] = f->words[i];
      ng[i] = f->gbl[i];
    }
    if (f->words) free(f->words);
    if (f->gbl) free(f->gbl);
    f->words = nw;
    f->gbl = ng;
    f->cap = nc;
  }
  f->words[f->nwords] = s_dup(s);
  if (!f->words[f->nwords]) return -1;
  f->gbl[f->nwords] = (char)globbable;
  f->nwords++;
  return 0;
}

/* append one char to the last field (create it if needed) */
static void field_putc(struct fields *f, char c, int globbable) {
  if (f->nwords == 0) {
    char s[2] = { c, 0 };
    fields_add(f, s, globbable);
    return;
  }
  char *cur = f->words[f->nwords - 1];
  int len = s_len(cur);
  char *nu = (char *)malloc((size_t)len + 2);
  if (!nu) return;
  for (int i = 0; i < len; i++) nu[i] = cur[i];
  nu[len] = c;
  nu[len + 1] = 0;
  free(cur);
  f->words[f->nwords - 1] = nu;
  if (globbable) f->gbl[f->nwords - 1] = 1;
}

static void field_puts(struct fields *f, const char *s, int globbable) {
  for (const char *p = s; *p; p++) field_putc(f, *p, globbable);
}

static int capture_run(const char *script, char *out, int cap);

/* split `val` on IFS and CONCATENATE the pieces into fields like an
 * unquoted expansion does: the first piece joins the current (pending)
 * field, each further piece becomes a new field.  An empty result
 * contributes nothing (the expansion vanishes). */
static void split_join(struct fields *f, const char *val) {
  const char *ifs = " \t\n";
  struct shell_var *v = var_find("IFS");
  if (v && v->val && v->val[0]) ifs = v->val;
  const char *p = val;
  const char *tok = p;
  int first = 1;
  while (*p) {
    int issep = 0;
    for (const char *q = ifs; *q; q++) {
      if (*p == *q) { issep = 1; break; }
    }
    if (issep) {
      if (p != tok) {
        if (first) {
          for (const char *c = tok; c < p; c++) field_putc(f, *c, 1);
          first = 0;
        } else {
          char *piece = (char *)malloc((size_t)(p - tok) + 1);
          for (int i = 0; i < (int)(p - tok); i++) piece[i] = tok[i];
          piece[p - tok] = 0;
          fields_add(f, piece, 1);
          free(piece);
        }
      }
      tok = p + 1;
    }
    p++;
  }
  if (p != tok) {
    if (first) {
      for (const char *c = tok; c < p; c++) field_putc(f, *c, 1);
    } else {
      char *piece = (char *)malloc((size_t)(p - tok) + 1);
      for (int i = 0; i < (int)(p - tok); i++) piece[i] = tok[i];
      piece[p - tok] = 0;
      fields_add(f, piece, 1);
      free(piece);
    }
  }
}

/* minimal $(( )) integer arithmetic: + - * / % ( ) unary - and
 * variable names ($x or x).  Recursive descent over the expr string. */
static long arith_expr(const char *s, int *ip);
static void arith_skip(const char *s, int *ip) {
  while (s[*ip] == ' ' || s[*ip] == '\t') (*ip)++;
}
static long sh_arith(const char *s, int *ok) {
  int i = 0;
  long v = arith_expr(s, &i);
  arith_skip(s, &i);
  *ok = (s[i] == 0);
  return *ok ? v : 0;
}
static long arith_factor(const char *s, int *ip) {
  arith_skip(s, ip);
  long v = 0;
  if (s[*ip] == '(') {
    (*ip)++;
    v = arith_expr(s, ip);
    arith_skip(s, ip);
    if (s[*ip] == ')') (*ip)++;
  } else if (s[*ip] == '-' || s[*ip] == '+') {
    int neg = (s[*ip] == '-');
    (*ip)++;
    v = arith_factor(s, ip);
    if (neg) v = -v;
  } else if (s[*ip] >= '0' && s[*ip] <= '9') {
    while (s[*ip] >= '0' && s[*ip] <= '9') {
      v = v * 10 + (s[*ip] - '0');
      (*ip)++;
    }
  } else if (s[*ip] != 0) {
    char name[64];
    int n = 0;
    if (s[*ip] == '$') {
      if (n < 62) name[n++] = '$';
      (*ip)++;
    }
    if (name[0] == '$' && s[*ip] >= '0' && s[*ip] <= '9') {
      /* $1, $2, ... positional params inside arithmetic */
      int num = 0;
      while (s[*ip] >= '0' && s[*ip] <= '9') {
        num = num * 10 + (s[*ip] - '0');
        (*ip)++;
      }
      const char *pv = (num == 0) ? g_argv0_buf : pos_get(num);
      if (pv && pv[0]) {
        int subok = 0;
        long sub = sh_arith(pv, &subok);
        v = subok ? sub : atol(pv);
      }
      return v;
    }
    if (name[0] == '$' && s[*ip] == '#') {
      (*ip)++;
      v = (long)g_npos;
      return v;
    }
    while (((s[*ip] >= 'A' && s[*ip] <= 'Z') ||
            (s[*ip] >= 'a' && s[*ip] <= 'z') ||
            (s[*ip] >= '0' && s[*ip] <= '9') || s[*ip] == '_')) {
      if (n < 62) name[n++] = s[*ip];
      (*ip)++;
    }
    name[n] = 0;
    const char *val = var_get(name[0] == '$' ? name + 1 : name);
    if (val && val[0]) {
      int subok = 0;
      long sub = sh_arith(val, &subok);
      v = subok ? sub : atol(val);
    }
  }
  return v;
}
static long arith_term(const char *s, int *ip) {
  arith_skip(s, ip);
  long v = arith_factor(s, ip);
  arith_skip(s, ip);
  while (s[*ip] == '*' || s[*ip] == '/' || s[*ip] == '%') {
    char op = s[(*ip)++];
    long r = arith_factor(s, ip);
    arith_skip(s, ip);
    if (op == '*') v *= r;
    else if (op == '/') v = (r != 0) ? v / r : 0;
    else v = (r != 0) ? v % r : 0;
  }
  return v;
}
static long arith_expr(const char *s, int *ip) {
  arith_skip(s, ip);
  long v = arith_term(s, ip);
  arith_skip(s, ip);
  while (s[*ip] == '+' || s[*ip] == '-') {
    char op = s[(*ip)++];
    long r = arith_term(s, ip);
    arith_skip(s, ip);
    if (op == '+') v += r;
    else v -= r;
  }
  return v;
}

/* append a captured command-substitution result to the fields */
static void put_capture(struct fields *out, const char *cap, int quoted) {
  if (quoted) field_puts(out, cap, 0);
  else split_join(out, cap);
}

static void expand_word_raw(const char *text, int no_split, struct fields *out);

/* expand $... at p (given quoted context); returns the new p. */
static const char *expand_param(const char *p, struct fields *out, int quoted) {
  const char *after = p + 1;
  if (*after == '(' && after[1] == '(') {
    /* $(( arithmetic )) */
    const char *q = after + 2;
    int depth = 1;
    while (*q && depth > 0) {
      if (q[0] == '(') {
        depth++;
        q++;
      } else if (q[0] == ')' && depth == 1 && q[1] == ')') {
        depth = 0; /* q points at the first ')' of the closing "))" */
      } else if (q[0] == ')') {
        depth--;
        q++;
      } else {
        q++;
      }
    }
    if (depth == 0) {
      char expr[SH_MAX_LINE];
      int n = 0;
      const char *w = after + 2;
      while (w < q && n < SH_MAX_LINE - 1) expr[n++] = *w++;
      expr[n] = 0;
      int ok = 0;
      long v = sh_arith(expr, &ok);
      if (!ok) {
        /* fall back: try running as a command substitution */
        char cmd[SH_MAX_LINE];
        int cn = 0;
        const char *cc = after + 2;
        while (cc < q && cn < SH_MAX_LINE - 1) cmd[cn++] = *cc++;
        cmd[cn] = 0;
        char cap[SH_MAX_CAPTURE];
        cap[0] = 0;
        int st = capture_run(cmd, cap, sizeof cap);
        (void)st;
        put_capture(out, cap, quoted);
      } else {
        char b[24];
        itoa10((int)v, b, sizeof b);
        if (quoted) field_puts(out, b, 0);
        else split_join(out, b);
      }
    }
    return q + 2;
  }
  if (*after == '(') {
    /* $( command substitution ) */
    const char *q = after + 1;
    int depth = 1;
    while (*q && depth > 0) {
      if (q[0] == '$' && q[1] == '(') depth++;
      else if (*q == ')') depth--;
      q++;
    }
    char cmd[SH_MAX_LINE];
    int cn = 0;
    const char *cc = after + 1;
    while (cc < q - 1 && cn < SH_MAX_LINE - 1) cmd[cn++] = *cc++;
    cmd[cn] = 0;
    char cap[SH_MAX_CAPTURE];
    cap[0] = 0;
    int st = capture_run(cmd, cap, sizeof cap);
    (void)st;
#ifdef SH_TRACE
    out2("CMD=[");
    out2(cmd);
    out2("]\n");
    out2("CAP=[");
    out2(cap);
    out2("]\n");
#endif
    put_capture(out, cap, quoted);
    return q;
  }
  if (*after == '{') {
    const char *q = after + 1;
    char name[64];
    int n = 0;
    int depth = 1;
    /* scan to the MATCHING outer '}', accounting for nested ${...} */
    while (*q && depth > 0) {
      if (*q == '$' && q[1] == '{') {
        depth++;
        if (n < 62) name[n++] = *q;
        if (n < 62) name[n++] = q[1];
        q += 2;
        continue;
      }
      if (*q == '}') {
        depth--;
        if (depth == 0) {
          q++;
          break;
        }
      }
      if (n < 62) name[n++] = *q;
      q++;
    }
    name[n] = 0;
    int has_colon = 0;
    for (int i = 0; name[i]; i++) {
      if (name[i] == ':' && (name[i + 1] == '-' || name[i + 1] == '=')) {
        has_colon = 1;
        break;
      }
    }
    if (has_colon) {
      char vn[64];
      int k = 0;
      while (name[k] && name[k] != ':' && k < 62) {
        vn[k] = name[k];
        k++;
      }
      vn[k] = 0;
      int is_eq = (name[k + 1] == '=');
      const char *def = &name[k + 2];
      const char *val = var_get(vn);
      if (is_eq && (!val || !val[0])) var_set(vn, def, 0);
      if (val && val[0]) {
        /* var set: use its (already expanded) value verbatim */
        if (quoted) field_puts(out, val, 0);
        else split_join(out, val);
      } else {
        /* fallback word is raw script text: expand it (nested
           ${...}, $(...), $var all work) in this quoting context */
        expand_word_raw(def, quoted ? 1 : 0, out);
      }
    } else {
      const char *val = var_get(name);
      if (val) {
        if (quoted) field_puts(out, val, 0);
        else split_join(out, val);
      }
    }
    return q; /* already advanced past the closing '}' */
  }
  if (*after == '@' || *after == '*') {
    /* $@ and unquoted $*: each positional param its own field */
    int star = (*after == '*');
    if (star && quoted) {
      /* "$*": single field joined by IFS */
      char *joined = 0;
      int jl = 0;
      for (int i = 1; i <= g_npos; i++) {
        const char *pi = pos_get(i);
        int pl = s_len(pi);
        char *nj = (char *)malloc((size_t)jl + pl + 2);
        for (int k = 0; k < jl; k++) nj[k] = joined ? joined[k] : 0;
        for (int k = 0; k < pl; k++) nj[jl + k] = pi[k];
        if (i < g_npos) nj[jl + pl] = ' ';
        if (joined) free(joined);
        joined = nj;
        jl = s_len(joined);
      }
      if (joined) {
        field_puts(out, joined, 0);
        free(joined);
      }
    } else if (quoted) {
      /* "$@": first param appends to the in-flight field (so adjacent text
         like "Q=[$@]" merges), subsequent params start fresh fields */
      for (int i = 1; i <= g_npos; i++) {
        if (i == 1) field_puts(out, pos_get(i), 0);
        else fields_add(out, pos_get(i), 1);
      }
    } else {
      /* unquoted $@ / $*: same per-param fields, prefix merging with any
         in-flight text (POSIX: "star:$*" keeps "star:" attached) */
      for (int i = 1; i <= g_npos; i++) {
        if (i == 1) field_puts(out, pos_get(i), 1);
        else fields_add(out, pos_get(i), 1);
      }
    }
    return after + 1;
  }
  if (*after == '?') {
    char b[16];
    itoa10(g_laststatus, b, sizeof b);
    if (quoted) field_puts(out, b, 0);
    else split_join(out, b);
    return after + 1;
  }
  if (*after == '#') {
    char b[16];
    itoa10(g_npos, b, sizeof b);
    if (quoted) field_puts(out, b, 0);
    else split_join(out, b);
    return after + 1;
  }
  if (*after == '$') {
    char b[16];
    itoa10(getpid(), b, sizeof b);
    if (quoted) field_puts(out, b, 0);
    else split_join(out, b);
    return after + 1;
  }
  if (*after >= '0' && *after <= '9') {
    int num = 0;
    const char *a2 = after;
    while (*a2 >= '0' && *a2 <= '9' && num < 1000) {
      num = num * 10 + (*a2 - '0');
      a2++;
    }
    const char *val = (num == 0) ? g_argv0_buf : pos_get(num);
    if (quoted) field_puts(out, val, 0);
    else split_join(out, val);
    return a2;
  }
  if ((*after >= 'A' && *after <= 'Z') ||
      (*after >= 'a' && *after <= 'z') || *after == '_') {
    char name[64];
    int n = 0;
    while (((*after >= 'A' && *after <= 'Z') ||
            (*after >= 'a' && *after <= 'z') ||
            (*after >= '0' && *after <= '9') || *after == '_') &&
           n < 62) {
      name[n++] = *after;
      after++;
    }
    name[n] = 0;
    const char *val = var_get(name);
    if (val) {
      if (quoted) field_puts(out, val, 0);
      else split_join(out, val);
    }
    return after;
  }
  field_putc(out, '$', 1);
  return after;
}

/* expand one raw word (quote markers preserved) into fields.
 * quirk: the caller passes mode; uses the quote-aware scanner. */
static void expand_word_raw(const char *text, int no_split, struct fields *out) {
  (void)no_split;
  const char *p = text;
  while (*p) {
    char c = *p;
    if (c == '\'') {
      p++;
      while (*p && *p != '\'') field_putc(out, *p++, 0);
      if (*p == '\'') p++;
      continue;
    }
    if (c == '"') {
      p++;
      while (*p && *p != '"') {
        if (*p == '\\' && (p[1] == '$' || p[1] == '`' || p[1] == '"' ||
                           p[1] == '\\')) {
          field_putc(out, p[1], 0);
          p += 2;
          continue;
        }
        if (*p == '$') {
          p = expand_param(p, out, 1);
          continue;
        }
        if (*p == '`') {
          const char *q = p + 1;
          char cmd[SH_MAX_LINE];
          int cn = 0;
          while (*q && *q != '`' && cn < SH_MAX_LINE - 1) cmd[cn++] = *q++;
          cmd[cn] = 0;
          char cap[SH_MAX_CAPTURE];
          cap[0] = 0;
          int st = capture_run(cmd, cap, sizeof cap);
          (void)st;
          field_puts(out, cap, 0);
          p = (*q == '`') ? q + 1 : q;
          continue;
        }
        field_putc(out, *p, 0);
        p++;
      }
      if (*p == '"') p++;
      continue;
    }
    if (c == '\\') {
      if (p[1]) {
        field_putc(out, p[1], 1);
        p += 2;
      } else {
        p++;
      }
      continue;
    }
    if (c == '$') {
      /* wq==1 (quoted) and wq==2 (case) words must not split on expansions */
      p = expand_param(p, out, no_split != 0);
      continue;
    }
    if (c == '`') {
      const char *q = p + 1;
      char cmd[SH_MAX_LINE];
      int cn = 0;
      while (*q && *q != '`' && cn < SH_MAX_LINE - 1) cmd[cn++] = *q++;
      cmd[cn] = 0;
      char cap[SH_MAX_CAPTURE];
      cap[0] = 0;
      int st = capture_run(cmd, cap, sizeof cap);
      (void)st;
      split_join(out, cap);
      p = (*q == '`') ? q + 1 : q;
      continue;
    }
    field_putc(out, c, 1);
    p++;
  }
}

static int has_glob(const char *w) {
  for (const char *p = w; *p; p++) {
    if (*p == '*' || *p == '?' || *p == '[') return 1;
  }
  return 0;
}

static int pat_match(const char *pat, const char *str) {
  for (;;) {
    if (!*pat) return !*str;
    if (*pat == '*') {
      pat++;
      if (!*pat) return 1;
      for (const char *s = str; ; s++) {
        if (pat_match(pat, s)) return 1;
        if (!*s) return 0;
      }
    }
    if (*pat == '?') {
      if (!*str) return 0;
      pat++;
      str++;
      continue;
    }
    if (*pat == '[') {
      if (!*str) return 0;
      const char *p = pat + 1;
      int neg = 0;
      if (*p == '!') {
        neg = 1;
        p++;
      }
      int matched = 0;
      char c = *str;
      while (*p && *p != ']') {
        if (p[1] == '-' && p[2] && p[2] != ']') {
          if (c >= p[0] && c <= p[2]) matched = 1;
          p += 3;
        } else {
          if (*p == c) matched = 1;
          p++;
        }
      }
      if (neg) matched = !matched;
      if (!matched) return 0;
      while (*p && *p != ']') p++;
      if (*p == ']') p++;
      pat = p;
      str++;
      continue;
    }
    if (*pat != *str) return 0;
    pat++;
    str++;
  }
}

static char **glob_dir(const char *dir, const char *pat, int *n_out) {
#ifdef HOST_TEST
  char **out = 0;
  int n = 0;
  DIR *d = opendir(dir[0] ? dir : ".");
  if (d) {
    struct dirent *de;
    while ((de = readdir(d)) != 0) {
      if (de->d_name[0] == '.' && pat[0] != '.') continue;
      if (!pat_match(pat, de->d_name)) continue;
      char **no = (char **)malloc((size_t)(n + 1) * sizeof(char *));
      for (int i = 0; i < n; i++) no[i] = out[i];
      if (out) free(out);
      no[n] = s_dup(de->d_name);
      out = no;
      n++;
    }
    closedir(d);
  }
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      if (strcmp(out[i], out[j]) > 0) {
        char *t = out[i];
        out[i] = out[j];
        out[j] = t;
      }
    }
  }
  *n_out = n;
  return out;
#else
  char **out = 0;
  int n = 0;
  int idx = 0;
  for (;;) {
    struct sys_dirent ent;
    int r = read_dir(dir, idx, &ent);
    if (r != 0) break;
    idx++;
    if (ent.name[0] == '\0') break;
    if (ent.name[0] == '.') {
      if (pat[0] != '.') continue;
    }
    if (!pat_match(pat, ent.name)) continue;
    char **no = (char **)malloc((size_t)(n + 1) * sizeof(char *));
    for (int i = 0; i < n; i++) no[i] = out[i];
    if (out) free(out);
    no[n] = s_dup(ent.name);
    out = no;
    n++;
  }
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      if (strcmp(out[i], out[j]) > 0) {
        char *t = out[i];
        out[i] = out[j];
        out[j] = t;
      }
    }
  }
  *n_out = n;
  return out;
#endif
}

/* full word expansion: variables, command substitution, globbing */
static void expand_word(struct node *n, int idx, struct fields *out) {
  const char *text = n->words[idx];
  int no_split = n->wq[idx]; /* any quoting: suppress IFS splitting */
  fields_init(out);
  expand_word_raw(text, no_split, out);

  /* glob each field that came from unquoted text (never for case words) */
  struct fields after;
  fields_init(&after);
  int no_glob = (n->wq[idx] == 2);
  for (int i = 0; i < out->nwords; i++) {
    const char *w = out->words[i];
    if (!no_glob && out->gbl[i] && has_glob(w)) {
      char dir[128] = ".";
      char pat[128];
      const char *slash = 0;
      for (const char *c = w; *c; c++) {
        if (*c == '/') slash = c;
      }
      if (slash) {
        int dl = (int)(slash - w);
        for (int k = 0; k < dl && k < 127; k++) dir[k] = w[k];
        dir[dl] = 0;
        if (dl == 0) {
          dir[0] = '/';
          dir[1] = 0;
        }
        int k = 0;
        for (const char *c = slash + 1; *c && k < 127; c++) pat[k++] = *c;
        pat[k] = 0;
      } else {
        int k = 0;
        for (const char *c = w; *c && k < 127; c++) pat[k++] = *c;
        pat[k] = 0;
      }
      int gn = 0;
      char **g = glob_dir(dir, pat, &gn);
      if (g) {
        for (int gi = 0; gi < gn; gi++) {
          char *full = 0;
          if (slash) {
            full = s_cat(dir, "/");
            char *tmp = s_cat(full, g[gi]);
            free(full);
            full = tmp;
          } else {
            full = s_dup(g[gi]);
          }
          fields_add(&after, full, 0);
          free(full);
          free(g[gi]);
        }
        free(g);
      } else {
        fields_add(&after, w, 0);
      }
    } else {
      fields_add(&after, w, 0);
    }
  }
  fields_free(out);
  out->words = after.words;
  out->gbl = after.gbl;
  out->nwords = after.nwords;
  out->cap = after.cap;
}

/* ------------------------------------------------------------------ */
/* command substitution                                                */
/* ------------------------------------------------------------------ */

static int g_capture_depth = 0;

static int sh_eval(struct node *n, int in_fd, int out_fd, int err_fd);

/* Run `script` as a shell command with stdout captured into out (trailing
 * newlines stripped). Runs in a forked subshell so a child writing a full
 * pipe cannot deadlock the parent. */
static int capture_run(const char *script, char *out, int cap) {
  if (cap <= 0) return -1;
  out[0] = 0;
  if (g_capture_depth >= SH_CAPTURE_DEPTH) return 0;
  g_capture_depth++;
  int p[2];
  if (pipe(p) != 0) {
    g_capture_depth--;
    return -1;
  }
  int pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    g_capture_depth--;
    return -1;
  }
  if (pid == 0) {
    /* child: stdout -> pipe, run the script, exit with its status */
    close(p[0]);
    dup2(p[1], 1);
    close(p[1]);
    int st = 0;
    tok_reset();
    if (sh_tokenize(script) == 0) {
      struct node *n2 = parse_program();
      if (n2) st = sh_eval(n2, 0, 1, 2);
    }
    exit(st);
  }
  close(p[1]);
  int len = 0;
  for (;;) {
    char b[256];
    int r = read(p[0], b, sizeof b);
    if (r <= 0) break;
    for (int i = 0; i < r && len < cap - 1; i++) out[len++] = b[i];
  }
  close(p[0]);
  out[len] = 0;
  while (len > 0 && (out[len - 1] == '\n')) {
    out[len - 1] = 0;
    len--;
  }
  int wst = 0;
  waitpid(pid, &wst, 0);
  g_capture_depth--;
  return (wst >> 8) & 0xff;
}

/* ------------------------------------------------------------------ */
/* executor                                                            */
/* ------------------------------------------------------------------ */

static int g_bg_pids[SH_MAX_BG];
static int g_nbg = 0;
static int g_exit_requested = 0;
static int g_exit_status = 0;
static int g_break_count = 0;
static int g_continue_count = 0;
static int g_returned = 0;
static int g_return_status = 0;

/* resolve an external command path (8.3 uppercased, cwd then root) */
static int pick_binary(const char *cmd, char *resolved, size_t cap) {
  int has_slash = 0;
  for (const char *p = cmd; *p; p++) {
    if (*p == '/') {
      has_slash = 1;
      break;
    }
  }
  if (has_slash) {
    int fd = open(cmd, O_RDONLY);
    if (fd >= 0) {
      close(fd);
      int l = s_len(cmd);
      if (l >= (int)cap) l = (int)cap - 1;
      for (int i = 0; i < l; i++) resolved[i] = cmd[i];
      resolved[l] = 0;
      return 1;
    }
    return 0;
  }
  /* bare name: uppercase to NAME.BIN, try cwd then root */
  char base[40];
  int i = 0;
  while (cmd[i] && i < 8) {
    char c = cmd[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    base[i] = c;
    i++;
  }
  base[i] = '.';
  base[i + 1] = 'B';
  base[i + 2] = 'I';
  base[i + 3] = 'N';
  base[i + 4] = 0;
  int fd = open(base, O_RDONLY);
  if (fd >= 0) {
    close(fd);
    int l = s_len(base);
    if (l >= (int)cap) l = (int)cap - 1;
    for (int k = 0; k < l; k++) resolved[k] = base[k];
    resolved[l] = 0;
    return 1;
  }
  char root[40];
  root[0] = '/';
  int ri = 1;
  for (int k = 0; base[k] && ri < (int)cap - 2; k++) root[ri++] = base[k];
  root[ri] = 0;
  fd = open(root, O_RDONLY);
  if (fd >= 0) {
    close(fd);
    int l = s_len(root);
    if (l >= (int)cap) l = (int)cap - 1;
    for (int k = 0; k < l; k++) resolved[k] = root[k];
    resolved[l] = 0;
    return 1;
  }
  return 0;
}

static int is_builtin(const char *name) {
  static const char *bi[] = {
    "cd", "pwd", "echo", "printf", "exit", "export", "unset", "set",
    "shift", "read", "test", "[", "true", "false", ":", "umask", "wait",
    "help", "clear", "break", "continue", "return", 0
  };
  for (int i = 0; bi[i]; i++) {
    if (s_eq(name, bi[i])) return i;
  }
  return -1;
}

static int builtin_run(int which, int argc, char **argv);
static int call_function(const char *name, int argc, char **argv);

/* apply redirections; returns 0 on success, 1 on failure (already
 * reported).  lin/lout/lerr receive the final fds. */
static int apply_redirs(struct node *n, int *lin, int *lout, int *lerr) {
  for (int i = 0; i < n->nredirs; i++) {
    struct redir *r = &n->redirs[i];
    /* expand the file name */
    struct fields f;
    struct node tmpn;
    int wq = 0;
    tmpn.words = &r->file;
    tmpn.wq = &wq;
    tmpn.nwords = 1;
    expand_word(&tmpn, 0, &f);
    const char *fname = f.nwords > 0 ? f.words[0] : r->file;
    int fd = -1;
    if (r->mode == 3) {
      /* dup redirection: N>&M — the tokenizer swallowed the '&' into the
         operator, so the file word is the bare target "2"; accept both */
      int tgt = atoi(fname[0] == '&' ? fname + 1 : fname);
      if (tgt < 0 || tgt > 2 || (fname[0] == '-' && fname[1] == 0)) {
        out2("sh: bad dup target: ");
        out2(fname);
        out2("\n");
        fields_free(&f);
        return 1;
      }
      /* dup the target onto the source fd (source = r->fd) */
      if (r->fd == 0) *lin = tgt;
      else if (r->fd == 1) *lout = tgt;
      else *lerr = tgt;
      fields_free(&f);
      continue;
    }
    if (r->mode == 0) fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC);
    else if (r->mode == 1) fd = open(fname, O_WRONLY | O_CREAT | O_APPEND);
    else fd = open(fname, O_RDONLY);
    if (fd < 0) {
      out2("sh: cannot open ");
      out2(fname);
      out2("\n");
      fields_free(&f);
      return 1;
    }
    if (r->fd == 0) {
      if (*lin != 0) close(*lin);
      *lin = fd;
    } else if (r->fd == 1) {
      if (*lout != 1) close(*lout);
      *lout = fd;
    } else {
      if (*lerr != 2) close(*lerr);
      *lerr = fd;
    }
    fields_free(&f);
  }
  return 0;
}

/* run a simple command with the given fds; returns exit status.  A
 * builtin runs in-process only when the desired fds ARE the shell's own
 * 0/1/2 (so `echo > file` or a pipeline stage forks). */
static int eval_simple(struct node *n, int in_fd, int out_fd, int err_fd) {
  struct fields f;
  fields_init(&f);
  for (int i = 0; i < n->nwords; i++) {
    struct fields one;
    expand_word(n, i, &one);
    for (int k = 0; k < one.nwords; k++) {
      fields_add(&f, one.words[k], 0);
    }
    fields_free(&one);
  }

  int lin = in_fd, lout = out_fd, lerr = err_fd;
  if (apply_redirs(n, &lin, &lout, &lerr) != 0) {
    fields_free(&f);
    return 1;
  }

  int st = 0;
  int need_fork = (lin != in_fd) || (lout != out_fd) || (lerr != err_fd);

  /* assignment prefixes: `NAME=VAL cmd args` sets variables.  Detection
     happens on the RAW words (before expansion) so `i=$((i+1))` and
     `x=${x}c` work; only the value portion is expanded. */
  int astart = 0;
  while (astart < n->nwords) {
    const char *a = n->words[astart];
    const char *eq = 0;
    for (const char *p = a; *p; p++) {
      if (*p == '=') {
        eq = p;
        break;
      }
    }
    if (eq && ((a[0] >= 'A' && a[0] <= 'Z') || (a[0] >= 'a' && a[0] <= 'z') ||
               a[0] == '_')) {
      /* expand the value in a quoted context (no splitting, no glob) */
      struct fields vf;
      struct node mini;
      int wq = 1;
      char *valcopy = s_dup(eq + 1);
      mini.words = &valcopy;
      mini.wq = &wq;
      mini.nwords = 1;
      expand_word(&mini, 0, &vf);
      const char *val = vf.nwords > 0 ? vf.words[0] : "";
      var_set_val(a, (int)(eq - a), val, 0);
      fields_free(&vf);
      free(valcopy);
      astart++;
    } else {
      break;
    }
  }
  if (astart >= n->nwords) {
    if (lin != 0 && lin != in_fd) close(lin);
    if (lout != 1 && lout != out_fd) close(lout);
    if (lerr != 2 && lerr != err_fd) close(lerr);
    fields_free(&f);
    return 0;
  }
  const char *arg0 = f.words[astart];
  char **cmdargv = &f.words[astart];
  int cmdargc = f.nwords - astart;
  if (is_builtin(arg0) >= 0 && !need_fork && in_fd == 0 &&
             out_fd == 1 && err_fd == 2) {
    st = builtin_run(is_builtin(arg0), cmdargc, cmdargv);
  } else if (is_builtin(arg0) >= 0) {
    /* Redirections (or inherited stage fds): run the builtin in THIS
       process under a temporary fd swap so state-changing builtins (read,
       cd, umask, shift, export...) keep their effect in the shell, per
       POSIX.  Only forked, background and external commands leave this
       path (via the branches below). */
    int saved0 = -1, saved1 = -1, saved2 = -1;
#ifdef SH_TRACE
    {
      char nb0[8], nb1[8], nb2[8];
      itoa10(lin, nb0, sizeof nb0);
      itoa10(lout, nb1, sizeof nb1);
      itoa10(lerr, nb2, sizeof nb2);
      out2("SWAP lin=");
      out2(nb0);
      out2(" lout=");
      out2(nb1);
      out2(" lerr=");
      out2(nb2);
      out2("\n");
    }
#endif
    if (lin != 0) {
      saved0 = dup(0);
      dup2(lin, 0);
    }
    if (lout != 1) {
      saved1 = dup(1);
      dup2(lout, 1);
    }
    if (lerr != 2) {
      saved2 = dup(2);
      dup2(lerr, 2);
    }
    st = builtin_run(is_builtin(arg0), cmdargc, cmdargv);
    if (saved0 != -1) {
      dup2(saved0, 0);
      close(saved0);
    }
    if (saved1 != -1) {
      dup2(saved1, 1);
      close(saved1);
    }
    if (saved2 != -1) {
      dup2(saved2, 2);
      close(saved2);
    }
    if (lin != 0 && lin != in_fd) close(lin);
    if (lout != 1 && lout != out_fd) close(lout);
    if (lerr != 2 && lerr != err_fd) close(lerr);
  } else {
    /* user-defined function first */
    int fst = call_function(arg0, cmdargc, cmdargv);
    if (fst >= 0) {
      st = fst;
    } else {
      char path[64];
      if (!pick_binary(arg0, path, sizeof path)) {
        out2("sh: command not found: ");
        out2(arg0);
        out2("\n");
        st = 127;
      } else {
        int pid = fork();
        if (pid < 0) {
          out2("sh: fork failed\n");
          st = 126;
        } else if (pid == 0) {
          if (lin != 0) {
            dup2(lin, 0);
            if (lin != 0) close(lin);
          }
          if (lout != 1) {
            dup2(lout, 1);
            if (lout != 1) close(lout);
          }
          if (lerr != 2) {
            dup2(lerr, 2);
            if (lerr != 2) close(lerr);
          }
          char **av = (char **)malloc((size_t)(cmdargc + 1) * sizeof(char *));
          for (int i = 0; i < cmdargc; i++) av[i] = cmdargv[i];
          av[cmdargc] = 0;
          execv(path, av);
          out2("sh: exec failed: ");
          out2(path);
          out2("\n");
          exit(127);
        } else {
          int wst = 0;
          if (waitpid(pid, &wst, 0) < 0) st = 127;
          else st = (wst >> 8) & 0xff;
        }
      }
    }
  }

  if (lin != 0 && lin != in_fd) close(lin);
  if (lout != 1 && lout != out_fd) close(lout);
  if (lerr != 2 && lerr != err_fd) close(lerr);
  fields_free(&f);
  return st;
}

/* ------------------------------------------------------------------ */
/* pipelines                                                           */
/* ------------------------------------------------------------------ */

/* Run a pipeline AST (N_LIST nodes with op '|'): fork each stage, wiring
 * pipes between consecutive stages, wait for all, return the last stage's
 * status.  Every stage runs in a child (dup2 done there), so the shell's
 * own fds are never disturbed. */
static int eval_pipeline(struct node *n, int in_fd, int out_fd, int err_fd) {
  struct node *stages[16];
  int nst = 0;
  struct node *cur = n;
  while (cur->kind == N_LIST && cur->op == '|') {
    if (nst >= 16) break;
    stages[nst++] = cur->right;
    cur = cur->left;
  }
  if (nst < 16) stages[nst++] = cur;
  /* reverse into stage order */
  for (int i = 0; i < nst / 2; i++) {
    struct node *t = stages[i];
    stages[i] = stages[nst - 1 - i];
    stages[nst - 1 - i] = t;
  }

  if (nst == 1) {
    return sh_eval(stages[0], in_fd, out_fd, err_fd);
  }

  int pfd[15][2];
  for (int i = 0; i < nst - 1; i++) {
    if (pipe(pfd[i]) != 0) {
      out2("sh: pipe failed\n");
      return 1;
    }
  }

  int pids[16];
  int npids = 0;
  for (int i = 0; i < nst; i++) {
    int si = (i == 0) ? in_fd : pfd[i - 1][0];
    int so = (i == nst - 1) ? out_fd : pfd[i][1];
    int pid = fork();
    if (pid < 0) {
      /* try to keep going; mark the stage failed */
      pids[npids++] = -1;
      continue;
    }
    if (pid == 0) {
      /* child: close every pipe fd we do not use, then run the stage */
      for (int k = 0; k < nst - 1; k++) {
        if (pfd[k][0] != si && pfd[k][0] != so) close(pfd[k][0]);
        if (pfd[k][1] != si && pfd[k][1] != so) close(pfd[k][1]);
      }
      int st = sh_eval(stages[i], si, so, err_fd);
      exit(st);
    }
    pids[npids++] = pid;
  }

  /* parent: close all pipe fds and wait for every stage */
  for (int k = 0; k < nst - 1; k++) {
    close(pfd[k][0]);
    close(pfd[k][1]);
  }
  int last = 0;
  for (int i = 0; i < npids; i++) {
    if (pids[i] < 0) {
      last = 1;
      continue;
    }
    int wst = 0;
    waitpid(pids[i], &wst, 0);
    if (i == npids - 1) last = (wst >> 8) & 0xff;
  }
  if (stages[0]->negate) last = (last == 0) ? 1 : 0;
  return last;
}

static int sh_eval(struct node *n, int in_fd, int out_fd, int err_fd) {
  if (!n) return 0;
#ifdef SH_TRACE
  {
    char nb[8];
    const char *nm = n->kind == N_SIMPLE ? "SIMPLE" : n->kind == N_LIST ? "LIST" :
                     n->kind == N_IF ? "IF" : n->kind == N_WHILE ? "WHILE" :
                     n->kind == N_FOR ? "FOR" : n->kind == N_CASE ? "CASE" :
                     n->kind == N_FUNC ? "FUNC" : n->kind == N_BLOCK ? "BLOCK" :
                     n->kind == N_SUBSH ? "SUBSH" : "?";
    out2("EVAL [");
    out2(nm);
    out2("] op=");
    itoa10(n->op, nb, sizeof nb);
    out2(nb);
    out2("\n");
  }
#endif
  int st = 0;
  switch (n->kind) {
  case N_SIMPLE:
    st = eval_simple(n, in_fd, out_fd, err_fd);
    if (n->negate) st = (st == 0) ? 1 : 0;
    break;
  case N_BLOCK:
    st = sh_eval(n->body, in_fd, out_fd, err_fd);
    break;
  case N_SUBSH: {
    int pid = fork();
    if (pid < 0) {
      st = 126;
      break;
    }
    if (pid == 0) {
      int s2 = sh_eval(n->body, in_fd, out_fd, err_fd);
      exit(s2);
    }
    int wst = 0;
    waitpid(pid, &wst, 0);
    st = (wst >> 8) & 0xff;
    break;
  }
  case N_LIST:
    if (n->op == '|') {
      st = eval_pipeline(n, in_fd, out_fd, err_fd);
      break;
    }
    if (n->op == 'a') {
      int ls = sh_eval(n->left, in_fd, out_fd, err_fd);
      st = (ls == 0) ? sh_eval(n->right, in_fd, out_fd, err_fd) : ls;
      break;
    }
    if (n->op == 'o') {
      int ls = sh_eval(n->left, in_fd, out_fd, err_fd);
      st = (ls != 0) ? sh_eval(n->right, in_fd, out_fd, err_fd) : ls;
      break;
    }
    /* ';': run left (with background semantics) then right */
    if (n->left->bg) {
      /* background the left side */
      int pid = fork();
      if (pid == 0) {
        int s2 = sh_eval(n->left, in_fd, out_fd, err_fd);
        exit(s2);
      }
      if (pid > 0 && g_nbg < SH_MAX_BG) g_bg_pids[g_nbg++] = pid;
      st = sh_eval(n->right, in_fd, out_fd, err_fd);
    } else {
      int ls = sh_eval(n->left, in_fd, out_fd, err_fd);
      g_laststatus = ls;
      if (g_exit_requested) {
        st = g_exit_status;
        break;
      }
      if (g_break_count > 0 || g_continue_count > 0) {
        st = ls;
        break;
      }
      st = sh_eval(n->right, in_fd, out_fd, err_fd);
    }
    break;
  case N_IF: {
    int c = sh_eval(n->cond, in_fd, out_fd, err_fd);
    if (c == 0) st = sh_eval(n->thenn, in_fd, out_fd, err_fd);
    else if (n->elsee) st = sh_eval(n->elsee, in_fd, out_fd, err_fd);
    break;
  }
  case N_WHILE:
    st = 0;
    for (;;) {
      int c = sh_eval(n->cond, in_fd, out_fd, err_fd);
      if (n->until ? (c == 0) : (c != 0)) break;
      st = sh_eval(n->body, in_fd, out_fd, err_fd);
      if (g_break_count > 0) {
        g_break_count--;
        break;
      }
      if (g_continue_count > 0) {
        g_continue_count--;
        continue;
      }
      if (g_exit_requested) break;
    }
    break;
  case N_FOR: {
    st = 0;
    if (n->ninit > 0) {
      /* expand each init word (field splitting + globbing), iterate */
      struct node mini;
      for (int wi = 0; wi < n->ninit && !g_exit_requested; wi++) {
        mini.words = n->init_words;
        mini.wq = n->init_wq;
        mini.nwords = n->ninit;
        struct fields f;
        expand_word(&mini, wi, &f);
        for (int k = 0; k < f.nwords && !g_exit_requested; k++) {
          var_set(n->var, f.words[k], 0);
          st = sh_eval(n->body, in_fd, out_fd, err_fd);
          if (g_break_count > 0) {
            g_break_count--;
            k = f.nwords;
            wi = n->ninit;
            break;
          }
          if (g_continue_count > 0) {
            g_continue_count--;
            break;
          }
        }
        fields_free(&f);
      }
    } else {
      for (int i = 0; i < g_npos && !g_exit_requested; i++) {
        var_set(n->var, pos_get(i + 1), 0);
        st = sh_eval(n->body, in_fd, out_fd, err_fd);
        if (g_break_count > 0) {
          g_break_count--;
          break;
        }
        if (g_continue_count > 0) {
          g_continue_count--;
          continue;
        }
      }
    }
    break;
  }
  case N_CASE: {
    struct node mini;
    int wq = 2; /* marker: expand but never glob/split */
    mini.words = &n->case_word;
    mini.wq = &wq;
    mini.nwords = 1;
    struct fields fw;
    expand_word(&mini, 0, &fw);
    const char *wval = fw.nwords > 0 ? fw.words[0] : "";
    st = 0;
    for (int i = 0; i < n->npats; i++) {
      int got = 0;
      for (int k = 0; k < n->pats[i].npats; k++) {
        if (pat_match(n->pats[i].pats[k], wval)) {
          got = 1;
          break;
        }
      }
      if (got) {
        st = sh_eval(n->pats[i].body, in_fd, out_fd, err_fd);
        break;
      }
    }
    fields_free(&fw);
    break;
  }
  case N_FUNC: {
    struct func_def *fd = 0;
    for (int i = 0; i < g_func_n; i++) {
      if (s_eq(g_funcs[i].name, n->fname)) {
        fd = &g_funcs[i];
        break;
      }
    }
    if (!fd) {
      if (g_func_n >= SH_MAX_FUNCS) break;
      fd = &g_funcs[g_func_n++];
      fd->name = s_dup(n->fname);
    }
    fd->body = n->body;
    fd->defined = 1;
    break;
  }
  default:
    break;
  }
  g_laststatus = st;
  return st;
}

/* ------------------------------------------------------------------ */
/* builtins                                                            */
/* ------------------------------------------------------------------ */

/* Read the current directory for the interactive prompt. */
#ifdef HOST_TEST
/* The host build links compat.c, whose getcwd/chdir are a fixed in-memory
 * mock used by the desktop-app tests.  The shell needs the REAL cwd to
 * match dash on parity scripts, so bypass the mock via raw syscalls. */
#define sh_chdir(p) ((int)syscall(SYS_chdir, (p)))
#define sh_getcwd(b, s) ((syscall(SYS_getcwd, (b), (s)) > 0) ? 1 : 0)
#else
#define sh_chdir chdir
#define sh_getcwd(b, s) ((getcwd((b), (s)) != 0) ? 1 : 0)
#endif

static int do_cd(int argc, char **argv) {
  const char *dst = (argc > 1) ? argv[1] : "";
  if (!dst[0]) dst = "/";
  int rc = sh_chdir(dst);
  if (rc != 0) {
    out2("cd: no such file or directory: ");
    out2(dst);
    out2("\n");
    return 1;
  }
  return 0;
}

static int do_pwd(int argc, char **argv) {
  (void)argc;
  (void)argv;
  char buf[256];
  if (sh_getcwd(buf, sizeof buf)) {
    out1(buf);
    out1("\n");
    return 0;
  }
  return 1;
}

static int do_echo(int argc, char **argv) {
  int i = 1;
  int newline = 1;
  int printed = 0;
  if (i < argc && s_eq(argv[i], "-n")) {
    newline = 0;
    i++;
  }
  for (; i < argc; i++) {
    if (printed) out1(" ");
    out1(argv[i]);
    printed = 1;
  }
  if (newline) out1("\n");
  return 0;
}

static int printf_emit(const char *fmt, int argc, char **argv, int *argp) {
  /* returns 1 if an arg was consumed by this format item */
  const char *p = fmt;
  (void)argc;
  (void)argv;
  (void)argp;
  char conv = 0;
  if (*p == 0) return 0;
  conv = *p;
  int a = *argp;
  switch (conv) {
  case 's': {
    if (a < argc) {
      out1(argv[a]);
      *argp = a + 1;
    }
    break;
  }
  case 'd': case 'i': {
    if (a < argc) {
      int v = atoi(argv[a]);
      char b[24];
      itoa10(v, b, sizeof b);
      out1(b);
      *argp = a + 1;
    }
    break;
  }
  case 'c': {
    if (a < argc) {
      char c = argv[a][0];
      char s[2] = { c, 0 };
      out1(s);
      *argp = a + 1;
    }
    break;
  }
  case 'u': {
    if (a < argc) {
      unsigned long v = strtoul(argv[a], 0, 10);
      char b[24];
      int i = 0;
      unsigned long u = v;
      char t[24];
      if (u == 0) t[i++] = '0';
      while (u > 0 && i < 23) {
        t[i++] = (char)('0' + (u % 10));
        u /= 10;
      }
      int n = 0;
      while (i > 0) b[n++] = t[--i];
      b[n] = 0;
      out1(b);
      *argp = a + 1;
    }
    break;
  }
  case 'x': case 'X': case 'o': {
    if (a < argc) {
      unsigned long v = strtoul(argv[a], 0, 0);
      int base = (conv == 'o') ? 8 : 16;
      char b[32];
      int i = 0;
      if (v == 0) b[i++] = '0';
      while (v > 0 && i < 30) {
        int d = (int)(v % (unsigned long)base);
        b[i++] = (char)(d < 10 ? '0' + d : (conv == 'X' ? 'A' : 'a') + d - 10);
        v /= (unsigned long)base;
      }
      char r[32];
      int n = 0;
      while (i > 0) r[n++] = b[--i];
      r[n] = 0;
      out1(r);
      *argp = a + 1;
    }
    break;
  }
  default:
    break;
  }
  return 1;
}

static int do_printf(int argc, char **argv) {
  if (argc < 2) return 0;
  const char *fmt = argv[1];
  int argp = 2;
  char c;
  const char *p = fmt;
  while (*p) {
    c = *p;
    if (c == '\\') {
      p++;
      switch (*p) {
      case 'n': out1("\n"); p++; break;
      case 't': out1("\t"); p++; break;
      case 'r': out1("\r"); p++; break;
      case 'a': out1("\a"); p++; break;
      case 'b': out1("\b"); p++; break;
      case 'f': out1("\f"); p++; break;
      case 'v': out1("\v"); p++; break;
      case '\\': out1("\\"); p++; break;
      case '0': {
        int v = 0;
        p++;
        for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++, p++) {
          v = v * 8 + (*p - '0');
        }
        char s[2] = { (char)v, 0 };
        out1(s);
        break;
      }
      case 'x': {
        int v = 0;
        p++;
        for (int i = 0; i < 2 && ((*p >= '0' && *p <= '9') ||
                                  (*p >= 'a' && *p <= 'f') ||
                                  (*p >= 'A' && *p <= 'F')); i++, p++) {
          int d = (*p <= '9') ? *p - '0' :
                  ((*p >= 'a') ? *p - 'a' + 10 : *p - 'A' + 10);
          v = v * 16 + d;
        }
        char s[2] = { (char)v, 0 };
        out1(s);
        break;
      }
      default:
        break;
      }
      continue;
    }
    if (c == '%') {
      /* skip flags/width/precision, then the conversion */
      p++;
      while (*p && strchr("-+ #0", *p) && *p) p++;
      while (*p >= '0' && *p <= '9') p++;
      if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
      }
      if (*p == '%') {
        out1("%");
        p++;
      } else if (*p == 's' || *p == 'd' || *p == 'i' || *p == 'c' ||
                 *p == 'u' || *p == 'x' || *p == 'X' || *p == 'o') {
        char item[2] = { *p, 0 };
        printf_emit(item, argc, argv, &argp);
        p++;
      } else {
        if (*p) p++;
      }
      continue;
    }
    if (c) out1((char[2]){ c, 0 });
    p++;
  }
  return 0;
}

static int do_exit(int argc, char **argv) {
  g_exit_requested = 1;
  if (argc > 1) {
    g_exit_status = atoi(argv[1]);
  } else {
    g_exit_status = g_laststatus;
  }
  return g_exit_status;
}

static int do_export(int argc, char **argv) {
  if (argc == 1) {
    for (int i = 0; i < g_nvars; i++) {
      if (!g_vars[i].exported) continue;
      if (!g_vars[i].name[0]) continue;
      out1("export ");
      out1(g_vars[i].name);
      out1("='");
      out1(g_vars[i].val ? g_vars[i].val : "");
      out1("'\n");
    }
    return 0;
  }
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *eq = 0;
    for (const char *p = a; *p; p++) {
      if (*p == '=') {
        eq = p;
        break;
      }
    }
    if (eq) {
      char name[64];
      int n = (int)(eq - a);
      if (n > 63) n = 63;
      for (int k = 0; k < n; k++) name[k] = a[k];
      name[n] = 0;
      var_set(name, eq + 1, 1);
    } else {
      /* mark existing variable as exported */
      struct shell_var *v = var_find(a);
      if (v) v->exported = 1;
    }
  }
  return 0;
}

static int do_unset(int argc, char **argv) {
  for (int i = 1; i < argc; i++) var_unset(argv[i]);
  return 0;
}

static int do_set(int argc, char **argv) {
  if (argc == 1) {
    for (int i = 0; i < g_nvars; i++) {
      if (!g_vars[i].name[0]) continue;
      out1(g_vars[i].name);
      out1("='");
      out1(g_vars[i].val ? g_vars[i].val : "");
      out1("'\n");
    }
    return 0;
  }
  if (s_eq(argv[1], "--")) {
    pos_set_all(&argv[2], argc - 2);
    return 0;
  }
  if (s_eq(argv[1], "-x") || s_eq(argv[1], "-e") || s_eq(argv[1], "-u")) {
    return 0; /* accepted, no effect */
  }
  return 0;
}

static int do_shift(int argc, char **argv) {
  int k = (argc > 1) ? atoi(argv[1]) : 1;
  if (k < 0) k = 1;
  if (k > g_npos) {
    out2("sh: shift count out of range\n");
    return 1;
  }
  pos_shift(k);
  return 0;
}

static int do_read(int argc, char **argv) {
  /* read one line from fd 0, split on IFS, assign to the named vars */
  char line[SH_MAX_LINE];
  int len = 0;
  int r;
  while (len < SH_MAX_LINE - 1) {
    char c;
    r = read(0, &c, 1);
    if (r <= 0) break;
    if (c == '\n') break;
    line[len++] = c;
  }
  if (len == 0 && r <= 0) return 1; /* EOF with no input */
  line[len] = 0;
#ifdef SH_TRACE
  {
    out2("READ_LINE=[");
    out2(line);
    out2("]\n");
  }
#endif
  /* strip trailing \r (CRLF terminals) */
  while (len > 0 && (line[len - 1] == '\r')) {
    line[len - 1] = 0;
    len--;
  }
  const char *ifs = " \t";
  /* split into words, last var gets the remainder */
  const char *p = line;
  int vi = 1; /* argv index */
  int nvars = argc - 1;
  if (nvars == 0) nvars = 1; /* REPLY */
  for (; vi <= nvars; vi++) {
    /* skip leading IFS */
    while (*p && strchr(ifs, *p)) p++;
    const char *start = p;
    if (vi < nvars) {
      while (*p && !strchr(ifs, *p)) p++;
      char *word = (char *)malloc((size_t)(p - start) + 1);
      for (int i = 0; i < (int)(p - start); i++) word[i] = start[i];
      word[p - start] = 0;
      if (vi <= argc - 1) var_set(argv[vi], word, 0);
      else var_set("REPLY", word, 0);
      free(word);
    } else {
      /* last var: rest of the line */
      while (*p) p++;
      char *word = (char *)malloc((size_t)(p - start) + 1);
      for (int i = 0; i < (int)(p - start); i++) word[i] = start[i];
      word[p - start] = 0;
      if (vi <= argc - 1) var_set(argv[vi], word, 0);
      else var_set("REPLY", word, 0);
      free(word);
    }
  }
  return 0;
}

static int file_exists(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  close(fd);
  return 1;
}

static int test_unary(const char *op, const char *arg) {
  if (s_eq(op, "-n")) return s_len(arg) > 0;
  if (s_eq(op, "-z")) return s_len(arg) == 0;
  if (s_eq(op, "-e")) return file_exists(arg);
  if (s_eq(op, "-f")) return file_exists(arg);
  if (s_eq(op, "-d")) {
#ifdef HOST_TEST
    DIR *d = opendir(arg);
    if (d) {
      closedir(d);
      return 1;
    }
    return 0;
#else
    /* directory?  read_dir enumerates directories */
    struct sys_dirent e;
    int fd = open(arg, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    if (read_dir(arg, 0, &e) == 0) return 1; /* enumerable => directory */
    return 0;
#endif
  }
  if (s_eq(op, "-r") || s_eq(op, "-w") || s_eq(op, "-x")) {
    return file_exists(arg);
  }
  if (s_eq(op, "-s")) {
    return file_exists(arg);
  }
  return -1; /* unknown unary */
}

static int test_binary(const char *op, const char *a, const char *b) {
  if (s_eq(op, "=") || s_eq(op, "==")) return s_eq(a, b);
  if (s_eq(op, "!=")) return !s_eq(a, b);
  if (s_eq(op, "-eq")) return atoi(a) == atoi(b);
  if (s_eq(op, "-ne")) return atoi(a) != atoi(b);
  if (s_eq(op, "-lt")) return atoi(a) < atoi(b);
  if (s_eq(op, "-le")) return atoi(a) <= atoi(b);
  if (s_eq(op, "-gt")) return atoi(a) > atoi(b);
  if (s_eq(op, "-ge")) return atoi(a) >= atoi(b);
  return -1;
}

static int test_eval(int n, char **args) {
  /* args[0..n-1] are the expressions (argv shifted by 1) */
  if (n == 0) return 1;
  if (n == 1) return s_len(args[0]) > 0 ? 0 : 1;
  if (n == 2) {
    if (s_eq(args[0], "!")) {
      int v = test_eval(1, &args[1]);
      return v ? 0 : 1;
    }
    int t = test_unary(args[0], args[1]);
    if (t >= 0) return t ? 0 : 1;
    return 2; /* error */
  }
  if (n == 3) {
    if (s_eq(args[0], "!")) {
      int v = test_eval(2, &args[1]);
      return v ? 0 : 1;
    }
    int tb = test_binary(args[1], args[0], args[2]);
    if (tb >= 0) return tb ? 0 : 1;
    int tu = test_unary(args[0], args[1]);
    if (tu >= 0 && !args[2][0]) return tu ? 0 : 1;
    return 2;
  }
  if (n == 4) {
    if (s_eq(args[0], "!")) {
      int v = test_eval(3, &args[1]);
      return v ? 0 : 1;
    }
    return 2;
  }
  return 2;
}

static int do_test(int argc, char **argv) {
  /* argv[0] is test or [ */
  int n = argc - 1;
  if (s_eq(argv[0], "[")) {
    if (n >= 1 && s_eq(argv[n], "]")) n--;
    else {
      out2("sh: [: missing ]\n");
      return 2;
    }
  }
  int rc = test_eval(n, &argv[1]);
  return rc;
}

static int g_umask_value = 0022;

static int do_umask(int argc, char **argv) {
  if (argc >= 2) {
    int m = 0;
    const char *s = argv[1];
    while (*s && *s >= '0' && *s <= '7') {
      m = m * 8 + (*s - '0');
      s++;
    }
    g_umask_value = m & 0777;
    return 0;
  }
  char b[5];
  int d = g_umask_value & 0777;
  b[0] = '0';
  b[1] = (char)('0' + ((d >> 6) & 7));
  b[2] = (char)('0' + ((d >> 3) & 7));
  b[3] = (char)('0' + (d & 7));
  b[4] = 0;
  out1(b);
  out1("\n");
  return 0;
}

static int do_wait(int argc, char **argv) {
  if (argc > 1) {
    int pid = atoi(argv[1]);
    if (pid > 0) {
      int wst = 0;
      waitpid(pid, &wst, 0);
    }
    return 0;
  }
  for (int i = 0; i < g_nbg; i++) {
    int wst = 0;
    if (kill(g_bg_pids[i], 0) == 0) waitpid(g_bg_pids[i], &wst, 0);
  }
  g_nbg = 0;
  return 0;
}

static int do_help(int argc, char **argv) {
  (void)argc;
  (void)argv;
  out1("HobbyOS Bash-like Shell - POSIX-style scripting shell\n");
  out1("Builtins: cd pwd echo printf exit export unset set shift read test\n");
  out1("          [ true false : umask wait help clear\n");
  out1("Control:  if/elif/else/fi  while/until/do/done  for VAR in ... /do/done\n");
  out1("          case/esac  { list; }  ( subshell )  name() { ... } functions\n");
  out1("Operators: | && || ; &   Redirs: > >> < 2> 2>>\n");
  out1("Expansion: $VAR ${VAR} ${VAR:-def} $? $# $$,$@ $* $(cmd) `cmd`  globs * ? []\n");
  return 0;
}

static int do_clear(int argc, char **argv) {
  (void)argc;
  (void)argv;
  out1("\033[2J\033[H\f");
  return 0;
}

static int builtin_run(int which, int argc, char **argv) {
  switch (which) {
  case 0: return do_cd(argc, argv);
  case 1: return do_pwd(argc, argv);
  case 2: return do_echo(argc, argv);
  case 3: return do_printf(argc, argv);
  case 4: return do_exit(argc, argv);
  case 5: return do_export(argc, argv);
  case 6: return do_unset(argc, argv);
  case 7: return do_set(argc, argv);
  case 8: return do_shift(argc, argv);
  case 9: return do_read(argc, argv);
  case 10: case 11: return do_test(argc, argv);
  case 12: return 0; /* true */
  case 13: return 1; /* false */
  case 14: return 0; /* : */
  case 15: return do_umask(argc, argv);
  case 16: return do_wait(argc, argv);
  case 17: return do_help(argc, argv);
  case 18: return do_clear(argc, argv);
  case 19: { /* break [n] */
    int n = (argc > 1) ? atoi(argv[1]) : 1;
    if (n < 1) n = 1;
    g_break_count += n;
    return 0;
  }
  case 20: { /* continue [n] */
    int n = (argc > 1) ? atoi(argv[1]) : 1;
    if (n < 1) n = 1;
    g_continue_count += n;
    return 0;
  }
  case 21: { /* return [n] */
    int n = (argc > 1) ? atoi(argv[1]) : g_laststatus;
    g_returned = 1;
    g_return_status = n;
    return n;
  }
  default: return 0;
  }
}

/* call a user-defined function by name (if defined); returns -1 when not
 * found so the caller falls through to external lookup */
static int call_function(const char *name, int argc, char **argv) {
  for (int i = 0; i < g_func_n; i++) {
    if (g_funcs[i].defined && s_eq(g_funcs[i].name, name)) {
      /* save old positionals, set new ones from argv[1..] */
      char **saved_pos = g_pos;
      int saved_n = g_npos;
      g_pos = 0;
      g_npos = 0;
      pos_set_all(&argv[1], argc - 1);
      int st = sh_eval(g_funcs[i].body, 0, 1, 2);
      if (g_returned) {
        st = g_return_status;
        g_returned = 0;
      }
      /* restore positionals */
      for (int k = 0; k < g_npos; k++) {
        if (g_pos && g_pos[k]) free(g_pos[k]);
      }
      if (g_pos) free(g_pos);
      g_pos = saved_pos;
      g_npos = saved_n;
      return st;
    }
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* run whole input buffers                                             */
/* ------------------------------------------------------------------ */

/* Execute a full script text; returns the final exit status. */
static int sh_run_text(const char *text) {
#ifdef SH_TRACE
  out2("S1\n");
#endif
  tok_reset();
  if (sh_tokenize(text) != 0) {
    out2("sh: unterminated quote or bad escape\n");
    return 2;
  }
#ifdef SH_TRACE
  out2("S2\n");
#endif
  struct node *n = parse_program();
#ifdef SH_TRACE
  out2("S3\n");
#endif
  if (!n) return 0;
  int st = sh_eval(n, 0, 1, 2);
#ifdef SH_TRACE
  out2("S4\n");
#endif
  return st;
}

static int read_file(const char *name, char *buf, int cap) {
  int fd = open(name, O_RDONLY);
  if (fd < 0) return -1;
  int len = 0;
  for (;;) {
    int r = read(fd, buf + len, (cap - 1 - len) > 512 ? 512 : (cap - 1 - len));
    if (r <= 0) break;
    len += r;
    if (len >= cap - 1) break;
  }
  close(fd);
  buf[len] = 0;
  return len;
}

/* interactive REPL - speaks the desktop protocol like the original shell */
static int sh_interactive(void) {
  char curdir[128] = "/";
  char line_buf[SH_MAX_LINE];
  int line_len = 0;

  out1("\f=== Welcome to HobbyOS Shell ===\n");
  out1("Type 'help' to see available builtins and syntax.\n\n");

  getcwd(curdir, sizeof curdir);
  out1("user@hobbyos:");
  out1(curdir);
  out1("$ ");
  while (!g_exit_requested) {
    char c;
    int r = read(0, &c, 1);
    if (r > 0) {
      if (c == '\n') {
        line_buf[line_len] = 0;
        out1("\n");
        if (line_len > 0) {
          int st = sh_run_text(line_buf);
          g_laststatus = st;
          if (g_exit_requested) break;
        }
        line_len = 0;
        getcwd(curdir, sizeof curdir);
        out1("user@hobbyos:");
        out1(curdir);
        out1("$ ");
      } else if (c == '\b') {
        if (line_len > 0) {
          line_len--;
          out1("\b");
        }
      } else if (c == 27) {
        char seq[2];
        if (read(0, seq, 2) != 2) {
          /* just escape */
        }
      } else if (c >= 32 && c <= 126) {
        if (line_len < SH_MAX_LINE - 1) {
          line_buf[line_len++] = c;
          char o[1] = { c };
          write(1, o, 1);
        }
      }
    } else {
      out1("\nsh: stdin EOF, exiting\n");
      break;
    }
  }
  return g_exit_requested ? g_exit_status : 0;
}

int main(int argc, char **argv) {
  if (argc > 0 && argv[0] && argv[0][0]) {
    int i = 0;
    for (; argv[0][i] && i < 31; i++) g_argv0_buf[i] = argv[0][i];
    g_argv0_buf[i] = 0;
    g_argv0_set = 1;
  }
  /* default environment variables */
  var_set("IFS", " \t\n", 0);
  var_set("PATH", "/", 1);
  var_set("HOME", "/", 1);
  var_set("USER", "user", 1);
#ifdef HOST_TEST
  {
    unsigned int m = (unsigned int)umask(0);
    umask((unsigned int)m);
    g_umask_value = (int)(m & 0777);
  }
#endif

  if (argc >= 3 && s_eq(argv[1], "-c")) {
    /* sh -c script [name arg...] */
    const char *script = argv[2];
    if (argc >= 4) {
      pos_set_all(&argv[3], argc - 3);
    }
    int st = sh_run_text(script);
    return st;
  }
  if (argc >= 2 && !s_eq(argv[1], "-c") && argv[1][0] != '-') {
    /* sh scriptfile [args...] */
    static char script[65536];
    if (read_file(argv[1], script, sizeof script) < 0) {
      out2("sh: cannot open ");
      out2(argv[1]);
      out2("\n");
      return 127;
    }
    if (argc >= 3) pos_set_all(&argv[2], argc - 2);
    int st = sh_run_text(script);
    return st;
  }
  return sh_interactive();
}
