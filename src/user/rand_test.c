/* RANDTST.BIN — Phase F1.4 (browser.md): SYS_GETRANDOM acceptance.
 *
 * Quality bar actually enforced here (the deliverable's bar):
 *   - 16+ consecutive 8-byte draws all differ from each other;
 *   - a 4KiB buffer is filled (return == len) and is not all zero;
 *   - no 8-byte all-zero window occurs anywhere in the page (probability
 *     ~2^-64 per window if the mixer were sound);
 *   - consecutive 4KiB pages differ;
 *   - byte-value diversity: a sound stream over 4096 bytes should use well
 *     over 100 distinct byte values (an all-one-value or low-entropy
 *     collapse trips this).
 *   - flags != 0 -> -1/EINVAL; len 0 -> 0; a small partial-fill check.
 *
 * Regression value: this is what would catch a constant/degenerate entropy
 * source after a refactor (the pool is mixed from the arch cycle counter,
 * jitter and the RTC; see net.c's F1.4 section).
 *
 * Output convention: "  RANDTST <name>: PASS/FAIL" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "RANDTST FAILED: n".  Self-
 * terminating, no sleeps.
 */

#include "libc.h"
#include <stdint.h>

static int fails;

static void check(const char *name, int ok) {
  print_console("  RANDTST ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok) fails++;
}

static uint64_t load64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
  return v;
}

static void test_draws_differ(void) {
  uint64_t draws[16];
  int ok = 1;
  for (int i = 0; i < 16; i++) {
    if (getrandom(&draws[i], sizeof(uint64_t), 0) != (int)sizeof(uint64_t)) {
      ok = 0;
      break;
    }
  }
  for (int i = 0; ok && i < 16; i++) {
    for (int j = i + 1; j < 16; j++) {
      if (draws[i] == draws[j]) ok = 0;
    }
  }
  check("16 consecutive 8-byte draws all differ", ok);
  if (ok) {
    print_console("  RANDTST first draw: ");
    print_hex((long)draws[0]);
    print_console("\n");
  }
}

static void test_page_entropy(void) {
  static uint8_t page[4096];
  static uint8_t page2[4096];

  int n = getrandom(page, sizeof(page), 0);
  check("getrandom(4096) returns 4096", n == (int)sizeof(page));

  int all_zero = 1;
  for (unsigned i = 0; i < sizeof(page); i++) {
    if (page[i] != 0) {
      all_zero = 0;
      break;
    }
  }
  check("4KiB page is not all zero", !all_zero);

  int zero_window = 0;
  for (unsigned i = 0; i + 8 <= sizeof(page); i++) {
    if (load64(page + i) == 0) zero_window = 1;
  }
  check("no all-zero 8-byte window in the page", !zero_window);

  /* Byte-value diversity over the page. */
  unsigned seen[256];
  for (int i = 0; i < 256; i++) seen[i] = 0;
  for (unsigned i = 0; i < sizeof(page); i++) seen[page[i]]++;
  int distinct = 0;
  for (int i = 0; i < 256; i++) {
    if (seen[i]) distinct++;
  }
  check("byte values are diverse (>= 100 distinct)", distinct >= 100);

  int m = getrandom(page2, sizeof(page2), 0);
  int same = (m == (int)sizeof(page2));
  for (unsigned i = 0; i < sizeof(page) && same; i++) {
    if (page[i] != page2[i]) same = 0;
  }
  check("consecutive 4KiB pages differ", m == (int)sizeof(page2) && !same);
}

static void test_api_edges(void) {
  errno = 0;
  uint64_t v = 0;
  int r = getrandom(&v, sizeof(v), 1 /* unknown flags */);
  check("getrandom(flags != 0) -> -1/EINVAL", r == -1 && errno == EINVAL);

  r = getrandom(&v, 0, 0);
  check("getrandom(len 0) -> 0", r == 0);

  uint8_t small[3] = {0, 0, 0};
  r = getrandom(small, 3, 0);
  check("getrandom(3 bytes) returns 3 and touches the buffer",
        r == 3 && (small[0] || small[1] || small[2]));

  /* Sequential small draws must not repeat (the mixer advances every
   * call). */
  uint64_t a = 0, b = 0;
  getrandom(&a, sizeof(a), 0);
  getrandom(&b, sizeof(b), 0);
  check("two back-to-back draws differ", a != b);
}

__attribute__((section(".text._start"))) void _start(void) {
  print_console("RANDTST: F1.4 SYS_GETRANDOM entropy checks\n");

  test_draws_differ();
  test_page_entropy();
  test_api_edges();

  if (fails == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    exit(0);
  }
  print_console("RANDTST FAILED: ");
  print_dec(fails);
  print_console(" check(s)\n");
  exit(1);
}
