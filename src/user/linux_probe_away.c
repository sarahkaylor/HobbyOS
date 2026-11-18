/*
 * src/user/linux_probe_away.c — the kernel's [AWAY] instrument (H4-kglitch
 * round 2, H5-05; src/kernel/linux_compat_away.c) names what a periodic
 * waiter does while it is away from its clock.
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * A "timer" thread sets an ntsync auto-reset event every 10 ms (winealsa's
 * period event); a "client" thread waits on it in a loop (the WASAPI client).
 * Every 50th period the client goes away:
 *   phase S: nanosleep(80 ms)            -> an [AWAY] row with B=35:n or 230:n (musl: clock_nanosleep)
 *   phase U: a 80 ms busy loop in user   -> an [AWAY] row with U >= 4
 * tools/gate/lp_check_away.py grades the console: both rows, ms in 70..200.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct nts_event_args { uint32_t manual, signaled; };
struct nts_wait_args {
  uint64_t timeout, objs;
  uint32_t count, index, flags, owner, alert, pad;
};
#define NT_IOC_WAIT_ANY _IOWR('N', 0x82, struct nts_wait_args)
#define NT_IOC_CREATE_EVENT _IOW('N', 0x87, struct nts_event_args)
#define NT_IOC_EVENT_SET _IOR('N', 0x88, uint32_t)

static int dev, ev;
static volatile int stop;

static uint64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void *timer(void *a) {
  (void)a;
  while (!stop) {
    uint32_t prev = 0;
    ioctl(ev, NT_IOC_EVENT_SET, &prev);
    usleep(10000);
  }
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  dev = open("/dev/ntsync", O_RDWR);
  struct nts_event_args ea = {0, 0};
  ev = dev >= 0 ? ioctl(dev, NT_IOC_CREATE_EVENT, &ea) : -1;
  if (ev < 0) {
    printf("[LP] W0 /dev/ntsync event: open/create failed -> FAIL\n[LP] done rc=1\n");
    return 1;
  }
  pthread_t t;
  pthread_create(&t, 0, timer, 0);
  int32_t objs[1] = {ev};
  int waits = 0, aways = 0;
  uint64_t t0 = now_ns();
  /* 1.5 s of clock first, so the kernel calls the event a clock */
  while (now_ns() - t0 < 8000000000ull) {
    struct nts_wait_args w = {0};
    w.timeout = now_ns() + 200000000ull;
    w.objs = (uint64_t)(uintptr_t)objs;
    w.count = 1;
    ioctl(dev, NT_IOC_WAIT_ANY, &w);
    waits++;
    if (now_ns() - t0 > 1500000000ull && waits % 50 == 0) {
      if (aways % 2 == 0) {
        struct timespec ts = {0, 80000000};
        nanosleep(&ts, 0);
      } else {
        uint64_t b = now_ns();
        while (now_ns() - b < 80000000ull)
          ;
      }
      aways++;
    }
  }
  stop = 1;
  pthread_join(t, 0);
  printf("[LP] W1 %-44s waits=%d aways=%d -> %s (the console check decides)\n",
         "a periodic ntsync waiter went away", waits, aways, aways >= 4 ? "PASS" : "FAIL");
  printf("[LP] done rc=%d\n", aways >= 4 ? 0 : 1);
  return aways >= 4 ? 0 : 1;
}
