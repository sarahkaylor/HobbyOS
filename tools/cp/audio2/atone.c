/* tools/cp/audio2/atone.c -- the winealsa-model tone player (lane H5-audio2,
 * docs/cp-audio2.md).  A static musl Linux program linked against a static
 * alsa-lib 1.2.14 (tools/cp/audio2/build.sh); it runs as a boot-wave member
 * (tools/cp/audio2/run-atone.sh) with the opt-in virtio-sound device.
 *
 * It drives the kernel exactly the way Wine's winealsa.drv drives it in a game
 * boot (Wine 11.0 dlls/winealsa.drv/alsa.c, read for this model):
 *   - PCM "default" = plug -> mmap_emul -> hw:0,0 (the staged alsa.conf block,
 *     embedded here because the wave has no /CP/WINE/share/alsa);
 *   - FLOAT_LE 48 kHz stereo on the client side (Wine's mix format; the plug's
 *     lfloat converts to the card's S16);
 *   - period_time_near(10 ms), periods_near(4), start_threshold 1,
 *     stop_threshold = buffer;
 *   - a "timer" thread that every 10 ms (select() sleep with winealsa's
 *     +-period/2 drift clamp) runs winealsa's alsa_write_data: avail_update,
 *     XRUN state -> silent recover + reset + prepare, a one-period silence
 *     lead-in when ALSA holds nothing, then everything the local buffer holds
 *     up to avail, then signals the client event;
 *   - a "game" thread that waits for the event and fills the local (WASAPI)
 *     buffer to full with a continuous sine.
 * So every winealsa lead-in after an underrun is an exact-zero 10 ms period in
 * the host WAV, the signature of cp-hunt H1-01 (tools/cp/hunt/wavgaps.py).
 *
 *   ATONE.ELF SECONDS [key=value ...]
 *     hz=440         tone frequency
 *     lbuf_ms=40     local (WASAPI) buffer, ms
 *     stall_ms=0     inject one stall of the timer thread of this many ms ...
 *     stall_every=10 ... every this many seconds (first at stall_every)
 *     fmt=float|s16  client format
 *     skip=0         the game renders nothing on 1 event in N (random; a late
 *                    audio thread)
 *     gate=0         the candidate lead-in gate (wine-candidates/0101): a play
 *                    clock of what is still queued (frames written minus
 *                    elapsed x rate, never below buffer - avail); the lead-in
 *                    is written only when that is under gate periods
 *                    (gateh=N: N half periods)
 *     qsrc=clock|delay  where the gate's queue figure comes from: clock = the
 *                    player's own play clock (0101 v1), delay = snd_pcm_delay()
 *                    (0101 v2; the kernel's DELAY counts what QEMU holds)
 *     top=0          the top-up variant: lead-in only when the play clock says
 *                    fewer than top/2 periods are queued, and only enough
 *                    silence to bring the queue back to top/2 periods
 *     acct=orig|fix  winealsa's played-frames accounting: orig = Wine 11.0
 *                    (min(data_in_alsa, avail)), fix = the proposed patch
 *                    (data_in_alsa minus what ALSA still holds), docs/cp-audio2.md §6
 *     fill=0         periods the game renders per event (0 = fill the local
 *                    buffer; 1 = one period, an engine that keeps little
 *                    padding, so winealsa's lead-in after an xrun is silence)
 *
 * Rows: "[AT] ..." (a stat row every 10 s, one per underrun up to 64), then
 * "[AT] done rc=0 ...".  The verdict is the host's (WAV + kernel rows). */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

#define RATE 48000
#define CH 2

static const char CONF[] =
    "pcm.cpdefault { type plug slave.pcm { type mmap_emul slave.pcm { type hw card 0 device 0 } } }\n";

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static snd_pcm_t *pcm;
static int use_float = 1;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev = PTHREAD_COND_INITIALIZER;
static int ev_set, quit;

/* winealsa's stream bookkeeping (frames) */
static unsigned bufsize_frames, period_frames;
static snd_pcm_uframes_t alsa_bufsize, alsa_period;
static unsigned held_frames, lcl_offs, wri_offs, data_in_alsa;
static float *local_f; /* bufsize_frames * CH */
static short *local_s;
static float *sil_f;
static short *sil_s;
static double ph, hz = 440.0;
static int fill_periods, skip_n, acct_fix, gate_periods, top_half;
static double q_est, q_est_t;
static int qsrc_delay;
static long n_gated;
static unsigned rng = 12345;

/* counters */
static long n_xrun_state, n_epipe, n_leadin, n_ticks, n_late15, n_late25, n_late40;
static long n_starved_fill; /* ticks where the local buffer held < one period */
static double max_gap;
static long frames_written, frames_silence;
static int lib_err_rows;

static void lib_err(const char *file, int line, const char *fn, int err, const char *fmt, ...) {
  (void)file;
  (void)err;
  if (lib_err_rows++ < 16) {
    char m[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    printf("[AT] alsa-lib %s:%d %s\n", fn ? fn : "?", line, m);
  }
}

static snd_pcm_sframes_t write_best_effort(const void *buf, snd_pcm_uframes_t frames) {
  snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, frames);
  if (w < 0) {
    if (w == -EAGAIN) return 0;
    n_epipe++;
    int r = snd_pcm_recover(pcm, (int)w, 1);
    if (r < 0) return r;
    w = snd_pcm_writei(pcm, buf, frames);
  }
  if (w > 0) q_est += (double)w;
  return w;
}

static const void *local_at(unsigned off) {
  return use_float ? (const void *)(local_f + (size_t)off * CH) : (const void *)(local_s + (size_t)off * CH);
}

static snd_pcm_sframes_t write_wrap(unsigned offs, unsigned to_write) {
  snd_pcm_sframes_t ret = 0;
  while (to_write) {
    unsigned chunk = offs + to_write > bufsize_frames ? bufsize_frames - offs : to_write;
    snd_pcm_sframes_t t = write_best_effort(local_at(offs), chunk);
    if (t < 0) return ret;
    if (!t) break;
    ret += t;
    to_write -= (unsigned)t;
    offs = (offs + (unsigned)t) % bufsize_frames;
  }
  return ret;
}

static unsigned data_not_in_alsa(void) {
  unsigned d = lcl_offs <= wri_offs ? wri_offs - lcl_offs : bufsize_frames - (lcl_offs - wri_offs);
  if (d) return d;
  return held_frames - data_in_alsa;
}

/* winealsa alsa_write_data (lk held) */
static void write_data(double t_rel) {
  snd_pcm_sframes_t a = snd_pcm_avail_update(pcm);
  snd_pcm_uframes_t avail = a < 0 ? 0 : (snd_pcm_uframes_t)a;
  {
    double tn = now_s();
    if (q_est_t > 0) q_est -= (tn - q_est_t) * RATE;
    q_est_t = tn;
    double queued = avail < alsa_bufsize ? (double)(alsa_bufsize - avail) : 0;
    if (q_est < queued) q_est = queued;
    snd_pcm_sframes_t d = 0;
    if (qsrc_delay) q_est = snd_pcm_delay(pcm, &d) == 0 && d > 0 ? (double)d : 0;
  }
  if (snd_pcm_state(pcm) == SND_PCM_STATE_XRUN) {
    n_xrun_state++;
    if (n_xrun_state <= 64)
      printf("[AT] xrun-state t=%.3f held=%u in_alsa=%u\n", t_rel, held_frames, data_in_alsa);
    avail = alsa_bufsize;
    q_est = 0;
    snd_pcm_recover(pcm, -EPIPE, 1);
    snd_pcm_reset(pcm);
    snd_pcm_prepare(pcm);
  }
  double top_q = top_half * (double)alsa_period / 2;
  if (data_in_alsa == 0 && held_frames < alsa_period &&
      ((gate_periods > 0 && q_est >= (double)gate_periods * alsa_period / 2) || (top_half > 0 && q_est >= top_q)))
    n_gated++;
  else if (data_in_alsa == 0 && held_frames < alsa_period) {
    unsigned n = top_half > 0 ? (unsigned)(top_q - q_est) + 1 : (unsigned)alsa_period - held_frames;
    if (n > alsa_period) n = (unsigned)alsa_period;
    write_best_effort(use_float ? (const void *)sil_f : (const void *)sil_s, n);
    n_leadin++;
    frames_silence += n;
  }
  if (held_frames < period_frames) n_starved_fill++;
  unsigned max_copy = data_not_in_alsa();
  unsigned played;
  if (acct_fix) {
    unsigned queued = avail < alsa_bufsize ? (unsigned)(alsa_bufsize - avail) : 0;
    played = data_in_alsa > queued ? data_in_alsa - queued : 0;
  } else {
    played = data_in_alsa < avail ? data_in_alsa : (unsigned)avail;
  }
  data_in_alsa -= played;
  if (held_frames > played) held_frames -= played;
  else held_frames = 0;
  while (avail && max_copy) {
    unsigned tw = avail < max_copy ? (unsigned)avail : max_copy;
    snd_pcm_sframes_t w = write_wrap(lcl_offs, tw);
    if (w <= 0) break;
    avail -= (snd_pcm_uframes_t)w;
    lcl_offs = (lcl_offs + (unsigned)w) % bufsize_frames;
    data_in_alsa += (unsigned)w;
    max_copy -= (unsigned)w;
    frames_written += w;
  }
  ev_set = 1;
  pthread_cond_signal(&ev);
}

/* the "game": on each event, fill the local buffer (GetBuffer/ReleaseBuffer) */
static void *game_thread(void *arg) {
  (void)arg;
  pthread_mutex_lock(&lk);
  while (!quit) {
    while (!ev_set && !quit) pthread_cond_wait(&ev, &lk);
    ev_set = 0;
    unsigned room = bufsize_frames - held_frames;
    if (skip_n > 0) {
      rng = rng * 1103515245u + 12345u;
      if ((rng >> 16) % (unsigned)skip_n == 0) room = 0;
    }
    if (fill_periods > 0 && room > (unsigned)fill_periods * period_frames) room = (unsigned)fill_periods * period_frames;
    for (unsigned i = 0; i < room; i++) {
      float v = (float)(0.5 * sin(ph));
      unsigned o = (wri_offs + i) % bufsize_frames;
      if (use_float) local_f[o * CH] = local_f[o * CH + 1] = v;
      else local_s[o * CH] = local_s[o * CH + 1] = (short)(v * 32767);
      ph += 2 * M_PI * hz / RATE;
      if (ph > 2 * M_PI) ph -= 2 * M_PI;
    }
    wri_offs = (wri_offs + room) % bufsize_frames;
    held_frames += room;
  }
  pthread_mutex_unlock(&lk);
  return 0;
}

static void sleep_100ns(long long hns) {
  if (hns <= 0) return;
  struct timeval tv = {(time_t)(hns / 10000000), (suseconds_t)((hns % 10000000) / 10)};
  select(0, 0, 0, 0, &tv);
}

int main(int argc, char **argv) {
  double secs = argc > 1 ? atof(argv[1]) : 60;
  int lbuf_ms = 40, stall_ms = 0;
  double stall_every = 10;
  for (int i = 2; i < argc; i++) {
    if (!strncmp(argv[i], "hz=", 3)) hz = atof(argv[i] + 3);
    else if (!strncmp(argv[i], "lbuf_ms=", 8)) lbuf_ms = atoi(argv[i] + 8);
    else if (!strncmp(argv[i], "stall_ms=", 9)) stall_ms = atoi(argv[i] + 9);
    else if (!strncmp(argv[i], "stall_every=", 12)) stall_every = atof(argv[i] + 12);
    else if (!strncmp(argv[i], "fmt=", 4)) use_float = argv[i][4] == 'f';
    else if (!strncmp(argv[i], "fill=", 5)) fill_periods = atoi(argv[i] + 5);
    else if (!strncmp(argv[i], "skip=", 5)) skip_n = atoi(argv[i] + 5);
    else if (!strncmp(argv[i], "acct=", 5)) acct_fix = argv[i][5] == 'f';
    else if (!strncmp(argv[i], "gate=", 5)) gate_periods = atoi(argv[i] + 5) * 2;
    else if (!strncmp(argv[i], "gateh=", 6)) gate_periods = atoi(argv[i] + 6);
    else if (!strncmp(argv[i], "top=", 4)) top_half = atoi(argv[i] + 4);
    else if (!strncmp(argv[i], "qsrc=", 5)) qsrc_delay = argv[i][5] == 'd';
  }
  setvbuf(stdout, 0, _IOLBF, 0);
  printf("[AT] atone alsa-lib %s secs=%.0f hz=%.0f lbuf_ms=%d stall_ms=%d stall_every=%.1f fmt=%s fill=%d skip=%d acct=%s gate=%d top=%d\n",
         snd_asoundlib_version(), secs, hz, lbuf_ms, stall_ms, stall_every, use_float ? "float" : "s16", fill_periods, skip_n,
         acct_fix ? "fix" : "orig", gate_periods, top_half);
  snd_lib_error_set_handler(lib_err);
  snd_config_t *top = 0;
  snd_input_t *in = 0;
  int err = snd_config_top(&top);
  if (!err) err = snd_input_buffer_open(&in, CONF, (ssize_t)strlen(CONF));
  if (!err) err = snd_config_load(top, in);
  if (in) snd_input_close(in);
  if (!err) err = snd_pcm_open_lconf(&pcm, "cpdefault", SND_PCM_STREAM_PLAYBACK, 0, top);
  if (err < 0) {
    /* no device (a plain wave boot): nothing to measure */
    printf("[AT] open: %s\n[AT] done rc=%d\n", snd_strerror(err), err == -ENOENT ? 0 : 1);
    return err == -ENOENT ? 0 : 1;
  }
  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(pcm, hw);
  snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(pcm, hw, use_float ? SND_PCM_FORMAT_FLOAT_LE : SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_rate(pcm, hw, RATE, 0);
  snd_pcm_hw_params_set_channels(pcm, hw, CH);
  unsigned period_us = 10000; /* mmdevapi's default 10 ms period (REFERENCE_TIME 100000) */
  err = snd_pcm_hw_params_set_period_time_near(pcm, hw, &period_us, 0);
  period_frames = RATE / 100;
  if (err < 0 || period_us < 10000) {
    snd_pcm_uframes_t b = period_frames * 4;
    err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &b);
  } else {
    unsigned periods = 4;
    err = snd_pcm_hw_params_set_periods_near(pcm, hw, &periods, 0);
  }
  if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
    printf("[AT] hw_params: %s\n[AT] done rc=1\n", snd_strerror(err));
    return 1;
  }
  snd_pcm_hw_params_get_period_size(hw, &alsa_period, 0);
  snd_pcm_hw_params_get_buffer_size(hw, &alsa_bufsize);
  snd_pcm_sw_params_t *sw;
  snd_pcm_sw_params_alloca(&sw);
  snd_pcm_sw_params_current(pcm, sw);
  snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
  snd_pcm_sw_params_set_stop_threshold(pcm, sw, alsa_bufsize);
  if ((err = snd_pcm_sw_params(pcm, sw)) < 0 || (err = snd_pcm_prepare(pcm)) < 0) {
    printf("[AT] sw_params/prepare: %s\n[AT] done rc=1\n", snd_strerror(err));
    return 1;
  }
  bufsize_frames = (unsigned)(RATE * lbuf_ms / 1000);
  local_f = calloc(bufsize_frames * CH, sizeof(float));
  local_s = calloc(bufsize_frames * CH, sizeof(short));
  sil_f = calloc(alsa_period * CH, sizeof(float));
  sil_s = calloc(alsa_period * CH, sizeof(short));
  printf("[AT] alsa period=%lu buffer=%lu local=%u frames\n", (unsigned long)alsa_period,
         (unsigned long)alsa_bufsize, bufsize_frames);
  pthread_t gt;
  pthread_create(&gt, 0, game_thread, 0);

  /* winealsa alsa_timer_loop, in 100 ns units like REFERENCE_TIME */
  const long long per = 100000;
  double t0 = now_s(), last = t0, next_stat = 10, next_stall = stall_every;
  long long next = (long long)(t0 * 1e7) + per, delay = per;
  long stalls = 0, last_xs = 0, last_ep = 0, last_li = 0;
  long last_wr = 0;
  pthread_mutex_lock(&lk);
  for (;;) {
    double t = now_s();
    double gap = t - last;
    last = t;
    if (n_ticks > 0) {
      if (gap > max_gap) max_gap = gap;
      if (gap > 0.015) n_late15++;
      if (gap > 0.025) n_late25++;
      if (gap > 0.040) n_late40++;
    }
    n_ticks++;
    write_data(t - t0);
    if (t - t0 >= secs) break;
    pthread_mutex_unlock(&lk);
    if (stall_ms > 0 && t - t0 >= next_stall) {
      struct timespec st = {stall_ms / 1000, (long)(stall_ms % 1000) * 1000000L};
      nanosleep(&st, 0);
      stalls++;
      next_stall += stall_every;
    }
    sleep_100ns(delay);
    pthread_mutex_lock(&lk);
    long long nw = (long long)(now_s() * 1e7);
    long long adj = next - nw;
    if (adj > per / 2) adj = per / 2;
    else if (adj < -per / 2) adj = -per / 2;
    delay = per + adj;
    next += per;
    if (t - t0 >= next_stat) {
      printf("[AT] stat t=%.1f ticks=%ld late15=%ld late25=%ld late40=%ld maxgap_ms=%.1f xrun_state=+%ld epipe=+%ld "
             "leadin=+%ld starved_fill=%ld written_s=+%.2f stalls=%ld\n",
             t - t0, n_ticks, n_late15, n_late25, n_late40, max_gap * 1e3, n_xrun_state - last_xs,
             n_epipe - last_ep, n_leadin - last_li, n_starved_fill, (frames_written - last_wr) / (double)RATE,
             stalls);
      last_xs = n_xrun_state;
      last_ep = n_epipe;
      last_li = n_leadin;
      last_wr = frames_written;
      max_gap = 0;
      next_stat += 10;
    }
  }
  quit = 1;
  pthread_cond_signal(&ev);
  pthread_mutex_unlock(&lk);
  pthread_join(gt, 0);
  snd_pcm_drain(pcm);
  snd_pcm_close(pcm);
  double el = now_s() - t0;
  printf("[AT] total elapsed_s=%.2f ticks=%ld late15=%ld late25=%ld late40=%ld xrun_state=%ld epipe=%ld leadin=%ld "
         "silence_s=%.3f written_s=%.2f stalls=%ld gated=%ld\n",
         el, n_ticks, n_late15, n_late25, n_late40, n_xrun_state, n_epipe, n_leadin,
         frames_silence / (double)RATE, frames_written / (double)RATE, stalls, n_gated);
  printf("[AT] done rc=0\n");
  return 0;
}
