/* src/host/lsnd_host_dev.c -- a host model of the virtio-sound device plus
 * the kernel services linux_snd.c needs, for tools/cp/audio/host-alsa-test.sh
 * (docs/cp-audio.md §5).
 *
 * The device model is QEMU's behaviour as measured in the guest: one output
 * stream (S16, 48000 Hz, 1..2 ch), a TX message's bytes are copied when it is
 * posted, and the message completes when its frames have "played" at 48 kHz
 * after PCM_START (real time, CLOCK_MONOTONIC).  Completed bytes are appended
 * to the WAV at $LSND_HOST_WAV (16-bit stereo 48 kHz), like QEMU's `wav`
 * audiodev, so tools/cp/audio/wavcheck.py grades a host run exactly as it
 * grades a guest run.  Nothing here opens a real sound device.
 *
 * LSND_HOST_QEMU=1 (H5-audio2, tools/cp/audio2/pace-host-test.sh) swaps the
 * ideal device for QEMU 10.2's audio path as docs/cp-audio2.md §2 describes
 * it: a 10 ms audio timer that fires once when due (late after a stall, never
 * twice), a 1024-frame mixing buffer that the virtio-snd callback fills from
 * the TX queue (a message completes when its last frame is mixed), and the
 * wav backend's rate control -- frames owed = elapsed time x rate - frames
 * written, reset when outside [0, 65536] -- which drains the mixing buffer by
 * what is owed.  Time the device starved is owed and paid back faster than
 * real time: the behaviour the guest driver's pacing exists for. */
#include "lsnd_host.h"
#include "../include/virtio_snd.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

spinlock_t proc_lock;
static struct process host_proc = {1, 0};
struct process *current_process(void) { return &host_proc; }
int process_try_wake_locked(int pid) { (void)pid; return 1; }
void sched_wake_idle_cpu(void) {}
void poll_notify_obj(const void *obj) { (void)obj; }
uint64_t timer_get_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
int vm_user_read(struct process *p, uint64_t va, void *dst, int len) {
  (void)p;
  if (!va) return -1;
  memcpy(dst, (const void *)(uintptr_t)va, (size_t)len);
  return 0;
}
int vm_kwrite(struct process *p, uint64_t va, const void *src, int len) {
  (void)p;
  if (!va) return -1;
  memcpy((void *)(uintptr_t)va, src, (size_t)len);
  return 0;
}
void uart_puts(const char *s) { fprintf(stderr, "%s", s); }

int virtio_snd_irq_vector = -1;
#define NSLOT 21
static struct {
  int busy;
  uint32_t len;
  uint64_t due;
  uint8_t *data;
} slot[NSLOT];
static int fifo[NSLOT], fhead, fcount, inflight;
static int started;
static uint32_t rate = 48000, channels = 2;
static uint64_t last_due;
static FILE *wav;
static uint64_t wav_bytes;
static struct vsnd_stats st;

static void wav_put32(uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  fwrite(b, 1, 4, wav);
}
static void wav_fini(void) {
  if (!wav) return;
  fseek(wav, 4, SEEK_SET);
  wav_put32((uint32_t)(36 + wav_bytes));
  fseek(wav, 40, SEEK_SET);
  wav_put32((uint32_t)wav_bytes);
  fclose(wav);
  wav = 0;
}
static void wav_open(void) {
  const char *p = getenv("LSND_HOST_WAV");
  if (!p || wav) return;
  wav = fopen(p, "wb");
  if (!wav) return;
  fwrite("RIFF", 1, 4, wav);
  wav_put32(36);
  fwrite("WAVEfmt ", 1, 8, wav);
  wav_put32(16);
  uint8_t f[16] = {1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0, 4, 0, 16, 0}; /* PCM 2ch 48000 192000 4 16 */
  fwrite(f, 1, 16, wav);
  fwrite("data", 1, 4, wav);
  wav_put32(0);
  atexit(wav_fini);
}

/* --- LSND_HOST_QEMU: QEMU's audio timer + mixing buffer + rate control ----- */
#define QMIX 1024
static int qemu_mode = -1;
static uint64_t q_next_tick, q_rate_start, q_sent;
static uint32_t q_live;            /* frames in the mixing buffer */
static uint32_t q_off[NSLOT];      /* bytes of a queued message already mixed */
static uint64_t q_done;            /* messages returned to the guest */
static int is_qemu(void) {
  if (qemu_mode < 0) qemu_mode = getenv("LSND_HOST_QEMU") && getenv("LSND_HOST_QEMU")[0] == '1';
  return qemu_mode;
}
static void q_tick(uint64_t now) {
  uint32_t fb = channels * 2;
  /* virtio_snd_pcm_out_cb: copy queued messages into the free mixing space */
  for (int i = 0; i < fcount && q_live < QMIX; i++) {
    int k = fifo[(fhead + i) % NSLOT];
    if (slot[k].due == 0) continue; /* already complete */
    uint32_t left = (slot[k].len - q_off[k]) / fb;
    uint32_t take = QMIX - q_live < left ? QMIX - q_live : left;
    if (wav && channels == 2) fwrite(slot[k].data + q_off[k], 1, take * fb, wav), wav_bytes += take * fb;
    q_off[k] += take * fb;
    q_live += take;
    if (q_off[k] >= slot[k].len) slot[k].due = 0, q_done++; /* returned to the guest */
  }
  /* wav_write_out: audio_rate_get_bytes */
  int64_t owed = (int64_t)((now - q_rate_start) * rate / 1000000u) - (int64_t)q_sent;
  if (owed < 0 || owed > 65536) {
    q_rate_start = now;
    q_sent = 0;
    owed = 0;
    st.ctl_timeouts++; /* "Resetting rate control" count, reported in the close row */
  }
  uint32_t w = owed < (int64_t)q_live ? (uint32_t)owed : q_live;
  q_live -= w;
  q_sent += w;
}

/* QEMU's audio timer runs on its own (main-loop) thread: a late timer fires
   once and re-arms from now (audio_reset_timer); SIGSTOP of the process
   freezes it with the "guest", like a host stall of the whole QEMU. */
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lsnd_host_big = PTHREAD_MUTEX_INITIALIZER; /* lsnd_preload.c */
extern void linux_snd_irq(void);
static void *q_timer(void *a) {
  (void)a;
  for (;;) {
    uint64_t now = timer_get_us();
    if (now < q_next_tick) {
      struct timespec ts = {(time_t)((q_next_tick - now) / 1000000u), (long)((q_next_tick - now) % 1000000u) * 1000L};
      nanosleep(&ts, 0);
      continue;
    }
    pthread_mutex_lock(&q_mu);
    uint64_t done0 = q_done;
    if (started) q_tick(now);
    int irq = q_done != done0;
    pthread_mutex_unlock(&q_mu);
    if (irq) { /* virtio_notify: the guest's completion interrupt */
      pthread_mutex_lock(&lsnd_host_big);
      linux_snd_irq();
      pthread_mutex_unlock(&lsnd_host_big);
    }
    q_next_tick = timer_get_us() + 10000;
  }
  return 0;
}

int virtio_snd_init(void) {
  wav_open();
  if (is_qemu()) {
    pthread_t t;
    q_next_tick = timer_get_us() + 10000;
    pthread_create(&t, 0, q_timer, 0);
  }
  return 0;
}
int virtio_snd_present(void) { return 1; }
int virtio_snd_caps(struct vsnd_stream_caps *o) {
  memset(o, 0, sizeof *o);
  o->formats = 1ull << VSND_PCM_FMT_S16;
  o->rates = 1ull << VSND_PCM_RATE_48000;
  o->ch_min = 1;
  o->ch_max = 2;
  o->n_streams = 1;
  return 0;
}
int virtio_snd_set_params(uint32_t b, uint32_t p, uint8_t c, uint8_t f, uint8_t r, uint32_t *s) {
  (void)b; (void)p; (void)f; (void)r;
  channels = c;
  st.ctl_cmds++;
  if (s) *s = VSND_S_OK;
  return 0;
}
static int pcm_cmd_locked(uint32_t code, uint32_t *s);
int virtio_snd_pcm_cmd(uint32_t code, uint32_t *s) {
  pthread_mutex_lock(&q_mu);
  int r = pcm_cmd_locked(code, s);
  pthread_mutex_unlock(&q_mu);
  return r;
}
static int pcm_cmd_locked(uint32_t code, uint32_t *s) {
  st.ctl_cmds++;
  if (code == VSND_R_PCM_START && is_qemu()) {
    if (!started) { /* wav_enable_out: audio_rate_start */
      q_rate_start = timer_get_us();
      q_sent = 0;
    }
    started = 1;
  } else if (code == VSND_R_PCM_START) {
    started = 1;
    uint64_t now = timer_get_us();
    for (int i = 0; i < fcount; i++) { /* queued before START: play from now */
      int k = fifo[(fhead + i) % NSLOT];
      uint64_t d = (uint64_t)slot[k].len * 1000000u / (rate * channels * 2);
      if (last_due < now) last_due = now;
      last_due += d;
      slot[k].due = last_due;
    }
  } else if (code == VSND_R_PCM_STOP) {
    started = 0;
  } else if (code == VSND_R_PCM_RELEASE) {
    for (int i = 0; i < fcount; i++) slot[fifo[(fhead + i) % NSLOT]].due = 0; /* flushed */
  }
  if (s) *s = VSND_S_OK;
  return 0;
}
int virtio_snd_tx_slots(void) { return NSLOT; }
int virtio_snd_tx_inflight(void) { return inflight; }
static int tx_post_locked(uint64_t pa, uint32_t len);
int virtio_snd_tx_post(uint64_t pa, uint32_t len) {
  pthread_mutex_lock(&q_mu);
  int r = tx_post_locked(pa, len);
  pthread_mutex_unlock(&q_mu);
  return r;
}
static int tx_post_locked(uint64_t pa, uint32_t len) {
  int k;
  for (k = 0; k < NSLOT; k++)
    if (!slot[k].busy) break;
  if (k == NSLOT || fcount == NSLOT) return -1;
  slot[k].busy = 1;
  slot[k].len = len;
  slot[k].data = realloc(slot[k].data, len);
  memcpy(slot[k].data, (const void *)(uintptr_t)pa, len); /* QEMU copies at pop */
  uint64_t now = timer_get_us();
  if (is_qemu()) {
    slot[k].due = ~0ull; /* completes when q_tick has mixed it */
    q_off[k] = 0;
  } else if (started) {
    if (last_due < now) last_due = now;
    last_due += (uint64_t)len * 1000000u / (rate * channels * 2);
    slot[k].due = last_due;
  } else {
    slot[k].due = ~0ull;
  }
  fifo[(fhead + fcount) % NSLOT] = k;
  fcount++;
  inflight++;
  st.tx_posted++;
  return k;
}
int virtio_snd_tx_reap(void (*cb)(int, uint32_t, void *), void *ctx) {
  int n = 0;
  uint64_t now = timer_get_us();
  pthread_mutex_lock(&q_mu);
  while (fcount) {
    int k = fifo[fhead];
    if (slot[k].due > now) break;
    fhead = (fhead + 1) % NSLOT;
    fcount--;
    if (wav && slot[k].due && !is_qemu()) { /* a flushed message was not played */
      if (channels == 2) fwrite(slot[k].data, 1, slot[k].len, wav), wav_bytes += slot[k].len;
    }
    slot[k].busy = 0;
    inflight--;
    st.tx_done++;
    n++;
    if (cb) cb(k, VSND_S_OK, ctx);
  }
  pthread_mutex_unlock(&q_mu);
  return n;
}
void virtio_snd_handle_irq(void) {}
void virtio_snd_stats(struct vsnd_stats *o) { *o = st; }
