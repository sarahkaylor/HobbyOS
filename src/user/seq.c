/* seq -- print a sequence of numbers.
   Fresh port with loose GNU coreutils parity: supports
     seq LAST            seq FIRST LAST          seq FIRST INCR LAST
   and -s SEP (separator), -w (equal width).  Integer-only: HobbyOS's
   ARM build uses -mgeneral-regs-only so doubles are unavailable; coreutils
   seq itself does exact integer math for the common integer cases. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *sep = "\n";
static int widen = 0;
static long first = 1, incr = 1, last = 0;

/* Parse a strict integer; error message mimicking seq's "invalid
 * floating point argument" (so scripts see a helpful, familiar line). */
static int parse_long(const char *s, long *out) {
  if (!s || !*s) return 0;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0') return 0;
  *out = v;
  return 1;
}

int main(int argc, char **argv) {
  int i;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && a[1] == 's') {
      if (a[2]) sep = a + 2;
      else if (i + 1 < argc) sep = argv[++i];
      continue;
    }
    if (a[0] == '-' && a[1] == 'w') { widen = 1; continue; }
    if (a[0] == '-' && a[1] == 'f') {
      fprintf(stderr, "seq: -f is not supported on HobbyOS (integer+%s only)\n",
              "-s/-w");
      return 1;
    }
  }

  {
    long vals[3];
    int nv = 0;
    for (i = 1; i < argc && nv < 3; i++) {
      const char *a = argv[i];
      if (a[0] == '-' && a[1] == 's') { i++; continue; } /* skip -s <sep> */
      if (a[0] == '-' && a[1] != '\0' && a[1] != '-' && a[1] != '+' &&
          (a[1] < '0' || a[1] > '9'))
        continue; /* an option */
      if (a[0] == '-' && a[1] == '-' && a[2] == '\0') continue;
      if (!parse_long(a, &vals[nv])) {
        fprintf(stderr, "seq: invalid floating point argument: '%s'\n", a);
        return 1;
      }
      nv++;
    }
    if (nv == 1) { first = 1; incr = 1; last = vals[0]; }
    else if (nv == 2) { first = vals[0]; incr = 1; last = vals[1]; }
    else if (nv >= 3) { first = vals[0]; incr = vals[1]; last = vals[2]; }
    else {
      fprintf(stderr, "seq: missing operand\n");
      return 1;
    }
  }

  if (incr == 0) {
    fprintf(stderr, "seq: invalid zero increment\n");
    return 1;
  }

  /* digit width for -w */
  int wfield = 0;
  if (widen) {
    char b[40];
    snprintf(b, sizeof b, "%ld", first);
    int l1 = (int)strlen(b);
    snprintf(b, sizeof b, "%ld", last);
    int l2 = (int)strlen(b);
    wfield = l1 > l2 ? l1 : l2;
  }

  int firstflag = 1;
  long v = first;
  long guard = 0;
  while ((incr > 0 ? v <= last : v >= last) && guard < 100000000L) {
    if (!firstflag) fputs(sep, stdout);
    firstflag = 0;
    if (widen) printf("%0*ld", wfield, v);
    else printf("%ld", v);
    v += incr;
    guard++;
  }
  if (!firstflag)
    fputc('\n', stdout);
  return 0;
}
