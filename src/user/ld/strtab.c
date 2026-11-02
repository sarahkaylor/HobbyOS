/*
 * strtab.c — string, hash and error-buffer primitives for LDHOBBY.SO.
 *
 * No libc: the interpreter is freestanding on both targets and everything it
 * needs is here.  The GNU/SysV hashes must match ld.so exactly or a valid
 * symbol silently fails to resolve, so they are written to the published
 * algorithms rather than an "equivalent" one.
 */

#include "strtab.h"

uint64_t hb_ldso_strlen(const char *s) {
  uint64_t n = 0;
  if (!s)
    return 0;
  while (s[n])
    n++;
  return n;
}

int hb_ldso_strcmp(const char *a, const char *b) {
  if (!a || !b)
    return a == b ? 0 : (a ? 1 : -1);
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

int hb_ldso_strncmp(const char *a, const char *b, uint64_t n) {
  if (!a || !b)
    return a == b ? 0 : (a ? 1 : -1);
  while (n && *a && *a == *b) {
    a++;
    b++;
    n--;
  }
  if (n == 0)
    return 0;
  return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

void hb_ldso_memcpy(void *dst, const void *src, uint64_t n) {
  volatile uint8_t *d = (volatile uint8_t *)dst;
  const volatile uint8_t *s = (const volatile uint8_t *)src;
  while (n--)
    *d++ = *s++;
}

void hb_ldso_memset(void *dst, int c, uint64_t n) {
  volatile uint8_t *d = (volatile uint8_t *)dst;
  while (n--)
    *d++ = (uint8_t)c;
}

void hb_ldso_strcpy(char *dst, const char *src, uint32_t cap) {
  uint32_t i = 0;
  if (!dst || cap == 0)
    return;
  if (src) {
    while (src[i] && i + 1 < cap) {
      dst[i] = src[i];
      i++;
    }
  }
  dst[i] = 0;
}

/* The GNU hash: h = 5381; h = h * 33 + c.  Must be 32-bit wrapping. */
uint32_t hb_ldso_gnu_hash(const char *name) {
  uint32_t h = 5381;
  const unsigned char *p = (const unsigned char *)name;
  while (*p)
    h = (h << 5) + h + (uint32_t)*p++; /* h*33 + c */
  return h;
}

/* The SysV hash (both the classic and the ELF variant of the loop). */
uint32_t hb_ldso_sysv_hash(const char *name) {
  uint32_t h = 0, g;
  const unsigned char *p = (const unsigned char *)name;
  while (*p) {
    h = (h << 4) + (uint32_t)*p++;
    g = h & 0xf0000000u;
    if (g)
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

/* --- error buffer ------------------------------------------------------ */

static uint32_t err_len(char *err, uint32_t cap) {
  uint32_t n = 0;
  if (!err || cap == 0)
    return 0;
  while (n + 1 < cap && err[n])
    n++;
  return n;
}

void hb_ldso_err_reset(char *err, uint32_t cap) {
  if (err && cap)
    err[0] = 0;
}

void hb_ldso_err_add(char *err, uint32_t cap, const char *s) {
  uint32_t n = err_len(err, cap);
  if (!err || cap == 0 || !s)
    return;
  while (*s && n + 1 < cap)
    err[n++] = *s++;
  err[n] = 0;
}

void hb_ldso_err_add_hex(char *err, uint32_t cap, uint64_t v) {
  static const char digits[] = "0123456789abcdef";
  char tmp[17];
  uint32_t i = 0, n;
  hb_ldso_err_add(err, cap, "0x");
  if (v == 0) {
    hb_ldso_err_add(err, cap, "0");
    return;
  }
  while (v && i < sizeof(tmp)) {
    tmp[i++] = digits[v & 0xfu];
    v >>= 4;
  }
  n = err_len(err, cap);
  while (i && err && n + 1 < cap)
    err[n++] = tmp[--i];
  if (err && cap)
    err[n] = 0;
}

void hb_ldso_err_add_dec(char *err, uint32_t cap, uint64_t v) {
  char tmp[21];
  uint32_t i = 0, n;
  if (v == 0) {
    hb_ldso_err_add(err, cap, "0");
    return;
  }
  while (v && i < sizeof(tmp)) {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  n = err_len(err, cap);
  while (i && err && n + 1 < cap)
    err[n++] = tmp[--i];
  if (err && cap)
    err[n] = 0;
}
