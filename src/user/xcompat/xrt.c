/*
 * xrt.c - the three bytes primitives the compiler emits by itself.
 *
 * A DSO this early must not depend on the process's libc for anything, not
 * even the symbols clang synthesises behind your back: a struct assignment
 * becomes a memcpy() call, a large zeroing loop becomes memset(), and
 * string handling becomes strlen().  Those references would resolve from
 * whatever libc the compat process happens to have mapped, which is the
 * one thing this tree controls least.
 *
 * So the DSO carries hidden (non-exported) definitions.  Hidden matters:
 * if these were exported they could interpose on the game's own libc calls.
 *
 * Compiled only into the DSO (never into the host test, where glibc already
 * provides them).
 */

__attribute__((visibility("hidden"))) void *memcpy(void *dst, const void *src,
                                                   unsigned long n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  for (unsigned long i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

__attribute__((visibility("hidden"))) void *memset(void *dst, int c,
                                                   unsigned long n) {
  unsigned char *d = (unsigned char *)dst;
  for (unsigned long i = 0; i < n; i++) d[i] = (unsigned char)c;
  return dst;
}

__attribute__((visibility("hidden"))) unsigned long strlen(const char *s) {
  unsigned long n = 0;
  while (s[n]) n++;
  return n;
}

/* Some compiler-rt paths also reach for these two on x86-64; keeping them
 * here means the DSO never gains a new undefined symbol when optimisation
 * decisions change. */
__attribute__((visibility("hidden"))) int memcmp(const void *a, const void *b,
                                                 unsigned long n) {
  const unsigned char *x = (const unsigned char *)a;
  const unsigned char *y = (const unsigned char *)b;
  for (unsigned long i = 0; i < n; i++)
    if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  return 0;
}

__attribute__((visibility("hidden"))) void *memmove(void *dst, const void *src,
                                                    unsigned long n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  if (d < s) {
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
  } else {
    for (unsigned long i = n; i > 0; i--) d[i - 1] = s[i - 1];
  }
  return dst;
}
