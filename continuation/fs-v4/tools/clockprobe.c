/* clockprobe.c - T2-19 guest-clock receipt probe (FS lane V2c).
 *
 * Prints the guest RTC wall clock (sysinfo cmd 6) to the console/serial so a
 * runner console step can capture it side-by-side with the host wall clock.
 * Receipt tooling only -- documents the guest-vs-host relationship, does not
 * change any clock behavior ("probe, don't fix").
 *
 * Build (standalone, no Makefile change):
 *   cc -O2 -Wall -Wextra -Isrc/user_include -Isrc/user_include/graphics \
 *      -Isrc/include -Isrc/libc/include --target=aarch64-none-elf \
 *      -ffreestanding -mcpu=cortex-a53 -c src/user/libc.c -o obj/libc.o ... (see lane notes)
 * Link with src/user/linker.ld, objcopy -O binary.
 */
#include "libc.h"
#include <stdint.h>

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

int main(void) {
  struct sys_time t;
  int rc = sysinfo(6, &t, (int)sizeof t);
  if (rc != 0) {
    print("[CLOCKPX] no-rtc rc=");
    print_dec(rc);
    print("\n");
    return 1;
  }
  static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  int w = t.weekday;
  if (w < 0 || w > 6) w = 0;
  print("[CLOCKPX] epoch=");
  print_dec((long)(t.epoch));
  print(" sec; ");
  print_dec((long)t.year);
  print("-");
  if (t.month < 10) print("0");
  print_dec((long)t.month);
  print("-");
  if (t.day < 10) print("0");
  print_dec((long)t.day);
  print(" ");
  if (t.hour < 10) print("0");
  print_dec((long)t.hour);
  print(":");
  if (t.minute < 10) print("0");
  print_dec((long)t.minute);
  print(":");
  if (t.second < 10) print("0");
  print_dec((long)t.second);
  print(" UTC ");
  print(wd[w]);
  print("\n");
  return 0;
}
