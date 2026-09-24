/*
 * HobbyOS Phase-1 host test: malloc.c's realloc (compiled as hb_*).
 *
 * The allocator's contract (independent of glibc's): realloc(NULL, n) ==
 * malloc(n); realloc(p, 0) == NULL (and frees); grow/shrink always
 * preserve the min(old,new) prefix byte-for-byte; a shrink keeps the same
 * pointer; a grow keeps the same pointer when the next block is free.
 * Churn test: random malloc/realloc/free with per-block tag patterns.
 */
#include <stdio.h>
#include <stdlib.h>

extern void *hb_malloc(size_t);
extern void hb_free(void *);
extern void *hb_calloc(size_t, size_t);
extern void *hb_realloc(void *, size_t);
extern int hb_heap_integrity(void);

static int total_checks = 0;
static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        total_checks++;                                                       \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s (line %d)\n", msg, __LINE__);                     \
        }                                                                     \
    } while (0)

static unsigned long rng_state = 0x9E3779B9UL;

static unsigned long prng(void) {
  rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
  return rng_state >> 33;
}

#define MAX_LIVE 48

static struct live {
  void *p;
  size_t n;
  int tag;
  int used;
} live[MAX_LIVE];

static void fill_pattern(struct live *L) {
  unsigned char *b = (unsigned char *)L->p;
  size_t i;
  for (i = 0; i < L->n; i++)
    b[i] = (unsigned char)((L->tag * 131 + (int)(i * 7)) & 0xFF);
}

static int check_pattern(struct live *L, size_t upto) {
  unsigned char *b = (unsigned char *)L->p;
  size_t i;
  for (i = 0; i < upto; i++) {
    if (b[i] != (unsigned char)((L->tag * 131 + (int)(i * 7)) & 0xFF))
      return 0;
  }
  return 1;
}

static void test_semantics(void) {
  char *p;

  p = (char *)hb_realloc(NULL, 100);
  CHECK(p != NULL, "realloc(NULL, 100) allocates");
  if (p) {
    size_t i;
    for (i = 0; i < 100; i++) p[i] = (char)i;
    CHECK(hb_realloc(p, 0) == NULL, "realloc(p, 0) returns NULL");
  }
}

static void test_grow_shrink(void) {
  char *p;
  size_t i;

  p = (char *)hb_malloc(100);
  CHECK(p != NULL, "malloc(100)");
  for (i = 0; i < 100; i++) p[i] = (char)(i & 0x7F);

  p = (char *)hb_realloc(p, 10000);
  CHECK(p != NULL, "grow 100 -> 10000");
  if (p) {
    int ok = 1;
    for (i = 0; i < 100; i++)
      if (p[i] != (char)(i & 0x7F)) ok = 0;
    CHECK(ok, "grow preserves first 100 bytes");
    for (i = 100; i < 10000; i++) p[i] = (char)(i & 0x7F);
  }

  p = (char *)hb_realloc(p, 16 * 1024 * 1024);
  CHECK(p != NULL, "grow to 16MB");
  if (p) {
    int ok = 1;
    for (i = 0; i < 100; i++)
      if (p[i] != (char)(i & 0x7F)) ok = 0;
    CHECK(ok, "grow to 16MB preserves prefix");
  } else {
    /* 16MB must fit in the fake host heap, but if it ever fails the
     * shrink below still runs on the old pointer — so only shrink
     * when the grow succeeded. */
  }

  /* shrink keeps the same pointer */
  {
    char *q = (char *)hb_malloc(2000);
    char *r;
    int ok;
    CHECK(q != NULL, "malloc(2000)");
    for (i = 0; i < 2000; i++) q[i] = (char)(i & 0x3F);
    r = q;
    r = (char *)hb_realloc(r, 40);
    CHECK(r == q, "shrink keeps pointer");
    if (r) {
      ok = 1;
      for (i = 0; i < 40; i++)
        if (r[i] != (char)(i & 0x3F)) ok = 0;
      CHECK(ok, "shrink preserves first 40 bytes");
    }
  }
}

static void test_churn(void) {
  int iter;
  int i;
  const int ITERS = 4000;
  const int verbose = 0;

  for (iter = 0; iter < ITERS; iter++) {
    int op = (int)(prng() % 100);
    if (op < 35) {
      /* malloc a new live block */
      int slot = -1;
      for (i = 0; i < MAX_LIVE; i++)
        if (!live[i].used) { slot = i; break; }
      if (slot >= 0) {
        size_t n = 1 + (size_t)(prng() % 20000);
        live[slot].p = hb_malloc(n);
        if (verbose) fprintf(stderr, "op%d MALLOC slot%d n%lu -> %p\n", iter, slot, (unsigned long)n, live[slot].p);
        live[slot].n = n;
        live[slot].tag = slot + 7;
        live[slot].used = live[slot].p != NULL;
        if (live[slot].used) fill_pattern(&live[slot]);
      }
    } else if (op < 60) {
      /* realloc an existing live block */
      int slot = (int)(prng() % MAX_LIVE);
      size_t oldn;
      if (!live[slot].used) continue;
      if (prng() % 8 == 0) {
        hb_free(live[slot].p);
        live[slot].used = 0;
        continue;
      }
      oldn = live[slot].n;
      if (prng() % 4 == 0) {
        /* shrink — must keep the same pointer */
        size_t n2 = 1 + (prng() % oldn);
        void *r = hb_realloc(live[slot].p, n2);
        if (verbose) fprintf(stderr, "op%d SHRINK slot%d %lu->%lu %p->%p\n", iter, slot, (unsigned long)oldn, (unsigned long)n2, live[slot].p, r);
        CHECK(r == live[slot].p, "churn shrink keeps pointer");
        live[slot].p = r;
        live[slot].n = n2;
      } else {
        size_t n2 = oldn + 1 + (size_t)(prng() % 20000);
        void *r = hb_realloc(live[slot].p, n2);
        if (verbose) fprintf(stderr, "op%d GROW slot%d %lu->%lu %p->%p\n", iter, slot, (unsigned long)oldn, (unsigned long)n2, live[slot].p, r);
        if (!r) { CHECK(0, "churn grow returned NULL"); continue; }
        live[slot].p = r;
        live[slot].n = n2;
      }
      CHECK(check_pattern(&live[slot], oldn < live[slot].n ? oldn
                                                           : live[slot].n),
            "churn realloc preserves pattern");
      fill_pattern(&live[slot]);
    } else if (op < 80) {
      int slot = (int)(prng() % MAX_LIVE);
      if (live[slot].used) {
        hb_free(live[slot].p);
        live[slot].used = 0;
      }
    } else {
      /* calloc */
      size_t n = 1 + (size_t)(prng() % 500);
      unsigned char *c = (unsigned char *)hb_calloc(1, n);
      int zero = 1;
      size_t k;
      if (c) {
        for (k = 0; k < n; k++)
          if (c[k] != 0) zero = 0;
        CHECK(zero, "calloc zeroes");
        hb_free(c);
      }
    }
    {
      int bad = hb_heap_integrity();
      if (bad && bad != 0) {
        fprintf(stderr, "HEAP CORRUPT after op %d: %d\n", iter, bad);
        abort();
      }
    }
  }

  /* free everything left and repeat a small second pass to exercise
   * coalescing after fragmentation */
  for (i = 0; i < MAX_LIVE; i++) {
    if (live[i].used) {
      hb_free(live[i].p);
      live[i].used = 0;
    }
  }
  {
    void *a = hb_malloc(1000);
    void *b = hb_malloc(1000);
    void *c;
    CHECK(a && b, "post-churn first-fit");
    hb_free(a);
    hb_free(b);
    c = hb_malloc(2000);
    CHECK(c != NULL, "post-churn coalesced 2000");
    if (c) hb_free(c);
  }
}

int main(void) {
  test_semantics();
  test_grow_shrink();
  test_churn();

  printf("libc_realloc_test: %d checks, %d failures\n", total_checks,
         failures);
  if (failures == 0) {
    printf("LIB C REALLOC TEST PASSED\n");
    return 0;
  }
  printf("LIB C REALLOC TEST FAILED\n");
  return 1;
}
