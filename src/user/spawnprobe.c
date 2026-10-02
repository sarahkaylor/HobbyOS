/* SPAWNPR.BIN — TEMP MEASUREMENT PROBE (l8-wave-harden lane, NOT a suite).
 *
 * Loaded first in the boot wave.  Measures the two quantities the
 * wave-harden lane's deliverables ask for:
 *   1. the table-full window: how long the live non-thread process count
 *      (sysinfo cmd 3) stays >= 55 of 63 slots while ~59 programs load,
 *   2. child-spawn latency: spawn HELLO.BIN every ~250 ms and time the
 *      spawn2() call; a latency >= 250 ms is the kernel's child-spawn
 *      park (LOAD_RETRY_CHILD_MS = 20000) firing, and errno EAGAIN after
 *      the park means the spawn worker gave up on the full table.
 *
 * Prints one line per notable event (latency >= 250 ms, EAGAIN, every
 * 5 s drain snapshot) plus a summary line, then exits — self-terminating
 * so it never blocks the wave.
 */
#include "libc.h"

static void putnum(long v) {
  char b[20];
  int i = 19, neg = 0;
  if (v < 0) {
    neg = 1;
    v = -v;
  }
  b[i--] = 0;
  if (v == 0) b[i--] = '0';
  while (v > 0) {
    b[i--] = (char)('0' + v % 10);
    v /= 10;
  }
  if (neg) b[i--] = '-';
  print_console(&b[i + 1]);
}

static void live_line(const char *tag, long now, int live) {
  print_console("[SPAWNPR] ");
  print_console(tag);
  print_console(" t=");
  putnum(now);
  print_console(" live=");
  putnum(live);
  print_console("\n");
}

__attribute__((section(".text._start"))) void _start(void) {
  print_console("[SPAWNPR] TEMP probe started\n");
  long t_start = sysinfo(1, 0, 0);
  long t_saturated = -1;
  long window_ms = 0;
  long first_drained = -1;
  long max_lat = 0;
  long slow_spawns = 0;
  long eagain = 0;
  long spawns = 0;
  long iter = 0;
  for (;;) {
    long now = sysinfo(1, 0, 0);
    if (now - t_start > 90000)
      break;
    struct sys_procinfo info[64];
    int live = sysinfo(3, info, (int)sizeof info);
    if (live >= 55) {
      if (t_saturated < 0)
        t_saturated = now;
      window_ms += 50;
    } else if (first_drained < 0 && live < 40) {
      first_drained = now;
      live_line("drained<40", now - t_start, live);
    }
    if (iter++ % 100 == 0)
      live_line("snapshot", now - t_start, live);
    if (iter % 5 == 0) {
      long t0 = sysinfo(1, 0, 0);
      errno = 0;
      int pid = spawn2("HELLO.BIN", -1, -1, -1, 0);
      long lat = sysinfo(1, 0, 0) - t0;
      spawns++;
      if (lat > max_lat)
        max_lat = lat;
      if (lat >= 250) {
        slow_spawns++;
        print_console("[SPAWNPR] SLOW spawn lat=");
        putnum(lat);
        print_console(" ms live=");
        putnum(live);
        print_console("\n");
      }
      if (pid < 0 && errno == EAGAIN) {
        eagain++;
        print_console("[SPAWNPR] EAGAIN after ");
        putnum(lat);
        print_console(" ms live=");
        putnum(live);
        print_console("\n");
      }
      if (pid >= 0)
        waitpid(pid, 0, 0);
    }
    usleep(50000); /* 50 ms */
  }
  print_console("[SPAWNPR] SUMMARY total_ms=");
  putnum(90000);
  print_console(" saturated_ms=");
  putnum(window_ms);
  print_console(" first_saturated_ms=");
  putnum(t_saturated < 0 ? -1 : t_saturated - t_start);
  print_console(" drained_at_ms=");
  putnum(first_drained < 0 ? -1 : first_drained - t_start);
  print_console(" spawns=");
  putnum(spawns);
  print_console(" max_lat_ms=");
  putnum(max_lat);
  print_console(" slow_spawns=");
  putnum(slow_spawns);
  print_console(" eagain=");
  putnum(eagain);
  print_console("\n");
  print_console("[SPAWNPR] probe done\n");
  exit(0);
}
