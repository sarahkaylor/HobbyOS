#ifndef KLINE_H
#define KLINE_H

/*
 * kline — assemble one console record and emit it with a SINGLE uart_puts.
 *
 * `uart_puts` holds uart_lock for its whole argument, so a record built from
 * several puts/print_int calls is atomic nowhere: another CPU interleaves at
 * every call boundary.  Measured on this tree, that shredded the console badly
 * enough that fault records and loader diagnostics appeared welded together
 * ("... local_ip=0x00000F02000A[KERNEL] pid=26 ..."), which is how several
 * wrong readings of the intel wave survived: the evidence itself was damaged.
 *
 * Every diagnostic that spans more than one value should use this.  The fault
 * record and [ALIVE-SET]/[HALT-CD] already carry private copies of the same
 * idea; this header exists so new ones do not add a fourth.
 *
 * Freestanding: no libc, no allocation, bounded output (truncates rather than
 * overruns).
 */

struct kline {
  char b[256];
  int n;
};

static inline void kl_init(struct kline *k) { k->n = 0; }

static inline void kl_s(struct kline *k, const char *s) {
  while (*s && k->n < (int)sizeof k->b - 1) k->b[k->n++] = *s++;
}

static inline void kl_c(struct kline *k, char c) {
  if (k->n < (int)sizeof k->b - 1) k->b[k->n++] = c;
}

static inline void kl_i(struct kline *k, long v) {
  char t[24];
  int i = 0, neg = 0;
  unsigned long u;
  if (v < 0) { neg = 1; u = (unsigned long)(-v); } else { u = (unsigned long)v; }
  if (u == 0) t[i++] = '0';
  while (u > 0 && i < 23) { t[i++] = (char)('0' + (u % 10)); u /= 10; }
  if (neg) kl_c(k, '-');
  while (i > 0) kl_c(k, t[--i]);
}

static inline void kl_hex(struct kline *k, unsigned long v) {
  static const char hx[] = "0123456789ABCDEF";
  int started = 0;
  kl_s(k, "0x");
  for (int sh = (int)(sizeof(unsigned long) * 8) - 4; sh >= 0; sh -= 4) {
    unsigned int d = (unsigned int)((v >> sh) & 0xF);
    if (d || started || sh == 0) { kl_c(k, hx[d]); started = 1; }
  }
}

/* Terminate and emit.  One uart_puts => one lock hold => one unbroken record. */
extern void uart_puts(const char *s);
static inline void kl_emit(struct kline *k) {
  if (k->n < (int)sizeof k->b) k->b[k->n] = '\0';
  else k->b[sizeof k->b - 1] = '\0';
  uart_puts(k->b);
  k->n = 0;
}

#endif /* KLINE_H */
