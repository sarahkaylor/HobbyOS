#ifndef AUDIO_STUB_H
#define AUDIO_STUB_H

/* audio_stub.h — device-free compat audio-surface decision (GB-029, GB-051).
 *
 * GB-029 recorded the audio decision for the M2 static-musl SDL build: ship
 * SDL with the **dummy** audio driver (silent-safe by construction,
 * SDL_AUDIODRIVER=dummy) and make the compat `/dev/dsp` (OSS) node a
 * silent-safe stub that returns `ENODEV` on open, so a program probing OSS
 * audio gets a clean "no audio device" rather than a hang or silent garbage,
 * and SDL falls back to its dummy driver and the game runs without sound.
 *
 * This is GB-051's "audio assert" substrate: both halves of that decision are
 * pure, device-free functions (no /dev/dsp node, no ALSA lib), so the SAME
 * single TU links into the kernel and the host golden suite (the
 * nfs_proto / ipc_proto / evdev / fb_ioctl / input_node single-TU precedent).
 * The in-OS T5 playthrough leg (running the app with SDL audio on intel) is
 * separate and gated on the intel compat-run wall; this locks the decision
 * that the gate asserts.
 */

/* ENODEV as the compat surface reports it (Linux errno 19).  Locked here so
 * the host test differentially checks it against the host's <errno.h>. */
#define HB_ENODEV 19

/* Open the compat `/dev/dsp` (OSS) node.  Silent-safe stub: always -ENODEV —
 * a clean "no audio device", never a hang and never silent garbage.  Mirrors
 * the recorded GB-029 decision. */
int hb_oss_dsp_open(void);

/* The SDL audio driver chosen for the silent-safe path.  Returns 1 when the
 * mechanism is the dummy driver (SDL_AUDIODRIVER=dummy — silent-safe by
 * construction, no device node, no ALSA lib).  Exposes the decision so the
 * host assert can lock "SDL dummy = silent-safe". */
int hb_audio_silent_safe_is_dummy(void);

#endif /* AUDIO_STUB_H */
