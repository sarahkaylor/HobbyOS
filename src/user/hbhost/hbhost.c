/*
 * HBHOST.BIN - run one native HobbyOS app inside a real X11 window
 * (icewm.md §9).
 *
 *   HBHOST.BIN <PROGRAM.BIN> [args...]
 *
 * Native apps speak the default desktop's window protocol (window.h) on
 * stdin/stdout and, for pixel apps and games, paint a mapped framebuffer
 * and read raw input.  Under IceWM the X server owns the real framebuffer
 * and the kernel input ring, so HBHOST:
 *
 *   1. creates a memfd the size of the screen and ATTACHES it as a virtual
 *      display (SYS_VDISPLAY): the app it spawns inherits the attachment, so
 *      its SYS_MAP_FB maps that private surface, its flushes only record
 *      damage, and its SYS_GET_EVENTS/SYS_INPUT_READ read a private queue;
 *   2. plays the desktop for that one window (hbhost_desk.c compiles the
 *      desktop's own code): text tail, terminal mode, pixel mode, menus,
 *      title, close request, mouse/keys in the desktop's exact encodings;
 *   3. blits the surface into its X window (XPutImage of the damaged rect);
 *      a "fullscreen" game (maps + flushes the fb without ESC ] X) is shown
 *      whole, scaled to the window, and gets raw key events (PS/2-style
 *      EV_KEY press/release) injected into the private queue when it reads
 *      that queue -- otherwise keys go to stdin like the desktop sends them.
 *
 * The child's exit (stdout EOF) closes the window; WM_DELETE_WINDOW sends a
 * pixel/game app ESC [ D ~ and kills it after the desktop's 250 ms grace (a
 * text app is killed at once, like the desktop's X button).
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "display_mode.h"
#include "hbhost.h"

/* Native HobbyOS calls (src/user_include/libc.h; declared here to keep the
 * device header's select()/print() names away from the X headers). */
int spawn2(const char *filename, int stdin_fd, int stdout_fd, int stderr_fd, const char *args);
int available(int fd);
int sysinfo(int cmd, void *buf, int size);

#define SURF_W DISPLAY_WIDTH
#define SURF_H DISPLAY_HEIGHT
#define SURF_BYTES ((size_t)DISPLAY_STRIDE_BYTES * DISPLAY_HEIGHT)

#define TEXT_W 800
#define TEXT_H 560
#define GAME_W 960
#define GAME_H 540
#define CLOSE_GRACE_MS 250
#define MENU_W 100
#define MENU_ITEM_H 20

static Display *dpy;
static Window win;
static GC gc;
static XImage *surf_img;     /* XImage over the virtual display (no copy) */
static unsigned *surf;
static int win_w = TEXT_W, win_h = TEXT_H;
static Atom wm_delete, wm_protocols, net_wm_name, utf8_string;

static int child_pid = -1;
static int child_out = -1, child_in = -1;
static int game_mode = 0;
static unsigned *game_buf;   /* scaled frame for game mode */
static XImage *game_img;
static int game_w, game_h;
static unsigned last_seq = 0;
static int first_flush_ms = -1;
static int flushes_seen = 0;
static unsigned host_maps = 0;

static int menu_open = -1, menu_x0 = 0;
static int close_ms = -1;     /* ms when the close was requested */
static char last_title[64];
static const char *prog_name = "HBHOST";

static int now_ms(void) { return sysinfo(1, 0, 0); }

static void logmsg(const char *a, const char *b) {
  fprintf(stderr, "[HBHOST] %s%s\n", a, b ? b : "");
}

/* ---- L84: blit accounting ------------------------------------------------
 * "the window never repaints" is four different bugs and only one of them is
 * in the app: (a) the app never flushes, (b) it flushes but the surface
 * damage is empty or already consumed, (c) HBHOST decides to blit and clips
 * the rect to nothing, (d) HBHOST blits and the X window is showing something
 * else.  Every decision below bumps a counter and the counters reach the
 * serial log, so the pixel evidence can be read against a row instead of a
 * guess.  Row classes:
 *
 *   [HB]  one line per decision that put pixels (rate-limited) -- the kind of
 *         blit, its rect, and the state that produced it.
 *   [HBS] one line every 2 s with the totals; a steady state with
 *         blit advancing and dmg=0 is the app presenting into its own shadow
 *         without telling anyone, and blit=0 with flush advancing is HBHOST
 *         dropping the damage.
 *
 * L88 adds the present *budget*, because "HBHOST is the bottleneck" has to be
 * a measurement:
 *
 *   [HBT]  one row per HBT_EVERY blits (and the first four): the rect and the
 *          time inside XPutImage (and inside the sampled checksum, when this
 *          blit took one).
 *   [HBTS] every 2 s: blits, blits/s, mean and max XPutImage time, Mpx/s and
 *          `busy` -- the permille of the window spent inside XPutImage.  A
 *          busy near 0 says the host's blit is not the frame; busy 500 says
 *          it is half of it.  `copy_us` is a reference memcpy of the same
 *          rect in the same process: XPutImage minus copy is the wire to
 *          XHOBBY plus what the server does with it.
 *
 * The guest's CLOCK_MONOTONIC ticks at 1 ms (rule 33), so the timing base is
 * the TSC, calibrated against sysinfo(1) once per process -- the clock the
 * kernel's own timer_get_us() divides down.  Everything prints in us.
 */
#define HBT_EVERY 16

static inline unsigned long long tsc_now(void) {
  unsigned lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)hi << 32) | lo;
}

struct hbt_stats {
  int t0_ms;
  unsigned long long tsc0;
  unsigned long long cpm;      /* cycles per ms; 0 until calibrated */
  int tsc_dead;
  unsigned long long blits, px;
  unsigned long long put_cyc, put_cyc_max, crc_cyc;
  /* this window */
  int w_ms;
  unsigned long long w_tsc;
  unsigned long long w_blits, w_px, w_put_cyc, w_put_cyc_max, w_crc_cyc;
  /* static-frame probe */
  unsigned probes;
  int have_probe_crc, last_probe_ms;
  unsigned long long probe_crc;
};
static struct hbt_stats hbt;

static unsigned long long cyc_us(unsigned long long c) {
  return hbt.cpm ? (c * 1000ull) / hbt.cpm : 0;
}

static void hbt_calibrate(int t) {
  if (hbt.cpm || hbt.tsc_dead) return;
  int d = t - hbt.t0_ms;
  if (d < 1000) return;
  unsigned long long c = tsc_now() - hbt.tsc0;
  if (c < 1000000ull) {
    hbt.tsc_dead = 1;
    fprintf(stderr, "[HBT] clock: rdtsc reads %llu over %d ms -- ms timing only\n",
            c, d);
    return;
  }
  hbt.cpm = c / (unsigned long long)d;
  fprintf(stderr, "[HBT] clock cyc_per_ms=%llu (%llu MHz) over %d ms\n",
          hbt.cpm, hbt.cpm / 1000ull, d);
}

/* One reference copy of `w x h` from the surface into a scratch buffer: the
 * guest's own bandwidth for exactly this rect, so `put_us - copy_us` is what
 * the X connection costs on top of the copy. */
static unsigned *hbt_scratch;
static unsigned long long hbt_copy_ref(int w, int h) {
  if (!hbt_scratch) {
    hbt_scratch = (unsigned *)malloc((size_t)SURF_W * SURF_H * 4);
    if (!hbt_scratch) return 0;
  }
  if (w <= 0 || h <= 0 || w > SURF_W || h > SURF_H) return 0;
  unsigned long long c0 = tsc_now();
  for (int r = 0; r < h; r++) {
    unsigned *d = hbt_scratch + (size_t)r * w;
    const unsigned *s = surf + (size_t)r * SURF_W;
    for (int i = 0; i < w; i++) d[i] = s[i];
  }
  return tsc_now() - c0;
}

struct hb_stats {
  unsigned vd, flush, dmg, restamp, paint;
  unsigned blit, blit_empty, px;
  unsigned b_full, b_dmg, b_restamp, b_paint;
  unsigned game_enter;
  int last_row_ms;
};
static struct hb_stats hb;

static unsigned long long surf_crc(int x, int y, int w, int h, unsigned *nz);

static void hb_summary(int t) {
  fprintf(stderr, "[HBS] t=%d vd=%u flush=%u dmg=%u restamp=%u paint=%u "
                  "blit=%u empty=%u px=%u full=%u dmgblit=%u restampblit=%u "
                  "paintblit=%u game=%d pix=%d win=%dx%d\n",
          t, hb.vd, hb.flush, hb.dmg, hb.restamp, hb.paint, hb.blit,
          hb.blit_empty, hb.px, hb.b_full, hb.b_dmg, hb.b_restamp,
          hb.b_paint, game_mode, hd_pixel_mode(), win_w, win_h);
}

static void hbt_summary(int t) {
  hbt_calibrate(t);
  int dms = t - hbt.w_ms;
  unsigned long long wall = hbt.w_tsc ? (tsc_now() - hbt.w_tsc) : 0;
  unsigned long long busy = wall ? (1000ull * hbt.w_put_cyc) / wall : 0;
  unsigned long long bps = dms > 0 ? (hbt.w_blits * 1000ull) / (unsigned)dms : 0;
  unsigned long long mpx_s = dms > 0 ? (hbt.w_px / 1000ull) * 1000ull /
                                      (unsigned long long)dms : 0;
  unsigned long long put_avg = hbt.w_blits ? hbt.w_put_cyc / hbt.w_blits : 0;
  int cx, cy, cw, ch;
  unsigned long long copy = 0;
  hd_content_rect(&cx, &cy, &cw, &ch);
  if (cw > 0 && ch > 0 && cw <= SURF_W && ch <= SURF_H) copy = hbt_copy_ref(cw, ch);
  fprintf(stderr, "[HBTS] t=%d dms=%d blits=%llu bps=%llu put_avg_us=%llu "
                  "put_max_us=%llu crc_avg_us=%llu copy_us=%llu px=%llu "
                  "Mpx_s=%llu busy_permille=%llu full_px=%d cpm=%llu\n",
          t, dms, hbt.w_blits, bps, cyc_us(put_avg), cyc_us(hbt.w_put_cyc_max),
          hbt.w_blits ? cyc_us(hbt.w_crc_cyc / hbt.w_blits) : 0,
          cyc_us(copy), hbt.w_px, mpx_s, busy, cw * ch, hbt.cpm);
  hbt.w_ms = t;
  hbt.w_tsc = tsc_now();
  hbt.w_blits = hbt.w_px = hbt.w_put_cyc = hbt.w_put_cyc_max = hbt.w_crc_cyc = 0;
}

/* A marker file, the same idiom as /XPROF: /HBTPROBE content "1" or "on"
 * turns the static-frame probe on. */
static int probe_on(void) {
  static int on = -1;
  if (on < 0) {
    char b[8];
    int n = 0;
    int fd = open("/HBTPROBE", O_RDONLY);
    on = 0;
    if (fd >= 0) {
      n = (int)read(fd, b, sizeof b - 1);
      close(fd);
      if (n > 0 && (b[0] == '1' || b[0] == 'o')) on = 1;
    }
  }
  return on;
}

static int probe_dump(const char *path, const unsigned char *base, int stride,
                      int w, int h) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  for (int y = 0; y < h; y++) {
    const char *p = (const char *)base + (size_t)y * (size_t)stride;
    unsigned long n = (unsigned long)w * 4, off = 0;
    while (off < n) {
      int k = (int)write(fd, p + off, n - off);
      if (k <= 0) { close(fd); return -1; }
      off += (unsigned long)k;
    }
  }
  close(fd);
  return 0;
}

/* Static-frame record: what the surface holds and what the X server shows for
 * the same rect, compared in-guest and written out for a cross-build compare.
 * "The optimisation did not change the image" is then two numbers: the diff
 * inside one build, and the byte compare of the two builds' dumps. */
static void hbt_probe(int t) {
  /* Only the game's own window: the text-mode window (800x560) is not the
   * thing under test and would consume both probes before the game starts. */
  if (hbt.probes >= 6 || !probe_on() || !hd_pixel_mode()) return;
  if (hbt.last_probe_ms && t - hbt.last_probe_ms < 150) return;
  int cx, cy, cw, ch;
  hd_content_rect(&cx, &cy, &cw, &ch);
  if ((long)cw * (long)ch < 400000) return;   /* not the game's content yet */
  unsigned nz = 0;
  unsigned long long crc = surf_crc(cx, cy, cw, ch, &nz);
  /* `nz` counts only the rows the checksum samples (every 8th), so compare it
   * with the sampled maximum, not with the rect's pixel count. */
  if (nz < (unsigned)((long)cw * (ch / 8 + 1) / 2)) {
    /* A black or nearly-black frame is not the image under test. */
    hbt.last_probe_ms = t;
    return;
  }
  if (!hbt.have_probe_crc || crc != hbt.probe_crc) {
    /* Not static yet: remember it and look again in a few seconds. */
    hbt.probe_crc = crc;
    hbt.have_probe_crc = 1;
    hbt.last_probe_ms = t;
    return;
  }
  XImage *im = XGetImage(dpy, win, cx, cy, (unsigned)cw, (unsigned)ch,
                         AllPlanes, ZPixmap);
  if (!im) {
    fprintf(stderr, "[HBT] probe: XGetImage %dx%d failed\n", cw, ch);
    return;
  }
  long diff = 0;
  for (int y = 0; y < ch; y++) {
    const unsigned *a = (const unsigned *)(im->data + (size_t)y * im->bytes_per_line);
    const unsigned *b = surf + (size_t)(cy + y) * SURF_W + cx;
    for (int x = 0; x < cw; x++)
      if (((a[x] ^ b[x]) & 0xFFFFFFu) != 0) diff++;
  }
  hbt.probes++;
  char ps[32], pw[32];
  snprintf(ps, sizeof ps, "/HBTSURF%u.raw", hbt.probes);
  snprintf(pw, sizeof pw, "/HBTWIN%u.raw", hbt.probes);
  int es = probe_dump(ps, (const unsigned char *)(surf + (size_t)cy * SURF_W + cx),
                      SURF_W * 4, cw, ch);
  int ew = probe_dump(pw, (const unsigned char *)im->data, im->bytes_per_line,
                      cw, ch);
  fprintf(stderr, "[HBT] probe n=%u t=%d rect=%d,%d,%dx%d px=%d "
                  "window_vs_surface_diff=%ld crc=0x%llx surf=%s(%d) win=%s(%d)\n",
          hbt.probes, t, cx, cy, cw, ch, cw * ch, diff, crc, ps, es, pw, ew);
  im->data = 0;
  XDestroyImage(im);
  hbt.last_probe_ms = t;
}

/* Sampled checksum of a surface rect: every 8th row, FNV-1a over the pixels.
 * Cheap enough to run on a rate-limited row, and it is what separates "we
 * blitted" from "we blitted something new": an unchanged window shows the
 * same crc every row, the app's own frames do not. */
static unsigned long long surf_crc(int x, int y, int w, int h, unsigned *nz) {
  unsigned long long c = 1469598103934665603ull;
  unsigned n = 0;
  int step = h > 8 ? 8 : 1;
  for (int r = y; r < y + h; r += step) {
    const unsigned *p = surf + (size_t)r * SURF_W + x;
    for (int i = 0; i < w; i++) {
      unsigned v = p[i];
      if (v & 0xFFFFFF) n++;
      c = (c ^ (v & 0xFF)) * 1099511628211ull;
      c = (c ^ ((v >> 8) & 0xFF)) * 1099511628211ull;
      c = (c ^ ((v >> 16) & 0xFF)) * 1099511628211ull;
    }
  }
  if (nz) *nz = n;
  return c;
}

/* ---- blits ---- */
static void blit_rect(const char *kind, int x, int y, int w, int h) {
  if (game_mode) return;
  int rw = w, rh = h;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > win_w) w = win_w - x;
  if (y + h > win_h) h = win_h - y;
  if (w <= 0 || h <= 0) {
    hb.blit_empty++;
    fprintf(stderr, "[HB] blit kind=%s DROPPED rect=%d,%d,%d,%d win=%dx%d\n",
            kind, x, y, rw, rh, win_w, win_h);
    return;
  }
  /* Rate-limit the decision rows: the first three, then one a second.  A
   * pixel defect that needs frame-by-frame rows is a different lane; what has
   * to be provable here is that the decision is taken on changing pixels. */
  int t = now_ms();
  int row = (t - hb.last_row_ms >= 1000 || hb.blit <= 3);
  unsigned long long crc = 0;
  unsigned nz = 0;
  unsigned long long c0 = tsc_now();
  if (row) crc = surf_crc(x, y, w, h, &nz);
  unsigned long long c1 = tsc_now();
  XPutImage(dpy, win, gc, surf_img, x, y, x, y, (unsigned)w, (unsigned)h);
  unsigned long long c2 = tsc_now();
  hbt.blits++;
  hbt.w_blits++;
  hbt.put_cyc += c2 - c1;
  hbt.w_put_cyc += c2 - c1;
  hbt.crc_cyc += c1 - c0;
  hbt.w_crc_cyc += c1 - c0;
  if (c2 - c1 > hbt.put_cyc_max) hbt.put_cyc_max = c2 - c1;
  if (c2 - c1 > hbt.w_put_cyc_max) hbt.w_put_cyc_max = c2 - c1;
  hbt.px += (unsigned long long)w * (unsigned long long)h;
  hbt.w_px += (unsigned long long)w * (unsigned long long)h;
  hb.blit++;
  hb.px += (unsigned)w * (unsigned)h;
  if (kind[0] == 'f') hb.b_full++;
  else if (kind[0] == 'd') hb.b_dmg++;
  else if (kind[0] == 'r') hb.b_restamp++;
  else hb.b_paint++;
  if (hbt.blits <= 4 || (hbt.blits % HBT_EVERY) == 0) {
    fprintf(stderr, "[HBT] n=%llu kind=%s rect=%d,%d,%d,%d px=%d put_us=%llu "
                    "crc_us=%llu win=%dx%d\n",
            hbt.blits, kind, x, y, w, h, w * h, cyc_us(c2 - c1),
            cyc_us(c1 - c0), win_w, win_h);
  }
  if (row) {
    hb.last_row_ms = t;
    fprintf(stderr, "[HB] blit kind=%s rect=%d,%d,%d,%d win=%dx%d pix=%d "
                    "game=%d flush=%u dmg=%u restamp=%u crc=0x%llx nz=%u\n",
            kind, x, y, w, h, win_w, win_h, hd_pixel_mode(), game_mode,
            hb.flush, hb.dmg, hb.restamp, crc, nz);
  }
}

static void draw_menu(void);

static void blit_game(void) {
  if (!game_buf || game_w != win_w || game_h != win_h) {
    if (game_img) { game_img->data = 0; XDestroyImage(game_img); game_img = 0; }
    free(game_buf);
    game_w = win_w; game_h = win_h;
    game_buf = (unsigned *)malloc((size_t)game_w * game_h * 4);
    if (!game_buf) return;
    game_img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24, ZPixmap, 0,
                            (char *)game_buf, game_w, game_h, 32, game_w * 4);
  }
  /* Aspect-preserving nearest-neighbour fit, letterboxed. */
  int sw = game_w, sh = game_w * SURF_H / SURF_W;
  if (sh > game_h) { sh = game_h; sw = game_h * SURF_W / SURF_H; }
  int ox = (game_w - sw) / 2, oy = (game_h - sh) / 2;
  for (int y = 0; y < game_h; y++) {
    unsigned *d = game_buf + (size_t)y * game_w;
    if (y < oy || y >= oy + sh) { memset(d, 0, (size_t)game_w * 4); continue; }
    const unsigned *s = surf + (size_t)((y - oy) * SURF_H / sh) * SURF_W;
    for (int x = 0; x < game_w; x++) {
      if (x < ox || x >= ox + sw) d[x] = 0;
      else d[x] = s[(x - ox) * SURF_W / sw];
    }
  }
  XPutImage(dpy, win, gc, game_img, 0, 0, 0, 0, game_w, game_h);
}

static void redraw_all(void) {
  if (game_mode) blit_game();
  else blit_rect("full", 0, 0, win_w, win_h);
  draw_menu();
}

/* ---- window menus: a dropdown drawn with Xlib over the surface blit ---- */
static void draw_menu(void) {
  if (menu_open < 0) return;
  int n = hd_menu_items(menu_open);
  XSetForeground(dpy, gc, 0xFFFFFF);
  XFillRectangle(dpy, win, gc, menu_x0, HD_MENUBAR_H, MENU_W, n * MENU_ITEM_H);
  XSetForeground(dpy, gc, 0x808080);
  XDrawRectangle(dpy, win, gc, menu_x0, HD_MENUBAR_H, MENU_W - 1, n * MENU_ITEM_H - 1);
  XSetForeground(dpy, gc, 0x202020);
  for (int i = 0; i < n; i++) {
    const char *s = hd_menu_item(menu_open, i);
    XDrawString(dpy, win, gc, menu_x0 + 5, HD_MENUBAR_H + i * MENU_ITEM_H + 14, s, (int)strlen(s));
  }
}

static void close_menu(void) {
  if (menu_open < 0) return;
  int n = hd_menu_items(menu_open);
  menu_open = -1;
  blit_rect("menu", menu_x0, HD_MENUBAR_H, MENU_W, n * MENU_ITEM_H);
}

/* ---- title ---- */
static void set_title(const char *t) {
  if (!t || !t[0]) return;
  if (strncmp(t, last_title, sizeof last_title) == 0) return;
  strncpy(last_title, t, sizeof last_title - 1);
  last_title[sizeof last_title - 1] = '\0';
  XStoreName(dpy, win, last_title);
  XSetIconName(dpy, win, last_title);
  XChangeProperty(dpy, win, net_wm_name, utf8_string, 8, PropModeReplace,
                  (const unsigned char *)last_title, (int)strlen(last_title));
}

/* Absolute position of the client area on the X screen -- evidence needs the
 * window rect, and the WM (not HBHOST) owns the frame offset. */
static void log_geometry(void) {
  Window r, child;
  int rx = 0, ry = 0;
  if (XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child))
    fprintf(stderr, "[HB] geometry client=%d,%d,%dx%d screen=%dx%d\n",
            rx, ry, win_w, win_h, DisplayWidth(dpy, DefaultScreen(dpy)),
            DisplayHeight(dpy, DefaultScreen(dpy)));
}

static void resize_window(int w, int h) {
  if (w == win_w && h == win_h) return;
  XResizeWindow(dpy, win, (unsigned)w, (unsigned)h);
}

/* ---- ESC ] R: open the requested program in another HBHOST window ---- */
static int run_pids[8];
static int n_run_pids = 0;

void hbhost_run_request(const char *bin, const char *args) {
  char a[200];
  snprintf(a, sizeof a, "%s%s%s", bin, (args && args[0]) ? " " : "", args ? args : "");
  int pid = spawn2("HBHOST.BIN", 0, 1, 2, a);
  logmsg("run request: ", a);
  if (pid > 0 && n_run_pids < 8) run_pids[n_run_pids++] = pid;
}

/* Reap only our OWN children by pid (the app and run-request hosts): a
 * waitpid(-1) here raced the app's descendants' waits (see icewm.md §9). */
static void reap_run_children(void) {
  int wst;
  for (int i = 0; i < n_run_pids; i++) {
    if (waitpid(run_pids[i], &wst, WNOHANG) > 0) {
      run_pids[i] = run_pids[--n_run_pids];
      i--;
    }
  }
}

/* ---- close ---- */
static void request_close(void) {
  if (close_ms >= 0) return;
  close_ms = now_ms();
  if (hd_pixel_mode() || game_mode) {
    hd_send_close();           /* ESC [ D ~, then the grace */
  } else {
    kill(child_pid, SIGKILL);  /* text windows: immediate, like the desktop */
  }
}

/* ---- X input ---- */
/* Does the app read its private raw queue (SYS_GET_EVENTS/SYS_INPUT_READ)
 * at all?  Sticky once seen: games poll it every frame. */
static int raw_seen = 0;
static void note_raw_reader(const struct hd_vstatus *st) {
  if (st->reads > 0) raw_seen = 1;
}

/* Keys.  Under the default desktop a game's raw read and the desktop's own
 * read race for each kernel input event, and the desktop "usually wins"
 * (every native game drains stdin for exactly that reason), so presses go
 * to stdin in the desktop's exact encoding.  What stdin cannot carry goes
 * to the private raw queue when the app reads it: key RELEASES (DOOM's
 * key-up; the other games ignore value 0) and presses with no byte mapping
 * (Ctrl/Alt/Shift -- DOOM fires with Ctrl).  So every key reaches a game
 * exactly once per edge. */
static void handle_key(XKeyEvent *ke, int press) {
  int code = (int)ke->keycode - 8;   /* XHOBBY: X keycode = evdev code + 8 */
  if (code <= 0 || code >= 0x100) return;
  if (press && code == 62) {         /* F4: the desktop's close key */
    request_close();
    return;
  }
  if (!press) {
    if (raw_seen) hd_vd_inject_key(code, 0);
    return;
  }
  unsigned s = ke->state;
  int sent = hd_key_press(code, (s & ShiftMask) != 0, (s & ControlMask) != 0,
                          (s & Mod1Mask) != 0, (s & LockMask) != 0);
  if (!sent && raw_seen) hd_vd_inject_key(code, 1);
}

static void handle_button(XButtonEvent *be, int press) {
  int x = be->x, y = be->y;
  if (game_mode) return;
  if (be->button == 4 || be->button == 5) {
    if (press) hd_wheel(x, y, be->button == 4 ? 1 : -1);
    return;
  }
  if (be->button != 1) return;
  if (press) {
    if (menu_open >= 0) {
      int n = hd_menu_items(menu_open);
      if (x >= menu_x0 && x < menu_x0 + MENU_W && y >= HD_MENUBAR_H &&
          y < HD_MENUBAR_H + n * MENU_ITEM_H) {
        int m = menu_open;
        close_menu();
        hd_menu_select(m, (y - HD_MENUBAR_H) / MENU_ITEM_H);
      } else {
        close_menu();
      }
      return;
    }
    int mx;
    int m = hd_menu_hit(x, y, &mx);
    if (m >= 0) {
      menu_open = m;
      menu_x0 = mx;
      draw_menu();
      return;
    }
    hd_press(x, y);
  } else {
    hd_release(x, y);
  }
}

static int handle_x_event(XEvent *ev) {
  switch (ev->type) {
  case Expose:
    if (game_mode) blit_game();
    else blit_rect("expose", ev->xexpose.x, ev->xexpose.y, ev->xexpose.width, ev->xexpose.height);
    draw_menu();
    break;
  case ConfigureNotify:
    if (ev->xconfigure.width != win_w || ev->xconfigure.height != win_h) {
      int ow = win_w, oh = win_h;
      win_w = ev->xconfigure.width;
      win_h = ev->xconfigure.height;
      fprintf(stderr, "[HB] configure %dx%d -> %dx%d pix=%d game=%d\n",
              ow, oh, win_w, win_h, hd_pixel_mode(), game_mode);
      if (!game_mode) hd_set_size(win_w, win_h);
      else blit_game();
      if (win_w > 200 && win_h > 200) log_geometry();
    }
    break;
  case KeyPress: handle_key(&ev->xkey, 1); break;
  case KeyRelease: handle_key(&ev->xkey, 0); break;
  case ButtonPress: handle_button(&ev->xbutton, 1); break;
  case ButtonRelease: handle_button(&ev->xbutton, 0); break;
  case MotionNotify:
    if (!game_mode) {
      hd_motion(ev->xmotion.x, ev->xmotion.y);
      hd_hover(ev->xmotion.x, ev->xmotion.y, now_ms());
    }
    break;
  case ClientMessage:
    if (ev->xclient.message_type == wm_protocols &&
        (Atom)ev->xclient.data.l[0] == wm_delete)
      request_close();
    break;
  default: break;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: HBHOST.BIN <PROGRAM.BIN> [args...]\n");
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  const char *prog = argv[1];
  char args[240];
  args[0] = '\0';
  for (int i = 2; i < argc; i++) {
    size_t l = strlen(args);
    snprintf(args + l, sizeof args - l, "%s%s", l ? " " : "", argv[i]);
  }
  /* Title: the base name without directory or extension (the desktop's
   * app_name_from_bin). */
  static char pname[32];
  {
    const char *b = prog;
    for (const char *p = prog; *p; p++) if (*p == '/') b = p + 1;
    int i = 0;
    while (b[i] && b[i] != '.' && i < (int)sizeof pname - 1) { pname[i] = b[i]; i++; }
    pname[i] = '\0';
    prog_name = pname;
  }

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    logmsg("cannot open X display for ", prog);
    return 1;
  }

  /* 1. the virtual display */
  int mfd = memfd_create("hbhost-vdisplay", 0);
  if (mfd < 0 || ftruncate(mfd, (off_t)SURF_BYTES) != 0) {
    logmsg("memfd failed for ", prog);
    return 1;
  }
  if (hd_vd_attach(mfd) != 0) {
    logmsg("SYS_VDISPLAY attach failed for ", prog);
    return 1;
  }
  close(mfd);
  surf = hd_init();
  if (!surf) {
    logmsg("cannot map the virtual display for ", prog);
    return 1;
  }
  {
    struct hd_vstatus st;
    hd_vd_status(&st);
    host_maps = st.mapped;   /* our own SYS_MAP_FB; the app's come on top */
    last_seq = st.flush_seq;
  }

  /* 2. the X window */
  int scr = DefaultScreen(dpy);
  Visual *vis = DefaultVisual(dpy, scr);
  win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 40, 40, TEXT_W, TEXT_H, 0,
                            BlackPixel(dpy, scr), 0xF0F0F0);
  XSelectInput(dpy, win, ExposureMask | KeyPressMask | KeyReleaseMask |
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                         StructureNotifyMask);
  wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
  wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
  utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
  XSetWMProtocols(dpy, win, &wm_delete, 1);
  {
    XClassHint ch;
    ch.res_name = (char *)pname;
    ch.res_class = (char *)"HobbyOS";
    XSetClassHint(dpy, win, &ch);
  }
  gc = XCreateGC(dpy, win, 0, NULL);
  surf_img = XCreateImage(dpy, vis, 24, ZPixmap, 0, (char *)surf, SURF_W, SURF_H, 32,
                          DISPLAY_STRIDE_BYTES);
  if (!surf_img) {
    logmsg("XCreateImage failed for ", prog);
    return 1;
  }
  set_title(pname);

  /* 3. the app */
  int in_pipe[2], out_pipe[2];
  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
    logmsg("pipe failed for ", prog);
    return 1;
  }
  child_pid = spawn2(prog, in_pipe[0], out_pipe[1], -1, args[0] ? args : 0);
  close(in_pipe[0]);
  close(out_pipe[1]);
  if (child_pid < 0) {
    logmsg("cannot start ", prog);
    return 1;
  }
  child_out = out_pipe[0];
  child_in = in_pipe[1];
  hd_create(child_pid, child_out, child_in, pname);
  hd_set_size(TEXT_W, TEXT_H);
  XMapWindow(dpy, win);
  fprintf(stderr, "[HBHOST] %s pid=%d window=0x%lx\n", prog, child_pid, (unsigned long)win);

  int pref_done = 0;
  int last_hover_ms = 0;
  int last_game_blit = 0;
  int last_sum_ms = 0;
  int last_pix = 0;
  int child_gone = 0;
  int kill_sent_ms = -1;
  /* L88: the present budget's clock base.  Calibrated on the first 2 s
   * summary against sysinfo(1), so no boot-time stall. */
  hbt.t0_ms = now_ms();
  hbt.tsc0 = tsc_now();
  hbt.w_ms = hbt.t0_ms;
  hbt.w_tsc = hbt.tsc0;
  struct pollfd pfd[2];

  while (!child_gone) {
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      handle_x_event(&ev);
    }

    /* App output (bounded drain, like the desktop's). */
    int drained = 0;
    for (;;) {
      int av = available(child_out);
      if (av < 0) { if (drained == 0) child_gone = 1; break; }
      if (av == 0) break;
      char buf[512];
      int r = (int)read(child_out, buf, av > (int)sizeof buf ? (int)sizeof buf : av);
      if (r <= 0) break;
      hd_feed(buf, r);
      drained += r;
      if (drained >= 8192) break;
    }

    int t = now_ms();
    struct hd_vstatus st;
    hd_vd_status(&st);
    hb.vd++;
    note_raw_reader(&st);
    int flushed = st.flush_seq != last_seq;
    last_seq = st.flush_seq;
    if (flushed) hb.flush++;
    if (st.dw > 0) hb.dmg++;
    if (hd_pixel_mode() != last_pix) {
      last_pix = hd_pixel_mode();
      fprintf(stderr, "[HB] pixel mode -> %d t=%d win=%dx%d mapped=%u "
                      "flushed=%d menu=%d\n",
              last_pix, t, win_w, win_h, st.mapped, flushed, menu_open);
      if (last_pix) {
        /* The window shows the text tail until the app's own pixels replace
         * it, and the app only repaints what it is asked to.  Drop the tail
         * and ask for the whole content. */
        hd_pixel_enter();
        int cx, cy, cw, chh;
        hd_content_rect(&cx, &cy, &cw, &chh);
        blit_rect("clear", cx, cy, cw, chh);
      }
    }

    /* A fullscreen game: the app maps + flushes the framebuffer but never
     * asked for a pixel window (ESC ] X). */
    if (!game_mode && !hd_pixel_mode() && st.mapped > host_maps && flushed) {
      if (first_flush_ms < 0) first_flush_ms = t;
      flushes_seen++;
      if (flushes_seen >= 3 || t - first_flush_ms >= 400) {
        game_mode = 1;
        hb.game_enter++;
        close_menu();
        /* drop what the text window painted: the game repaints its own
           area every frame, the rest of the screen stays black */
        memset(surf, 0, SURF_BYTES);
        fprintf(stderr, "[HB] fullscreen fb app -> game view t=%d pix=%d "
                        "mapped=%u host_maps=%u flushes=%d win=%dx%d\n",
                t, hd_pixel_mode(), st.mapped, host_maps, flushes_seen,
                win_w, win_h);
        resize_window(GAME_W, GAME_H);
      }
    }

    if (game_mode) {
      if ((flushed || st.dw > 0) && t - last_game_blit >= 30) {
        blit_game();
        last_game_blit = t;
      }
    } else {
      if (hd_pixel_mode() && !pref_done) {
        int ww, wh;
        pref_done = 1;
        /* Content size from ESC ] X, clamped to what IceWM will actually
         * show (title bar + borders + taskbar).  Resizing the X window to the
         * raw request made the WM shrink it afterwards and the bottom of the
         * game fell off; hd_fit_pref also re-sends the app its content rect. */
        if (hd_fit_pref(&ww, &wh) && ww > 0 && wh > 0) {
          int pw, ph;
          hd_pref_size(&pw, &ph);
          fprintf(stderr, "[HB] window pref=%dx%d -> win=%dx%d\n", pw, ph, ww, wh);
          resize_window(ww, wh);
        }
      }
      int x, y, w, h;
      int painted = hd_paint(&x, &y, &w, &h);
      if (painted) {
        hb.paint++;
        blit_rect("paint", x, y, w, h);
      }
      if (hd_pixel_mode()) {
        int px, py, pw, ph;
        int restamped = hd_take_present(&px, &py, &pw, &ph);
        if (restamped) hb.restamp++;
        /* st.dw is this iteration's surface damage (the vdisplay consumes it
         * on read, so it is read exactly once per loop).  A compat X client
         * does not flush the surface at all: it writes into the mapped fb and
         * tells the desktop with ESC ] F, which lands here as a restamp. */
        if (st.dw > 0) {
          blit_rect("dmg", st.dx, st.dy, st.dw, st.dh);
        } else if (restamped) {
          int cx, cy, cw, chh;
          hd_content_rect(&cx, &cy, &cw, &chh);
          /* L88: blit the rect the app actually presented instead of the
           * whole content.  The surface is shared memory, so every pixel
           * outside that rect is byte-for-byte what the last blit put in the
           * window; the image is the same and the bytes on the X connection
           * are only the ones that changed. */
          if (pw > 0 && ph > 0) {
            int bx = cx + (px < 0 ? 0 : px), by = cy + (py < 0 ? 0 : py);
            int bw = px < 0 ? pw + px : pw, bh = py < 0 ? ph + py : ph;
            if (bx + bw > cx + cw) bw = cx + cw - bx;
            if (by + bh > cy + chh) bh = cy + chh - by;
            blit_rect("restamp", bx, by, bw, bh);
          } else {
            blit_rect("restamp", cx, cy, cw, chh);
          }
        }
        /* Pointer tracking (ESC [ T): the desktop reports the pointer
         * wherever it is; poll it so it keeps flowing outside the window. */
        if (hd_mouse_events() && t - last_hover_ms >= 40) {
          Window r, c;
          int rx, ry, wx, wy;
          unsigned mask;
          if (XQueryPointer(dpy, win, &r, &c, &rx, &ry, &wx, &wy, &mask))
            hd_hover(wx, wy, t);
          last_hover_ms = t;
        }
      }
      if (painted || st.dw > 0) draw_menu();
      set_title(hd_title());
    }

    /* Close grace (pixel/game windows), then the kill; give up waiting for
     * the pipe 2 s after the kill (a hang is a defect). */
    if (close_ms >= 0 && kill_sent_ms < 0 && t - close_ms >= CLOSE_GRACE_MS) {
      kill(child_pid, SIGKILL);
      kill_sent_ms = t;
    }
    if (kill_sent_ms >= 0 && t - kill_sent_ms > 2000) break;

    reap_run_children();
    if (t - last_sum_ms >= 2000) {
      hb_summary(t);
      hbt_summary(t);
      hbt_probe(t);
      last_sum_ms = t;
    }
    XFlush(dpy);
    pfd[0].fd = ConnectionNumber(dpy);
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd = child_out;
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;
    poll(pfd, 2, game_mode || hd_pixel_mode() ? 10 : 20);
  }

  fprintf(stderr, "[HBHOST] %s exited, closing window\n", prog);
  XDestroyWindow(dpy, win);
  XFlush(dpy);
  surf_img->data = 0;
  XDestroyImage(surf_img);
  XCloseDisplay(dpy);
  hd_vd_detach();
  {
    int wst;
    waitpid(child_pid, &wst, WNOHANG);
  }
  return 0;
}
