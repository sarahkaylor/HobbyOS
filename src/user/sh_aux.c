/* sh_aux.c -- shell-only libc extras, linked ONLY into sh.bin (never
 * archived into libc.a, so its symbols cannot collide with the new
 * libc's stdlib.o (atoi/atol/strtoul) or appear as a second dup2).
 *
 * The old-trio link (user_libc.o + user_malloc.o + libc_string.o) has no
 * fd-duplication or integer parsing; the shell is the only old-trio
 * program that needs them, so they live here instead of user_libc.o.
 */
#include "libc.h"
#include "syscall.h"

#if defined(__x86_64__)
static long hb_syscall(long num, long a0, long a1, long a2) {
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx)
                   : "rcx", "r11", "memory");
  return ret;
}
#else /* aarch64 */
static long hb_syscall(long num, long a0, long a1, long a2) {
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2)
                   : "memory");
  return x0;
}
#endif

int dup(int fd) {
  return (int)hb_syscall(SYS_DUP, fd, 0, 0);
}

int dup2(int oldfd, int newfd) {
  return (int)hb_syscall(SYS_DUP2, oldfd, newfd, 0);
}

int atoi(const char *s) {
  int v = 0, sign = 1;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '-' || *s == '+') {
    if (*s == '-') sign = -1;
    s++;
  }
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v * sign;
}

long atol(const char *s) {
  long v = 0;
  int sign = 1;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '-' || *s == '+') {
    if (*s == '-') sign = -1;
    s++;
  }
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v * sign;
}

unsigned long strtoul(const char *s, char **endp, int base) {
  unsigned long v = 0;
  int sign = 1;
  const char *p = s;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '-' || *p == '+') {
    if (*p == '-') sign = -1;
    p++;
  }
  if (base == 0 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    base = 16;
    p += 2;
  } else if (base == 0 && p[0] == '0') {
    base = 8;
    p++;
  } else if (base == 0) {
    base = 10;
  }
  if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
  for (; *p; p++) {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
    else break;
    if (d >= base) break;
    v = v * (unsigned long)base + (unsigned long)d;
  }
  if (endp) *endp = (char *)p;
  if (sign < 0) v = (unsigned long)(0UL - v);
  return v;
}
