/* tools/cp/kglitch/xrunone.c -- one deliberate ALSA underrun (H4-kglitch,
 * cp-hunt H1-11).  Plays 300 ms, stops writing for 400 ms (the 100 ms buffer
 * runs dry: an xrun), recovers, plays 300 ms more.  Run under
 * tools/cp/kglitch/xrun-host-test.sh: the kernel's own linux_snd.c (host
 * shim) must print an "[SND] xrun" row while the PCM is still open -- before,
 * the count only appeared in the close row, which a game never reaches. */
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

static long play(snd_pcm_t *pcm, int ms, double *ph) {
  static short buf[480 * 2];
  long total = 48000L * ms / 1000, done = 0, recovers = 0;
  while (done < total) {
    for (int i = 0; i < 480; i++) {
      short v = (short)(12000 * sin(*ph));
      buf[2 * i] = buf[2 * i + 1] = v;
      *ph += 2 * M_PI * 440 / 48000;
    }
    snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, 480);
    if (w < 0) {
      recovers++;
      if (snd_pcm_recover(pcm, (int)w, 1) < 0)
        return -1;
      continue;
    }
    done += w;
  }
  return recovers;
}

int main(int argc, char **argv) {
  const char *dev = argc > 1 ? argv[1] : "hw:0,0";
  snd_pcm_t *pcm = 0;
  if (snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0 ||
      snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 100000) < 0) {
    printf("[XRUN] open/set_params failed\n[XRUN] done rc=1\n");
    return 1;
  }
  double ph = 0;
  long r1 = play(pcm, 300, &ph);
  printf("[XRUN] starving the PCM for 400 ms\n");
  fflush(stdout);
  usleep(400000);
  long r2 = play(pcm, 300, &ph);
  printf("[XRUN] recovers before=%ld after=%ld (want after >= 1)\n", r1, r2);
  fflush(stdout);
  snd_pcm_drain(pcm);
  snd_pcm_close(pcm);
  printf("[XRUN] done rc=%d\n", r2 >= 1 ? 0 : 1);
  return r2 >= 1 ? 0 : 1;
}
