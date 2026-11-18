/* tools/cp/audio/alsatone.c -- the `aplay`-equivalent alsa-lib test (lane
 * C4-audio, docs/cp-audio.md): an ordinary glibc-2.31 Linux program linked
 * against the alsa-lib built by tools/cp/audio/build-alsa.sh.
 *
 *   alsatone [DEVICE [HZ [MS]]]     default: "default" 660 1500
 *
 * Opens DEVICE for playback with snd_pcm_open (blocking), asks for S16_LE,
 * 2 channels, 48 kHz, 100 ms latency through snd_pcm_set_params (the plug
 * layer converts whatever the hw side refines to), writes MS ms of a HZ sine
 * with snd_pcm_writei (snd_pcm_recover on -EPIPE), drains and closes.  It
 * prints what alsa-lib negotiated (snd_pcm_dump) and "[ALSA] done rc=N".
 * The tone is graded on the host: tools/cp/audio/wavcheck.py --freq HZ. */
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
  const char *dev = argc > 1 ? argv[1] : "default";
  double hz = argc > 2 ? atof(argv[2]) : 660.0;
  int ms = argc > 3 ? atoi(argv[3]) : 1500;
  printf("[ALSA] alsa-lib %s, device '%s', %.0f Hz, %d ms\n", snd_asoundlib_version(), dev, hz, ms);
  fflush(stdout);
  int card = -1, ncards = 0;
  while (snd_card_next(&card) == 0 && card >= 0) {
    char *name = 0;
    snd_card_get_name(card, &name);
    printf("[ALSA] card %d: %s\n", card, name ? name : "?");
    free(name);
    ncards++;
  }
  snd_pcm_t *pcm = 0;
  int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    printf("[ALSA] snd_pcm_open: %s\n[ALSA] done rc=1\n", snd_strerror(err));
    return 1;
  }
  err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 100000);
  if (err < 0) {
    printf("[ALSA] snd_pcm_set_params: %s\n[ALSA] done rc=1\n", snd_strerror(err));
    return 1;
  }
  snd_output_t *out = 0;
  if (snd_output_stdio_attach(&out, stdout, 0) == 0) {
    snd_pcm_dump(pcm, out);
    snd_output_close(out);
  }
  snd_pcm_uframes_t bsz = 0, psz = 0;
  snd_pcm_get_params(pcm, &bsz, &psz);
  printf("[ALSA] buffer %lu frames, period %lu frames\n", (unsigned long)bsz, (unsigned long)psz);
  enum { CH = 480 };
  static short buf[CH * 2];
  double ph = 0;
  long total = 48 * ms, done = 0, recovers = 0;
  double t0 = now_s();
  while (done < total) {
    for (int i = 0; i < CH; i++) {
      short v = (short)(16000 * sin(ph));
      buf[2 * i] = buf[2 * i + 1] = v;
      ph += 2 * M_PI * hz / 48000.0;
      if (ph > 2 * M_PI) ph -= 2 * M_PI;
    }
    int off = 0;
    while (off < CH) {
      snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf + 2 * off, CH - off);
      if (w < 0) {
        recovers++;
        if (snd_pcm_recover(pcm, (int)w, 1) < 0) {
          printf("[ALSA] writei: %s\n[ALSA] done rc=1\n", snd_strerror((int)w));
          return 1;
        }
        continue;
      }
      off += (int)w;
    }
    done += CH;
  }
  double t1 = now_s();
  snd_pcm_sframes_t delay = 0;
  snd_pcm_delay(pcm, &delay);
  err = snd_pcm_drain(pcm);
  double t2 = now_s();
  printf("[ALSA] wrote %ld frames in %.3f s, delay %ld, drain %s in %.3f s, recovers %ld\n", done, t1 - t0,
         (long)delay, err < 0 ? snd_strerror(err) : "ok", t2 - t1, recovers);
  snd_pcm_close(pcm);
  int rc = (ncards < 1) + (err < 0) + ((t2 - t0) < ms / 1000.0 * 0.85);
  printf("[ALSA] done rc=%d\n", rc);
  return rc;
}
