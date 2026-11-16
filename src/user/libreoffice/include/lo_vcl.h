#ifndef LO_VCL_H
#define LO_VCL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "lo_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lo_vcl_surface lo_vcl_surface_t;

/* Event Types */
typedef enum {
    LO_VCL_EVENT_NONE = 0,
    LO_VCL_EVENT_EXPOSE,
    LO_VCL_EVENT_RESIZE,        /* ConfigureNotify */
    LO_VCL_EVENT_KEY_PRESS,
    LO_VCL_EVENT_MOUSE_DOWN,
    LO_VCL_EVENT_MOUSE_UP,
    LO_VCL_EVENT_MOUSE_MOVE,
    LO_VCL_EVENT_CLOSE          /* WM_DELETE_WINDOW / DestroyNotify */
} lo_vcl_event_type_t;

/* Modifier Flags */
#define LO_VCL_MOD_SHIFT (1 << 0)
#define LO_VCL_MOD_CTRL  (1 << 1)
#define LO_VCL_MOD_ALT   (1 << 2)

/* Normalized Keycodes */
#define LO_VCL_KEY_UNKNOWN     0
#define LO_VCL_KEY_RETURN      1
#define LO_VCL_KEY_BACKSPACE   2
#define LO_VCL_KEY_DELETE      3
#define LO_VCL_KEY_TAB         4
#define LO_VCL_KEY_ESCAPE      5
#define LO_VCL_KEY_LEFT        6
#define LO_VCL_KEY_RIGHT       7
#define LO_VCL_KEY_UP          8
#define LO_VCL_KEY_DOWN        9
#define LO_VCL_KEY_PAGE_UP     10
#define LO_VCL_KEY_PAGE_DOWN   11
#define LO_VCL_KEY_HOME        12
#define LO_VCL_KEY_END         13
#define LO_VCL_KEY_F1          14
#define LO_VCL_KEY_F2          15
#define LO_VCL_KEY_F3          16
#define LO_VCL_KEY_F4          17
#define LO_VCL_KEY_F5          18
#define LO_VCL_KEY_F6          19
#define LO_VCL_KEY_F7          20
#define LO_VCL_KEY_F8          21
#define LO_VCL_KEY_F9          22
#define LO_VCL_KEY_F10         23
#define LO_VCL_KEY_F11         24
#define LO_VCL_KEY_F12         25

typedef struct {
    uint32_t keycode;     /* LO_VCL_KEY_* */
    uint32_t keysym;      /* X11 KeySym value (e.g. XK_Return) */
    uint32_t modifiers;   /* LO_VCL_MOD_* */
    char text[16];        /* Printable ASCII/UTF-8 character(s) from XLookupString */
} lo_vcl_key_event_t;

typedef struct {
    int x;
    int y;
    int button;           /* 1=Left, 2=Middle, 3=Right, 4=ScrollUp, 5=ScrollDown */
    uint32_t modifiers;   /* LO_VCL_MOD_* */
} lo_vcl_mouse_event_t;

typedef struct {
    int width;
    int height;
} lo_vcl_resize_event_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    int count;
} lo_vcl_expose_event_t;

typedef struct lo_vcl_event {
    lo_vcl_event_type_t type;
    union {
        union {
            lo_vcl_key_event_t key;
            lo_vcl_mouse_event_t mouse;
            lo_vcl_resize_event_t resize;
            lo_vcl_expose_event_t expose;
        };
        union {
            lo_vcl_key_event_t key;
            lo_vcl_mouse_event_t mouse;
            lo_vcl_resize_event_t resize;
            lo_vcl_expose_event_t expose;
        } u;
    };
} lo_vcl_event_t;

lo_vcl_surface_t *lo_vcl_surface_create_headless(int w, int h);
lo_vcl_surface_t *lo_vcl_surface_create_x11(void *dpy, unsigned long win, int w, int h);
lo_vcl_surface_t *lo_vcl_window_create(const char *title, const char *res_name, int w, int h);
lo_vcl_surface_t *lo_vcl_create_window(const char *title, const char *res_name, int w, int h);
void lo_vcl_surface_destroy(lo_vcl_surface_t *surf);

/* Font identifiers */
typedef enum {
    LO_VCL_FONT_SANS = 0,       /* DejaVu Sans (Regular) */
    LO_VCL_FONT_SANS_BOLD = 1,  /* DejaVu Sans Bold */
    LO_VCL_FONT_SERIF = 2,      /* DejaVu Serif */
    LO_VCL_FONT_MONO = 3,       /* DejaVu Sans Mono */
    LO_VCL_FONT_COUNT = 4
} lo_vcl_font_id_t;

/* FreeType2 Anti-Aliased Typography */
int lo_vcl_draw_text_aa(lo_vcl_surface_t *surf, int x, int y, const char *text, int size_px, int font_id, uint32_t fg_rgb);
void lo_vcl_preload_fonts(void);
int lo_vcl_measure_text_aa(const char *text, int size_px, int font_id);

/* 2D VCL Drawing Primitives */
int lo_vcl_draw_rect(lo_vcl_surface_t *surf, int x, int y, int w, int h, uint32_t color, bool fill);
int lo_vcl_draw_line(lo_vcl_surface_t *surf, int x1, int y1, int x2, int y2, uint32_t color, int width);
int lo_vcl_draw_text(lo_vcl_surface_t *surf, int x, int y, const char *text, uint32_t color);
int lo_vcl_fill_rounded_rect(lo_vcl_surface_t *surf, int x, int y, int w, int h, int radius, uint32_t color);
int lo_vcl_draw_rounded_rect(lo_vcl_surface_t *surf, int x, int y, int w, int h, int radius, uint32_t color, int line_width);
int lo_vcl_draw_shadow(lo_vcl_surface_t *surf, int x, int y, int w, int h, int radius, int blur, uint32_t shadow_color);
int lo_vcl_draw_circle_aa(lo_vcl_surface_t *surf, int cx, int cy, int radius, uint32_t color, bool fill);
int lo_vcl_draw_triangle(lo_vcl_surface_t *surf, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, bool fill);
int lo_vcl_draw_blit_rgba(lo_vcl_surface_t *surf, int dst_x, int dst_y, int src_w, int src_h, const uint32_t *rgba_pixels);
int lo_vcl_window_set_icon(lo_vcl_surface_t *surf, int w, int h, const uint32_t *argb_pixels);

int lo_vcl_clear(lo_vcl_surface_t *surf, uint32_t color);
int lo_vcl_present(lo_vcl_surface_t *surf);
const uint32_t *lo_vcl_get_pixels(lo_vcl_surface_t *surf);
int lo_vcl_surface_get_width(lo_vcl_surface_t *surf);
int lo_vcl_surface_get_height(lo_vcl_surface_t *surf);

/* Event loop APIs */
bool lo_vcl_surface_is_x11(const lo_vcl_surface_t *surf);
int lo_vcl_wait_event(lo_vcl_surface_t *surf, lo_vcl_event_t *ev);
bool lo_vcl_poll_event(lo_vcl_surface_t *surf, lo_vcl_event_t *ev);

#ifdef __cplusplus
}
#endif

#endif /* LO_VCL_H */
