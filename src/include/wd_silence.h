#ifndef WD_SILENCE_H
#define WD_SILENCE_H
/* wd_silence.h -- the x64 stall watchdog's "console silent" test
 * (src/kernel/arch/x64/trap.c watchdog_tick), as a pure function so the host
 * test (src/host/wd_silence_test.c) can check it.
 *
 * H4-kglitch / cp-hunt H1-14: watchdog_tick reads `now`, then its own [NFSIO]
 * row (or another CPU's print) stamps uart_last_activity_ms a millisecond
 * LATER.  The unsigned `now - last < limit` then wrapped to ~2^64 and the
 * watchdog reported "console silent for more than 20s" (and dumped every CPU)
 * right after the console printed: 15 of 22 H1-hunt game boots.  A console
 * stamped at or after `now` is not silent. */
#include <stdint.h>

static inline int wd_console_silent(uint64_t now, uint64_t last, uint64_t limit_ms) {
#ifdef WD_SILENCE_OLD /* the integ15 expression, for the host test's before-run */
  return !(now - last < limit_ms);
#else
  return last < now && now - last >= limit_ms;
#endif
}
#endif
