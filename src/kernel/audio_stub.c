/* src/kernel/audio_stub.c — device-free compat audio-surface decision (GB-029,
 * GB-051).  Same single TU links into the kernel and the host golden suite.
 *
 * See audio_stub.h for the contract.  These are the two halves of the recorded
 * silent-safe audio decision, made host-testable so GB-051's "audio assert"
 * (and the T5 in-OS gate when it runs) has a locked, verified substrate:
 *   - opening the compat `/dev/dsp` OSS node is a stub that returns ENODEV
 *     (clean "no audio device", silent-safe — never a hang, never garbage);
 *   - the SDL driver for the silent-safe path is the dummy driver.
 */
#include "audio_stub.h"

int hb_oss_dsp_open(void) {
  /* Recorded GB-029 decision: silent-safe stub.  A program that probes OSS
     audio gets ENODEV here, and SDL falls back to its dummy driver. */
  return -HB_ENODEV;
}

int hb_audio_silent_safe_is_dummy(void) {
  /* The silent-safe path is SDL's dummy driver (SDL_AUDIODRIVER=dummy):
     silent-safe by construction (no device node, no ALSA lib).  Returns 1 to
     lock that the chosen mechanism is dummy, not ALSA/OSS. */
  return 1;
}
