/*
 * src/user/libreoffice/core/lo_vcl.c
 *
 * Visual Class Library (VCL) display and rasterization abstraction for HobbyOS.
 * Supports both headless offscreen surfaces and interactive X11 window surfaces.
 */

#include "lo_vcl.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <ft2build.h>
#include FT_FREETYPE_H

struct lo_vcl_surface {
    int width;
    int height;
    uint32_t *pixels;
    bool is_x11;
    bool owns_window;
    Display *dpy;
    Window win;
    GC gc;
    XImage *ximage;
    Atom wm_delete;
    Atom wm_protocols;
};

static uint32_t map_keysym(KeySym ks) {
    switch (ks) {
        case XK_Return:
        case XK_KP_Enter:    return LO_VCL_KEY_RETURN;
        case XK_BackSpace:   return LO_VCL_KEY_BACKSPACE;
        case XK_Delete:      return LO_VCL_KEY_DELETE;
        case XK_Tab:         return LO_VCL_KEY_TAB;
        case XK_Escape:      return LO_VCL_KEY_ESCAPE;
        case XK_Left:        return LO_VCL_KEY_LEFT;
        case XK_Right:       return LO_VCL_KEY_RIGHT;
        case XK_Up:          return LO_VCL_KEY_UP;
        case XK_Down:        return LO_VCL_KEY_DOWN;
        case XK_Page_Up:     return LO_VCL_KEY_PAGE_UP;
        case XK_Page_Down:   return LO_VCL_KEY_PAGE_DOWN;
        case XK_Home:        return LO_VCL_KEY_HOME;
        case XK_End:         return LO_VCL_KEY_END;
        case XK_F1:          return LO_VCL_KEY_F1;
        case XK_F2:          return LO_VCL_KEY_F2;
        case XK_F3:          return LO_VCL_KEY_F3;
        case XK_F4:          return LO_VCL_KEY_F4;
        case XK_F5:          return LO_VCL_KEY_F5;
        case XK_F6:          return LO_VCL_KEY_F6;
        case XK_F7:          return LO_VCL_KEY_F7;
        case XK_F8:          return LO_VCL_KEY_F8;
        case XK_F9:          return LO_VCL_KEY_F9;
        case XK_F10:         return LO_VCL_KEY_F10;
        case XK_F11:         return LO_VCL_KEY_F11;
        case XK_F12:         return LO_VCL_KEY_F12;
        default:             return LO_VCL_KEY_UNKNOWN;
    }
}

static uint32_t map_modifiers(unsigned int state) {
    uint32_t mods = 0;
    if (state & ShiftMask)   mods |= LO_VCL_MOD_SHIFT;
    if (state & ControlMask) mods |= LO_VCL_MOD_CTRL;
    if (state & Mod1Mask)    mods |= LO_VCL_MOD_ALT;
    return mods;
}

static bool process_xevent(lo_vcl_surface_t *surf, XEvent *xev, lo_vcl_event_t *ev) {
    if (!surf || !xev || !ev) return false;
    memset(ev, 0, sizeof(*ev));

    switch (xev->type) {
        case Expose:
            ev->type = LO_VCL_EVENT_EXPOSE;
            ev->expose.x = xev->xexpose.x;
            ev->expose.y = xev->xexpose.y;
            ev->expose.width = xev->xexpose.width;
            ev->expose.height = xev->xexpose.height;
            ev->expose.count = xev->xexpose.count;
            return true;

        case ConfigureNotify: {
            XConfigureEvent *ce = &xev->xconfigure;
            if (ce->width > 0 && ce->height > 0 &&
                (ce->width != surf->width || ce->height != surf->height)) {
                surf->width = ce->width;
                surf->height = ce->height;

                if (surf->ximage) {
                    surf->ximage->data = NULL; /* Prevent double-free */
                    XDestroyImage(surf->ximage);
                    surf->ximage = NULL;
                }
                if (surf->pixels) {
                    free(surf->pixels);
                    surf->pixels = NULL;
                }
                surf->pixels = (uint32_t *)calloc((size_t)surf->width * surf->height, sizeof(uint32_t));
                if (surf->pixels && surf->dpy) {
                    int screen = DefaultScreen(surf->dpy);
                    Visual *visual = DefaultVisual(surf->dpy, screen);
                    surf->ximage = XCreateImage(surf->dpy, visual, 24, ZPixmap, 0,
                                                (char *)surf->pixels,
                                                (unsigned int)surf->width,
                                                (unsigned int)surf->height, 32, 0);
                }
            }
            ev->type = LO_VCL_EVENT_RESIZE;
            ev->resize.width = surf->width;
            ev->resize.height = surf->height;
            return true;
        }

        case KeyPress: {
            char buf[16] = {0};
            KeySym ks = 0;
            int n = XLookupString(&xev->xkey, buf, sizeof(buf) - 1, &ks, NULL);
            if (n > 0) buf[n] = '\0'; else buf[0] = '\0';

            ev->type = LO_VCL_EVENT_KEY_PRESS;
            ev->key.keycode = map_keysym(ks);
            ev->key.keysym = (uint32_t)ks;
            ev->key.modifiers = map_modifiers(xev->xkey.state);
            strncpy(ev->key.text, buf, sizeof(ev->key.text) - 1);
            ev->key.text[sizeof(ev->key.text) - 1] = '\0';
            return true;
        }

        case ButtonPress:
            ev->type = LO_VCL_EVENT_MOUSE_DOWN;
            ev->mouse.x = xev->xbutton.x;
            ev->mouse.y = xev->xbutton.y;
            ev->mouse.button = xev->xbutton.button;
            ev->mouse.modifiers = map_modifiers(xev->xbutton.state);
            return true;

        case ButtonRelease:
            ev->type = LO_VCL_EVENT_MOUSE_UP;
            ev->mouse.x = xev->xbutton.x;
            ev->mouse.y = xev->xbutton.y;
            ev->mouse.button = xev->xbutton.button;
            ev->mouse.modifiers = map_modifiers(xev->xbutton.state);
            return true;

        case MotionNotify:
            ev->type = LO_VCL_EVENT_MOUSE_MOVE;
            ev->mouse.x = xev->xmotion.x;
            ev->mouse.y = xev->xmotion.y;
            ev->mouse.button = 0;
            ev->mouse.modifiers = map_modifiers(xev->xmotion.state);
            return true;

        case ClientMessage:
            if (surf->wm_delete != None && (Atom)xev->xclient.data.l[0] == surf->wm_delete) {
                ev->type = LO_VCL_EVENT_CLOSE;
                return true;
            }
            return false;

        case DestroyNotify:
            ev->type = LO_VCL_EVENT_CLOSE;
            return true;

        default:
            return false;
    }
}

lo_vcl_surface_t *lo_vcl_surface_create_headless(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;

    lo_vcl_surface_t *surf = (lo_vcl_surface_t *)calloc(1, sizeof(lo_vcl_surface_t));
    if (!surf) return NULL;

    surf->width = w;
    surf->height = h;
    surf->pixels = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
    if (!surf->pixels) {
        free(surf);
        return NULL;
    }
    surf->is_x11 = false;
    return surf;
}

lo_vcl_surface_t *lo_vcl_surface_create_x11(void *dpy_ptr, unsigned long win_id, int w, int h) {
    Display *dpy = (Display *)dpy_ptr;
    Window win = (Window)win_id;

    if (!dpy || win == 0) {
        return lo_vcl_surface_create_headless(w, h);
    }
    if (w <= 0 || h <= 0) return NULL;

    lo_vcl_surface_t *surf = (lo_vcl_surface_t *)calloc(1, sizeof(lo_vcl_surface_t));
    if (!surf) return NULL;

    surf->width = w;
    surf->height = h;
    surf->pixels = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
    if (!surf->pixels) {
        free(surf);
        return NULL;
    }

    surf->is_x11 = true;
    surf->owns_window = false;
    surf->dpy = dpy;
    surf->win = win;
    surf->gc = XCreateGC(dpy, win, 0, NULL);
    surf->wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    surf->wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);

    int screen = DefaultScreen(dpy);
    Visual *visual = DefaultVisual(dpy, screen);
    surf->ximage = XCreateImage(dpy, visual, 24, ZPixmap, 0,
                                (char *)surf->pixels, (unsigned int)w, (unsigned int)h, 32, 0);

    return surf;
}

lo_vcl_surface_t *lo_vcl_window_create(const char *title, const char *res_name, int w, int h) {
    if (w <= 0 || h <= 0) return NULL;

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        return lo_vcl_surface_create_headless(w, h);
    }

    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    Window win = XCreateSimpleWindow(dpy, root, 50, 50,
                                     (unsigned int)w, (unsigned int)h, 0,
                                     BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    if (!win) {
        XCloseDisplay(dpy);
        return lo_vcl_surface_create_headless(w, h);
    }

    /* Event mask selection: full input suite */
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask |
                           ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);

    /* Window title */
    const char *win_title = title ? title : "LibreOffice";
    XStoreName(dpy, win, win_title);

    /* _NET_WM_NAME (UTF8_STRING) */
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    if (net_wm_name != None && utf8_string != None) {
        XChangeProperty(dpy, win, net_wm_name, utf8_string, 8, PropModeReplace,
                        (const unsigned char *)win_title, (int)strlen(win_title));
    }

    /* WM_CLASS (res_name, res_class="LibreOffice") */
    XClassHint ch;
    ch.res_name = (char *)(res_name ? res_name : "soffice");
    ch.res_class = (char *)"LibreOffice";
    XSetClassHint(dpy, win, &ch);

    /* WM_DELETE_WINDOW protocol */
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    if (wm_delete != None) {
        XSetWMProtocols(dpy, win, &wm_delete, 1);
    }

    /* Size hints */
    XSizeHints sh;
    memset(&sh, 0, sizeof(sh));
    sh.flags = PMinSize | PSize;
    sh.min_width = 320;
    sh.min_height = 240;
    sh.width = w;
    sh.height = h;
    XSetWMNormalHints(dpy, win, &sh);

    /* _NET_WM_ICON (16x16 + 32x32) */
    Atom net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
    if (net_wm_icon != None) {
        unsigned long icon_buf[2 + 256 + 2 + 1024];
        uint32_t brand_color = 0xFF33393B;
        if (res_name) {
            if (strstr(res_name, "writer")) brand_color = 0xFF1C71D8;
            else if (strstr(res_name, "calc")) brand_color = 0xFF26A269;
            else if (strstr(res_name, "impress")) brand_color = 0xFFE66100;
            else if (strstr(res_name, "draw")) brand_color = 0xFFE5A50A;
            else if (strstr(res_name, "math")) brand_color = 0xFF9141AC;
            else if (strstr(res_name, "base")) brand_color = 0xFF613583;
        }

        /* 16x16 */
        icon_buf[0] = 16;
        icon_buf[1] = 16;
        for (int iy = 0; iy < 16; iy++) {
            for (int ix = 0; ix < 16; ix++) {
                int idx = iy * 16 + ix;
                if (ix == 0 || iy == 0 || ix == 15 || iy == 15) {
                    icon_buf[2 + idx] = 0;
                } else if (ix >= 4 && ix <= 11 && iy >= 4 && iy <= 11) {
                    icon_buf[2 + idx] = 0xFFFFFFFF;
                } else {
                    icon_buf[2 + idx] = (unsigned long)brand_color;
                }
            }
        }

        /* 32x32 */
        icon_buf[258] = 32;
        icon_buf[259] = 32;
        for (int iy = 0; iy < 32; iy++) {
            for (int ix = 0; ix < 32; ix++) {
                int idx = iy * 32 + ix;
                if (ix == 0 || iy == 0 || ix == 31 || iy == 31) {
                    icon_buf[260 + idx] = 0;
                } else if (ix >= 8 && ix <= 23 && iy >= 8 && iy <= 23) {
                    icon_buf[260 + idx] = 0xFFFFFFFF;
                } else {
                    icon_buf[260 + idx] = (unsigned long)brand_color;
                }
            }
        }

        XChangeProperty(dpy, win, net_wm_icon, XA_CARDINAL, 32, PropModeReplace,
                        (const unsigned char *)icon_buf, 1284);
    }

    /* Map window and flush */
    XMapWindow(dpy, win);
    XFlush(dpy);

    /* Create VCL surface */
    lo_vcl_surface_t *surf = lo_vcl_surface_create_x11(dpy, (unsigned long)win, w, h);
    if (!surf) {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return lo_vcl_surface_create_headless(w, h);
    }

    surf->owns_window = true;
    surf->wm_delete = wm_delete;
    surf->wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    return surf;
}

lo_vcl_surface_t *lo_vcl_create_window(const char *title, const char *res_name, int w, int h) {
    return lo_vcl_window_create(title, res_name, w, h);
}

int lo_vcl_window_set_icon(lo_vcl_surface_t *surf, int w, int h, const uint32_t *argb_pixels) {
    if (!surf || !surf->is_x11 || !surf->dpy || !surf->win || !argb_pixels || w <= 0 || h <= 0) {
        return LO_ERR_NULL_ARG;
    }
    size_t count = 2 + (size_t)w * h;
    unsigned long *buf = (unsigned long *)malloc(count * sizeof(unsigned long));
    if (!buf) return LO_ERR_OUT_OF_MEMORY;
    buf[0] = (unsigned long)w;
    buf[1] = (unsigned long)h;
    for (int i = 0; i < w * h; i++) {
        buf[2 + i] = (unsigned long)argb_pixels[i];
    }
    Atom net_wm_icon = XInternAtom(surf->dpy, "_NET_WM_ICON", False);
    if (net_wm_icon != None) {
        XChangeProperty(surf->dpy, surf->win, net_wm_icon, XA_CARDINAL, 32, PropModeReplace,
                        (const unsigned char *)buf, (int)count);
    }
    free(buf);
    return LO_OK;
}

void lo_vcl_surface_destroy(lo_vcl_surface_t *surf) {
    if (!surf) return;

    if (surf->is_x11 && surf->dpy) {
        if (surf->ximage) {
            surf->ximage->data = NULL; /* Prevent double free of surf->pixels */
            XDestroyImage(surf->ximage);
            surf->ximage = NULL;
        }
        if (surf->gc) {
            XFreeGC(surf->dpy, surf->gc);
            surf->gc = NULL;
        }
        if (surf->owns_window) {
            if (surf->win) {
                XDestroyWindow(surf->dpy, surf->win);
                surf->win = 0;
            }
            XCloseDisplay(surf->dpy);
            surf->dpy = NULL;
        }
    }

    if (surf->pixels) {
        free(surf->pixels);
        surf->pixels = NULL;
    }
    free(surf);
}

int lo_vcl_clear(lo_vcl_surface_t *surf, uint32_t color) {
    if (!surf || !surf->pixels) return LO_ERR_NULL_ARG;

    size_t count = (size_t)surf->width * surf->height;
    for (size_t i = 0; i < count; i++) {
        surf->pixels[i] = color;
    }
    return LO_OK;
}

int lo_vcl_draw_rect(lo_vcl_surface_t *surf, int x, int y, int w, int h, uint32_t color, bool fill) {
    if (!surf || !surf->pixels) return LO_ERR_NULL_ARG;
    if (w <= 0 || h <= 0) return LO_OK;

    if (fill) {
        int x0 = (x < 0) ? 0 : x;
        int y0 = (y < 0) ? 0 : y;
        int x1 = (x + w > surf->width) ? surf->width : (x + w);
        int y1 = (y + h > surf->height) ? surf->height : (y + h);

        if (x0 >= x1 || y0 >= y1) return LO_OK;

        for (int r = y0; r < y1; r++) {
            uint32_t *row = surf->pixels + (r * surf->width);
            for (int c = x0; c < x1; c++) {
                row[c] = color;
            }
        }
    } else {
        lo_vcl_draw_line(surf, x, y, x + w - 1, y, color, 1);
        lo_vcl_draw_line(surf, x, y + h - 1, x + w - 1, y + h - 1, color, 1);
        lo_vcl_draw_line(surf, x, y, x, y + h - 1, color, 1);
        lo_vcl_draw_line(surf, x + w - 1, y, x + w - 1, y + h - 1, color, 1);
    }
    return LO_OK;
}

int lo_vcl_draw_line(lo_vcl_surface_t *surf, int x1, int y1, int x2, int y2, uint32_t color, int width) {
    if (!surf || !surf->pixels) return LO_ERR_NULL_ARG;

    int dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    int dy = (y2 > y1) ? (y2 - y1) : (y1 - y2);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    int x = x1, y = y1;

    int half_w = (width > 1) ? (width / 2) : 0;

    while (1) {
        if (width <= 1) {
            if (x >= 0 && x < surf->width && y >= 0 && y < surf->height) {
                surf->pixels[y * surf->width + x] = color;
            }
        } else {
            for (int oy = -half_w; oy <= half_w; oy++) {
                int py = y + oy;
                if (py < 0 || py >= surf->height) continue;
                for (int ox = -half_w; ox <= half_w; ox++) {
                    int px = x + ox;
                    if (px >= 0 && px < surf->width) {
                        surf->pixels[py * surf->width + px] = color;
                    }
                }
            }
        }

        if (x == x2 && y == y2) break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }

    return LO_OK;
}

int lo_vcl_draw_text(lo_vcl_surface_t *surf, int x, int y, const char *text, uint32_t color) {
    if (!surf || !surf->pixels || !text) return LO_ERR_NULL_ARG;

    int cur_x = x;
    int cur_y = y;

    for (size_t i = 0; text[i] != '\0'; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            cur_x = x;
            cur_y += 8;
            continue;
        }

        if (c < 32 || c > 127) c = '?';
        const uint8_t *glyph = font8x8[c - 32];

        for (int row = 0; row < 8; row++) {
            int py = cur_y + row;
            if (py < 0 || py >= surf->height) continue;
            uint8_t row_bits = glyph[row];
            if (!row_bits) continue;

            for (int col = 0; col < 8; col++) {
                int px = cur_x + col;
                if (px < 0 || px >= surf->width) continue;
                if (row_bits & (1 << (7 - col))) {
                    surf->pixels[py * surf->width + px] = color;
                }
            }
        }
        cur_x += 8;
    }

    return LO_OK;
}

int lo_vcl_present(lo_vcl_surface_t *surf) {
    if (!surf) return LO_ERR_NULL_ARG;

    if (surf->is_x11 && surf->dpy && surf->win && surf->ximage && surf->gc) {
        XPutImage(surf->dpy, surf->win, surf->gc, surf->ximage, 0, 0, 0, 0,
                  (unsigned int)surf->width, (unsigned int)surf->height);
        XFlush(surf->dpy);
    }
    return LO_OK;
}

const uint32_t *lo_vcl_get_pixels(lo_vcl_surface_t *surf) {
    return surf ? surf->pixels : NULL;
}

int lo_vcl_surface_get_width(lo_vcl_surface_t *surf) {
    return surf ? surf->width : 0;
}

int lo_vcl_surface_get_height(lo_vcl_surface_t *surf) {
    return surf ? surf->height : 0;
}

bool lo_vcl_surface_is_x11(const lo_vcl_surface_t *surf) {
    return (surf != NULL && surf->is_x11 && surf->dpy != NULL);
}

int lo_vcl_wait_event(lo_vcl_surface_t *surf, lo_vcl_event_t *ev) {
    if (!surf || !surf->is_x11 || !surf->dpy || !ev) return -1;
    memset(ev, 0, sizeof(*ev));
    for (;;) {
        XEvent xev;
        XNextEvent(surf->dpy, &xev);
        if (process_xevent(surf, &xev, ev)) {
            return 0;
        }
    }
}

bool lo_vcl_poll_event(lo_vcl_surface_t *surf, lo_vcl_event_t *ev) {
    if (!surf || !surf->is_x11 || !surf->dpy || !ev) return false;
    memset(ev, 0, sizeof(*ev));
    while (XPending(surf->dpy) > 0) {
        XEvent xev;
        XNextEvent(surf->dpy, &xev);
        if (process_xevent(surf, &xev, ev)) {
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * FreeType2 Typography & Anti-Aliased Rasterization
 * ========================================================================= */

static FT_Library g_ft_lib = NULL;

typedef struct {
    const char *primary_path;
    const char *alt_path;
    const char *alt_intel;
    const char *host_path;
    uint8_t *data;
    size_t size;
    FT_Face face;
    bool attempted;
} lo_font_desc_t;

static lo_font_desc_t g_fonts[LO_VCL_FONT_COUNT] = {
    { "/X11/FONTS/DJVSANS.TTF",  "third_party/x11/build/arm/disk/X11/FONTS/DJVSANS.TTF",  "third_party/x11/build/intel/disk/X11/FONTS/DJVSANS.TTF",  "third_party/fonts/dejavu-2.37/ttf/DejaVuSans.ttf",       NULL, 0, NULL, false },
    { "/X11/FONTS/DJVSANSB.TTF", "third_party/x11/build/arm/disk/X11/FONTS/DJVSANSB.TTF", "third_party/x11/build/intel/disk/X11/FONTS/DJVSANSB.TTF", "third_party/fonts/dejavu-2.37/ttf/DejaVuSans-Bold.ttf",  NULL, 0, NULL, false },
    { "/X11/FONTS/DJVSERIF.TTF", "third_party/x11/build/arm/disk/X11/FONTS/DJVSERIF.TTF", "third_party/x11/build/intel/disk/X11/FONTS/DJVSERIF.TTF", "third_party/fonts/dejavu-2.37/ttf/DejaVuSerif.ttf",      NULL, 0, NULL, false },
    { "/X11/FONTS/DJVMONO.TTF",  "third_party/x11/build/arm/disk/X11/FONTS/DJVMONO.TTF",  "third_party/x11/build/intel/disk/X11/FONTS/DJVMONO.TTF",  "third_party/fonts/dejavu-2.37/ttf/DejaVuSansMono.ttf",  NULL, 0, NULL, false },
};

static FT_Face get_font_face(int font_id) {
    if (font_id < 0 || font_id >= LO_VCL_FONT_COUNT) font_id = LO_VCL_FONT_SANS;
    lo_font_desc_t *f = &g_fonts[font_id];
    if (f->attempted) return f->face;
    f->attempted = true;

    if (!g_ft_lib) {
        if (FT_Init_FreeType(&g_ft_lib) != 0) {
            g_ft_lib = NULL;
            return NULL;
        }
    }

    FILE *fp = fopen(f->primary_path, "rb");
    if (!fp && f->alt_path) fp = fopen(f->alt_path, "rb");
    if (!fp && f->alt_intel) fp = fopen(f->alt_intel, "rb");
    if (!fp && f->host_path) fp = fopen(f->host_path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        return NULL;
    }

    f->data = (uint8_t *)malloc((size_t)sz);
    if (!f->data) {
        fclose(fp);
        return NULL;
    }

    size_t rd = fread(f->data, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) {
        free(f->data);
        f->data = NULL;
        return NULL;
    }
    f->size = (size_t)sz;

    FT_Face face = NULL;
    if (FT_New_Memory_Face(g_ft_lib, f->data, (FT_Long)f->size, 0, &face) == 0) {
        f->face = face;
    } else {
        free(f->data);
        f->data = NULL;
        f->face = NULL;
    }
    return f->face;
}

void lo_vcl_preload_fonts(void) {
    for (int i = 0; i < LO_VCL_FONT_COUNT; i++) {
        get_font_face(i);
    }
}

#define LO_VCL_GLYPH_CACHE_BUCKETS 1024

typedef struct {
    bool valid;
    uint8_t font_id;
    uint16_t size_px;
    uint32_t codepoint;
    int16_t bitmap_left;
    int16_t bitmap_top;
    uint16_t width;
    uint16_t height;
    int16_t advance_x;
    uint8_t *bitmap;
} lo_vcl_glyph_t;

static lo_vcl_glyph_t g_glyph_cache[LO_VCL_GLYPH_CACHE_BUCKETS];

static uint32_t hash_glyph(uint8_t font_id, uint16_t size_px, uint32_t cp) {
    uint32_t h = ((uint32_t)font_id * 31337) ^ ((uint32_t)size_px * 65521) ^ (cp * 104729);
    return h & (LO_VCL_GLYPH_CACHE_BUCKETS - 1);
}

static const lo_vcl_glyph_t *get_cached_glyph(int font_id, int size_px, uint32_t cp) {
    if (font_id < 0 || font_id >= LO_VCL_FONT_COUNT) font_id = LO_VCL_FONT_SANS;
    if (size_px <= 0) size_px = 12;

    uint32_t idx = hash_glyph((uint8_t)font_id, (uint16_t)size_px, cp);
    for (int probe = 0; probe < 16; probe++) {
        uint32_t slot = (idx + probe) & (LO_VCL_GLYPH_CACHE_BUCKETS - 1);
        lo_vcl_glyph_t *g = &g_glyph_cache[slot];
        if (g->valid && g->font_id == (uint8_t)font_id && g->size_px == (uint16_t)size_px && g->codepoint == cp) {
            return g;
        }
        if (!g->valid) {
            FT_Face face = get_font_face(font_id);
            g->font_id = (uint8_t)font_id;
            g->size_px = (uint16_t)size_px;
            g->codepoint = cp;
            g->valid = true;

            if (face) {
                FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size_px);
                if (FT_Load_Char(face, cp, FT_LOAD_RENDER) == 0) {
                    FT_GlyphSlot slot_glyph = face->glyph;
                    g->bitmap_left = (int16_t)slot_glyph->bitmap_left;
                    g->bitmap_top = (int16_t)slot_glyph->bitmap_top;
                    g->width = (uint16_t)slot_glyph->bitmap.width;
                    g->height = (uint16_t)slot_glyph->bitmap.rows;
                    g->advance_x = (int16_t)(slot_glyph->advance.x >> 6);

                    if (g->width > 0 && g->height > 0 && slot_glyph->bitmap.buffer) {
                        size_t bsz = (size_t)g->width * g->height;
                        g->bitmap = (uint8_t *)malloc(bsz);
                        if (g->bitmap) {
                            memcpy(g->bitmap, slot_glyph->bitmap.buffer, bsz);
                        }
                    } else {
                        g->bitmap = NULL;
                    }
                    return g;
                }
            }

            g->bitmap_left = 0;
            g->bitmap_top = (int16_t)size_px;
            g->width = (uint16_t)((size_px > 8) ? (size_px * 6 / 10) : 6);
            g->height = (uint16_t)size_px;
            g->advance_x = (int16_t)g->width;
            g->bitmap = NULL;
            return g;
        }
    }

    lo_vcl_glyph_t *g = &g_glyph_cache[idx];
    if (g->bitmap) { free(g->bitmap); g->bitmap = NULL; }
    FT_Face face = get_font_face(font_id);
    g->font_id = (uint8_t)font_id;
    g->size_px = (uint16_t)size_px;
    g->codepoint = cp;
    g->valid = true;

    if (face) {
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size_px);
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER) == 0) {
            FT_GlyphSlot slot_glyph = face->glyph;
            g->bitmap_left = (int16_t)slot_glyph->bitmap_left;
            g->bitmap_top = (int16_t)slot_glyph->bitmap_top;
            g->width = (uint16_t)slot_glyph->bitmap.width;
            g->height = (uint16_t)slot_glyph->bitmap.rows;
            g->advance_x = (int16_t)(slot_glyph->advance.x >> 6);

            if (g->width > 0 && g->height > 0 && slot_glyph->bitmap.buffer) {
                size_t bsz = (size_t)g->width * g->height;
                g->bitmap = (uint8_t *)malloc(bsz);
                if (g->bitmap) {
                    memcpy(g->bitmap, slot_glyph->bitmap.buffer, bsz);
                }
            } else {
                g->bitmap = NULL;
            }
            return g;
        }
    }

    g->bitmap_left = 0;
    g->bitmap_top = (int16_t)size_px;
    g->width = (uint16_t)((size_px > 8) ? (size_px * 6 / 10) : 6);
    g->height = (uint16_t)size_px;
    g->advance_x = (int16_t)g->width;
    g->bitmap = NULL;
    return g;
}

static uint32_t next_utf8_char(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    if (!*s) return 0;
    uint32_t cp = 0;
    if (*s < 0x80) {
        cp = *s++;
    } else if ((*s & 0xE0) == 0xC0) {
        cp = (*s++ & 0x1F) << 6;
        if ((*s & 0xC0) == 0x80) cp |= (*s++ & 0x3F);
    } else if ((*s & 0xF0) == 0xE0) {
        cp = (*s++ & 0x0F) << 12;
        if ((*s & 0xC0) == 0x80) cp |= (*s++ & 0x3F) << 6;
        if ((*s & 0xC0) == 0x80) cp |= (*s++ & 0x3F);
    } else if ((*s & 0xF8) == 0xF0) {
        cp = (*s++ & 0x07) << 18;
        if ((*s & 0xC0) == 0x80) cp |= (*s++ & 0x3F) << 12;
        if ((*s & 0xC0) == 0x80) cp |= (*s++ & 0x3F) << 6;
        if ((*s & 0xC0) == 0x80) cp |= (*s++ & 0x3F);
    } else {
        cp = *s++;
    }
    *p = (const char *)s;
    return cp;
}

int lo_vcl_measure_text_aa(const char *text, int size_px, int font_id) {
    if (!text || size_px <= 0) return 0;
    int total_w = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp = next_utf8_char(&p);
        if (cp == 0) break;
        if (cp == '\n') continue;
        const lo_vcl_glyph_t *g = get_cached_glyph(font_id, size_px, cp);
        if (g) {
            total_w += g->advance_x;
        } else {
            total_w += (size_px * 6) / 10;
        }
    }
    return total_w;
}

int lo_vcl_draw_text_aa(lo_vcl_surface_t *surf, int x, int y, const char *text, int size_px, int font_id, uint32_t fg_rgb) {
    if (!surf || !surf->pixels || !text || size_px <= 0) return LO_ERR_NULL_ARG;

    int cur_x = x;
    int cur_y = y;
    int baseline = y + (size_px * 4) / 5;

    uint32_t fg_r = (fg_rgb >> 16) & 0xFF;
    uint32_t fg_g = (fg_rgb >> 8) & 0xFF;
    uint32_t fg_b = fg_rgb & 0xFF;

    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            cur_x = x;
            cur_y += size_px;
            baseline = cur_y + (size_px * 4) / 5;
            p++;
            continue;
        }

        uint32_t cp = next_utf8_char(&p);
        if (cp == 0) break;

        const lo_vcl_glyph_t *g = get_cached_glyph(font_id, size_px, cp);
        if (!g) {
            cur_x += (size_px * 6) / 10;
            continue;
        }

        if (g->bitmap && g->width > 0 && g->height > 0) {
            int gx = cur_x + g->bitmap_left;
            int gy = baseline - g->bitmap_top;

            for (int r = 0; r < g->height; r++) {
                int py = gy + r;
                if (py < 0 || py >= surf->height) continue;
                uint32_t *row_pixels = surf->pixels + (py * surf->width);
                const uint8_t *src_row = g->bitmap + (r * g->width);

                for (int c = 0; c < g->width; c++) {
                    int px = gx + c;
                    if (px < 0 || px >= surf->width) continue;
                    uint32_t a = src_row[c];
                    if (a == 0) continue;
                    if (a == 255) {
                        row_pixels[px] = fg_rgb & 0x00FFFFFF;
                    } else {
                        uint32_t bg = row_pixels[px];
                        uint32_t inv_a = 255 - a;
                        uint32_t rv = (fg_r * a + ((bg >> 16) & 0xFF) * inv_a + 128) >> 8;
                        uint32_t gv = (fg_g * a + ((bg >> 8) & 0xFF) * inv_a + 128) >> 8;
                        uint32_t bv = (fg_b * a + (bg & 0xFF) * inv_a + 128) >> 8;
                        row_pixels[px] = (rv << 16) | (gv << 8) | bv;
                    }
                }
            }
        } else if (cp >= 32 && cp <= 127) {
            const uint8_t *glyph8 = font8x8[cp - 32];
            for (int r = 0; r < 8; r++) {
                int py = cur_y + (r * size_px) / 8;
                if (py < 0 || py >= surf->height) continue;
                uint8_t row_bits = glyph8[r];
                if (!row_bits) continue;
                for (int col = 0; col < 8; col++) {
                    int px = cur_x + (col * size_px) / 8;
                    if (px < 0 || px >= surf->width) continue;
                    if (row_bits & (1 << (7 - col))) {
                        surf->pixels[py * surf->width + px] = fg_rgb & 0x00FFFFFF;
                    }
                }
            }
        }

        cur_x += g->advance_x;
    }
    return LO_OK;
}

/* =========================================================================
 * 2D VCL Drawing Primitives
 * ========================================================================= */

int lo_vcl_fill_rounded_rect(lo_vcl_surface_t *surf, int x, int y, int w, int h, int radius, uint32_t color) {
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return LO_OK;
    if (radius <= 0) return lo_vcl_draw_rect(surf, x, y, w, h, color, true);

    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    lo_vcl_draw_rect(surf, x, y + radius, w, h - 2 * radius, color, true);
    lo_vcl_draw_rect(surf, x + radius, y, w - 2 * radius, radius, color, true);
    lo_vcl_draw_rect(surf, x + radius, y + h - radius, w - 2 * radius, radius, color, true);

    int r = radius;
    int r2 = r * r;
    int r_in2 = (r > 1) ? (r - 1) * (r - 1) : 0;
    uint32_t fg_r = (color >> 16) & 0xFF;
    uint32_t fg_g = (color >> 8) & 0xFF;
    uint32_t fg_b = color & 0xFF;

    int cx[4] = { x + r, x + w - r - 1, x + r, x + w - r - 1 };
    int cy[4] = { y + r, y + r, y + h - r - 1, y + h - r - 1 };
    int x_start[4] = { x, x + w - r, x, x + w - r };
    int x_end[4]   = { x + r, x + w, x + r, x + w };
    int y_start[4] = { y, y, y + h - r, y + h - r };
    int y_end[4]   = { y + r, y + r, y + h, y + h };

    for (int corner = 0; corner < 4; corner++) {
        int ccx = cx[corner];
        int ccy = cy[corner];
        for (int py = y_start[corner]; py < y_end[corner]; py++) {
            if (py < 0 || py >= surf->height) continue;
            uint32_t *row_pixels = surf->pixels + (py * surf->width);
            int dy = py - ccy;
            for (int px = x_start[corner]; px < x_end[corner]; px++) {
                if (px < 0 || px >= surf->width) continue;
                int dx = px - ccx;
                int dist2 = dx * dx + dy * dy;
                if (dist2 <= r_in2) {
                    row_pixels[px] = color & 0x00FFFFFF;
                } else if (dist2 <= r2) {
                    int a = 128;
                    uint32_t bg = row_pixels[px];
                    uint32_t inv_a = 255 - a;
                    uint32_t rv = (fg_r * a + ((bg >> 16) & 0xFF) * inv_a + 128) >> 8;
                    uint32_t gv = (fg_g * a + ((bg >> 8) & 0xFF) * inv_a + 128) >> 8;
                    uint32_t bv = (fg_b * a + (bg & 0xFF) * inv_a + 128) >> 8;
                    row_pixels[px] = (rv << 16) | (gv << 8) | bv;
                }
            }
        }
    }
    return LO_OK;
}

int lo_vcl_draw_rounded_rect(lo_vcl_surface_t *surf, int x, int y, int w, int h, int radius, uint32_t color, int line_width) {
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return LO_OK;
    if (line_width <= 0) line_width = 1;
    if (radius <= 0) {
        for (int lw = 0; lw < line_width; lw++) {
            lo_vcl_draw_rect(surf, x + lw, y + lw, w - 2 * lw, h - 2 * lw, color, false);
        }
        return LO_OK;
    }
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    for (int lw = 0; lw < line_width; lw++) {
        lo_vcl_draw_line(surf, x + radius, y + lw, x + w - radius - 1, y + lw, color, 1);
        lo_vcl_draw_line(surf, x + radius, y + h - 1 - lw, x + w - radius - 1, y + h - 1 - lw, color, 1);
        lo_vcl_draw_line(surf, x + lw, y + radius, x + lw, y + h - radius - 1, color, 1);
        lo_vcl_draw_line(surf, x + w - 1 - lw, y + radius, x + w - 1 - lw, y + h - radius - 1, color, 1);
    }

    int r = radius;
    int r_out2 = r * r;
    int r_in = (r >= line_width) ? (r - line_width) : 0;
    int r_in2 = r_in * r_in;

    int cx[4] = { x + r, x + w - r - 1, x + r, x + w - r - 1 };
    int cy[4] = { y + r, y + r, y + h - r - 1, y + h - r - 1 };
    int x_start[4] = { x, x + w - r, x, x + w - r };
    int x_end[4]   = { x + r, x + w, x + r, x + w };
    int y_start[4] = { y, y, y + h - r, y + h - r };
    int y_end[4]   = { y + r, y + r, y + h, y + h };

    for (int corner = 0; corner < 4; corner++) {
        int ccx = cx[corner];
        int ccy = cy[corner];
        for (int py = y_start[corner]; py < y_end[corner]; py++) {
            if (py < 0 || py >= surf->height) continue;
            uint32_t *row_pixels = surf->pixels + (py * surf->width);
            int dy = py - ccy;
            for (int px = x_start[corner]; px < x_end[corner]; px++) {
                if (px < 0 || px >= surf->width) continue;
                int dx = px - ccx;
                int dist2 = dx * dx + dy * dy;
                if (dist2 <= r_out2 && dist2 >= r_in2) {
                    row_pixels[px] = color & 0x00FFFFFF;
                }
            }
        }
    }
    return LO_OK;
}

int lo_vcl_draw_shadow(lo_vcl_surface_t *surf, int x, int y, int w, int h, int radius, int blur, uint32_t shadow_color) {
    (void)radius;
    if (!surf || !surf->pixels || w <= 0 || h <= 0 || blur <= 0) return LO_OK;

    int shadow_r = (shadow_color >> 16) & 0xFF;
    int shadow_g = (shadow_color >> 8) & 0xFF;
    int shadow_b = shadow_color & 0xFF;
    int base_alpha = (shadow_color >> 24) & 0xFF;
    if (base_alpha == 0) base_alpha = 0x40;

    int offset_y = 2;
    int x0 = x - blur;
    int y0 = y - blur + offset_y;
    int x1 = x + w + blur;
    int y1 = y + h + blur + offset_y;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > surf->width) x1 = surf->width;
    if (y1 > surf->height) y1 = surf->height;

    int inner_x0 = x;
    int inner_y0 = y + offset_y;
    int inner_x1 = x + w;
    int inner_y1 = y + h + offset_y;

    for (int py = y0; py < y1; py++) {
        uint32_t *row_pixels = surf->pixels + (py * surf->width);
        for (int px = x0; px < x1; px++) {
            if (px >= inner_x0 && px < inner_x1 && py >= inner_y0 && py < inner_y1) {
                continue;
            }

            int dx = 0;
            if (px < inner_x0) dx = inner_x0 - px;
            else if (px >= inner_x1) dx = px - inner_x1 + 1;

            int dy = 0;
            if (py < inner_y0) dy = inner_y0 - py;
            else if (py >= inner_y1) dy = py - inner_y1 + 1;

            int dist = 0;
            if (dx > 0 && dy > 0) {
                dist = (int)sqrtf((float)(dx * dx + dy * dy));
            } else {
                dist = (dx > dy) ? dx : dy;
            }

            if (dist < blur) {
                int t = (blur - dist);
                int a = (base_alpha * t * t) / (blur * blur);
                if (a > 0) {
                    uint32_t bg = row_pixels[px];
                    uint32_t inv_a = 255 - a;
                    uint32_t rv = (shadow_r * a + ((bg >> 16) & 0xFF) * inv_a + 128) >> 8;
                    uint32_t gv = (shadow_g * a + ((bg >> 8) & 0xFF) * inv_a + 128) >> 8;
                    uint32_t bv = (shadow_b * a + (bg & 0xFF) * inv_a + 128) >> 8;
                    row_pixels[px] = (rv << 16) | (gv << 8) | bv;
                }
            }
        }
    }
    return LO_OK;
}

int lo_vcl_draw_circle_aa(lo_vcl_surface_t *surf, int cx, int cy, int radius, uint32_t color, bool fill) {
    if (!surf || !surf->pixels || radius <= 0) return LO_OK;

    int x0 = cx - radius - 1;
    int y0 = cy - radius - 1;
    int x1 = cx + radius + 1;
    int y1 = cy + radius + 1;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= surf->width) x1 = surf->width - 1;
    if (y1 >= surf->height) y1 = surf->height - 1;

    int r2 = radius * radius;
    int r_in2 = (radius > 1) ? (radius - 1) * (radius - 1) : 0;
    int r_edge2 = (radius - 2 > 0) ? (radius - 2) * (radius - 2) : 0;

    uint32_t fg_r = (color >> 16) & 0xFF;
    uint32_t fg_g = (color >> 8) & 0xFF;
    uint32_t fg_b = color & 0xFF;

    for (int py = y0; py <= y1; py++) {
        uint32_t *row_pixels = surf->pixels + (py * surf->width);
        int dy = py - cy;
        for (int px = x0; px <= x1; px++) {
            int dx = px - cx;
            int dist2 = dx * dx + dy * dy;

            if (fill) {
                if (dist2 <= r_in2) {
                    row_pixels[px] = color & 0x00FFFFFF;
                } else if (dist2 <= r2) {
                    int a = 128;
                    uint32_t bg = row_pixels[px];
                    uint32_t inv_a = 255 - a;
                    uint32_t rv = (fg_r * a + ((bg >> 16) & 0xFF) * inv_a + 128) >> 8;
                    uint32_t gv = (fg_g * a + ((bg >> 8) & 0xFF) * inv_a + 128) >> 8;
                    uint32_t bv = (fg_b * a + (bg & 0xFF) * inv_a + 128) >> 8;
                    row_pixels[px] = (rv << 16) | (gv << 8) | bv;
                }
            } else {
                if (dist2 <= r2 && dist2 >= r_edge2) {
                    row_pixels[px] = color & 0x00FFFFFF;
                }
            }
        }
    }
    return LO_OK;
}

int lo_vcl_draw_triangle(lo_vcl_surface_t *surf, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, bool fill) {
    if (!surf || !surf->pixels) return LO_OK;
    if (!fill) {
        lo_vcl_draw_line(surf, x0, y0, x1, y1, color, 1);
        lo_vcl_draw_line(surf, x1, y1, x2, y2, color, 1);
        lo_vcl_draw_line(surf, x2, y2, x0, y0, color, 1);
        return LO_OK;
    }

    if (y0 > y1) { int tx = x0; x0 = x1; x1 = tx; int ty = y0; y0 = y1; y1 = ty; }
    if (y1 > y2) { int tx = x1; x1 = x2; x2 = tx; int ty = y1; y1 = y2; y2 = ty; }
    if (y0 > y1) { int tx = x0; x0 = x1; x1 = tx; int ty = y0; y0 = y1; y1 = ty; }

    int total_height = y2 - y0;
    if (total_height == 0) return LO_OK;

    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= surf->height) continue;
        bool second_half = y > y1 || y1 == y0;
        int segment_height = second_half ? (y2 - y1) : (y1 - y0);
        if (segment_height == 0) continue;

        int alpha = (y - y0);
        int beta = second_half ? (y - y1) : (y - y0);

        int ax = x0 + (x2 - x0) * alpha / total_height;
        int bx = second_half ? (x1 + (x2 - x1) * beta / segment_height)
                             : (x0 + (x1 - x0) * beta / segment_height);

        if (ax > bx) { int t = ax; ax = bx; bx = t; }
        if (ax < 0) ax = 0;
        if (bx >= surf->width) bx = surf->width - 1;

        uint32_t *row_pixels = surf->pixels + (y * surf->width);
        for (int x = ax; x <= bx; x++) {
            row_pixels[x] = color & 0x00FFFFFF;
        }
    }
    return LO_OK;
}

int lo_vcl_draw_blit_rgba(lo_vcl_surface_t *surf, int dst_x, int dst_y, int src_w, int src_h, const uint32_t *rgba_pixels) {
    if (!surf || !surf->pixels || !rgba_pixels || src_w <= 0 || src_h <= 0) return LO_OK;

    for (int r = 0; r < src_h; r++) {
        int py = dst_y + r;
        if (py < 0 || py >= surf->height) continue;
        uint32_t *dst_row = surf->pixels + (py * surf->width);
        const uint32_t *src_row = rgba_pixels + (r * src_w);

        for (int c = 0; c < src_w; c++) {
            int px = dst_x + c;
            if (px < 0 || px >= surf->width) continue;
            uint32_t sp = src_row[c];
            uint32_t a = (sp >> 24) & 0xFF;
            if (a == 0 && (sp & 0x00FFFFFF) != 0) a = 255;
            if (a == 0) continue;

            if (a == 255) {
                dst_row[px] = sp & 0x00FFFFFF;
            } else {
                uint32_t bg = dst_row[px];
                uint32_t inv_a = 255 - a;
                uint32_t sr = (sp >> 16) & 0xFF;
                uint32_t sg = (sp >> 8) & 0xFF;
                uint32_t sb = sp & 0xFF;
                uint32_t rv = (sr * a + ((bg >> 16) & 0xFF) * inv_a + 128) >> 8;
                uint32_t gv = (sg * a + ((bg >> 8) & 0xFF) * inv_a + 128) >> 8;
                uint32_t bv = (sb * a + (bg & 0xFF) * inv_a + 128) >> 8;
                dst_row[px] = (rv << 16) | (gv << 8) | bv;
            }
        }
    }
    return LO_OK;
}
