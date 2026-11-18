/* linux_snd.c — the ALSA kernel ABI subset over virtio-sound (lane C4-audio,
 * docs/cp-audio.md, AD-9).
 *
 * One card (0), one playback PCM device (0, one subdevice), one control node
 * with a master volume + switch.  Enough for an unmodified alsa-lib `hw:0,0`
 * (and `plughw:0,0` / `default` through its plug plugin) and for Wine's
 * winealsa.drv.  Layouts and request numbers are Linux's x86_64 uapi
 * <sound/asound.h>.
 *
 * Data path: WRITEI_FRAMES copies the frames into a kernel ring buffer of
 * buffer_size frames (the "DMA buffer"); the stream submits it to the
 * virtio TX queue in chunks that never cross a period boundary, so a chunk
 * is contiguous in the ring.  A full chunk is submitted as soon as the
 * application has written it; a partial one only when nothing is in flight
 * (the device would starve otherwise) or while draining.  hw_ptr advances
 * by a chunk's frames when the device returns it (QEMU returns a TX buffer
 * once its audio backend consumed the bytes), so hw_ptr granularity is a
 * chunk (SNDRV_PCM_INFO_BATCH).  An underrun is Linux's rule: playback
 * avail >= stop_threshold while RUNNING -> XRUN.
 *
 * The virtio stream is configured at HW_PARAMS (SET_PARAMS + PREPARE),
 * started at the first START and only stopped/released at HW_FREE/close:
 * the ALSA states in between (drop, xrun, prepare, pause) only change what
 * the driver submits.  Chunks a DROP/PREPARE/RESET abandoned are still in
 * the device; an epoch number on every chunk makes their late completions
 * not count.
 *
 * status/control mmap: refused (the compat mmap of a char device answers
 * -ENOTSUP), so alsa-lib takes its SYNC_PTR fallback (measured, see
 * docs/cp-audio.md §4).
 *
 * Locks: lsnd_op (ylock) serialises every process-context operation; it is
 * the only lock held across a control-queue command.  lsnd_lock (irqsave
 * spinlock) guards the stream state shared with the TX completion IRQ. */
#include "linux_snd.h"
#include "linux_snd_params.h"
#include "virtio_snd.h"
#ifdef LSND_HOST_TEST
/* tools/cp/audio/host-alsa-test.sh: this TU on the host under an unmodified
 * alsa-lib (src/host/lsnd_host.h supplies the kernel services it uses). */
#include "lsnd_host.h"
#else
#include "errno.h"
#include "lock.h"
#include "kmem.h"
#include "process.h"
#include "timer.h"
#include "vm.h"
#endif
#include "pidmask.h"
#include "kline.h"

#ifndef EBADFD
#define EBADFD 77
#endif
#ifndef ESTRPIPE
#define ESTRPIPE 86
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif
#ifndef EPIPE
#define EPIPE 32
#endif
#ifndef ENXIO
#define ENXIO 6
#endif

/* --- request numbers (x86_64 _IOC encodings) ---------------------------- */
#define CTL_PVERSION 0x80045500u
#define CTL_CARD_INFO 0x81785501u
#define CTL_ELEM_LIST 0xC0505510u
#define CTL_ELEM_INFO 0xC1105511u
#define CTL_ELEM_READ 0xC4C85512u
#define CTL_ELEM_WRITE 0xC4C85513u
#define CTL_ELEM_LOCK 0x40405514u
#define CTL_ELEM_UNLOCK 0x40405515u
#define CTL_SUBSCRIBE_EVENTS 0xC0045516u
#define CTL_TLV_READ 0xC008551Au
#define CTL_HWDEP_NEXT_DEVICE 0xC0045520u
#define CTL_PCM_NEXT_DEVICE 0x80045530u
#define CTL_PCM_INFO 0xC1205531u
#define CTL_PCM_PREFER_SUBDEVICE 0x40045532u
#define CTL_RAWMIDI_NEXT_DEVICE 0xC0045540u
#define CTL_POWER_STATE 0x800455D1u

#define PCM_PVERSION 0x80044100u
#define PCM_INFO 0x81204101u
#define PCM_TSTAMP 0x40044102u
#define PCM_TTSTAMP 0x40044103u
#define PCM_USER_PVERSION 0x40044104u
#define PCM_HW_REFINE 0xC2604110u
#define PCM_HW_PARAMS 0xC2604111u
#define PCM_HW_FREE 0x00004112u
#define PCM_SW_PARAMS 0xC0884113u
#define PCM_STATUS 0x80984120u
#define PCM_DELAY 0x80084121u
#define PCM_HWSYNC 0x00004122u
#define PCM_SYNC_PTR 0xC0884123u
#define PCM_STATUS_EXT 0xC0984124u
#define PCM_CHANNEL_INFO 0x80204132u
#define PCM_PREPARE 0x00004140u
#define PCM_RESET 0x00004141u
#define PCM_START 0x00004142u
#define PCM_DROP 0x00004143u
#define PCM_DRAIN 0x00004144u
#define PCM_PAUSE 0x40044145u
#define PCM_REWIND 0x40084146u
#define PCM_RESUME 0x00004147u
#define PCM_XRUN 0x00004148u
#define PCM_FORWARD 0x40084149u
#define PCM_WRITEI 0x40184150u
#define PCM_READI 0x80184151u
#define PCM_WRITEN 0x40184152u
#define PCM_LINK 0x40044160u
#define PCM_UNLINK 0x00004161u

#define SND_PCM_VERSION 0x0002000Eu /* 2.0.14 (Linux 5.4) */
#define SND_CTL_VERSION 0x00020007u

enum { ST_OPEN, ST_SETUP, ST_PREPARED, ST_RUNNING, ST_XRUN, ST_DRAINING, ST_PAUSED };

#define LSND_RING_BYTES (512u * 1024u)
static uint8_t lsnd_ring[LSND_RING_BYTES] __attribute__((aligned(4096)));

#define SLOTS 64
static struct {
  int state;
  int open;
  /* hw params */
  uint32_t channels, rate, fmt, frame_bytes;
  uint64_t period, periods, buffer, boundary;
  /* sw params */
  uint64_t avail_min, start_thr, stop_thr;
  int tstamp_mode, tstamp_type;
  /* pointers (frames, monotonic; reported modulo boundary) */
  uint64_t hw, appl, submit;
  uint32_t epoch;
  int inflight; /* chunks of this epoch in the device */
  uint32_t slot_epoch[SLOTS];
  uint32_t slot_frames[SLOTS];
  uint64_t trigger_us, avail_max;
  int dev_prepared, dev_started;
  uint32_t user_pversion;
  /* stats */
  uint64_t xruns, chunks, frames_done;
  uint64_t n_sync_ptr, n_writei, n_status, n_refine, n_parks;
} S;

static spinlock_t lsnd_lock;
static ylock_t lsnd_op;
static int lsnd_inited;
static int ctl_open_count;
static int32_t mix_vol[2] = {100, 100};
static int32_t mix_sw = 1;
static uint32_t lsnd_seq; /* bumped by every completion batch (park race) */
static struct pid_mask lsnd_waiters;
static uint8_t lsnd_parked[MAX_PROCESSES];

extern void uart_puts(const char *s);

/* --- small helpers ------------------------------------------------------- */

static void z(void *d, uint32_t n) {
  uint8_t *b = (uint8_t *)d;
  for (uint32_t i = 0; i < n; i++) b[i] = 0;
}
static void cpy(void *d, const void *s, uint32_t n) {
  uint8_t *a = (uint8_t *)d;
  const uint8_t *b = (const uint8_t *)s;
  for (uint32_t i = 0; i < n; i++) a[i] = b[i];
}
static void put_str(uint8_t *dst, uint32_t cap, const char *s) {
  uint32_t i = 0;
  for (; s[i] && i + 1 < cap; i++) dst[i] = (uint8_t)s[i];
  for (; i < cap; i++) dst[i] = 0;
}
static int str_eq(const uint8_t *a, const char *b, uint32_t cap) {
  uint32_t i = 0;
  for (; i < cap && b[i]; i++)
    if (a[i] != (uint8_t)b[i]) return 0;
  return i < cap ? a[i] == 0 : 1;
}
static void put_ts(uint8_t *dst, uint64_t us) {
  int64_t sec = (int64_t)(us / 1000000u), nsec = (int64_t)((us % 1000000u) * 1000u);
  cpy(dst, &sec, 8);
  cpy(dst + 8, &nsec, 8);
}
static int cin(struct process *p, uint64_t va, void *k, int n) {
  return vm_user_read(p, va, k, n) == 0 ? 0 : -EFAULT;
}
static int cout(struct process *p, uint64_t va, const void *k, int n) {
  return vm_kwrite(p, va, k, n) == 0 ? 0 : -EFAULT;
}

static void lsnd_init_once(void) {
  if (lsnd_inited) return;
  spinlock_init(&lsnd_lock);
  ylock_init(&lsnd_op);
  lsnd_inited = 1;
}

/* Boot hook (main.c): bring up the device and the locks. */
void linux_snd_init(void) {
  lsnd_init_once();
  if (virtio_snd_init() == 0) uart_puts("[SND] ALSA nodes: /dev/snd/controlC0 /dev/snd/pcmC0D0p\n");
}

int linux_snd_path_minor(const char *p) {
  static const char pre[] = "/dev/snd/";
  for (int i = 0; pre[i]; i++)
    if (p[i] != pre[i]) return -1;
  const char *r = p + 9;
  static const char c0[] = "controlC0", p0[] = "pcmC0D0p";
  int i;
  for (i = 0; c0[i] && r[i] == c0[i]; i++) {
  }
  if (!c0[i] && !r[i]) return LSND_MINOR_CONTROL;
  for (i = 0; p0[i] && r[i] == p0[i]; i++) {
  }
  if (!p0[i] && !r[i]) return LSND_MINOR_PCM_PLAY;
  return -2; /* under /dev/snd but not a node we have */
}

/* --- device caps ------------------------------------------------------------ */

static int lsnd_rate_code(uint32_t rate) {
  return rate == 48000 ? VSND_PCM_RATE_48000 : rate == 44100 ? VSND_PCM_RATE_44100 : -1;
}

static void lsnd_caps(struct lsnd_hw_caps *c) {
  struct vsnd_stream_caps vc;
  z(c, sizeof *c);
  if (virtio_snd_caps(&vc) != 0) return;
  c->access_mask = 1u << LSND_ACCESS_RW_INTERLEAVED;
  c->format_mask = (vc.formats & (1ull << VSND_PCM_FMT_S16)) ? (1ull << LSND_FORMAT_S16_LE) : 0;
  uint32_t ch = 2;
  if (vc.ch_min > 2 || vc.ch_max < 2) ch = vc.ch_max;
  c->channels_min = c->channels_max = ch;
  uint32_t rate = (vc.rates & (1ull << VSND_PCM_RATE_48000)) ? 48000 : 44100;
  c->rate_min = c->rate_max = rate;
  c->period_bytes_min = 1024;
  c->period_bytes_max = 65536;
  c->periods_min = 2;
  c->periods_max = 64;
  c->buffer_bytes_max = LSND_RING_BYTES;
  /* INTERLEAVED | BLOCK_TRANSFER | BATCH | PAUSE; no MMAP: RW access only */
  c->info = 0x100u | 0x10000u | 0x40000u | 0x80000u;
}

/* --- waits ------------------------------------------------------------------ */

static void lsnd_wake(void) {
  __atomic_fetch_add(&lsnd_seq, 1, __ATOMIC_SEQ_CST);
  int woke = 0;
  if (!pid_mask_empty(&lsnd_waiters)) {
    uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
    for (int wi = 0; wi < PID_MASK_WORDS; wi++) {
      uint64_t x = lsnd_waiters.w[wi];
      lsnd_waiters.w[wi] = 0;
      while (x) {
        int pid = (wi << 6) + __builtin_ctzll(x);
        x &= x - 1;
        if (pid >= MAX_PROCESSES || !lsnd_parked[pid]) continue;
        lsnd_parked[pid] = 0;
        if (process_try_wake_locked(pid) == 1) woke = 1;
      }
    }
    spinlock_release_irqrestore(&proc_lock, pf);
  }
  if (woke) sched_wake_idle_cpu();
  poll_notify_obj(&S);
}

static void lsnd_unpark(struct process *cur) {
  if (!cur || cur->pid < 0 || cur->pid >= MAX_PROCESSES) return;
  if (lsnd_parked[cur->pid]) {
    uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
    lsnd_parked[cur->pid] = 0;
    spinlock_release_irqrestore(&proc_lock, pf);
  }
}

/* Park the caller until the next completion batch or `wait_us`.  `seen` is
 * lsnd_seq read under lsnd_lock when the caller decided to wait. */
static int lsnd_park(uint32_t seen, uint64_t wait_us) {
  struct process *cur = current_process();
  if (!cur || cur->pid <= 0 || cur->pid >= MAX_PROCESSES) return -EAGAIN;
  S.n_parks++;
  uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
  if (__atomic_load_n(&lsnd_seq, __ATOMIC_SEQ_CST) == seen && process_park_ok_locked(cur)) {
    pid_mask_set(&lsnd_waiters, cur->pid);
    lsnd_parked[cur->pid] = 1;
    proc_set_deadline_us(cur, timer_get_us() + wait_us);
    cur->state = PROC_STATE_BLOCKED;
  }
  spinlock_release_irqrestore(&proc_lock, pf);
  return LSND_PARKED; /* not blocked when an event raced: the ioctl reruns */
}

static uint64_t lsnd_period_us(void) {
  uint64_t r = S.rate ? S.rate : 48000;
  return S.period * 1000000u / r;
}

/* --- stream engine (caller holds lsnd_lock) ---------------------------------- */

static uint64_t avail_locked(void) {
  uint64_t used = S.appl - S.hw;
  return used >= S.buffer ? 0 : S.buffer - used;
}

static void submit_locked(void) {
  if (S.state != ST_RUNNING && S.state != ST_DRAINING) return;
  uint64_t ring_pa = kimg_pa(lsnd_ring);
  while (S.submit < S.appl) {
    uint64_t pend = S.appl - S.submit;
    uint64_t to_b = S.period - (S.submit % S.period);
    uint64_t n = pend < to_b ? pend : to_b;
    if (n < to_b && S.state != ST_DRAINING && S.inflight > 0) break; /* wait for a full chunk */
    uint64_t off = (S.submit % S.buffer) * S.frame_bytes;
    int slot = virtio_snd_tx_post(ring_pa + off, (uint32_t)(n * S.frame_bytes));
    if (slot < 0 || slot >= SLOTS) break;
    S.slot_epoch[slot] = S.epoch;
    S.slot_frames[slot] = (uint32_t)n;
    S.submit += n;
    S.inflight++;
    S.chunks++;
  }
}

static void check_xrun_locked(void) {
  if (S.state == ST_RUNNING && S.stop_thr < S.boundary && avail_locked() >= S.stop_thr) {
    S.state = ST_XRUN;
    S.xruns++;
    S.trigger_us = timer_get_us();
  }
}

static void reap_cb(int slot, uint32_t status, void *ctx) {
  (void)status;
  (void)ctx;
  if (slot < 0 || slot >= SLOTS) return;
  if (S.slot_epoch[slot] != S.epoch) return; /* abandoned by drop/prepare/reset */
  S.hw += S.slot_frames[slot];
  S.frames_done += S.slot_frames[slot];
  if (S.inflight > 0) S.inflight--;
}

/* Reap completions and keep the device fed.  Returns 1 when anything moved. */
static int service_locked(void) {
  int n = virtio_snd_tx_reap(reap_cb, 0);
  if (n > 0) {
    if (S.hw > S.appl) S.hw = S.appl; /* never: the device returns what we gave it */
    uint64_t av = avail_locked();
    if (av > S.avail_max) S.avail_max = av;
    check_xrun_locked();
    if (S.state == ST_DRAINING && S.hw >= S.appl) S.state = ST_SETUP;
  }
  submit_locked();
  return n > 0;
}

void linux_snd_irq(void) {
  if (!lsnd_inited) return;
  uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
  int moved = service_locked();
  spinlock_release_irqrestore(&lsnd_lock, fl);
  if (moved) lsnd_wake();
}

static void service(void) {
  uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
  int moved = service_locked();
  spinlock_release_irqrestore(&lsnd_lock, fl);
  if (moved) lsnd_wake();
}

/* --- open/close ------------------------------------------------------------ */

int linux_snd_open(int minor, uint32_t flags) {
  (void)flags;
  lsnd_init_once();
  if (!virtio_snd_present()) return -ENOENT;
  if (minor == LSND_MINOR_CONTROL) {
    ylock_acquire(&lsnd_op);
    ctl_open_count++;
    ylock_release(&lsnd_op);
    return 0;
  }
  if (minor != LSND_MINOR_PCM_PLAY) return -ENOENT;
  ylock_acquire(&lsnd_op);
  int r = 0;
  if (S.open) {
    r = -EBUSY;
  } else {
    uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
    S.open = 1;
    S.state = ST_OPEN;
    S.hw = S.appl = S.submit = 0;
    S.epoch++;
    S.inflight = 0;
    S.user_pversion = 0;
    S.tstamp_mode = S.tstamp_type = 0;
    S.xruns = S.chunks = S.frames_done = 0;
    S.n_sync_ptr = S.n_writei = S.n_status = S.n_refine = S.n_parks = 0;
    spinlock_release_irqrestore(&lsnd_lock, fl);
  }
  ylock_release(&lsnd_op);
  return r;
}

static void dev_teardown(void) { /* lsnd_op held */
  if (S.dev_started) virtio_snd_pcm_cmd(VSND_R_PCM_STOP, 0);
  if (S.dev_prepared) virtio_snd_pcm_cmd(VSND_R_PCM_RELEASE, 0);
  S.dev_started = S.dev_prepared = 0;
}

void linux_snd_release(int minor) {
  if (!lsnd_inited) return;
  ylock_acquire(&lsnd_op);
  if (minor == LSND_MINOR_CONTROL) {
    if (ctl_open_count > 0) ctl_open_count--;
  } else if (minor == LSND_MINOR_PCM_PLAY && S.open) {
    uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
    S.state = ST_OPEN;
    S.epoch++;
    S.inflight = 0;
    S.open = 0;
    spinlock_release_irqrestore(&lsnd_lock, fl);
    dev_teardown();
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[SND] pcm closed: chunks=");
    kl_i(&kl, (long)S.chunks);
    kl_s(&kl, " frames=");
    kl_i(&kl, (long)S.frames_done);
    kl_s(&kl, " xruns=");
    kl_i(&kl, (long)S.xruns);
    kl_s(&kl, " sync_ptr=");
    kl_i(&kl, (long)S.n_sync_ptr);
    kl_s(&kl, " writei=");
    kl_i(&kl, (long)S.n_writei);
    kl_s(&kl, " status=");
    kl_i(&kl, (long)S.n_status);
    kl_s(&kl, " refine=");
    kl_i(&kl, (long)S.n_refine);
    kl_s(&kl, " parks=");
    kl_i(&kl, (long)S.n_parks);
    {
      struct vsnd_stats vs;
      virtio_snd_stats(&vs);
      kl_s(&kl, " irqs=");
      kl_i(&kl, (long)vs.irqs);
      kl_s(&kl, " tx=");
      kl_i(&kl, (long)vs.tx_done);
      kl_s(&kl, " ctl=");
      kl_i(&kl, (long)vs.ctl_cmds);
      kl_s(&kl, " ctl_timeouts=");
      kl_i(&kl, (long)vs.ctl_timeouts);
    }
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  ylock_release(&lsnd_op);
  lsnd_wake();
}

/* --- poll ------------------------------------------------------------------- */

const void *linux_snd_poll_key(void) { return &S; }

void linux_snd_poll_probe(int minor, int *r, int *w, int *e) {
  *r = *w = *e = 0;
  if (minor != LSND_MINOR_PCM_PLAY || !lsnd_inited) return;
  service();
  uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
  switch (S.state) {
  case ST_PREPARED:
  case ST_RUNNING:
  case ST_PAUSED:
    if (avail_locked() >= S.avail_min) *w = 1;
    break;
  case ST_DRAINING:
    break;
  default:
    *w = 1;
    *e = 1;
    break;
  }
  spinlock_release_irqrestore(&lsnd_lock, fl);
}

/* --- control node ------------------------------------------------------------- */

#define ELEM_ID_SZ 64
static const char *elem_name[3] = {0, "Master Playback Volume", "Master Playback Switch"};

static void fill_elem_id(uint8_t *id, int numid) {
  z(id, ELEM_ID_SZ);
  uint32_t v = (uint32_t)numid;
  cpy(id, &v, 4);
  v = 2; /* SNDRV_CTL_ELEM_IFACE_MIXER */
  cpy(id + 4, &v, 4);
  put_str(id + 16, 44, elem_name[numid]);
}

static int find_elem(const uint8_t *id) {
  uint32_t numid, iface, index;
  cpy(&numid, id, 4);
  cpy(&iface, id + 4, 4);
  cpy(&index, id + 60, 4);
  if (numid) return (numid == 1 || numid == 2) ? (int)numid : -ENOENT;
  if (iface != 2 || index != 0) return -ENOENT;
  for (int k = 1; k <= 2; k++)
    if (str_eq(id + 16, elem_name[k], 44)) return k;
  return -ENOENT;
}

static void fill_pcm_info(uint8_t *b) { /* 288 bytes */
  z(b, 288);
  put_str(b + 16, 64, "virtio-snd");
  put_str(b + 80, 80, "VirtIO PCM 0");
  put_str(b + 160, 32, "subdevice #0");
  uint32_t one = 1, avail = S.open ? 0 : 1;
  cpy(b + 200, &one, 4);
  cpy(b + 204, &avail, 4);
}

static int ctl_ioctl(struct process *p, uint32_t req, uint64_t arg) {
  switch (req) {
  case CTL_PVERSION: {
    uint32_t v = SND_CTL_VERSION;
    return cout(p, arg, &v, 4);
  }
  case CTL_CARD_INFO: {
    uint8_t b[376];
    z(b, sizeof b);
    put_str(b + 8, 16, "VirtIO");
    put_str(b + 24, 16, "virtio-snd");
    put_str(b + 40, 32, "VirtIO SoundCard");
    put_str(b + 72, 80, "VirtIO SoundCard (HobbyOS virtio-sound)");
    put_str(b + 168, 80, "virtio-snd");
    return cout(p, arg, b, sizeof b);
  }
  case CTL_PCM_NEXT_DEVICE:
  case CTL_HWDEP_NEXT_DEVICE:
  case CTL_RAWMIDI_NEXT_DEVICE: {
    int32_t d;
    int r = cin(p, arg, &d, 4);
    if (r) return r;
    d = (req == CTL_PCM_NEXT_DEVICE && d < 0) ? 0 : -1;
    return cout(p, arg, &d, 4);
  }
  case CTL_PCM_INFO: {
    uint8_t b[288];
    int r = cin(p, arg, b, sizeof b);
    if (r) return r;
    uint32_t dev, sub;
    int32_t stream;
    cpy(&dev, b, 4);
    cpy(&sub, b + 4, 4);
    cpy(&stream, b + 8, 4);
    if (dev != 0) return -ENXIO;
    if (stream != 0) return -ENOENT;
    if (sub != 0) return -ENXIO;
    fill_pcm_info(b);
    return cout(p, arg, b, sizeof b);
  }
  case CTL_PCM_PREFER_SUBDEVICE:
  case CTL_SUBSCRIBE_EVENTS: {
    int32_t v;
    int r = cin(p, arg, &v, 4);
    if (r) return r;
    if (req == CTL_SUBSCRIBE_EVENTS && v < 0) {
      v = 0;
      return cout(p, arg, &v, 4);
    }
    return 0;
  }
  case CTL_POWER_STATE: {
    uint32_t v = 0; /* D0 */
    return cout(p, arg, &v, 4);
  }
  case CTL_ELEM_LIST: {
    uint8_t b[80];
    int r = cin(p, arg, b, sizeof b);
    if (r) return r;
    uint32_t off, space, used = 0, count = 2;
    uint64_t pids;
    cpy(&off, b, 4);
    cpy(&space, b + 4, 4);
    cpy(&pids, b + 16, 8);
    for (uint32_t k = off; k < count && used < space; k++, used++) {
      uint8_t id[ELEM_ID_SZ];
      fill_elem_id(id, (int)k + 1);
      if ((r = cout(p, pids + (uint64_t)used * ELEM_ID_SZ, id, ELEM_ID_SZ)) != 0) return r;
    }
    cpy(b + 8, &used, 4);
    cpy(b + 12, &count, 4);
    return cout(p, arg, b, sizeof b);
  }
  case CTL_ELEM_INFO: {
    uint8_t b[272];
    int r = cin(p, arg, b, sizeof b);
    if (r) return r;
    int k = find_elem(b);
    if (k < 0) return k;
    z(b, sizeof b);
    fill_elem_id(b, k);
    uint32_t type = k == 1 ? 2 : 1, access = 3, count = k == 1 ? 2 : 1;
    int64_t mn = 0, mx = k == 1 ? 100 : 1, st = k == 1 ? 1 : 0;
    cpy(b + 64, &type, 4);
    cpy(b + 68, &access, 4);
    cpy(b + 72, &count, 4);
    cpy(b + 80, &mn, 8);
    cpy(b + 88, &mx, 8);
    cpy(b + 96, &st, 8);
    return cout(p, arg, b, sizeof b);
  }
  case CTL_ELEM_READ:
  case CTL_ELEM_WRITE: {
    static uint8_t b[1224]; /* under lsnd_op */
    int r = cin(p, arg, b, sizeof b);
    if (r) return r;
    int k = find_elem(b);
    if (k < 0) return k;
    if (req == CTL_ELEM_WRITE) {
      int64_t v0, v1;
      cpy(&v0, b + 72, 8);
      cpy(&v1, b + 80, 8);
      if (k == 1) {
        if (v0 < 0 || v0 > 100 || v1 < 0 || v1 > 100) return -EINVAL;
        mix_vol[0] = (int32_t)v0;
        mix_vol[1] = (int32_t)v1;
      } else {
        mix_sw = v0 ? 1 : 0;
      }
      return 0;
    }
    z(b + 64, sizeof b - 64);
    fill_elem_id(b, k);
    int64_t v0 = k == 1 ? mix_vol[0] : mix_sw, v1 = k == 1 ? mix_vol[1] : 0;
    cpy(b + 72, &v0, 8);
    cpy(b + 80, &v1, 8);
    return cout(p, arg, b, sizeof b);
  }
  case CTL_ELEM_LOCK:
  case CTL_ELEM_UNLOCK:
    return 0;
  case CTL_TLV_READ:
    return -ENXIO;
  default:
    return -ENOTTY;
  }
}

/* --- PCM node ------------------------------------------------------------------- */

static void fill_status(uint8_t *b) { /* 152 bytes, lsnd_lock held */
  z(b, 152);
  int32_t st = S.state;
  cpy(b, &st, 4);
  put_ts(b + 8, S.trigger_us);
  put_ts(b + 24, timer_get_us());
  uint64_t bd = S.boundary ? S.boundary : ~0ull;
  uint64_t appl = S.appl % bd, hw = S.hw % bd, av = avail_locked();
  int64_t delay = (S.state == ST_RUNNING || S.state == ST_DRAINING || S.state == ST_PAUSED)
                      ? (int64_t)(S.appl - S.hw) : 0;
  cpy(b + 40, &appl, 8);
  cpy(b + 48, &hw, 8);
  cpy(b + 56, &delay, 8);
  cpy(b + 64, &av, 8);
  uint64_t am = S.avail_max > av ? S.avail_max : av;
  cpy(b + 72, &am, 8);
  S.avail_max = av;
  put_ts(b + 112, timer_get_us());
}

static int hwsync_state_err(void) {
  switch (S.state) {
  case ST_RUNNING:
  case ST_DRAINING:
  case ST_PREPARED:
  case ST_PAUSED:
    return 0;
  case ST_XRUN:
    return -EPIPE;
  default:
    return -EBADFD;
  }
}

/* START: PREPARED -> RUNNING.  lsnd_op held, lsnd_lock NOT held (may send the
 * device's PCM_START). */
static int do_start(void) {
  uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
  if (S.state != ST_PREPARED) {
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return -EBADFD;
  }
  if (S.appl == S.hw && S.stop_thr < S.boundary) {
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return -EPIPE; /* Linux: no playback data */
  }
  spinlock_release_irqrestore(&lsnd_lock, fl);
  if (!S.dev_started) {
    uint32_t st = 0;
    if (virtio_snd_pcm_cmd(VSND_R_PCM_START, &st) != 0) return -EIO;
    S.dev_started = 1;
  }
  fl = spinlock_acquire_irqsave(&lsnd_lock);
  S.state = ST_RUNNING;
  S.trigger_us = timer_get_us();
  submit_locked();
  spinlock_release_irqrestore(&lsnd_lock, fl);
  return 0;
}

static int do_hw_params(struct process *p, uint64_t arg, int refine_only) {
  struct lsnd_hw_params hp;
  struct lsnd_hw_caps caps;
  int r = cin(p, arg, &hp, sizeof hp);
  if (r) return r;
  lsnd_caps(&caps);
  if (refine_only) {
    r = lsnd_hw_refine(&hp, &caps);
    if (r < 0) return r;
    return cout(p, arg, &hp, sizeof hp);
  }
  if (S.state != ST_OPEN && S.state != ST_SETUP && S.state != ST_PREPARED) return -EBADFD;
  struct lsnd_hw_params in = hp;
  r = lsnd_hw_choose(&hp, &caps);
  if (r < 0) {
    /* one row per refused HW_PARAMS: what the caller asked for */
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[SND] HW_PARAMS refused r=");
    kl_i(&kl, r);
    kl_s(&kl, " fmt=");
    kl_hex(&kl, in.masks[LSND_PARAM_FORMAT].bits[0]);
    static const char *nm[12] = {"sbits", "fbits", "ch", "rate", "ptime", "psize",
                                 "pbytes", "periods", "btime", "bsize", "bbytes", "tick"};
    for (int k = 0; k < 12; k++) {
      const struct lsnd_interval *v = &in.intervals[k];
      if (v->min == 0 && v->max == 0xFFFFFFFFu && !v->flags) continue;
      kl_c(&kl, ' ');
      kl_s(&kl, nm[k]);
      kl_c(&kl, (v->flags & LSND_IV_OPENMIN) ? '(' : '[');
      kl_i(&kl, (long)v->min);
      kl_c(&kl, ',');
      kl_i(&kl, (long)v->max);
      kl_c(&kl, (v->flags & LSND_IV_OPENMAX) ? ')' : ']');
      if (v->flags & LSND_IV_INTEGER) kl_c(&kl, 'i');
    }
    kl_c(&kl, '\n');
    kl_emit(&kl);
    return r;
  }
  uint32_t ch = lsnd_iv_value(lsnd_iv(&hp, LSND_PARAM_CHANNELS));
  uint32_t rate = lsnd_iv_value(lsnd_iv(&hp, LSND_PARAM_RATE));
  uint64_t period = lsnd_iv_value(lsnd_iv(&hp, LSND_PARAM_PERIOD_SIZE));
  uint64_t periods = lsnd_iv_value(lsnd_iv(&hp, LSND_PARAM_PERIODS));
  uint64_t buffer = lsnd_iv_value(lsnd_iv(&hp, LSND_PARAM_BUFFER_SIZE));
  uint32_t fb = ch * 2;
  int rc = lsnd_rate_code(rate);
  if (rc < 0 || buffer != period * periods || buffer * fb > LSND_RING_BYTES) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[SND] HW_PARAMS inconsistent choice period=");
    kl_i(&kl, (long)period);
    kl_s(&kl, " periods=");
    kl_i(&kl, (long)periods);
    kl_s(&kl, " buffer=");
    kl_i(&kl, (long)buffer);
    kl_c(&kl, '\n');
    kl_emit(&kl);
    return -EINVAL;
  }
  dev_teardown();
  uint32_t st = 0;
  if (virtio_snd_set_params((uint32_t)(buffer * fb), (uint32_t)(period * fb), (uint8_t)ch,
                            VSND_PCM_FMT_S16, (uint8_t)rc, &st) != 0 ||
      virtio_snd_pcm_cmd(VSND_R_PCM_PREPARE, &st) != 0) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[SND] device SET_PARAMS/PREPARE failed status=");
    kl_hex(&kl, st);
    kl_c(&kl, '\n');
    kl_emit(&kl);
    return -EIO;
  }
  S.dev_prepared = 1;
  uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
  S.channels = ch;
  S.rate = rate;
  S.fmt = LSND_FORMAT_S16_LE;
  S.frame_bytes = fb;
  S.period = period;
  S.periods = periods;
  S.buffer = buffer;
  S.boundary = buffer;
  while (S.boundary * 2 <= 0x7FFFFFFFFFFFFFFFull - buffer) S.boundary *= 2;
  S.avail_min = period;
  S.start_thr = 1;
  S.stop_thr = buffer;
  S.hw = S.appl = S.submit = 0;
  S.epoch++;
  S.inflight = 0;
  S.state = ST_SETUP;
  spinlock_release_irqrestore(&lsnd_lock, fl);
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[SND] hw_params rate=");
  kl_i(&kl, rate);
  kl_s(&kl, " ch=");
  kl_i(&kl, ch);
  kl_s(&kl, " period=");
  kl_i(&kl, (long)period);
  kl_s(&kl, " periods=");
  kl_i(&kl, (long)periods);
  kl_s(&kl, " buffer=");
  kl_i(&kl, (long)buffer);
  kl_c(&kl, '\n');
  kl_emit(&kl);
  return cout(p, arg, &hp, sizeof hp);
}

static int do_sw_params(struct process *p, uint64_t arg) {
  uint8_t b[136];
  int r = cin(p, arg, b, sizeof b);
  if (r) return r;
  if (S.state == ST_OPEN) return -EBADFD;
  int32_t tmode;
  uint32_t ttype;
  uint64_t avail_min, start_thr, stop_thr, sil_thr, sil_size;
  cpy(&tmode, b, 4);
  cpy(&avail_min, b + 16, 8);
  cpy(&start_thr, b + 32, 8);
  cpy(&stop_thr, b + 40, 8);
  cpy(&sil_thr, b + 48, 8);
  cpy(&sil_size, b + 56, 8);
  cpy(&ttype, b + 76, 4);
  if (tmode < 0 || tmode > 1 || avail_min == 0 || ttype > 2) return -EINVAL;
  if (sil_size < S.boundary && (sil_size > sil_thr || sil_thr > S.buffer)) return -EINVAL;
  uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
  S.tstamp_mode = tmode;
  S.tstamp_type = (int)ttype;
  S.avail_min = avail_min;
  S.start_thr = start_thr;
  S.stop_thr = stop_thr;
  cpy(b + 64, &S.boundary, 8);
  spinlock_release_irqrestore(&lsnd_lock, fl);
  return cout(p, arg, b, sizeof b);
}

/* WRITEI_FRAMES: copy what fits; park when nothing fits (blocking fd). */
static int do_writei(struct process *p, uint64_t arg, int nonblock) {
  uint8_t x[24];
  int r = cin(p, arg, x, sizeof x);
  if (r) return r;
  uint64_t buf, frames;
  cpy(&buf, x + 8, 8);
  cpy(&frames, x + 16, 8);
  service();
  uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
  int st = S.state;
  if (st == ST_XRUN) {
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return -EPIPE;
  }
  if (st != ST_PREPARED && st != ST_RUNNING && st != ST_PAUSED) {
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return -EBADFD;
  }
  uint64_t av = avail_locked();
  uint32_t seen = __atomic_load_n(&lsnd_seq, __ATOMIC_SEQ_CST);
  uint64_t appl = S.appl;
  uint64_t fb = S.frame_bytes, bufsz = S.buffer;
  spinlock_release_irqrestore(&lsnd_lock, fl);
  if (frames == 0) {
    int64_t z0 = 0;
    cpy(x, &z0, 8);
    return cout(p, arg, x, sizeof x);
  }
  if (av == 0) {
    if (nonblock) return -EAGAIN;
    uint64_t w = st == ST_RUNNING ? 2 * lsnd_period_us() + 2000 : 100000;
    return lsnd_park(seen, w);
  }
  uint64_t n = frames < av ? frames : av;
  /* copy into the ring: at most two pieces.  Only this thread moves appl
   * (lsnd_op), and the device never reads [appl, hw + buffer). */
  uint64_t done = 0;
  while (done < n) {
    uint64_t pos = (appl + done) % bufsz;
    uint64_t piece = bufsz - pos;
    if (piece > n - done) piece = n - done;
    if (cin(p, buf + done * fb, lsnd_ring + pos * fb, (int)(piece * fb)) != 0) {
      if (done == 0) return -EFAULT;
      break;
    }
    if (!mix_sw || mix_vol[0] != 100 || mix_vol[1] != 100) {
      int16_t *s = (int16_t *)(lsnd_ring + pos * fb);
      uint64_t ns = piece * S.channels;
      for (uint64_t i = 0; i < ns; i++) {
        int32_t v = mix_sw ? mix_vol[(i % S.channels) & 1] : 0;
        s[i] = (int16_t)((int32_t)s[i] * v / 100);
      }
    }
    done += piece;
  }
  int need_start = 0;
  fl = spinlock_acquire_irqsave(&lsnd_lock);
  S.appl += done;
  if (S.state == ST_PREPARED && S.appl - S.hw >= S.start_thr) need_start = 1;
  submit_locked();
  spinlock_release_irqrestore(&lsnd_lock, fl);
  if (need_start) do_start();
  int64_t res = (int64_t)done;
  cpy(x, &res, 8);
  return cout(p, arg, x, sizeof x);
}

static int do_drain(int nonblock) {
  service();
  uint64_t fl = spinlock_acquire_irqsave(&lsnd_lock);
  int st = S.state;
  if (st == ST_OPEN) {
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return -EBADFD;
  }
  if (st == ST_XRUN || st == ST_SETUP || (st == ST_PREPARED && S.appl == S.hw)) {
    S.state = ST_SETUP;
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return 0;
  }
  spinlock_release_irqrestore(&lsnd_lock, fl);
  if (st == ST_PREPARED) {
    int r = do_start();
    if (r < 0) return r;
  }
  fl = spinlock_acquire_irqsave(&lsnd_lock);
  if (S.state == ST_RUNNING || S.state == ST_PAUSED) S.state = ST_DRAINING;
  submit_locked();
  int done = S.state != ST_DRAINING;
  uint32_t seen = __atomic_load_n(&lsnd_seq, __ATOMIC_SEQ_CST);
  spinlock_release_irqrestore(&lsnd_lock, fl);
  if (done) return 0;
  if (nonblock) return -EAGAIN;
  return lsnd_park(seen, 2 * lsnd_period_us() + 2000);
}

static int pcm_ioctl(struct process *p, uint32_t req, uint64_t arg, int nonblock) {
  uint64_t fl;
  switch (req) {
  case PCM_PVERSION: {
    uint32_t v = SND_PCM_VERSION;
    return cout(p, arg, &v, 4);
  }
  case PCM_INFO: {
    uint8_t b[288];
    fill_pcm_info(b);
    return cout(p, arg, b, sizeof b);
  }
  case PCM_TSTAMP:
  case PCM_TTSTAMP:
  case PCM_USER_PVERSION: {
    int32_t v;
    int r = cin(p, arg, &v, 4);
    if (r) return r;
    if (req == PCM_USER_PVERSION) S.user_pversion = (uint32_t)v;
    else if (req == PCM_TTSTAMP) {
      if (v < 0 || v > 2) return -EINVAL;
      S.tstamp_type = v;
    }
    return 0;
  }
  case PCM_HW_REFINE: S.n_refine++; return do_hw_params(p, arg, 1);
  case PCM_HW_PARAMS: return do_hw_params(p, arg, 0);
  case PCM_HW_FREE:
    if (S.state != ST_OPEN && S.state != ST_SETUP && S.state != ST_PREPARED) return -EBADFD;
    dev_teardown();
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    S.state = ST_OPEN;
    S.epoch++;
    S.inflight = 0;
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return 0;
  case PCM_SW_PARAMS: return do_sw_params(p, arg);
  case PCM_STATUS:
  case PCM_STATUS_EXT: {
    uint8_t b[152];
    S.n_status++;
    service();
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    fill_status(b);
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return cout(p, arg, b, sizeof b);
  }
  case PCM_DELAY:
  case PCM_HWSYNC: {
    service();
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    int e = hwsync_state_err();
    int64_t d = (int64_t)(S.appl - S.hw);
    spinlock_release_irqrestore(&lsnd_lock, fl);
    if (e) return e;
    return req == PCM_DELAY ? cout(p, arg, &d, 8) : 0;
  }
  case PCM_SYNC_PTR: {
    uint8_t b[136];
    S.n_sync_ptr++;
    int r = cin(p, arg, b, sizeof b);
    if (r) return r;
    uint32_t flags;
    cpy(&flags, b, 4);
    if (flags & 1u) service(); /* SNDRV_PCM_SYNC_PTR_HWSYNC */
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    uint64_t bd = S.boundary ? S.boundary : ~0ull;
    if (!(flags & 2u) && S.state != ST_OPEN) { /* !SYNC_PTR_APPL: apply the user's appl_ptr */
      uint64_t ua;
      cpy(&ua, b + 72, 8);
      uint64_t base = S.appl - (S.appl % bd);
      uint64_t na = base + (ua % bd);
      if (na < S.hw && na + bd <= S.hw + S.buffer) na += bd;
      if (na >= S.submit && na <= S.hw + S.buffer) S.appl = na;
    }
    if (!(flags & 4u)) { /* !SYNC_PTR_AVAIL_MIN */
      uint64_t am;
      cpy(&am, b + 80, 8);
      if (am) S.avail_min = am;
    }
    z(b + 8, 64);
    int32_t st = S.state;
    cpy(b + 8, &st, 4);
    uint64_t hw = S.hw % bd, appl = S.appl % bd;
    cpy(b + 16, &hw, 8);
    put_ts(b + 24, timer_get_us());
    cpy(b + 72, &appl, 8);
    cpy(b + 80, &S.avail_min, 8);
    int start = S.state == ST_PREPARED && S.appl - S.hw >= S.start_thr && S.appl > S.hw;
    submit_locked();
    spinlock_release_irqrestore(&lsnd_lock, fl);
    if (start) do_start();
    return cout(p, arg, b, sizeof b);
  }
  case PCM_PREPARE:
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    if (S.state == ST_OPEN) {
      spinlock_release_irqrestore(&lsnd_lock, fl);
      return -EBADFD;
    }
    if (S.state == ST_RUNNING || S.state == ST_DRAINING || S.state == ST_PAUSED) {
      spinlock_release_irqrestore(&lsnd_lock, fl);
      return -EBUSY;
    }
    S.hw = S.appl = S.submit = 0;
    S.epoch++;
    S.inflight = 0;
    S.avail_max = 0;
    S.state = ST_PREPARED;
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return 0;
  case PCM_RESET:
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    if (S.state != ST_RUNNING && S.state != ST_PREPARED && S.state != ST_PAUSED) {
      int e = S.state == ST_XRUN ? -EPIPE : -EBADFD;
      spinlock_release_irqrestore(&lsnd_lock, fl);
      return e;
    }
    S.epoch++;
    S.inflight = 0;
    S.appl = S.submit = S.hw;
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return 0;
  case PCM_START: return do_start();
  case PCM_DROP:
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    if (S.state == ST_OPEN) {
      spinlock_release_irqrestore(&lsnd_lock, fl);
      return -EBADFD;
    }
    if (S.state != ST_SETUP) {
      S.state = ST_SETUP;
      S.epoch++;
      S.inflight = 0;
      S.trigger_us = timer_get_us();
    }
    spinlock_release_irqrestore(&lsnd_lock, fl);
    lsnd_wake();
    return 0;
  case PCM_DRAIN: return do_drain(nonblock);
  case PCM_PAUSE: {
    int32_t push = (int32_t)arg; /* _IOW but alsa-lib passes the int by value */
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    int r = 0;
    if (push && S.state == ST_RUNNING) S.state = ST_PAUSED;
    else if (!push && S.state == ST_PAUSED) {
      S.state = ST_RUNNING;
      submit_locked();
    } else r = -EBADFD;
    S.trigger_us = timer_get_us();
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return r;
  }
  case PCM_XRUN:
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    if (S.state == ST_RUNNING || S.state == ST_PREPARED || S.state == ST_PAUSED ||
        S.state == ST_DRAINING) {
      S.state = ST_XRUN;
      S.xruns++;
    }
    spinlock_release_irqrestore(&lsnd_lock, fl);
    return S.state == ST_XRUN ? 0 : -EBADFD;
  case PCM_REWIND:
  case PCM_FORWARD: {
    uint64_t fr;
    int r = cin(p, arg, &fr, 8);
    if (r) return r;
    fl = spinlock_acquire_irqsave(&lsnd_lock);
    int e = hwsync_state_err();
    uint64_t moved = 0;
    if (!e) {
      if (req == PCM_REWIND) {
        uint64_t can = S.appl - S.submit;
        moved = fr < can ? fr : can;
        S.appl -= moved;
      } else {
        uint64_t can = avail_locked();
        moved = fr < can ? fr : can;
        S.appl += moved; /* forwarded frames play whatever the ring held */
        submit_locked();
      }
    }
    spinlock_release_irqrestore(&lsnd_lock, fl);
    if (e) return e;
    return cout(p, arg, &moved, 8); /* Linux: 0, the count written back */
  }
  case PCM_WRITEI: S.n_writei++; return do_writei(p, arg, nonblock);
  case PCM_RESUME: return -ENOSYS;
  case PCM_CHANNEL_INFO:
  case PCM_LINK:
  case PCM_UNLINK:
  case PCM_READI:
  case PCM_WRITEN:
    return -EINVAL;
  default:
    return -ENOTTY;
  }
}

int linux_snd_ioctl(struct process *p, int minor, uint32_t open_flags, uint32_t req,
                    uint64_t arg) {
  if (!lsnd_inited || !virtio_snd_present()) return -ENODEV;
  lsnd_unpark(current_process());
  ylock_acquire(&lsnd_op);
  int r;
  if (minor == LSND_MINOR_CONTROL)
    r = ctl_ioctl(p, req, arg);
  else
    r = pcm_ioctl(p, req, arg, (open_flags & 0x800u) != 0 /* O_NONBLOCK */);
  ylock_release(&lsnd_op);
  return r;
}
