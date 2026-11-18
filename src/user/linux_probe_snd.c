/*
 * src/user/linux_probe_snd.c — the ALSA kernel ABI over virtio-sound with raw
 * ioctls, no alsa-lib (lane C4-audio, docs/cp-audio.md).
 *
 * ORDINARY musl Linux program (runs on a Linux host with snd hardware too).
 * Rows "[LP] ...", last "[LP] done rc=<fails>".
 *
 * Without a sound device (every default gate boot) /dev/snd/controlC0 is
 * ENOENT: the probe checks that the PCM node is ENOENT too and passes.  With
 * the opt-in device (CP_AUDIO_WAV, tools/cp/audio/run-probe-snd.sh) it plays
 * a 1000 Hz tone that tools/cp/audio/wavcheck.py finds in the host's WAV:
 *
 *   C1  control: PVERSION 2.x, CARD_INFO driver "virtio-snd", PCM_NEXT_DEVICE
 *       -1 -> 0 -> -1, PCM_INFO(0,0,playback) ok, capture ENOENT
 *   C2  mixer: ELEM_LIST 2 elements, ELEM_INFO by name, READ/WRITE volume
 *   P1  pcm open; a second open is EBUSY; PVERSION/INFO/TTSTAMP/USER_PVERSION
 *   P2  mmap of the status page is refused (alsa-lib's SYNC_PTR fallback)
 *   P3  HW_REFINE(any): 48000 Hz, 2 ch, S16_LE; HW_PARAMS 480 x 4
 *   P4  SW_PARAMS; PREPARE -> PREPARED; START with no data -> EPIPE
 *   P5  2.0 s tone by blocking WRITEI: RUNNING, hw_ptr monotonic, real-time
 *       pacing, poll(POLLOUT) wakes, SYNC_PTR agrees with STATUS
 *   P6  DRAIN -> SETUP, all frames played (hw_ptr == appl_ptr)
 *   P7  XRUN: stop writing -> XRUN within ~100 ms, WRITEI EPIPE, PREPARE
 *       recovers, then 0.5 s more tone
 *   P8  O_NONBLOCK: fill to EAGAIN; PAUSE/unpause; DROP; HW_FREE; close
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define ROW(...) (printf("[LP] " __VA_ARGS__), printf("\n"), fflush(stdout))
#define CHECK(c, ...)                         \
  do {                                        \
    if (c) {                                  \
      printf("[LP] ok   ");                   \
    } else {                                  \
      printf("[LP] FAIL ");                   \
      fails++;                                \
    }                                         \
    printf(__VA_ARGS__);                      \
    printf("\n");                             \
    fflush(stdout);                           \
  } while (0)

#define CTL_PVERSION 0x80045500u
#define CTL_CARD_INFO 0x81785501u
#define CTL_ELEM_LIST 0xC0505510u
#define CTL_ELEM_INFO 0xC1105511u
#define CTL_ELEM_READ 0xC4C85512u
#define CTL_ELEM_WRITE 0xC4C85513u
#define CTL_PCM_NEXT_DEVICE 0x80045530u
#define CTL_PCM_INFO 0xC1205531u
#define CTL_PCM_PREFER_SUBDEVICE 0x40045532u
#define PCM_PVERSION 0x80044100u
#define PCM_INFO 0x81204101u
#define PCM_TTSTAMP 0x40044103u
#define PCM_USER_PVERSION 0x40044104u
#define PCM_HW_REFINE 0xC2604110u
#define PCM_HW_PARAMS 0xC2604111u
#define PCM_HW_FREE 0x00004112u
#define PCM_SW_PARAMS 0xC0884113u
#define PCM_STATUS 0x80984120u
#define PCM_DELAY 0x80084121u
#define PCM_SYNC_PTR 0xC0884123u
#define PCM_PREPARE 0x00004140u
#define PCM_START 0x00004142u
#define PCM_DROP 0x00004143u
#define PCM_DRAIN 0x00004144u
#define PCM_PAUSE 0x40044145u
#define PCM_WRITEI 0x40184150u

struct mask { uint32_t b[8]; };
struct iv { uint32_t min, max, flags; };
struct hwp {
  uint32_t flags;
  struct mask masks[3], mres[5];
  struct iv iv[12], ires[9];
  uint32_t rmask, cmask, info, msbits, rate_num, rate_den;
  uint64_t fifo;
  uint8_t res[64];
};
_Static_assert(sizeof(struct hwp) == 608, "hw_params");
struct swp {
  int32_t tstamp_mode;
  uint32_t period_step, sleep_min;
  uint64_t avail_min, xfer_align, start_thr, stop_thr, sil_thr, sil_size, boundary;
  uint32_t proto, tstamp_type;
  uint8_t res[56];
};
_Static_assert(sizeof(struct swp) == 136, "sw_params");
struct status {
  int32_t state, pad;
  struct timespec trig, ts;
  uint64_t appl, hw;
  int64_t delay;
  uint64_t avail, avail_max, overrange;
  int32_t susp;
  uint32_t atd;
  struct timespec ats, dts;
  uint32_t acc;
  uint8_t res[20];
};
_Static_assert(sizeof(struct status) == 152, "status");
struct syncptr {
  uint32_t flags, pad;
  union {
    struct { int32_t state, pad1; uint64_t hw; struct timespec ts; int32_t susp, pad2; struct timespec ats; } s;
    uint8_t r[64];
  } s;
  union {
    struct { uint64_t appl, avail_min; } c;
    uint8_t r[64];
  } c;
};
_Static_assert(sizeof(struct syncptr) == 136, "sync_ptr");
struct xferi { int64_t result; void *buf; uint64_t frames; };

enum { S_OPEN, S_SETUP, S_PREPARED, S_RUNNING, S_XRUN, S_DRAINING, S_PAUSED };

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static void msleep(int ms) {
  struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&t, 0);
}

#define RATE 48000
#define CH 2
#define TONE 1000.0
static double phase;
static void make_tone(int16_t *b, int frames) {
  for (int i = 0; i < frames; i++) {
    int16_t v = (int16_t)(16000.0 * sin(phase));
    b[2 * i] = b[2 * i + 1] = v;
    phase += 2 * M_PI * TONE / RATE;
    if (phase > 2 * M_PI) phase -= 2 * M_PI;
  }
}

static struct iv *IV(struct hwp *p, int k) { return &p->iv[k - 8]; }
static void any(struct hwp *p) {
  memset(p, 0, sizeof *p);
  for (int k = 0; k < 3; k++) memset(&p->masks[k], 0xff, sizeof p->masks[k]);
  for (int k = 8; k <= 19; k++) { IV(p, k)->min = 0; IV(p, k)->max = 0xffffffffu; }
  p->rmask = 0xffffffffu;
  p->info = 0xffffffffu;
}
static void fix(struct hwp *p, int k, uint32_t v) { IV(p, k)->min = IV(p, k)->max = v; IV(p, k)->flags = 0; }

static int pstatus(int fd, struct status *st) {
  memset(st, 0, sizeof *st);
  return ioctl(fd, PCM_STATUS, st);
}

static int writei(int fd, int16_t *buf, uint64_t frames) {
  struct xferi x = {0, buf, frames};
  if (ioctl(fd, PCM_WRITEI, &x) < 0) return -errno;
  return (int)x.result;
}

/* Write `frames` frames of tone, blocking; returns frames written or -errno. */
static long play(int fd, long frames, uint64_t *hw_last, int *mono_ok) {
  long done = 0;
  static int16_t chunk[480 * CH];
  while (done < frames) {
    int n = frames - done > 480 ? 480 : (int)(frames - done);
    make_tone(chunk, n);
    int off = 0;
    while (off < n) {
      int w = writei(fd, chunk + off * CH, n - off);
      if (w < 0) return w;
      off += w;
    }
    done += n;
    struct status st;
    if (hw_last && pstatus(fd, &st) == 0) {
      if (st.hw < *hw_last) *mono_ok = 0;
      *hw_last = st.hw;
    }
  }
  return done;
}

int main(void) {
  ROW("snd probe start");
  int ctl = open("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
  if (ctl < 0 && errno == ENOENT) {
    int e1 = errno;
    int pcm = open("/dev/snd/pcmC0D0p", O_RDWR);
    int e2 = errno;
    CHECK(pcm < 0 && e2 == ENOENT, "no sound device: controlC0 ENOENT (%d), pcmC0D0p ENOENT (%d)", e1, e2);
    ROW("snd: no virtio-sound device attached -> the absent-device rows only");
    printf("[LP] done rc=%d\n", fails);
    return fails;
  }
  CHECK(ctl >= 0, "open controlC0 fd=%d errno=%d", ctl, ctl < 0 ? errno : 0);
  if (ctl < 0) { printf("[LP] done rc=%d\n", ++fails); return fails; }

  /* C1 */
  uint32_t ver = 0;
  CHECK(ioctl(ctl, CTL_PVERSION, &ver) == 0 && (ver >> 16) == 2, "ctl PVERSION %#x", ver);
  uint8_t ci[376];
  memset(ci, 0, sizeof ci);
  int r = ioctl(ctl, CTL_CARD_INFO, ci);
  CHECK(r == 0 && !strcmp((char *)ci + 24, "virtio-snd"), "CARD_INFO driver='%s' name='%s'", ci + 24, ci + 40);
  int dev = -1;
  r = ioctl(ctl, CTL_PCM_NEXT_DEVICE, &dev);
  int dev2 = dev;
  int r2 = ioctl(ctl, CTL_PCM_NEXT_DEVICE, &dev2);
  CHECK(r == 0 && dev == 0 && r2 == 0 && dev2 == -1, "PCM_NEXT_DEVICE -1 -> %d -> %d", dev, dev2);
  uint8_t pi[288];
  memset(pi, 0, sizeof pi);
  r = ioctl(ctl, CTL_PCM_INFO, pi);
  CHECK(r == 0 && pi[200] == 1, "ctl PCM_INFO(0,0,playback) id='%s' name='%s' subdevices=%u", pi + 16, pi + 80, pi[200]);
  memset(pi, 0, sizeof pi);
  pi[8] = 1; /* capture */
  r = ioctl(ctl, CTL_PCM_INFO, pi);
  CHECK(r < 0 && errno == ENOENT, "ctl PCM_INFO capture -> ENOENT (r=%d errno=%d)", r, errno);
  int pref = -1;
  CHECK(ioctl(ctl, CTL_PCM_PREFER_SUBDEVICE, &pref) == 0, "PCM_PREFER_SUBDEVICE");

  /* C2 */
  uint8_t ids[2 * 64];
  uint8_t el[80];
  memset(el, 0, sizeof el);
  uint32_t space = 2;
  memcpy(el + 4, &space, 4);
  void *pids = ids;
  memcpy(el + 16, &pids, 8);
  r = ioctl(ctl, CTL_ELEM_LIST, el);
  uint32_t used, count;
  memcpy(&used, el + 8, 4);
  memcpy(&count, el + 12, 4);
  CHECK(r == 0 && used == 2 && count == 2, "ELEM_LIST used=%u count=%u [%s] [%s]", used, count, ids + 16, ids + 64 + 16);
  uint8_t info[272];
  memset(info, 0, sizeof info);
  info[4] = 2; /* MIXER */
  strcpy((char *)info + 16, "Master Playback Volume");
  r = ioctl(ctl, CTL_ELEM_INFO, info);
  int64_t mx;
  memcpy(&mx, info + 88, 8);
  CHECK(r == 0 && info[64] == 2 && info[72] == 2 && mx == 100, "ELEM_INFO by name: numid=%u type=%u count=%u max=%ld",
        info[0], info[64], info[72], (long)mx);
  static uint8_t val[1224];
  memset(val, 0, sizeof val);
  val[0] = 1;
  int64_t v = 50;
  memcpy(val + 72, &v, 8);
  memcpy(val + 80, &v, 8);
  r = ioctl(ctl, CTL_ELEM_WRITE, val);
  memset(val, 0, sizeof val);
  val[0] = 1;
  int r3 = ioctl(ctl, CTL_ELEM_READ, val);
  memcpy(&v, val + 72, 8);
  CHECK(r == 0 && r3 == 0 && v == 50, "ELEM_WRITE 50 / READ -> %ld", (long)v);
  v = 100;
  memcpy(val + 72, &v, 8);
  memcpy(val + 80, &v, 8);
  CHECK(ioctl(ctl, CTL_ELEM_WRITE, val) == 0, "ELEM_WRITE 100 (restore)");

  /* P1 */
  int fd = open("/dev/snd/pcmC0D0p", O_RDWR | O_CLOEXEC);
  CHECK(fd >= 0, "open pcmC0D0p fd=%d", fd);
  if (fd < 0) { printf("[LP] done rc=%d\n", ++fails); return fails; }
  int fd2 = open("/dev/snd/pcmC0D0p", O_RDWR);
  CHECK(fd2 < 0 && errno == EBUSY, "second open -> EBUSY (fd=%d errno=%d)", fd2, errno);
  ver = 0;
  CHECK(ioctl(fd, PCM_PVERSION, &ver) == 0 && (ver >> 16) == 2, "pcm PVERSION %#x", ver);
  memset(pi, 0xee, sizeof pi);
  CHECK(ioctl(fd, PCM_INFO, pi) == 0 && pi[8] == 0 && pi[12] == 0, "pcm INFO stream=%u card=%u name='%s'", pi[8], pi[12], pi + 80);
  int on = 1; /* MONOTONIC */
  CHECK(ioctl(fd, PCM_TTSTAMP, &on) == 0, "TTSTAMP monotonic");
  int upv = 0x0002000f;
  CHECK(ioctl(fd, PCM_USER_PVERSION, &upv) == 0, "USER_PVERSION");

  /* P2 */
  void *mp = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0x80000000L);
  CHECK(mp == MAP_FAILED, "mmap(status page) refused (errno=%d) -> SYNC_PTR path", errno);
  if (mp != MAP_FAILED) munmap(mp, 4096);

  /* P3 */
  struct hwp hp;
  any(&hp);
  r = ioctl(fd, PCM_HW_REFINE, &hp);
  CHECK(r == 0 && IV(&hp, 11)->min == RATE && IV(&hp, 11)->max == RATE && IV(&hp, 10)->min == CH &&
            hp.masks[1].b[0] == 4 && hp.masks[0].b[0] == 8,
        "HW_REFINE(any): rate %u..%u ch %u..%u fmt %#x access %#x period %u..%u buffer %u..%u",
        IV(&hp, 11)->min, IV(&hp, 11)->max, IV(&hp, 10)->min, IV(&hp, 10)->max, hp.masks[1].b[0],
        hp.masks[0].b[0], IV(&hp, 13)->min, IV(&hp, 13)->max, IV(&hp, 17)->min, IV(&hp, 17)->max);
  any(&hp);
  memset(&hp.masks[0], 0, 32);
  hp.masks[0].b[0] = 1u << 3; /* RW_INTERLEAVED */
  memset(&hp.masks[1], 0, 32);
  hp.masks[1].b[0] = 1u << 2; /* S16_LE */
  fix(&hp, 10, CH);
  fix(&hp, 11, RATE);
  fix(&hp, 13, 480);
  fix(&hp, 15, 4);
  r = ioctl(fd, PCM_HW_PARAMS, &hp);
  CHECK(r == 0 && IV(&hp, 17)->min == 1920 && IV(&hp, 17)->max == 1920, "HW_PARAMS 480x4 -> buffer %u period_time %u",
        IV(&hp, 17)->min, IV(&hp, 12)->min);
  const uint64_t period = 480, buffer = 1920;

  /* P4 */
  struct swp sw;
  memset(&sw, 0, sizeof sw);
  sw.avail_min = period;
  sw.start_thr = buffer / 2;
  sw.stop_thr = buffer;
  sw.boundary = buffer;
  while (sw.boundary * 2 <= 0x7fffffffffffffffull - buffer) sw.boundary *= 2;
  sw.proto = 0x0002000f;
  uint64_t bd = sw.boundary;
  r = ioctl(fd, PCM_SW_PARAMS, &sw);
  CHECK(r == 0 && sw.boundary == bd, "SW_PARAMS boundary=%#llx", (unsigned long long)sw.boundary);
  struct status st;
  CHECK(ioctl(fd, PCM_PREPARE) == 0 && pstatus(fd, &st) == 0 && st.state == S_PREPARED &&
            st.avail == buffer, "PREPARE -> state %d avail %llu", st.state, (unsigned long long)st.avail);
  r = ioctl(fd, PCM_START);
  CHECK(r < 0 && errno == EPIPE, "START with an empty buffer -> EPIPE (r=%d errno=%d)", r, errno);

  /* P5: 2 s of tone */
  uint64_t hw_last = 0;
  int mono = 1;
  double t0 = now_s();
  long w = play(fd, RATE / 4, &hw_last, &mono); /* first 250 ms: auto-start at half a buffer */
  CHECK(w == RATE / 4 && pstatus(fd, &st) == 0 && st.state == S_RUNNING, "auto-start at start_threshold: state %d hw %llu",
        st.state, (unsigned long long)st.hw);
  struct pollfd pf = {fd, POLLOUT, 0};
  /* fill until avail < avail_min, then poll must wake for space */
  {
    static int16_t c[480 * CH];
    make_tone(c, 480);
    while (pstatus(fd, &st) == 0 && st.avail >= period) {
      int k = writei(fd, c, period);
      if (k <= 0) break;
      w += k;
      make_tone(c, 480);
    }
    double tp = now_s();
    int pr = poll(&pf, 1, 1000);
    CHECK(pr == 1 && (pf.revents & POLLOUT), "poll(POLLOUT) woke after %.1f ms (revents %#x)", (now_s() - tp) * 1e3,
          pf.revents);
  }
  long w2 = play(fd, 2 * RATE - w, &hw_last, &mono);
  CHECK(w2 == 2 * RATE - w, "blocking WRITEI wrote 2.0 s total (%ld)", w + (w2 > 0 ? w2 : 0));
  double t1 = now_s();
  CHECK(mono, "hw_ptr monotonic during playback (last %llu)", (unsigned long long)hw_last);
  struct syncptr sp;
  memset(&sp, 0, sizeof sp);
  sp.flags = 1 | 2 | 4; /* HWSYNC | APPL | AVAIL_MIN: read only */
  r = ioctl(fd, PCM_SYNC_PTR, &sp);
  pstatus(fd, &st);
  CHECK(r == 0 && sp.c.c.appl == st.appl && sp.s.s.state == S_RUNNING && sp.c.c.appl - sp.s.s.hw <= buffer,
        "SYNC_PTR state %d appl %llu hw %llu (status appl %llu)", sp.s.s.state, (unsigned long long)sp.c.c.appl,
        (unsigned long long)sp.s.s.hw, (unsigned long long)st.appl);
  int64_t delay = -1;
  CHECK(ioctl(fd, PCM_DELAY, &delay) == 0 && delay >= 0 && delay <= (int64_t)buffer, "DELAY %lld frames", (long long)delay);

  /* P6 */
  r = ioctl(fd, PCM_DRAIN);
  double t2 = now_s();
  pstatus(fd, &st);
  CHECK(r == 0 && st.state == S_SETUP && st.hw == st.appl && st.appl == 2 * RATE,
        "DRAIN -> state %d hw %llu appl %llu", st.state, (unsigned long long)st.hw, (unsigned long long)st.appl);
  double secs = t2 - t0;
  CHECK(secs > 1.85 && secs < 3.0, "real-time pacing: 2.0 s of frames took %.3f s (write %.3f s, drain %.3f s)", secs,
        t1 - t0, t2 - t1);

  /* P7: underrun */
  CHECK(ioctl(fd, PCM_PREPARE) == 0, "PREPARE after drain");
  w = play(fd, 2 * period, 0, 0);
  pstatus(fd, &st);
  int started = st.state == S_RUNNING;
  msleep(150);
  pstatus(fd, &st);
  CHECK(started && st.state == S_XRUN, "stop writing -> XRUN (state %d, hw %llu appl %llu)", st.state,
        (unsigned long long)st.hw, (unsigned long long)st.appl);
  {
    static int16_t c[480 * CH];
    make_tone(c, 480);
    int k = writei(fd, c, 480);
    CHECK(k == -EPIPE, "WRITEI in XRUN -> EPIPE (%d)", k);
  }
  CHECK(ioctl(fd, PCM_PREPARE) == 0 && pstatus(fd, &st) == 0 && st.state == S_PREPARED, "PREPARE recovers (state %d)",
        st.state);
  w = play(fd, RATE / 2, 0, 0);
  CHECK(w == RATE / 2 && ioctl(fd, PCM_DRAIN) == 0, "0.5 s more tone after recovery, drained");

  /* P8 */
  CHECK(ioctl(fd, PCM_PREPARE) == 0, "PREPARE");
  int fl = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  {
    static int16_t c[RATE * CH / 10];
    memset(c, 0, sizeof c); /* silence: the WAV keeps the tone segments clean */
    long tot = 0;
    int k;
    while ((k = writei(fd, c, RATE / 10)) > 0) tot += k;
    pstatus(fd, &st);
    CHECK(k == -EAGAIN && st.appl - st.hw <= buffer, "O_NONBLOCK: %ld frames then EAGAIN (state %d)", tot, st.state);
  }
  CHECK(ioctl(fd, PCM_PAUSE, 1) == 0 && pstatus(fd, &st) == 0 && st.state == S_PAUSED, "PAUSE(1) -> state %d", st.state);
  CHECK(ioctl(fd, PCM_PAUSE, 0) == 0 && pstatus(fd, &st) == 0 && st.state == S_RUNNING, "PAUSE(0) -> state %d", st.state);
  CHECK(ioctl(fd, PCM_DROP) == 0 && pstatus(fd, &st) == 0 && st.state == S_SETUP, "DROP -> state %d", st.state);
  fcntl(fd, F_SETFL, fl);
  CHECK(ioctl(fd, PCM_HW_FREE) == 0 && pstatus(fd, &st) == 0 && st.state == S_OPEN, "HW_FREE -> state %d", st.state);
  close(fd);
  fd = open("/dev/snd/pcmC0D0p", O_RDWR);
  CHECK(fd >= 0, "re-open after close (not EBUSY)");
  if (fd >= 0) close(fd);
  close(ctl);
  ROW("tone: 1000 Hz, 2.0 s + 0.5 s at -6.2 dBFS (16000/32767) -> tools/cp/audio/wavcheck.py");
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
