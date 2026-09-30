/*
 * HobbyOS Phase-2 sysroot: stdio.c — vsnprintf family, byte-exact vs glibc.
 *
 * Implemented spec (C99 7.19.6.1 subset, no floats by design):
 *   flags   - + space # 0
 *   width   digits or *
 *   prec    .digits or .* (bare '.' == 0)
 *   length  hh h l ll z j t
 *   conv    d i u o x X c s p n %
 *
 * Deliberately absent: %f/%e/%g/%a (no FPU in userland), ' thousands
 * flag, %m, long double. Everything else matches glibc byte-for-byte,
 * proven by the host property test libc_printf_test.c.
 *
 * The vsscanf/sscanf family lives at the end of this file (P3.2: libc++'s
 * <locale> tier calls sscanf/__libcpp_sscanf_l).  Its numeric fields are
 * parsed through strto* so the byte-level behavior matches glibc's scanner
 * for the supported conversions; the host test libc_scanf_test.c diffs the
 * two implementations.
 *
 * Under HOST_TEST every public name is renamed hb_* so the host test can
 * link this TU together with glibc and diff the two implementations.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h> /* strto* for vsscanf's numeric fields */
#include <sys/types.h>
#ifndef HOST_TEST
#include "malloc.h" /* vasprintf's buffer (user_malloc.o in libc.a) */
#endif

#ifdef HOST_TEST
#define vsnprintf hb_vsnprintf
#define vsprintf  hb_vsprintf
#define snprintf  hb_snprintf
#define sprintf   hb_sprintf
#define vasprintf hb_vasprintf
#define asprintf  hb_asprintf
#define vsscanf   hb_vsscanf
#define sscanf    hb_sscanf
#endif

/* RIP (raw integer position) renderer state. */
typedef struct {
  char *dst;        /* destination buffer, or NULL when size == 0   */
  size_t size;      /* total capacity including the trailing NUL    */
  size_t n;         /* number of chars produced so far (glibc count)*/
} fmt_out_t;

static void out_char(fmt_out_t *o, char c) {
  if (o->dst && o->n + 1 < o->size)
    o->dst[o->n] = c;
  o->n++;
}

static void out_str(fmt_out_t *o, const char *s, size_t len) {
  size_t i;
  for (i = 0; i < len; i++)
    out_char(o, s[i]);
}

static void out_pad(fmt_out_t *o, char c, size_t count) {
  while (count-- > 0)
    out_char(o, c);
}

/* Render an unsigned value in base 2..16 into buf ASCENDING from the
 * least significant digit; caller emits from the end for MSB-first. */
static size_t fmt_uint_rev(unsigned long long v, unsigned base, int upper,
                           char *buf) {
  static const char ldig[] = "0123456789abcdef";
  static const char udig[] = "0123456789ABCDEF";
  const char *dig = upper ? udig : ldig;
  size_t n = 0;

  do {
    buf[n++] = dig[v % base];
    v /= base;
  } while (v != 0);
  return n;
}

#define FMT_FLAG_MINUS  0x01
#define FMT_FLAG_PLUS   0x02
#define FMT_FLAG_SPACE  0x04
#define FMT_FLAG_HASH   0x08
#define FMT_FLAG_ZERO   0x10

static void fmt_number(fmt_out_t *o, unsigned long long mag, int neg,
                       unsigned base, int upper, int flags, int width,
                       int prec, int has_prec, int show_sign) {
  char digits[66];
  size_t ndig, i;
  unsigned long long v = mag;
  size_t npref = 0;
  char prefix[3];
  int zeropad, spaces, total, preflen;

  ndig = fmt_uint_rev(mag, base, upper, digits);

  /* glibc folds %#o's leading zero INTO the digit string, so precision
   * pads inside it: %#.3o of 5 -> "005", %#.0o of 5 -> "05". */
  if (base == 8 && (flags & FMT_FLAG_HASH) && v != 0)
    digits[ndig++] = '0'; /* appended LSB-side -> emitted first */

  /* precision 0 with value 0 -> no digits (glibc), except %#o where
   * the '#' forces a single '0'. */
  if (has_prec && prec == 0 && v == 0
      && !(base == 8 && (flags & FMT_FLAG_HASH)))
    ndig = 0;

  /* sign: only d/i use +/space/'-' (glibc ignores them on u/o/x). */
  if (show_sign) {
    if (neg)
      prefix[npref++] = '-';
    else if (flags & FMT_FLAG_PLUS)
      prefix[npref++] = '+';
    else if (flags & FMT_FLAG_SPACE)
      prefix[npref++] = ' ';
  }
  if (base == 16 && (flags & FMT_FLAG_HASH) && v != 0) {
    prefix[npref++] = '0';
    prefix[npref++] = upper ? 'X' : 'x';
  }
  preflen = (int)npref;

  /* precision pads digits with zeros (min digits) */
  if (has_prec && (int)ndig < prec)
    zeropad = prec - (int)ndig;
  else
    zeropad = 0;

  total = preflen + zeropad + (int)ndig;

  /* '0' flag pads with zeros after the sign/prefix; a specified
   * precision disables it (glibc). */
  if ((flags & FMT_FLAG_ZERO) && !has_prec && !(flags & FMT_FLAG_MINUS)
      && total < width)
    zeropad += width - total;

  total = preflen + zeropad + (int)ndig;
  spaces = (width > total) ? width - total : 0;

  if (!(flags & FMT_FLAG_MINUS))
    out_pad(o, ' ', (size_t)spaces);

  out_str(o, prefix, npref);
  for (i = 0; i < (size_t)zeropad; i++)
    out_char(o, '0');
  for (i = 0; i < ndig; i++)
    out_char(o, digits[ndig - 1 - i]);

  if (flags & FMT_FLAG_MINUS)
    out_pad(o, ' ', (size_t)spaces);
}

/* Length-modifier table. */
enum {
  LEN_NONE = 0, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z, LEN_J, LEN_T
};

/* va_arg must be applied to the local va_list (clang's va_list is an
 * array/struct per ABI), so read the value with an inline expression
 * instead of passing the va_list around. */
#define GET_SIGNED(l)                                                   \
    ((l) == LEN_HH ? (long long)(signed char)va_arg(ap, int)            \
     : (l) == LEN_H ? (long long)(short)va_arg(ap, int)                 \
     : (l) == LEN_L ? (long long)va_arg(ap, long)                       \
     : (l) == LEN_LL ? (long long)va_arg(ap, long long)                 \
     : (l) == LEN_Z ? (long long)(__builtin_choose_expr(                \
         sizeof(size_t) == sizeof(long), va_arg(ap, long),              \
         va_arg(ap, long long)))                                        \
     : (l) == LEN_T ? (long long)(__builtin_choose_expr(                \
         sizeof(ptrdiff_t) == sizeof(long), va_arg(ap, long),           \
         va_arg(ap, long long)))                                        \
     : (l) == LEN_J ? (long long)va_arg(ap, intmax_t)                   \
     : (long long)va_arg(ap, int))

#define GET_UNSIGNED(l)                                                 \
    ((l) == LEN_HH ? (unsigned long long)(unsigned char)va_arg(ap, unsigned int)\
     : (l) == LEN_H ? (unsigned long long)(unsigned short)va_arg(ap, unsigned int)\
     : (l) == LEN_L ? (unsigned long long)va_arg(ap, unsigned long)     \
     : (l) == LEN_LL ? (unsigned long long)va_arg(ap, unsigned long long)\
     : (l) == LEN_Z ? (unsigned long long)(__builtin_choose_expr(       \
         sizeof(size_t) == sizeof(long), va_arg(ap, unsigned long),     \
         va_arg(ap, unsigned long long)))                               \
     : (l) == LEN_T ? (unsigned long long)(__builtin_choose_expr(       \
         sizeof(ptrdiff_t) == sizeof(long), va_arg(ap, unsigned long),  \
         va_arg(ap, unsigned long long)))                               \
     : (l) == LEN_J ? (unsigned long long)va_arg(ap, uintmax_t)         \
     : (unsigned long long)va_arg(ap, unsigned int))

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
  fmt_out_t o;
  o.dst = size ? str : NULL;
  o.size = size;
  o.n = 0;

  while (*fmt) {
    char c = *fmt++;
    int flags = 0, width = 0, prec = 0, has_prec = 0, len = LEN_NONE;
    long long sv;
    unsigned long long uv;

    if (c != '%') {
      out_char(&o, c);
      continue;
    }

    /* flags */
    for (;;) {
      if (*fmt == '-')      flags |= FMT_FLAG_MINUS, fmt++;
      else if (*fmt == '+') flags |= FMT_FLAG_PLUS, fmt++;
      else if (*fmt == ' ') flags |= FMT_FLAG_SPACE, fmt++;
      else if (*fmt == '#') flags |= FMT_FLAG_HASH, fmt++;
      else if (*fmt == '0') flags |= FMT_FLAG_ZERO, fmt++;
      else break;
    }

    /* width */
    if (*fmt == '*') {
      width = va_arg(ap, int);
      if (width < 0) { flags |= FMT_FLAG_MINUS; width = -width; }
      fmt++;
    } else {
      while (*fmt >= '0' && *fmt <= '9')
        width = width * 10 + (*fmt++ - '0');
    }

    /* precision */
    if (*fmt == '.') {
      fmt++;
      has_prec = 1;
      if (*fmt == '*') {
        prec = va_arg(ap, int);
        if (prec < 0) { has_prec = 0; prec = 0; }
        fmt++;
      } else {
        while (*fmt >= '0' && *fmt <= '9')
          prec = prec * 10 + (*fmt++ - '0');
      }
    }

    /* length */
    if (*fmt == 'h') {
      fmt++;
      if (*fmt == 'h') { len = LEN_HH; fmt++; } else len = LEN_H;
    } else if (*fmt == 'l') {
      fmt++;
      if (*fmt == 'l') { len = LEN_LL; fmt++; } else len = LEN_L;
    } else if (*fmt == 'z') { len = LEN_Z; fmt++; }
    else if (*fmt == 'j') { len = LEN_J; fmt++; }
    else if (*fmt == 't') { len = LEN_T; fmt++; }

    c = *fmt++;

    /* 'l' with c/s/% -> glibc ignores it. */
    switch (c) {
    case 'd':
    case 'i': {
        int neg;
        sv = GET_SIGNED(len);
        neg = (sv < 0);
        if (neg)
          uv = (unsigned long long)(-(sv + 1)) + 1ULL;
        else
          uv = (unsigned long long)sv;
        fmt_number(&o, uv, neg, 10, 0, flags, width, prec, has_prec, 1);
        break;
      }
    case 'u':
      uv = GET_UNSIGNED(len);
      fmt_number(&o, uv, 0, 10, 0, flags, width, prec, has_prec, 0);
      break;
    case 'o':
      uv = GET_UNSIGNED(len);
      fmt_number(&o, uv, 0, 8, 0, flags, width, prec, has_prec, 0);
      break;
    case 'x':
    case 'X':
      uv = GET_UNSIGNED(len);
      fmt_number(&o, uv, 0, 16, (c == 'X'), flags, width, prec,
                 has_prec, 0);
      break;
    case 'c': {
        char cc = (char)va_arg(ap, int);
        int sp = (width > 1) ? width - 1 : 0;
        if (!(flags & FMT_FLAG_MINUS))
          out_pad(&o, ' ', (size_t)sp);
        out_char(&o, cc);
        if (flags & FMT_FLAG_MINUS)
          out_pad(&o, ' ', (size_t)sp);
        break;
      }
    case 's': {
        const char *s = va_arg(ap, const char *);
        size_t slen, disp;
        int sp;
        if (!s) {
          /* glibc: NULL renders "(null)" EXCEPT when a precision
           * truncates it, then it renders nothing at all
           * (probe-verified: %.3s NULL -> "", %*.*s p=30 ->
           * "(null)"). */
          s = "(null)";
          slen = 6;
          disp = (has_prec && (size_t)prec < slen) ? 0 : slen;
        } else {
          slen = strlen(s);
          disp = (has_prec && (size_t)prec < slen) ? (size_t)prec
                                                  : slen;
        }
        sp = (width > (int)disp) ? width - (int)disp : 0;
        if (!(flags & FMT_FLAG_MINUS))
          out_pad(&o, ' ', (size_t)sp);
        out_str(&o, s, disp);
        if (flags & FMT_FLAG_MINUS)
          out_pad(&o, ' ', (size_t)sp);
        break;
      }
    case 'p': {
        void *p = va_arg(ap, void *);
        if (!p) {
          int sp = (width > 5) ? width - 5 : 0;
          if (!(flags & FMT_FLAG_MINUS))
            out_pad(&o, ' ', (size_t)sp);
          out_str(&o, "(nil)", 5);
          if (flags & FMT_FLAG_MINUS)
            out_pad(&o, ' ', (size_t)sp);
          break;
        }
        /* glibc: %p == 0x + minimal hex, honoring width, the +/space
         * sign flags, AND precision as the minimum digit count
         * (probe: %014.3p of 0x12 -> "0x012"). */
        uv = (unsigned long)(uintptr_t)p;
        fmt_number(&o, uv, 0, 16, 0, flags | FMT_FLAG_HASH, width, prec,
                   has_prec, 1);
        break;
      }
    case 'n': {
        long long cnt = (long long)o.n;
        switch (len) {
        case LEN_HH: *(signed char *)va_arg(ap, char *) = (signed char)cnt; break;
        case LEN_H:  *(short *)va_arg(ap, short *) = (short)cnt; break;
        case LEN_LL: *(long long *)va_arg(ap, long long *) = cnt; break;
        default:     *(int *)va_arg(ap, int *) = (int)cnt; break;
        }
        break;
      }
    case '%':
      out_char(&o, '%');
      break;
    default:
      out_char(&o, c);
      break;
    }
  }

  if (o.dst) {
    if (o.size > 0)
      o.dst[o.n < o.size ? o.n : o.size - 1] = '\0';
    else
      o.dst = NULL; /* size == 0: produce count only; glibc permits
                       a NULL str here, do not touch it */
  }
  return (int)o.n;
}

int vsprintf(char *str, const char *fmt, va_list ap) {
  return vsnprintf(str, (size_t)-1, fmt, ap);
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(str, size, fmt, ap);
  va_end(ap);
  return r;
}

/* GNU vasprintf/asprintf (P3.2: libc++'s verbose_abort.cpp + the locale
 * float-formatting fallback call vasprintf).  Two passes: size, then fill. */
int vasprintf(char **strp, const char *fmt, va_list ap) {
  va_list ap2;
  char *buf;
  int n;

  va_copy(ap2, ap);
  n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n < 0)
    return -1;

  buf = malloc((size_t)n + 1);
  if (buf == NULL) {
    errno = ENOMEM;
    return -1;
  }

  va_copy(ap2, ap);
  vsnprintf(buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);

  *strp = buf;
  return n;
}

int asprintf(char **strp, const char *fmt, ...) {
  va_list ap;
  int r;

  va_start(ap, fmt);
  r = vasprintf(strp, fmt, ap);
  va_end(ap);
  return r;
}

int sprintf(char *str, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(str, (size_t)-1, fmt, ap);
  va_end(ap);
  return r;
}

/* ------------------------------------------------------------- scan family
 *
 * C99 7.19.6.2 subset: %d %i %u %o %x %X %c %s %p %n %[set] and the
 * floating family %a %e %f %g (with E/G/A), each with hh h l ll j z t L
 * lengths, field widths and '*'-suppression.  No %m (glibc extension).
 *
 * Semantics follow glibc where the standard leaves room: whitespace is
 * skipped for every conversion except %c, %[ and %n; a conversion that
 * finds no input available at all is an input failure (EOF when nothing
 * has been assigned yet); a conversion that finds input but cannot match
 * it is a matching failure that stops the scan.  The host test
 * libc_scanf_test.c diffs both implementations on a shared corpus.
 */

enum {
  SCAN_LEN_NONE = 0,
  SCAN_LEN_HH,
  SCAN_LEN_H,
  SCAN_LEN_L,
  SCAN_LEN_LL,
  SCAN_LEN_J,
  SCAN_LEN_Z,
  SCAN_LEN_T,
  SCAN_LEN_BIG /* L */
};

typedef struct {
  const char *base; /* start of input, for %n */
  const char *p;    /* cursor */
} scan_t;

static int scan_is_space(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

static void scan_skip_ws(scan_t *in) {
  while (scan_is_space((unsigned char)*in->p))
    in->p++;
}

/* Failure exit: EOF only when no character was available at all and nothing
 * has been assigned yet. */
static int scan_fail(const scan_t *in, int assigns) {
  return (assigns == 0 && *in->p == '\0') ? EOF : assigns;
}

/* Cap a numeric field to `width` chars.  strto*() has no width argument, so
 * a width-limited field is parsed through a copy (heap-backed when it does
 * not fit on the stack).  Returns NULL when the field is empty (input
 * failure). */
#define SCAN_CAP_INLINE 128
static const char *scan_cap(const scan_t *in, int width, char *inline_buf,
                            size_t inline_sz, char **heap_out, int *wrapped) {
  size_t avail = strlen(in->p);
  *heap_out = NULL;
  *wrapped = 0;
  if (width > 0 && (size_t)width < avail)
    avail = (size_t)width;
  if (avail == 0)
    return NULL;
  if (avail < inline_sz) {
    memcpy(inline_buf, in->p, avail);
    inline_buf[avail] = '\0';
    return inline_buf;
  }
  {
    char *h = (char *)malloc(avail + 1);
    if (!h)
      return NULL;
    memcpy(h, in->p, avail);
    h[avail] = '\0';
    *heap_out = h;
    *wrapped = 1;
    return h;
  }
}

static void scan_store_signed(long long v, int len, int suppress, void *dst) {
  if (suppress)
    return;
  switch (len) {
  case SCAN_LEN_HH: *(signed char *)dst = (signed char)v; break;
  case SCAN_LEN_H:  *(short *)dst = (short)v; break;
  case SCAN_LEN_L:  *(long *)dst = (long)v; break;
  case SCAN_LEN_LL:
  case SCAN_LEN_J:
  case SCAN_LEN_Z:
  case SCAN_LEN_T:  *(long long *)dst = v; break;
  default:          *(int *)dst = (int)v; break;
  }
}

static void scan_store_unsigned(unsigned long long v, int len, int suppress,
                                void *dst) {
  if (suppress)
    return;
  switch (len) {
  case SCAN_LEN_HH: *(unsigned char *)dst = (unsigned char)v; break;
  case SCAN_LEN_H:  *(unsigned short *)dst = (unsigned short)v; break;
  case SCAN_LEN_L:  *(unsigned long *)dst = (unsigned long)v; break;
  case SCAN_LEN_LL:
  case SCAN_LEN_J:
  case SCAN_LEN_Z:
  case SCAN_LEN_T:  *(unsigned long long *)dst = v; break;
  default:          *(unsigned *)dst = (unsigned)v; break;
  }
}

static void scan_store_float(long double v, int len, int suppress, void *dst) {
  if (suppress)
    return;
  if (len == SCAN_LEN_BIG)
    *(long double *)dst = v;
  else if (len == SCAN_LEN_L)
    *(double *)dst = (double)v;
  else
    *(float *)dst = (float)v;
}

/* glibc's numeric scanners commit to a syntactic prefix once they see it:
 * "0x" without a following hex digit makes the whole conversion fail with
 * nothing consumed (probe-verified: "0x" -> rv 0, cursor unchanged).  Its
 * "%i accepts 0b1" binary extension is deliberately NOT implemented (no
 * in-tree caller needs it; documented in the header comment). */
static int scan_int_token_ok(const char *f, int base) {
  size_t i = 0;
  if (f[i] == '+' || f[i] == '-')
    i++;
  if ((base == 16 || base == 0) && f[i] == '0' &&
      (f[i + 1] == 'x' || f[i + 1] == 'X')) {
    char c = f[i + 2];
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
  }
  return 1;
}

/* Float-token scanner mirroring glibc's: an exponent marker ('e'/'E', or
 * 'p'/'P' for the hex form) requires at least one digit (after an optional
 * sign) or the conversion fails with nothing consumed (probe-verified:
 * "1e" -> rv 0, cursor unchanged).  Returns the token length, or 0 when
 * no valid float token starts here. */
static size_t scan_float_token(const char *f) {
  size_t i = 0;
  unsigned ndig = 0;

  if (f[i] == '+' || f[i] == '-')
    i++;

  {
    /* inf / infinity / nan / nan(chars) */
    const char *w = f + i;
    if ((w[0] == 'i' || w[0] == 'I') && (w[1] == 'n' || w[1] == 'N') &&
        (w[2] == 'f' || w[2] == 'F')) {
      i += 3;
      if ((w[3] == 'i' || w[3] == 'I') && (w[4] == 'n' || w[4] == 'N') &&
          (w[5] == 'i' || w[5] == 'I') && (w[6] == 't' || w[6] == 'T') &&
          (w[7] == 'y' || w[7] == 'Y'))
        i += 5;
      return i;
    }
    if ((w[0] == 'n' || w[0] == 'N') && (w[1] == 'a' || w[1] == 'A') &&
        (w[2] == 'n' || w[2] == 'N')) {
      i += 3;
      if (f[i] == '(') {
        i++;
        while (f[i] && f[i] != ')')
          i++;
        if (f[i] == ')')
          i++;
      }
      return i;
    }
  }

  if (f[i] == '0' && (f[i + 1] == 'x' || f[i + 1] == 'X')) {
    size_t j = i + 2;
    size_t hexes = 0;
    while ((f[j] >= '0' && f[j] <= '9') || (f[j] >= 'a' && f[j] <= 'f') ||
           (f[j] >= 'A' && f[j] <= 'F')) {
      j++;
      hexes++;
    }
    if (f[j] == '.') {
      j++;
      while ((f[j] >= '0' && f[j] <= '9') || (f[j] >= 'a' && f[j] <= 'f') ||
             (f[j] >= 'A' && f[j] <= 'F')) {
        j++;
        hexes++;
      }
    }
    if (hexes == 0)
      return 0;
    if (f[j] == 'p' || f[j] == 'P') {
      size_t k = j + 1;
      if (f[k] == '+' || f[k] == '-')
        k++;
      if (!(f[k] >= '0' && f[k] <= '9'))
        return 0;
      while (f[k] >= '0' && f[k] <= '9')
        k++;
      j = k;
    }
    return j;
  }

  while (f[i] >= '0' && f[i] <= '9') {
    i++;
    ndig++;
  }
  if (f[i] == '.') {
    i++;
    while (f[i] >= '0' && f[i] <= '9') {
      i++;
      ndig++;
    }
  }
  if (ndig == 0)
    return 0;
  if (f[i] == 'e' || f[i] == 'E') {
    size_t k = i + 1;
    if (f[k] == '+' || f[k] == '-')
      k++;
    if (!(f[k] >= '0' && f[k] <= '9'))
      return 0;
    while (f[k] >= '0' && f[k] <= '9')
      k++;
    i = k;
  }
  return i;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
  scan_t in;
  int assigns = 0;

  if (str == NULL || fmt == NULL)
    return EOF;
  in.base = str;
  in.p = str;

  while (*fmt) {
    int width;
    int suppress;
    int len;
    int spec;

    if (scan_is_space((unsigned char)*fmt)) {
      while (scan_is_space((unsigned char)*fmt))
        fmt++;
      scan_skip_ws(&in);
      continue;
    }
    if (*fmt != '%') {
      if (*in.p != *fmt)
        return scan_fail(&in, assigns);
      fmt++;
      in.p++;
      continue;
    }

    fmt++; /* past '%' */

    suppress = 0;
    if (*fmt == '*') {
      suppress = 1;
      fmt++;
    }

    width = 0;
    while (*fmt >= '0' && *fmt <= '9')
      width = width * 10 + (*fmt++ - '0');

    len = SCAN_LEN_NONE;
    if (*fmt == 'h') {
      fmt++;
      if (*fmt == 'h') { len = SCAN_LEN_HH; fmt++; } else len = SCAN_LEN_H;
    } else if (*fmt == 'l') {
      fmt++;
      if (*fmt == 'l') { len = SCAN_LEN_LL; fmt++; } else len = SCAN_LEN_L;
    } else if (*fmt == 'j') { len = SCAN_LEN_J; fmt++; }
    else if (*fmt == 'z') { len = SCAN_LEN_Z; fmt++; }
    else if (*fmt == 't') { len = SCAN_LEN_T; fmt++; }
    else if (*fmt == 'L') { len = SCAN_LEN_BIG; fmt++; }

    spec = (unsigned char)*fmt;
    if (spec == '\0')
      break; /* dangling '%' in the format: stop */

    if (spec != '%' && spec != 'c' && spec != 'n' && spec != '[')
      scan_skip_ws(&in);

    switch (spec) {
    case 'd':
    case 'i':
    case 'u':
    case 'o':
    case 'x':
    case 'X': {
        int base = 10;
        char cap[SCAN_CAP_INLINE];
        char *heap;
        char *endp = NULL;
        int wrapped;
        const char *field;

        if (spec == 'i')
          base = 0;
        else if (spec == 'o')
          base = 8;
        else if (spec == 'x' || spec == 'X')
          base = 16;

        field = scan_cap(&in, width, cap, sizeof(cap), &heap, &wrapped);
        if (!field)
          return scan_fail(&in, assigns);

        if ((spec == 'i' || spec == 'x' || spec == 'X') &&
            !scan_int_token_ok(field, base)) {
          if (wrapped)
            free(heap);
          return scan_fail(&in, assigns);
        }

        if (spec == 'd' || spec == 'i') {
          long long v = strtoll(field, &endp, base);
          if (endp == field)
            return scan_fail(&in, assigns);
          if (!suppress) /* a suppressed conversion consumes no argument */
            scan_store_signed(v, len, 0, va_arg(ap, void *));
        } else {
          unsigned long long v = strtoull(field, &endp, base);
          if (endp == field)
            return scan_fail(&in, assigns);
          if (!suppress)
            scan_store_unsigned(v, len, 0, va_arg(ap, void *));
        }
        in.p += (size_t)(endp - field);
        if (wrapped)
          free(heap);
        if (!suppress)
          assigns++;
        break;
      }
    case 'a':
    case 'e':
    case 'f':
    case 'g':
    case 'A':
    case 'E':
    case 'F':
    case 'G': {
        char cap[SCAN_CAP_INLINE];
        char *heap;
        char *endp = NULL;
        int wrapped;
        const char *field;
        long double v;

        field = scan_cap(&in, width, cap, sizeof(cap), &heap, &wrapped);
        if (!field)
          return scan_fail(&in, assigns);
        {
          size_t tok = scan_float_token(field);
          if (tok == 0) {
            if (wrapped)
              free(heap);
            return scan_fail(&in, assigns); /* glibc: nothing consumed */
          }
          if (len == SCAN_LEN_BIG)
            v = strtold(field, &endp);
          else
            v = (long double)strtod(field, &endp);
          if (endp == field || (size_t)(endp - field) != tok) {
            if (wrapped)
              free(heap);
            return scan_fail(&in, assigns);
          }
        }
        if (!suppress)
          scan_store_float(v, len, 0, va_arg(ap, void *));
        in.p += (size_t)(endp - field);
        if (wrapped)
          free(heap);
        if (!suppress)
          assigns++;
        break;
      }
    case 'p': {
        char cap[SCAN_CAP_INLINE];
        char *heap;
        char *endp = NULL;
        int wrapped;
        const char *field;
        unsigned long long v;

        field = scan_cap(&in, width, cap, sizeof(cap), &heap, &wrapped);
        if (!field)
          return scan_fail(&in, assigns);
        /* glibc parses %p like strtoul base 0 (num_put emits "0x...") */
        v = strtoull(field, &endp, 0);
        if (endp == field)
          return scan_fail(&in, assigns);
        if (!suppress)
          *(void **)va_arg(ap, void *) = (void *)(uintptr_t)v;
        in.p += (size_t)(endp - field);
        if (wrapped)
          free(heap);
        if (!suppress)
          assigns++;
        break;
      }
    case 's': {
        char *dst = suppress ? NULL : (char *)va_arg(ap, char *);
        int taken = 0;

        while (*in.p && !scan_is_space((unsigned char)*in.p) &&
               (width == 0 || taken < width)) {
          if (dst)
            dst[taken] = *in.p;
          taken++;
          in.p++;
        }
        if (taken == 0)
          return scan_fail(&in, assigns);
        if (dst)
          dst[taken] = '\0';
        if (!suppress)
          assigns++;
        break;
      }
    case 'c': {
        char *dst = suppress ? NULL : (char *)va_arg(ap, char *);
        int want = width > 0 ? width : 1;
        int taken = 0;

        while (taken < want && *in.p) {
          if (dst)
            dst[taken] = *in.p;
          taken++;
          in.p++;
        }
        if (taken < want)
          return scan_fail(&in, assigns);
        if (!suppress)
          assigns++;
        break;
      }
    case 'n': {
        /* C99: no input is consumed and the conversion is not counted in the
         * return value.  glibc stores even when the input is exhausted. */
        if (!suppress)
          scan_store_signed((long long)(in.p - in.base), len, 0,
                            va_arg(ap, void *));
        break;
      }
    case '[': {
        char set[256];
        int negate = 0;
        int i = 0;
        char *dst = NULL;
        int taken = 0;

        fmt++; /* past '[' */
        if (*fmt == '^') {
          negate = 1;
          fmt++;
        }
        if (*fmt == ']') { /* ']' first in the set is a literal */
          set[i++] = ']';
          fmt++;
        }
        while (*fmt && *fmt != ']' && i < 255) {
          if (*fmt == '-' && i > 0 && fmt[1] != '\0' && fmt[1] != ']') {
            unsigned char lo = (unsigned char)set[i - 1];
            unsigned char hi = (unsigned char)fmt[1];
            unsigned int c;
            for (c = (unsigned int)lo + 1; c <= (unsigned int)hi && i < 255; c++)
              set[i++] = (char)c;
            fmt += 2;
          } else {
            set[i++] = *fmt++;
          }
        }
        set[i] = '\0';
        if (*fmt == ']')
          fmt++; /* consume the closing ']' */

        if (!suppress)
          dst = (char *)va_arg(ap, char *);

        while (*in.p &&
               ((memchr(set, (unsigned char)*in.p, (size_t)i) != NULL) !=
                negate) &&
               (width == 0 || taken < width)) {
          if (dst)
            dst[taken] = *in.p;
          taken++;
          in.p++;
        }
        if (taken == 0)
          return scan_fail(&in, assigns);
        if (dst)
          dst[taken] = '\0';
        if (!suppress)
          assigns++;
        break;
      }
    case '%':
      if (*in.p != '%')
        return scan_fail(&in, assigns);
      in.p++;
      break;
    default:
      /* unknown conversion specifier: undefined behavior; stop scanning */
      return assigns;
    }
    fmt++;
  }
  return assigns;
}

int sscanf(const char *str, const char *fmt, ...) {
  va_list ap;
  int r;

  va_start(ap, fmt);
  r = vsscanf(str, fmt, ap);
  va_end(ap);
  return r;
}
