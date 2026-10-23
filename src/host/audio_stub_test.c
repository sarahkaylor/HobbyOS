/* src/host/audio_stub_test.c — GB-051 T1/T4 host golden "audio assert".
 *
 * Locks the recorded GB-029 silent-safe audio decision as a host test (the
 * same mechanism the kernel links), so the "audio assert" in GB-051's DoD is
 * proven before any kernel boots:
 *
 *   1. Opening the compat `/dev/dsp` (OSS) node returns ENODEV — a clean "no
 *      audio device", never a hang and never silent garbage.
 *   2. The silent-safe SDL path is the dummy driver and is silent-safe by
 *      construction.
 *   3. Differential: the ENODEV value we pin (19) matches the host's real
 *      <errno.h> ENODEV, so the compat constant is Linux-accurate.
 *
 * The in-OS T5 playthrough leg (running the app with SDL audio on intel under
 * the compat layer) is a separate, wall-gated boot; this locks the decision
 * the gate asserts.  Runs inside host_tests.
 */
#include <stdio.h>
#ifdef __linux__
#include <errno.h>
#endif

#include "audio_stub.h"

static int checks_run = 0, checks_failed = 0;
static void check(int ok, const char *name) {
  checks_run++;
  if (ok) printf("  PASS: %s\n", name);
  else    { checks_failed++; printf("  FAIL: %s\n", name); }
}

int main(void) {
  printf("=== GB-051 audio assert: silent-safe SDL + /dev/dsp ENODEV ===\n");

  /* 1. /dev/dsp (OSS) open is a silent-safe stub: ENODEV. */
  int rc = hb_oss_dsp_open();
  check(rc == -HB_ENODEV, "compat /dev/dsp open -> -ENODEV (clean no-device)");
  check(rc < 0, "compat /dev/dsp open is an error, not a hang / not garbage");

  /* 2. Silent-safe SDL path is the dummy driver. */
  check(hb_audio_silent_safe_is_dummy() == 1,
        "silent-safe SDL path is the dummy driver");

  /* 3. Differential: HB_ENODEV == real Linux ENODEV (19). */
#ifdef __linux__
  check(HB_ENODEV == ENODEV,
        "diff: HB_ENODEV matches host <errno.h> ENODEV");
#else
  check(HB_ENODEV == 19, "HB_ENODEV == 19 (Linux errno, locked)");
#endif

  printf("audio_stub_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed == 0 ? 0 : 1;
}
