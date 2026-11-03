/*
 * alsa_null.c - libasound.so.2, the "no audio device" sink for the compat
 * class.
 *
 * Unity's audio backend (FMOD) dlopen()s libasound.so.2 and enumerates
 * devices before it opens one.  HobbyOS has no ALSA and no /dev/dsp yet
 * (M5 adds a null sink; a real one is beyond v1), so this DSO answers the
 * exact call set the game asks for -- 19 symbols, measured from
 * UnityPlayer.so's string table and cross-checked against the host's
 * libasound -- with "no such device" and lets the engine fall back to
 * silent playback.  Unity logs the failure and keeps running; the M3 gate
 * (a window on screen) does not depend on audio at all.
 *
 * Why "enumerate zero devices, then -ENODEV" instead of pretending to
 * open: FMOD's ALSA backend trusts a successful open and then blocks in
 * snd_pcm_writei() waiting for a period that will never complete.  An
 * empty device list plus a refused open is the failure path FMOD already
 * handles -- it disables the audio system and carries on.
 *
 * Built like libx11.c: -shared -nostdlib, no libc, no DT_NEEDED.
 */

#define SNDAPI __attribute__((visibility("default")))

typedef int snd_pcm_t;
typedef int snd_pcm_hw_params_t;
typedef void snd_pcm_uframes_t;
typedef unsigned long snd_pcm_uframes_t_u;
typedef int snd_pcm_stream_t;

#define ENOENT_ 2
#define ENODEV_ 19
#define EINVAL_ 22
#define EBADFD_ 77

/* ---- device enumeration: none ---------------------------------------- */

SNDAPI int snd_device_name_hint(int card, const char *iface, void ***hints) {
  (void)card; (void)iface;
  /* ALSA's contract is a NULL-terminated array, not a NULL pointer: callers
   * do `while (hints[i])` without checking the array itself.  Returning
   * NULL here segfaults FMOD's device walk (measured: Unity dies with
   * SIGSEGV at audio init when PulseAudio is absent). */
  static void *no_devices[1] = { 0 };
  if (hints) *hints = no_devices;
  return 0;
}

SNDAPI int snd_device_name_free_hint(void **hints) {
  (void)hints;
  return 0;
}

SNDAPI char *snd_device_name_get_hint(const void *hint, const char *id) {
  (void)hint; (void)id;
  return 0;
}

/* ---- the PCM surface ------------------------------------------------- */

SNDAPI int snd_pcm_open(snd_pcm_t **pcm, const char *name,
                        snd_pcm_stream_t stream, int mode) {
  (void)name; (void)stream; (void)mode;
  if (pcm) *pcm = 0;
  return -ENODEV_;                       /* no hardware, ever (v1) */
}

SNDAPI int snd_pcm_close(snd_pcm_t *pcm) {
  (void)pcm;
  return 0;                              /* idempotent, like ALSA */
}

SNDAPI int snd_pcm_nonblock(snd_pcm_t *pcm, int nonblock) {
  (void)pcm; (void)nonblock;
  return 0;
}

SNDAPI int snd_pcm_hw_params_sizeof(void) { return 256; }

SNDAPI int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *p) {
  (void)pcm;
  if (p) {
    unsigned char *b = (unsigned char *)p;
    for (int i = 0; i < 256; i++) b[i] = 0;
  }
  return 0;
}

SNDAPI int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *p) {
  (void)pcm; (void)p;
  return -ENODEV_;
}

SNDAPI int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                        int access) {
  (void)pcm; (void)p; (void)access;
  return -ENODEV_;
}

SNDAPI int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                        int format) {
  (void)pcm; (void)p; (void)format;
  return -ENODEV_;
}

SNDAPI int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm,
                                          snd_pcm_hw_params_t *p,
                                          unsigned channels) {
  (void)pcm; (void)p; (void)channels;
  return -ENODEV_;
}

SNDAPI int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                      unsigned rate, int dir) {
  (void)pcm; (void)p; (void)rate; (void)dir;
  return -ENODEV_;
}

SNDAPI int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm,
                                                  snd_pcm_hw_params_t *p,
                                                  snd_pcm_uframes_t *frames) {
  (void)pcm; (void)p; (void)frames;
  return -ENODEV_;
}

SNDAPI int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm,
                                                  snd_pcm_hw_params_t *p,
                                                  snd_pcm_uframes_t *frames,
                                                  int *dir) {
  (void)pcm; (void)p; (void)frames; (void)dir;
  return -ENODEV_;
}

SNDAPI int snd_pcm_prepare(snd_pcm_t *pcm) {
  (void)pcm;
  return -ENODEV_;
}

SNDAPI long snd_pcm_writei(snd_pcm_t *pcm, const void *buf,
                           unsigned long frames) {
  (void)pcm; (void)buf; (void)frames;
  return -ENODEV_;
}

SNDAPI long snd_pcm_readi(snd_pcm_t *pcm, void *buf, unsigned long frames) {
  (void)pcm; (void)buf; (void)frames;
  return -ENODEV_;
}

/* ---- error text ------------------------------------------------------ */

SNDAPI const char *snd_strerror(int err) {
  switch (err) {
  case 0: return "Success";
  case -ENOENT_: return "No such file or directory";
  case -ENODEV_: return "No such device";
  case -EINVAL_: return "Invalid argument";
  case -EBADFD_: return "File descriptor in bad state";
  default: return "Unknown error";
  }
}
