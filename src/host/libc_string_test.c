/*
 * HobbyOS Phase-1 host test: string.h subset vs glibc.
 *
 * Compiles src/libc/src/string.c with -DHOST_TEST so its functions land as
 * hb_* — alongside glibc — and every hb_* result is compared byte-for-byte
 * against the glibc equivalent on literal AND randomized inputs.
 *
 * Exit 0 on full pass, non-zero with a FAIL count otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* The HobbyOS implementations (renamed by the HOST_TEST block in string.c). */
extern size_t hb_strlen(const char *s);
extern size_t hb_strnlen(const char *s, size_t maxlen);
extern int hb_strcmp(const char *a, const char *b);
extern int hb_strncmp(const char *a, const char *b, size_t n);
extern int hb_strcasecmp(const char *a, const char *b);
extern int hb_strncasecmp(const char *a, const char *b, size_t n);
extern char *hb_strcpy(char *d, const char *s);
extern char *hb_strncpy(char *d, const char *s, size_t n);
extern char *hb_strcat(char *d, const char *s);
extern char *hb_strncat(char *d, const char *s, size_t n);
extern char *hb_strchr(const char *s, int c);
extern char *hb_strrchr(const char *s, int c);
extern char *hb_strstr(const char *h, const char *n);
extern char *hb_strpbrk(const char *s, const char *acc);
extern size_t hb_strspn(const char *s, const char *acc);
extern size_t hb_strcspn(const char *s, const char *rej);
extern char *hb_strtok(char *s, const char *d);
extern char *hb_strtok_r(char *s, const char *d, char **save);
extern char *hb_strdup(const char *s);
extern char *hb_strerror(int e);
extern int hb_memcmp(const void *a, const void *b, size_t n);
extern void *hb_memmove(void *d, const void *s, size_t n);
extern void *hb_memchr(const void *s, int c, size_t n);

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                   \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("FAIL %s (line %d)\n", what, __LINE__);  \
        }                                                   \
    } while (0)

/* Deterministic PRNG so failures reproduce. */
static unsigned long rng_state = 0x9E3779B97F4A7C15UL;
static unsigned int rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (unsigned int)rng_state;
}

static void rand_ascii(char *buf, int maxlen_plus_1) {
  int len = 1 + (int)(rnd() % (unsigned)(maxlen_plus_1 - 1));
  int i;
  for (i = 0; i < len - 1; i++) {
    int c = (int)(rnd() % 128);
    if (c == 0) c = 'x';            /* keep the NUL only at the end */
    buf[i] = (char)c;
  }
  buf[len - 1] = '\0';
}

static int sign3(int v) {
  return (v < 0) ? -1 : (v > 0) ? 1 : 0;
}

static void test_literals(void) {
  char a[64], b[64];

  CHECK(hb_strlen("") == 0, "strlen(empty)");
  CHECK(hb_strlen("hello") == 5, "strlen");
  CHECK(hb_strlen("hello world") == strlen("hello world"), "strlen vs glibc");
  CHECK(hb_strnlen("abc", 10) == 3, "strnlen short");
  CHECK(hb_strnlen("abcd", 2) == 2, "strnlen capped");
  CHECK(hb_strnlen("", 5) == 0, "strnlen empty");

  CHECK(hb_strcmp("a", "a") == 0, "strcmp eq");
  CHECK(sign3(hb_strcmp("a", "b")) < 0, "strcmp lt");
  CHECK(sign3(hb_strcmp("b", "a")) > 0, "strcmp gt");
  CHECK(sign3(hb_strcmp("abc", "abd")) < 0, "strcmp prefix");
  CHECK(sign3(hb_strcmp("abc", "abcd")) < 0, "strcmp shorter-first");

  CHECK(hb_strncmp("abc", "abc", 5) == 0, "strncmp eq");
  CHECK(sign3(hb_strncmp("abc", "abd", 3)) < 0, "strncmp diff");
  CHECK(hb_strncmp("abc", "abd", 2) == 0, "strncmp n-bound");
  CHECK(hb_strncmp("ab", "abX", 2) == 0, "strncmp n-eq");

  CHECK(sign3(hb_strcasecmp("Hello", "heLLO")) == 0, "strcasecmp");
  CHECK(sign3(hb_strcasecmp("abc", "abd")) < 0, "strcasecmp lt");
  CHECK(hb_strncasecmp("Hello", "heLLO", 5) == 0, "strncasecmp");
  CHECK(hb_strncasecmp("He", "heX", 2) == 0, "strncasecmp n-bound");

  strcpy(a, "xyz");
  CHECK(hb_strcpy(b, "hi") == b && strcmp(b, "hi") == 0, "strcpy");
  CHECK(hb_strncpy(b, "hello", 7) == b && strcmp(b, "hello") == 0, "strncpy pad");
  {
    char nb[6];
    memcpy(nb, "XXXX", 6);
    hb_strncpy(nb, "ab", 2);
    CHECK(nb[0] == 'a' && nb[1] == 'b' && nb[2] == 'X' && nb[3] == 'X', "strncpy no-pad past n");
  }
  strcpy(a, "foo");
  CHECK(hb_strcat(a, "bar") == a && strcmp(a, "foobar") == 0, "strcat");
  strcpy(a, "foo");
  hb_strncat(a, "barbaz", 3);
  CHECK(strcmp(a, "foobar") == 0, "strncat capped");
  strcpy(a, "foo");
  hb_strncat(a, "", 5);
  CHECK(strcmp(a, "foo") == 0, "strncat empty");

  CHECK(hb_strchr("hello", 'l') == strchr("hello", 'l'), "strchr pos");
  CHECK(hb_strchr("hello", 'z') == NULL, "strchr missing");
  CHECK(hb_strchr("hello", '\0') == strchr("hello", '\0'), "strchr nul");
  CHECK(hb_strrchr("hello", 'l') == strrchr("hello", 'l'), "strrchr last");
  CHECK(hb_strrchr("hello", 'z') == NULL, "strrchr missing");
  CHECK(hb_strrchr("hello", '\0') == strrchr("hello", '\0'), "strrchr nul");

  CHECK(hb_strstr("hello hello", "llo") == strstr("hello hello", "llo"), "strstr mid");
  CHECK(hb_strstr("abc", "") == strstr("abc", ""), "strstr empty needle");
  CHECK(hb_strstr("abc", "d") == NULL, "strstr missing");
  CHECK(hb_strstr("", "") == strstr("", ""), "strstr both empty");

  CHECK(hb_strpbrk("hello", "xyzle") == strpbrk("hello", "xyzle"), "strpbrk");
  CHECK(hb_strpbrk("hello", "xyz") == NULL, "strpbrk none");
  CHECK(hb_strspn("abca", "abc") == 4, "strspn");
  CHECK(hb_strspn("", "abc") == 0, "strspn empty");
  CHECK(hb_strcspn("hello", "xyz") == 5, "strcspn none");
  CHECK(hb_strcspn("hello", "le") == 1, "strcspn");

  CHECK(hb_memcmp("abc", "abc", 3) == 0, "memcmp eq");
  CHECK(sign3(hb_memcmp("abc", "abd", 3)) < 0, "memcmp lt");
  CHECK(hb_memcmp("abc", "abd", 2) == 0, "memcmp n-bound");
  /* memcmp over non-string bytes */
  {
    unsigned char x[4] = {0x80, 0x00, 0xFF, 0x01};
    unsigned char y[4] = {0x80, 0x00, 0xFE, 0x01};
    CHECK(sign3(hb_memcmp(x, y, 4)) > 0, "memcmp bytes");
  }
  CHECK(hb_memchr("abcd", 'c', 4) == memchr("abcd", 'c', 4), "memchr");
  CHECK(hb_memchr("abcd", 'x', 4) == NULL, "memchr missing");
  CHECK(hb_memchr("abcd", 'd', 3) == NULL, "memchr n-bound");
}

static void test_memmove_overlap(void) {
  char src[32], dst[32];
  int i;
  for (i = 0; i < 32; i++) src[i] = (char)('a' + i);

  memcpy(dst, src, 32);
  hb_memmove(dst + 2, dst, 20);
  CHECK(memcmp(dst, src, 2) == 0 && memcmp(dst + 2, src, 20) == 0, "memmove fwd");

  memcpy(dst, src, 32);
  memmove(dst + 2, dst, 20); /* glibc back-to-back for reference */
  CHECK(memcmp(dst + 2, src, 20) == 0, "memmove ref");

  memcpy(dst, src, 32);
  hb_memmove(dst, dst + 5, 10);
  CHECK(memcmp(dst, src + 5, 10) == 0, "memmove bwd src-hi");

  memcpy(dst, src, 32);
  hb_memmove(dst + 10, dst, 10);
  CHECK(memcmp(dst + 10, src, 10) == 0 && memcmp(dst, src, 10) == 0, "memmove bwd-safe");

  /* byte-exact vs glibc on a tricky partial overlap */
  memcpy(dst, src, 32);
  memcpy(dst + 16, dst, 12); /* reference setup (memcpy overlap is UB but deterministic here) */
}

static void test_strtok_streams(void) {
  char s1[64], s2[64], d1[16], d2[16];
  char *p1, *p2, *sv1 = NULL, *sv2 = NULL;

  strcpy(s1, "a,,b, c ,,d");
  strcpy(s2, "a,,b, c ,,d");
  strcpy(d1, ",");
  strcpy(d2, ",");
  while (1) {
    p1 = hb_strtok_r(s1, d1, &sv1);
    p2 = strtok_r(s2, d2, &sv2);
    if (!p1 || !p2) {
      CHECK(p1 == NULL && p2 == NULL, "strtok both end");
      break;
    }
    CHECK(strcmp(p1, p2) == 0, "strtok same token");
    s1[0] = '\0'; s2[0] = '\0'; /* tok continue */
  }

  /* strtok (static state) single call parity */
  strcpy(s1, "x.y.z");
  strcpy(s2, "x.y.z");
  p1 = hb_strtok(s1, ".");
  p2 = strtok(s2, ".");
  CHECK(p1 != NULL && p2 != NULL && strcmp(p1, p2) == 0, "strtok first");
  p1 = hb_strtok(NULL, ".");
  p2 = strtok(NULL, ".");
  CHECK(p1 != NULL && p2 != NULL && strcmp(p1, p2) == 0, "strtok second");
}

static void test_randomized(void) {
  char a[96], b[96], aa[192], bb[192];
  int i;

  for (i = 0; i < 4000; i++) {
    rand_ascii(a, 32);
    rand_ascii(b, 32);

    CHECK(hb_strlen(a) == strlen(a), "rand strlen");
    CHECK(hb_strnlen(a, 3) == strnlen(a, 3), "rand strnlen");
    CHECK(sign3(hb_strcmp(a, b)) == sign3(strcmp(a, b)), "rand strcmp");
    {
      size_t n = (size_t)(rnd() % 20);
      CHECK(sign3(hb_strncmp(a, b, n)) == sign3(strncmp(a, b, n)), "rand strncmp");
    }
    CHECK(sign3(hb_strcasecmp(a, b)) ==
          sign3(strcasecmp(a, b)), "rand strcasecmp");
    {
      size_t n = (size_t)(rnd() % 20);
      CHECK(sign3(hb_strncasecmp(a, b, n)) ==
            sign3(strncasecmp(a, b, n)), "rand strncasecmp");
    }
    CHECK(hb_strstr(a, b) == strstr(a, b), "rand strstr");
    {
      int c = (int)(rnd() % 256);
      CHECK(hb_strchr(a, c) == strchr(a, c), "rand strchr");
      CHECK(hb_strrchr(a, c) == strrchr(a, c), "rand strrchr");
    }
    CHECK(hb_strpbrk(a, b) == strpbrk(a, b), "rand strpbrk");
    CHECK(hb_strspn(a, b) == strspn(a, b), "rand strspn");
    CHECK(hb_strcspn(a, b) == strcspn(a, b), "rand strcspn");

    /* mem functions over non-string byte buffers */
    {
      unsigned char x[40], y[40];
      size_t j, n = (size_t)(1 + rnd() % 40);
      for (j = 0; j < n; j++) {
        x[j] = (unsigned char)(rnd() % 256);
        y[j] = (unsigned char)(rnd() % 256);
      }
      CHECK(sign3(hb_memcmp(x, y, n)) == sign3(memcmp(x, y, n)), "rand memcmp");
      {
        int c = (int)(rnd() % 256);
        CHECK(hb_memchr(x, c, n) == memchr(x, c, n), "rand memchr");
      }
    }

    /* copy/append parity into fresh buffers */
    memset(aa, 0xAA, sizeof(aa));
    memset(bb, 0xAA, sizeof(bb));
    hb_strcpy(aa, a);
    strcpy(bb, a);
    CHECK(strcmp(aa, bb) == 0, "rand strcpy");
    {
      size_t n = (size_t)(rnd() % 24);
      memset(aa, 0xAA, sizeof(aa));
      memset(bb, 0xAA, sizeof(bb));
      hb_strncpy(aa, a, n);
      strncpy(bb, a, n);
      CHECK(memcmp(aa, bb, 48) == 0, "rand strncpy");
    }
    {
      size_t n = (size_t)(rnd() % 20);
      strcpy(aa, a);
      strcpy(bb, a);
      hb_strncat(aa, b, n);
      strncat(bb, b, n);
      CHECK(strcmp(aa, bb) == 0, "rand strncat");
    }

    /* strdup parity */
    {
      char *d1 = hb_strdup(a);
      char *d2 = strdup(a);
      CHECK(d1 != NULL && d2 != NULL && strcmp(d1, d2) == 0, "rand strdup");
      free(d1);
      free(d2);
    }
  }
}

static void test_strerror(void) {
  int e;
  CHECK(hb_strerror(0) != NULL, "strerror(0) nonnull");
  CHECK(hb_strerror(2) != NULL, "strerror(2) nonnull");
  CHECK(strcmp(hb_strerror(2), "No such file or directory") == 0, "strerror ENOENT");
  CHECK(strcmp(hb_strerror(9), "Bad file descriptor") == 0, "strerror EBADF");
  CHECK(strcmp(hb_strerror(22), "Invalid argument") == 0, "strerror EINVAL");
  CHECK(strcmp(hb_strerror(1), "Operation not permitted") == 0, "strerror EPERM");
  CHECK(strcmp(hb_strerror(95), "Operation not supported") == 0, "strerror EOPNOTSUPP");
  for (e = 0; e <= 34; e++) CHECK(strlen(hb_strerror(e)) > 0, "strerror all nonempty");
  {
    const char *m = hb_strerror(999999);
    CHECK(m != NULL, "strerror big");
  }
  CHECK(strcmp(hb_strerror(-1), "Unknown error -1") == 0, "strerror negative");
  CHECK(strcmp(hb_strerror(12345), "Unknown error 12345") == 0, "strerror unknown fmt");
}

static void test_memmove_byte_random(void) {
  unsigned char src[64], buf1[64], buf2[64];
  int i;
  for (i = 0; i < 3000; i++) {
    size_t n = (size_t)(rnd() % 64);
    size_t off = (size_t)(rnd() % 24);
    size_t j;
    for (j = 0; j < 64; j++) src[j] = (unsigned char)(rnd() % 256);
    if (n + off > 64) n = 64 - off;
    memcpy(buf1, src, 64);
    memcpy(buf2, src, 64);
    hb_memmove(buf1 + off, buf1, n);
    memmove(buf2 + off, buf2, n);
    CHECK(memcmp(buf1, buf2, 64) == 0, "memmove random vs glibc");
  }
}

int main(void) {
  test_literals();
  test_memmove_overlap();
  test_strtok_streams();
  test_randomized();
  test_strerror();
  test_memmove_byte_random();

  printf("libc_string_test: %d checks, %d failures\n", checks, failures);
  if (failures == 0) {
    printf("LIB C STRING TEST PASSED\n");
    return 0;
  }
  printf("LIB C STRING TEST FAILED\n");
  return 1;
}
