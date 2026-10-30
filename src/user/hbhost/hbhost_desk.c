/*
 * hbhost_desk.c - HBHOST.BIN's desktop side (icewm.md §9).
 *
 * This TU compiles the default desktop's own source (src/user/desktop.c,
 * unchanged; its main() renamed away and _start compiled out by
 * DESKTOP_TEST_WRAPPER, the same switch the desktop test wrapper uses) so
 * that one HobbyOS app window is decoded, painted and fed input by exactly
 * the code DESKTOP.BIN runs: the escape state machine, the terminal-mode
 * CSI dispatch, the ESC ] M/T/P/V/X/F handlers, the key payload mapping
 * (keymap/shift_keymap, Ctrl folding, Caps Lock, ESC [ K mods for pixel
 * windows), the mouse/drag/hover/wheel/close message builders.  The window
 * paints into the virtual display's surface through window.c/graphics.c;
 * hbhost.c blits that surface into the X window.
 *
 * The one thing the desktop does that HBHOST must not is open a new
 * desktop window for ESC ] R (run request): a byte pre-filter below routes
 * complete ESC ] R ... ~ sequences to hbhost_run_request() (a new HBHOST
 * window) and passes every other byte to the desktop parser verbatim.
 */
#define main hbhost_desktop_main_unused
#ifndef DESKTOP_TEST_WRAPPER
#define DESKTOP_TEST_WRAPPER 1
#endif
#include "../desktop.c"
#undef main

#include "vdisplay.h"
#include "hbhost.h"

/* ---- virtual display syscalls ---- */
int hd_vd_attach(int memfd) {
  return (int)my_syscall(SYS_VDISPLAY, VDISP_OP_ATTACH, memfd, 0, 0);
}
int hd_vd_detach(void) {
  return (int)my_syscall(SYS_VDISPLAY, VDISP_OP_DETACH, 0, 0, 0);
}
int hd_vd_status(struct hd_vstatus *out) {
  struct vdisp_status st;
  int r = (int)my_syscall(SYS_VDISPLAY, VDISP_OP_STATUS, (long)&st, 0, 0);
  if (r < 0) return r;
  out->flush_seq = st.flush_seq;
  out->dx = st.dx; out->dy = st.dy; out->dw = st.dw; out->dh = st.dh;
  out->mapped = st.mapped; out->reads = st.reads;
  out->read_age_ms = st.read_age_ms; out->queued = st.queued;
  return 0;
}
int hd_vd_inject_key(int code, int value) {
  struct vdisp_event ev;
  ev.type = EV_KEY;
  ev.code = (uint16_t)code;
  ev.value = (uint32_t)value;
  return (int)my_syscall(SYS_VDISPLAY, VDISP_OP_INJECT, (long)&ev, 1, 0);
}

/* ---- the window ---- */
static struct window *hw;   /* windows[0] once created */
static int hd_full = 1;     /* next paint repaints the whole window */
static int vis_w = 0, vis_h = 0;

unsigned *hd_init(void) {
  if (graphics_init() < 0) return 0;
  wm_init();
  return (unsigned *)graphics_fb();
}

int hd_create(int pid, int out_fd, int in_fd, const char *title) {
  int id = wm_create_window(THEME_SURFACE, pid, out_fd, in_fd);
  if (id < 0) return -1;
  hw = find_window(id);
  wm_set_window_title(id, title);
  focused_window = id;
  return 0;
}

void hd_set_size(int w, int h) {
  if (!hw) return;
  if (w < 32) w = 32;
  if (h < HD_MENUBAR_H + 16) h = HD_MENUBAR_H + 16;
  if (w > SCREEN_WIDTH - 4) w = SCREEN_WIDTH - 4;
  if (h > SCREEN_HEIGHT - 20) h = SCREEN_HEIGHT - 20;
  vis_w = w; vis_h = h;
  /* Frame/title off-surface: content x = hw->x + 2 = 0, menu bar rows
   * hw->y + 18 .. + 34 = 0 .. 16, content y = 16, content h = h - 16. */
  hw->x = -2;
  hw->y = -18;
  hw->w = w + 4;
  hw->h = h + 20;
  wm_window_invalidate(hw);
  if (hw->term_mode) wm_term_resize(hw);
  if (hw->pixel_mode) wm_pixel_resize(hw);
  hd_full = 1;
}

void hd_invalidate(void) { hd_full = 1; }

/* ---- output: ESC ] R pre-filter, then the desktop's parser ---- */
static char pf_buf[200];
static int pf_len = 0;
static int pf_state = 0;   /* 0 idle, 1 saw ESC, 2 inside OSC */

static void pf_flush(void) {
  for (int i = 0; i < pf_len; i++) desktop_process_output_byte(hw, pf_buf[i]);
  pf_len = 0;
  pf_state = 0;
}

void hd_feed(const char *buf, int n) {
  if (!hw) return;
  for (int k = 0; k < n; k++) {
    char c = buf[k];
    if (pf_state == 0) {
      if (c == '\033' && hw->escape_state == 0) {
        pf_state = 1;
        pf_buf[0] = c;
        pf_len = 1;
      } else {
        desktop_process_output_byte(hw, c);
      }
    } else if (pf_state == 1) {
      pf_buf[pf_len++] = c;
      if (c == ']') pf_state = 2;
      else pf_flush();
    } else {
      /* Terminators exactly as the desktop's OSC collector: BEL, '~', or
       * its 127-byte cap (escape_len counts the bytes after ESC). */
      int over = (pf_len - 1) >= (int)sizeof(hw->escape_buf) - 1;
      if (c == '\a' || c == '~' || over) {
        if (!over && pf_len >= 3 && pf_buf[1] == ']' && pf_buf[2] == 'R') {
          char seq[200];
          int j = 0;
          for (int i = 1; i < pf_len && j < (int)sizeof seq - 1; i++) seq[j++] = pf_buf[i];
          seq[j] = '\0';
          char bin[32], rargs[160];
          if (wm_parse_run_request(seq, bin, sizeof bin, rargs, sizeof rargs))
            hbhost_run_request(bin, rargs);
          pf_len = 0;
          pf_state = 0;
        } else {
          pf_buf[pf_len++] = c;
          pf_flush();
        }
      } else {
        pf_buf[pf_len++] = c;
      }
    }
  }
}

int hd_paint(int *x, int *y, int *w, int *h) {
  if (!hw) return 0;
  int painted = 0;
  if (hd_full || hw->chrome_dirty) {
    wm_draw_windows(hw->id);
    hw->chrome_dirty = 0;
    hd_full = 0;
    *x = 0; *y = 0; *w = vis_w; *h = vis_h;
    painted = 1;
  } else if (!hw->pixel_mode && wm_draw_window_rows(hw)) {
    if (hw->row_damage.w > 0) {
      *x = hw->row_damage.x; *y = hw->row_damage.y;
      *w = hw->row_damage.w; *h = hw->row_damage.h;
    } else {
      hd_content_rect(x, y, w, h);
    }
    painted = 1;
  }
  if (hw->pixel_mode) wm_pixel_flush_exposes(hw);
  return painted;
}

const char *hd_title(void) { return hw ? hw->title : ""; }
int hd_pixel_mode(void) { return hw ? hw->pixel_mode : 0; }
int hd_term_mode(void) { return hw ? hw->term_mode : 0; }
int hd_mouse_events(void) { return hw ? hw->mouse_events : 0; }
int hd_take_restamp(void) {
  if (!hw || !hw->pix_restamp) return 0;
  hw->pix_restamp = 0;
  return 1;
}
void hd_pref_size(int *w, int *h) {
  *w = hw ? hw->pix_pref_w : 0;
  *h = hw ? hw->pix_pref_h : 0;
}
void hd_content_rect(int *x, int *y, int *w, int *h) {
  if (!hw) { *x = *y = *w = *h = 0; return; }
  wm_pixel_content_rect(hw, x, y, w, h);
}

/* ---- input: the desktop main loop's key-press branch, one window ----
 * Returns 1 when the press went to the app's stdin (the desktop's byte
 * mapping produced a payload), 0 when it produced none (modifiers, Caps
 * Lock, F1, unmapped keys) -- hbhost.c then offers it as a raw event. */
int hd_key_press(int code, int shift, int ctrl, int alt, int caps) {
  if (!hw) return 0;
  shift_pressed = shift;
  ctrl_pressed = ctrl;
  alt_pressed = alt;
  caps_lock_on = caps;
  /* Modifiers and Caps Lock only change state (taken from X above). */
  if (code == 42 || code == 54 || code == 29 || code == 97 ||
      code == 56 || code == 100 || code == 58)
    return 0;
  if (code == 59) return 0;    /* F1: the desktop's own Apps-menu key */
  if (code == 62) return 0;    /* F4: close -- handled by hbhost.c */
  char probe[8];
  if (key_payload(code, probe) <= 0) return 0;
  forward_key_to_focused(code, sysinfo(1, 0, 0));
  return 1;
}

void hd_press(int x, int y) {
  if (!hw || !hw->mouse_events) return;
  if (y < HD_MENUBAR_H) return;   /* the desktop forwards content presses only */
  int lx, ly;
  char seq[24];
  wm_mouse_local(hw, x, y, &lx, &ly);
  int n = wm_build_mouse_seq(seq, sizeof seq, 'P', lx, ly, 1);
  desktop_send_hook(hw->id, seq, n);
  desktop_drag_begin(hw->id, lx, ly);
}
void hd_motion(int x, int y) { desktop_drag_move(x, y); }
void hd_release(int x, int y) { desktop_drag_end(x, y); }
void hd_wheel(int x, int y, int delta) { desktop_wheel(x, y, delta); }
void hd_hover(int x, int y, int now_ms) { wm_hover_service(x, y, now_ms); }

void hd_send_close(void) {
  if (!hw) return;
  char seq[8];
  int n = wm_build_close_seq(seq, sizeof seq);
  desktop_send_hook(hw->id, seq, n);
}

/* ---- window menus: the desktop's menu-bar geometry ---- */
int hd_menu_hit(int x, int y, int *menu_x) {
  if (!hw || y < 0 || y >= HD_MENUBAR_H) return -1;
  int m_x = hw->x + 10;
  for (int m = 0; m < hw->num_menus; m++) {
    int len = 0;
    while (hw->menus[m].name[len]) len++;
    int width = len * 8 + 16;
    if (x >= m_x && x < m_x + width) {
      *menu_x = m_x;
      return m;
    }
    m_x += width;
  }
  return -1;
}
int hd_menu_items(int m) {
  if (!hw || m < 0 || m >= hw->num_menus) return 0;
  return hw->menus[m].num_items;
}
const char *hd_menu_item(int m, int i) {
  if (!hw || m < 0 || m >= hw->num_menus || i < 0 || i >= hw->menus[m].num_items) return "";
  return hw->menus[m].items[i];
}
void hd_menu_select(int m, int i) {
  if (!hw) return;
  /* Same 7 bytes the desktop writes: ESC [ M <menu> ; <item> ~ */
  char seq[16] = "\033[M0;0~";
  seq[3] = (char)('0' + m);
  seq[5] = (char)('0' + i);
  int wr = write(hw->stdin_fd, seq, 7);
  (void)wr;
}
