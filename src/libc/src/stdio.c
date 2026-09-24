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
 * Under HOST_TEST every public name is renamed hb_* so the host test can
 * link this TU together with glibc and diff the two implementations.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>

#ifdef HOST_TEST
#define vsnprintf hb_vsnprintf
#define vsprintf  hb_vsprintf
#define snprintf  hb_snprintf
#define sprintf   hb_sprintf
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

int sprintf(char *str, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(str, (size_t)-1, fmt, ap);
  va_end(ap);
  return r;
}
