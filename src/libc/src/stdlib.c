/* HobbyOS Phase-1 libc: stdlib.c — numeric conversions, qsort/bsearch,
 * rand/srand, environment table, abort (posix.md Phase 1).
 *
 * Host builds rename everything to hb_* so tests can link this TU against
 * glibc and property-test byte-exact behavior (primarily the strtol
 * family). malloc/free are intentionally NOT renamed: on the host they are
 * glibc's, on the bare-metal targets they come from user_malloc.o (both
 * are already in libc.a).
 */
#include <stddef.h>
#include <limits.h>

#ifdef HOST_TEST
#include <stdlib.h>   /* glibc malloc/free for the env table + qsort */
#include <ctype.h>    /* glibc: identical whitespace classes */
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#else
#include "ctype.h"
#include "errno.h"
#include "malloc.h"
#endif

#ifdef HOST_TEST
#define atoi hb_atoi
#define atol hb_atol
#define atoll hb_atoll
#define strtol hb_strtol
#define strtoll hb_strtoll
#define strtoul hb_strtoul
#define strtoull hb_strtoull
#define abs hb_abs
#define labs hb_labs
#define llabs hb_llabs
#define qsort hb_qsort
#define bsearch hb_bsearch
#define rand hb_rand
#define srand hb_srand
#define getenv hb_getenv
#define setenv hb_setenv
#define putenv hb_putenv
#define unsetenv hb_unsetenv
#define abort hb_abort
#define environ hb_environ
#else
#include "stdlib.h"
#endif

/* ---------------- strtol family ---------------- */

static int hb_space(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
         c == '\f' || c == '\r';
}

static int hb_digit(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}

/* Shared strtox core. Returns 0 on success, -1 on overflow (magnitude
 * clamped to limit, ERANGE set), 1 when no conversion occurred (stop =
 * start, i.e. the whitespace-skipped start). */
static int hb_strtox_core(const char *nptr, const char **stop, int base,
                          int *sign_out, unsigned long long *out,
                          unsigned long long limit) {
  const char *p = nptr;
  int sign = 1;
  int cur_base = base;
  unsigned long long acc = 0;
  int overflow = 0;
  int any = 0;

  while (hb_space((unsigned char)*p)) p++;
  if (*p == '+' || *p == '-') {
    if (*p == '-') sign = -1;
    p++;
  }
  if (cur_base == 0) {
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
      cur_base = 16;
    } else if (p[0] == '0') {
      cur_base = 8;
    } else {
      cur_base = 10;
    }
  }
  if (cur_base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    p += 2; /* digits still required after the prefix */
  }
  for (;;) {
    int d = hb_digit((unsigned char)*p);
    if (d < 0 || d >= cur_base) {
      if (*p == '_') {
        /* glibc extension: digit separator, consumed only when a
         * valid digit follows it. */
        int nxt = hb_digit((unsigned char)p[1]);
        if (nxt >= 0 && nxt < cur_base) {
          p++;
          continue;
        }
      }
      break;
    }
    if (acc > (limit - (unsigned long long)d) /
              (unsigned long long)cur_base) {
      overflow = 1;
      acc = limit;
    } else {
      acc = acc * (unsigned long long)cur_base + (unsigned long long)d;
    }
    any = 1;
    p++;
  }
  *stop = p;
  *sign_out = sign;
  *out = acc;
  if (overflow) {
    errno = ERANGE;
    return -1;
  }
  return any ? 0 : 1;
}

static void hb_set_end(char **endptr, const char *nptr, const char *stop,
                       int rc) {
  if (endptr) *endptr = (char *)(rc == 1 ? nptr : stop);
}

long strtol(const char *nptr, char **endptr, int base) {
  /* Magnitude limit for BOTH signs: LONG_MIN is -9223372036854775808,
   * so the core clamps at 2^63 and reports overflow only past it; the
   * positive-side ERANGE is decided here (mag > LONG_MAX). */
  const unsigned long long LIM = 9223372036854775808ULL;
  const char *stop;
  int sign;
  unsigned long long mag;
  long r;
  int rc = hb_strtox_core(nptr, &stop, base, &sign, &mag, LIM);
  if (sign < 0) {
    r = (mag >= LIM) ? LONG_MIN : -(long)mag;
  } else {
    if (mag > (unsigned long long)LONG_MAX) {
      r = LONG_MAX;
      errno = ERANGE;
    } else {
      r = (long)mag;
    }
  }
  hb_set_end(endptr, nptr, stop, rc);
  return r;
}

long long strtoll(const char *nptr, char **endptr, int base) {
  const unsigned long long LIM = 9223372036854775808ULL;
  const char *stop;
  int sign;
  unsigned long long mag;
  long long r;
  int rc = hb_strtox_core(nptr, &stop, base, &sign, &mag, LIM);
  if (sign < 0) {
    r = (mag >= LIM) ? LLONG_MIN : -(long long)mag;
  } else {
    if (mag > (unsigned long long)LLONG_MAX) {
      r = LLONG_MAX;
      errno = ERANGE;
    } else {
      r = (long long)mag;
    }
  }
  hb_set_end(endptr, nptr, stop, rc);
  return r;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
  const char *stop;
  int sign;
  unsigned long long mag;
  unsigned long r;
  int rc = hb_strtox_core(nptr, &stop, base, &sign, &mag,
                          (unsigned long long)ULONG_MAX);
  if (sign < 0) {
    if (rc == -1) r = ULONG_MAX; /* negative overflow saturates */
    else r = (unsigned long)(0UL - (unsigned long)mag);
  } else {
    r = (unsigned long)mag;
  }
  hb_set_end(endptr, nptr, stop, rc);
  return r;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
  const char *stop;
  int sign;
  unsigned long long mag;
  unsigned long long r;
  int rc = hb_strtox_core(nptr, &stop, base, &sign, &mag, ULLONG_MAX);
  if (sign < 0) {
    if (rc == -1) r = ULLONG_MAX; /* negative overflow saturates */
    else r = 0ULL - mag;
  } else {
    r = mag;
  }
  hb_set_end(endptr, nptr, stop, rc);
  return r;
}

int atoi(const char *nptr) {
  return (int)strtol(nptr, (char **)0, 10);
}

long atol(const char *nptr) {
  return strtol(nptr, (char **)0, 10);
}

long long atoll(const char *nptr) {
  return strtoll(nptr, (char **)0, 10);
}

/* ---------------- abs/labs/llabs ---------------- */

int abs(int j) {
  unsigned int u = (unsigned int)j;
  return (j < 0) ? (int)(0U - u) : j;
}

long labs(long j) {
  unsigned long u = (unsigned long)j;
  return (j < 0) ? (long)(0UL - u) : j;
}

long long llabs(long long j) {
  unsigned long long u = (unsigned long long)j;
  return (j < 0) ? (long long)(0ULL - u) : j;
}

/* ---------------- qsort / bsearch ---------------- */

typedef int (*hb_cmpfn)(const void *, const void *);

static void hb_swap_bytes(char *a, char *b, size_t size) {
  char tmp;
  size_t i;
  for (i = 0; i < size; i++) {
    tmp = a[i];
    a[i] = b[i];
    b[i] = tmp;
  }
}

/* Swap-only insertion sort for small runs (no temp buffer needed). */
static void hb_insertion_sort(char *base, size_t n, size_t size, hb_cmpfn cmp) {
  size_t i;
  for (i = 1; i < n; i++) {
    size_t j = i;
    while (j > 0 && cmp(base + j * size, base + (j - 1) * size) < 0) {
      hb_swap_bytes(base + j * size, base + (j - 1) * size, size);
      j--;
    }
  }
}

/* Quicksort: median-of-three pivot, Lomuto partition (pivot ends up at the
 * right), recurse into the smaller side and loop on the larger so stack
 * depth stays O(log n) even in the degenerate case. */
static void hb_qsort_rec(char *base, size_t lo, size_t hi, size_t size,
                         hb_cmpfn cmp) {
  while (lo < hi) {
    size_t i, j, mid;

    if (hi - lo < 8) {
      hb_insertion_sort(base + lo * size, hi - lo + 1, size, cmp);
      return;
    }
    mid = lo + (hi - lo) / 2;
    if (cmp(base + lo * size, base + mid * size) > 0)
      hb_swap_bytes(base + lo * size, base + mid * size, size);
    if (cmp(base + lo * size, base + hi * size) > 0)
      hb_swap_bytes(base + lo * size, base + hi * size, size);
    if (cmp(base + mid * size, base + hi * size) > 0)
      hb_swap_bytes(base + mid * size, base + hi * size, size);
    hb_swap_bytes(base + mid * size, base + hi * size, size); /* pivot at hi */

    i = lo;
    for (j = lo; j < hi; j++) {
      if (cmp(base + j * size, base + hi * size) <= 0) {
        if (i != j) hb_swap_bytes(base + i * size, base + j * size, size);
        i++;
      }
    }
    hb_swap_bytes(base + i * size, base + hi * size, size);

    if (i - lo < hi - i) {
      hb_qsort_rec(base, lo, i > lo ? i - 1 : lo, size, cmp);
      lo = i + 1;
    } else {
      hb_qsort_rec(base, i + 1, hi, size, cmp);
      hi = i > lo ? i - 1 : lo;
    }
  }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
  if (nmemb <= 1 || size == 0) return;
  hb_qsort_rec((char *)base, 0, nmemb - 1, size, compar);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
  const char *b = (const char *)base;
  size_t lo = 0, hi = nmemb;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = compar(key, b + mid * size);
    if (c == 0) return (void *)(b + mid * size);
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return 0;
}

/* ---------------- rand/srand ---------------- */

static unsigned long hb_rand_state = 1;

void srand(unsigned int seed) {
  hb_rand_state = seed ? (unsigned long)seed : 1UL;
}

int rand(void) {
  hb_rand_state = (hb_rand_state * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
  return (int)hb_rand_state;
}

/* ---------------- environment table ---------------- */

char **environ = NULL;

/* Find the index of NAME (or -1) and its string length. */
static int hb_env_find(const char *name) {
  int i = 0;
  if (!environ) return -1;
  while (environ[i]) {
    const char *e = environ[i];
    const char *n = name;
    while (*n && *e == *n) {
      e++;
      n++;
    }
    if (*n == '\0' && *e == '=') return i;
    i++;
  }
  return -1;
}

static size_t hb_strlen(const char *s) {
  const char *p = s;
  while (*p) p++;
  return (size_t)(p - s);
}

static int hb_name_has_eq(const char *name) {
  while (*name) {
    if (*name == '=') return 1;
    name++;
  }
  return 0;
}

char *getenv(const char *name) {
  int i = hb_env_find(name);
  if (i < 0 || !environ) return 0;
  return environ[i] + (hb_strlen(name) + 1);
}

int setenv(const char *name, const char *value, int overwrite) {
  size_t nlen = hb_strlen(name);
  size_t vlen = hb_strlen(value);
  size_t i = 0, count = 0;
  char *entry = 0;
  char **narr;
  int idx;

  if (name[0] == '\0' || hb_name_has_eq(name)) {
    errno = EINVAL;
    return -1;
  }
  if ((idx = hb_env_find(name)) >= 0 && !overwrite) return 0;

  entry = (char *)malloc(nlen + vlen + 2);
  if (!entry) {
    errno = ENOMEM;
    return -1;
  }
  {
    char *d = entry;
    size_t k;
    for (k = 0; k < nlen; k++) *d++ = name[k];
    *d++ = '=';
    for (k = 0; k < vlen; k++) *d++ = value[k];
    *d = '\0';
  }

  if (idx >= 0) {
    /* Replace in place. */
    environ[idx] = entry;
    return 0;
  }

  if (environ) {
    while (environ[count]) count++;
  }
  narr = (char **)malloc((count + 2) * sizeof(char *));
  if (!narr) {
    errno = ENOMEM;
    return -1;
  }
  for (i = 0; i < count; i++) narr[i] = environ[i];
  narr[count] = entry;
  narr[count + 1] = 0;
  if (environ) free(environ);
  environ = narr;
  return 0;
}

int putenv(char *string) {
  const char *eq = 0;
  size_t nlen = 0;
  size_t count = 0;
  size_t i;
  char **narr;
  char namebuf[64];

  if (!string) {
    errno = EINVAL;
    return -1;
  }
  eq = string;
  while (*eq && *eq != '=') eq++;
  if (*eq != '=') {
    errno = EINVAL; /* no '=' -> invalid */
    return -1;
  }
  nlen = (size_t)(eq - string);
  if (nlen == 0 || nlen >= sizeof(namebuf)) {
    errno = EINVAL;
    return -1;
  }
  for (i = 0; i < nlen; i++) namebuf[i] = string[i];
  namebuf[nlen] = '\0';

  {
    int idx = hb_env_find(namebuf);
    if (idx >= 0) {
      environ[idx] = string; /* take ownership */
      return 0;
    }
  }
  if (environ) {
    while (environ[count]) count++;
  }
  narr = (char **)malloc((count + 2) * sizeof(char *));
  if (!narr) {
    errno = ENOMEM;
    return -1;
  }
  for (i = 0; i < count; i++) narr[i] = environ[i];
  narr[count] = string;
  narr[count + 1] = 0;
  if (environ) free(environ);
  environ = narr;
  return 0;
}

int unsetenv(const char *name) {
  int idx;
  size_t count = 0;
  if (name[0] == '\0' || hb_name_has_eq(name)) {
    errno = EINVAL;
    return -1;
  }
  idx = hb_env_find(name);
  if (idx < 0) return 0;
  while (environ[count]) count++;
  for (; (size_t)idx < count; idx++) environ[idx] = environ[idx + 1];
  return 0;
}

/* ---------------- assert support (_assert_fail) ---------------- */

void abort(void);   /* defined below; forward decl so _assert_fail can call it */

void _assert_fail(const char *file, int line, const char *func, const char *expr) {
  extern long write(int fd, const void *buf, unsigned long n);
  static const char pre[] = "assertion failed: ";
  static const char in[] = " in ";
  static const char op[] = " (";
  static const char cl[] = "):";
  static const char nl[] = "\n";
  char num[24];
  int i, len;

  write(2, pre, sizeof pre - 1);
  for (len = 0; expr && expr[len]; len++) ;
  write(2, expr, (unsigned long)len);
  write(2, in, sizeof in - 1);
  for (len = 0; func && func[len]; len++) ;
  write(2, func, (unsigned long)len);
  write(2, op, sizeof op - 1);
  for (len = 0; file && file[len]; len++) ;
  write(2, file, (unsigned long)len);
  write(2, cl, sizeof cl - 1);
  /* decimal line number, into num */
  i = 23;
  num[i] = '\0';
  if (line <= 0) {
    num[--i] = '0';
  } else {
    for (; line > 0 && i > 0; i--) {
      num[i - 1] = (char)('0' + line % 10);
      line /= 10;
    }
  }
  write(2, num + i, (unsigned long)(23 - i));
  write(2, nl, 1);
  abort();
}

/* ---------------- abort ---------------- */

void abort(void) {
#ifdef HOST_TEST
  /* Real SIGABRT death so host tests can fork+verify WIFSIGNALED. */
  kill(getpid(), SIGABRT);
  for (;;) { }
#else
  /* No signals yet (Phase 6); terminate with the SIGABRT status. exit
   * comes from user_libc.o (in libc.a) — avoid including libc.h here. */
  extern void exit(int status);
  exit(6);
#endif
}
