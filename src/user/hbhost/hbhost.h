/*
 * hbhost.h - the seam between HBHOST.BIN's X11 side (hbhost.c, real libX11)
 * and its HobbyOS-desktop side (hbhost_desk.c, which compiles the default
 * desktop's own src/user/desktop.c + window.c/graphics.c so the window
 * protocol is decoded and painted by exactly the code DESKTOP.BIN runs).
 * No X or desktop headers here: both TUs include it.  icewm.md §9.
 *
 * Coordinates: the X window's (0,0) is surface pixel (0,0) of the private
 * framebuffer (the virtual display).  The single desktop window sits at
 * (-2,-18) so its 18px title bar and 2px side frame are off-surface (IceWM
 * draws the real frame); its menu bar is the top HD_MENUBAR_H rows.
 */
#ifndef HBHOST_H
#define HBHOST_H

#define HD_MENUBAR_H 16

/* ---- virtual display (SYS_VDISPLAY) ---- */
struct hd_vstatus {
  unsigned flush_seq;
  int dx, dy, dw, dh;
  unsigned mapped, reads, read_age_ms, queued;
};
int hd_vd_attach(int memfd);
int hd_vd_detach(void);
int hd_vd_status(struct hd_vstatus *st);
int hd_vd_inject_key(int code, int value);

/* ---- desktop window ---- */
unsigned *hd_init(void);           /* graphics_init (after attach) + wm_init */
int hd_create(int pid, int out_fd, int in_fd, const char *title);
void hd_set_size(int w, int h);    /* X window client size */
void hd_feed(const char *buf, int n);
int hd_paint(int *x, int *y, int *w, int *h); /* 1 = surface rect changed */
void hd_invalidate(void);
const char *hd_title(void);
int hd_pixel_mode(void);
int hd_term_mode(void);
int hd_mouse_events(void);
int hd_take_restamp(void);         /* app sent ESC ] F since the last call */
void hd_pref_size(int *w, int *h); /* ESC ] X preference (0 = any) */
void hd_content_rect(int *x, int *y, int *w, int *h);

/* input, in surface coordinates */
int hd_key_press(int code, int shift, int ctrl, int alt, int caps); /* 1 = sent to stdin */
void hd_press(int x, int y);
void hd_motion(int x, int y);
void hd_release(int x, int y);
void hd_wheel(int x, int y, int delta);
void hd_hover(int x, int y, int now_ms);
void hd_send_close(void);

/* window menus (ESC ] M) */
int hd_menu_hit(int x, int y, int *menu_x); /* menu index under (x,y) or -1 */
int hd_menu_items(int m);
const char *hd_menu_item(int m, int i);
void hd_menu_select(int m, int i);

/* provided by hbhost.c: ESC ] R <bin>[;<args>] ~ from the app */
void hbhost_run_request(const char *bin, const char *args);

#endif
