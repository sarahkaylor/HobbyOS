// In-OS malloc/realloc integration test: exercises the real process heap
// (USER_VIRT_BASE-relative) which the host realloc test cannot reach.
#include "libc.h"
#include "malloc.h"

__attribute__((section(".text._start")))
void _start(void) {
  int failures = 0;
  char *a, *b, *c;
  int i;

  print_console("[MALLOC] starting\n");

  a = (char *)malloc(100);
  if (!a) { print_console("  FAIL malloc(100)\n"); failures++; goto out; }
  for (i = 0; i < 100; i++) a[i] = (char)(i & 0x7F);

  b = (char *)realloc(a, 5000);
  if (!b) { print_console("  FAIL realloc grow\n"); failures++; goto out; }
  for (i = 0; i < 100; i++) {
    if (b[i] != (char)(i & 0x7F)) {
      print_console("  FAIL realloc prefix\n");
      failures++;
      break;
    }
  }
  for (i = 100; i < 5000; i++) b[i] = (char)(i & 0x7F);

  c = (char *)realloc(b, 32);
  if (!c || c != b) {
    print_console("  FAIL realloc shrink in-place\n");
    failures++;
    goto out;
  }
  for (i = 0; i < 32; i++) {
    if (c[i] != (char)(i & 0x7F)) {
      print_console("  FAIL realloc shrink prefix\n");
      failures++;
      break;
    }
  }

  if (realloc(c, 0) != NULL) {
    print_console("  FAIL realloc(p,0)\n");
    failures++;
  }
  a = (char *)malloc(0);
  if (a != NULL) {
    print_console("  FAIL malloc(0)\n");
    failures++;
  }

out:
  if (failures == 0) {
    print_console("[MALLOC] ALL TESTS PASSED\n");
    exit(0);
  }
  print_console("[MALLOC] TESTS FAILED\n");
  exit(1);
}
