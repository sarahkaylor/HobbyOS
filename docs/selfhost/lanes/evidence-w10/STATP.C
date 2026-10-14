/* statp.c — w10-fstime lane stat probe (host-built, staged in-OS).
 * Prints path, size and mtime (epoch seconds + UTC calendar breakdown)
 * so serial evidence shows 1980/0 before the fix and a real 2026 date
 * after.  Uses only libc.h primitives + stat() from the sysroot.
 */
#include "libc.h"
#include <sys/stat.h>

static void print_2(int v) {
  if (v < 10) print("0");
  print_dec(v);
}

/* Convert epoch seconds to UTC civil time (Hinnant-style, matches kernel). */
static void to_civil(long epoch, int *y, int *m, int *d, int *hh, int *mi, int *s) {
  long days = epoch / 86400, rem = epoch % 86400;
  *hh = (int)(rem / 3600); rem %= 3600;
  *mi = (int)(rem / 60); *s = (int)(rem % 60);
  long z = days + 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long yy = (long)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned dd = doy - (153 * mp + 2) / 5 + 1;
  unsigned mm = mp + (mp < 10 ? 3 : -9);
  *y = (int)(yy + (mm <= 2)); *m = (int)mm; *d = (int)dd;
}

static void dump(const char *path) {
  struct stat st;
  int rc = stat(path, &st);
  print(path);
  print(": stat=");
  print_dec(rc);
  if (rc != 0) { print("\n"); return; }
  print(" size=");
  print_dec((int)st.st_size);
  print(" mtime=");
  print_dec((int)st.st_mtime);
  print(" ctime=");
  print_dec((int)st.st_ctime);
  int y, mo, d, h, mi, s;
  to_civil((long)st.st_mtime, &y, &mo, &d, &h, &mi, &s);
  print(" UTC=");
  print_dec(y); print("-"); print_2(mo); print("-"); print_2(d);
  print(" ");
  print_2(h); print(":"); print_2(mi); print(":"); print_2(s);
  print("\n");
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) dump(argv[i]);
  return 0;
}
