/* statp.c — w10-fstime lane stat probe (host-built, staged in-OS).
 * Subcommands:
 *   statp <path>...   print size + mtime/ctime (epoch + UTC breakdown)
 *   statp now         print the kernel RTC epoch as of now
 *   statp sleep N     busy-wait ~N wall seconds (RTC-anchored)
 * Serial output is the w10 timestamp evidence.
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

static void print_utc(long epoch) {
  int y, mo, d, h, mi, s;
  to_civil(epoch, &y, &mo, &d, &h, &mi, &s);
  print_dec(y); print("-"); print_2(mo); print("-"); print_2(d);
  print(" ");
  print_2(h); print(":"); print_2(mi); print(":"); print_2(s);
}

static long now_epoch(void) {
  struct sys_time t;
  if (sysinfo(6, &t, sizeof t) == 0) return (long)t.epoch;
  return -1;
}

static int eq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b) return 0;
    a++; b++;
  }
  return *a == *b;
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
  print(" UTC=");
  print_utc((long)st.st_mtime);
  print("\n");
}

static int parse_int(const char *s) {
  int v = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
  }
  return v;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (eq(argv[i], "now")) {
      long n = now_epoch();
      print("NOW-EPOCH=");
      print_dec((int)n);
      print(" UTC=");
      print_utc(n);
      print("\n");
    } else if (eq(argv[i], "sleep") && i + 1 < argc) {
      int sec = parse_int(argv[i + 1]);
      i++;
      print("SLEEP-START ");
      print_dec(sec);
      print("s\n");
      long start = now_epoch();
      while (now_epoch() < start + sec) {
      }
      print("SLEEP-END elapsed=");
      print_dec((int)(now_epoch() - start));
      print("s\n");
    } else {
      dump(argv[i]);
    }
  }
  return 0;
}
