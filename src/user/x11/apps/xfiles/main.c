/*
 * xfiles.c — "Files (X11)": a small GPL file browser for HobbyOS.
 *
 * Copyright (C) 2026 Sarah Kaylor
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * A single-window file browser drawn directly on Xlib, with Xft text and
 * hand-drawn icons: no toolkit, no theme engine, no configuration files.
 * It navigates directories, selects rows, sorts, filters hidden files,
 * opens text files in EDITOR.BIN and hosted programs through HBHOST.BIN,
 * and can create, rename and delete entries.
 *
 * Build (HobbyOS): third_party/x11/build-clients.sh  -> bin/XFILES.BIN
 * Build (host, for a quick look):
 *   cc -std=c99 -O2 -o xfiles xfiles.c $(pkg-config --cflags --libs xft x11)
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* The HobbyOS sysroot's <unistd.h> declares execvp() but not fork(); the
 * symbol itself is in libc.a.  A duplicate declaration is harmless on hosts
 * whose libc does declare it. */
extern pid_t fork(void);

/* ------------------------------------------------------------------ tunables */

#define APP_NAME   "Files (X11)"
#define DEFAULT_W  900
#define DEFAULT_H  620
#define MIN_W      420
#define MIN_H      240

#define TOPBAR_H   30
#define STATBAR_H  24
#define ROW_H      22
#define ICON_SZ    16
#define PAD        10

/* palette (0xRRGGBB) */
#define C_BG       0x24242e
#define C_BAR      0x1b1b24
#define C_TEXT     0xe9e9f2
#define C_DIM      0x9c9cb0
#define C_SEL      0x3b3b5f
#define C_ACCENT   0x8f8fff
#define C_LINE     0x35353f
#define C_FOLDER   0xd9b45c
#define C_FOLDER_D 0xa8873c
#define C_FILE     0xd2d2dc
#define C_FILE_D   0x9090a0
#define C_EXEC     0x7fd07f
#define C_IMG      0x74b8ff
#define C_ARC      0xd08fe0
#define C_TEXTF    0xf0d98a

/* file kinds for icon colouring */
enum { K_PARENT, K_DIR, K_TEXT, K_IMAGE, K_ARCHIVE, K_PROGRAM, K_FILE };

struct ent {
    char *name;
    int   dir;
    long  size;
    long  mtime;
    int   kind;
};

/* prompt modes */
enum { P_NONE, P_PATH, P_MKDIR, P_RENAME, P_DELETE, P_HELP };

/* ------------------------------------------------------------------ state */

static Display      *dpy;
static int           scr;
static Window        win;
static GC            gc;
static Visual       *vis;
static Colormap      cmap;
static int           W = DEFAULT_W, H = DEFAULT_H;

static Pixmap        backbuf;
static int           bb_w, bb_h;
static XftDraw      *xdraw;
static XftFont      *xfont;
static XftColor      xc_text, xc_dim, xc_sel_text, xc_accent;
static XFontStruct  *cfont;          /* fallback when Xft is unavailable */
static int           have_xft;

static unsigned long p_bg, p_bar, p_text, p_dim, p_sel, p_accent, p_line;
static unsigned long p_folder, p_folder_d, p_file, p_file_d, p_exec, p_img, p_arc, p_textf;

static char  cur_path[PATH_MAX] = "/";
static struct ent *ents;
static int   n_ents, cap_ents;
static int   sel, scroll;
static int   sort_mode;              /* 0 name, 1 size, 2 time */
static int   show_hidden;
static char  status[256];
static int   dirty = 1;
static int   prompt_mode = P_NONE;
static char  prompt_buf[PATH_MAX];
static int   prompt_len;
static char  prompt_label[PATH_MAX];

static Atom  wm_delete;
static Atom  net_wm_name;
static Atom  utf8_string;

static pid_t children[8];
static int   n_children;

/* ------------------------------------------------------------------ helpers */

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "xfiles: out of memory\n"); exit(1); }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static unsigned long alloc_pixel(Display *d, int scrn, unsigned long rgb) {
    XColor c;
    c.red   = (unsigned short)(((rgb >> 16) & 0xff) * 257);
    c.green = (unsigned short)(((rgb >> 8) & 0xff) * 257);
    c.blue  = (unsigned short)((rgb & 0xff) * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(d, DefaultColormap(d, scrn), &c)) return c.pixel;
    return BlackPixel(d, scrn);
}

static void fmt_size(char *out, size_t cap, long n) {
    if (n < 0) { snprintf(out, cap, "-"); return; }
    if (n < 1024)              snprintf(out, cap, "%ld B", n);
    else if (n < 1024L * 1024) snprintf(out, cap, "%ld.%ldK", n / 1024, (n % 1024) * 10 / 1024);
    else                       snprintf(out, cap, "%ld.%ldM", n / (1024L * 1024),
                                        (n % (1024L * 1024)) * 10 / (1024L * 1024));
}

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    dirty = 1;
}

/* ------------------------------------------------------------------ text */

static int text_width(const char *s) {
    if (have_xft) {
        XGlyphInfo gi;
        XftTextExtentsUtf8(dpy, xfont, (const FcChar8 *)s, (int)strlen(s), &gi);
        return (int)gi.xOff;
    }
    if (cfont) return XTextWidth(cfont, s, (int)strlen(s));
    return (int)strlen(s) * 6;
}

/* Draw at baseline y. */
static void draw_text(int x, int y, const char *s) {
    if (have_xft) {
        XftDrawStringUtf8(xdraw, &xc_text, xfont, x, y, (const FcChar8 *)s, (int)strlen(s));
    } else if (cfont) {
        XSetForeground(dpy, gc, p_text);
        XDrawString(dpy, backbuf, gc, x, y, s, (int)strlen(s));
    }
}

static void draw_text_dim(int x, int y, const char *s) {
    if (have_xft) {
        XftDrawStringUtf8(xdraw, &xc_dim, xfont, x, y, (const FcChar8 *)s, (int)strlen(s));
    } else if (cfont) {
        XSetForeground(dpy, gc, p_dim);
        XDrawString(dpy, backbuf, gc, x, y, s, (int)strlen(s));
    }
}

static void draw_text_sel(int x, int y, const char *s) {
    if (have_xft) {
        XftDrawStringUtf8(xdraw, &xc_sel_text, xfont, x, y, (const FcChar8 *)s, (int)strlen(s));
    } else if (cfont) {
        XSetForeground(dpy, gc, p_text);
        XDrawString(dpy, backbuf, gc, x, y, s, (int)strlen(s));
    }
}

/* Draw s clipped to maxw pixels, ending in ".." when shortened. */
static void draw_text_clip(int x, int y, int maxw, const char *s, int selected) {
    char buf[512];
    size_t n = strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    if (text_width(buf) > maxw && maxw > text_width("..") + 6) {
        while (n > 0) {
            buf[n] = '\0';
            if (n + 2 < sizeof buf) { buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '\0'; }
            if (text_width(buf) <= maxw) break;
            n--;
        }
    }
    if (selected) draw_text_sel(x, y, buf);
    else          draw_text(x, y, buf);
}

static int font_ascent(void) {
    if (have_xft) return xfont->ascent;
    if (cfont) return cfont->ascent;
    return 8;
}

/* ------------------------------------------------------------------ drawing */

static void fill_rect(int x, int y, int w, int h, unsigned long px) {
    if (w <= 0 || h <= 0) return;
    XSetForeground(dpy, gc, px);
    XFillRectangle(dpy, backbuf, gc, x, y, (unsigned)w, (unsigned)h);
}

static void hline(int x, int y, int w, unsigned long px) { fill_rect(x, y, w, 1, px); }

static void draw_icon(int x, int y, int kind) {
    switch (kind) {
    case K_PARENT: {                       /* up arrow in a folder */
        fill_rect(x, y + 5, 16, 10, p_folder_d);
        fill_rect(x + 1, y + 6, 14, 8, p_folder);
        XSetForeground(dpy, gc, p_bg);
        XFillRectangle(dpy, backbuf, gc, x + 7, y + 7, 2, 5);
        XFillRectangle(dpy, backbuf, gc, x + 5, y + 8, 6, 2);
        break;
    }
    case K_DIR: {
        fill_rect(x, y + 2, 9, 3, p_folder_d);      /* tab */
        fill_rect(x, y + 4, 16, 11, p_folder_d);
        fill_rect(x + 1, y + 5, 14, 9, p_folder);
        hline(x + 1, y + 6, 14, p_bg);
        break;
    }
    case K_IMAGE: {
        fill_rect(x + 1, y + 2, 14, 13, p_file_d);
        fill_rect(x + 2, y + 3, 12, 11, p_img);
        fill_rect(x + 3, y + 11, 10, 2, p_bg);
        fill_rect(x + 4, y + 5, 2, 2, p_bg);
        break;
    }
    case K_ARCHIVE: {
        fill_rect(x + 1, y + 2, 14, 13, p_file_d);
        fill_rect(x + 2, y + 3, 12, 11, p_arc);
        fill_rect(x + 7, y + 4, 2, 8, p_bg);
        break;
    }
    case K_PROGRAM: {
        fill_rect(x + 1, y + 2, 14, 13, p_file_d);
        fill_rect(x + 2, y + 3, 12, 11, p_exec);
        fill_rect(x + 5, y + 6, 6, 2, p_bg);
        fill_rect(x + 5, y + 9, 6, 2, p_bg);
        break;
    }
    case K_TEXT: {
        fill_rect(x + 1, y + 2, 14, 13, p_file_d);
        fill_rect(x + 2, y + 3, 12, 11, p_file);
        for (int i = 0; i < 3; i++) fill_rect(x + 4, y + 6 + i * 3, 8, 1, p_textf);
        break;
    }
    default: {
        fill_rect(x + 1, y + 2, 14, 13, p_file_d);
        fill_rect(x + 2, y + 3, 12, 11, p_file);
        fill_rect(x + 11, y + 3, 3, 3, p_file_d);  /* folded corner */
        break;
    }
    }
}

static int visible_rows(void) {
    int h = H - TOPBAR_H - STATBAR_H;
    int n = h / ROW_H;
    return n < 1 ? 1 : n;
}

static void clamp_view(void) {
    int vis = visible_rows();
    if (sel < 0) sel = 0;
    if (sel >= n_ents) sel = n_ents ? n_ents - 1 : 0;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + vis) scroll = sel - vis + 1;
    if (scroll > n_ents - vis) scroll = n_ents - vis;
    if (scroll < 0) scroll = 0;
}

static const char *kind_label(int kind, int dir, const char *name) {
    if (dir) return "directory";
    switch (kind) {
    case K_TEXT: return "text";
    case K_IMAGE: return "image";
    case K_ARCHIVE: return "archive";
    case K_PROGRAM: return "program";
    default: break;
    }
    if (strchr(name, '.')) return "file";
    return "file";
}

static void paint_scrollbar(int x, int y, int w, int h) {
    int vis = visible_rows();
    if (n_ents <= vis) return;
    fill_rect(x, y, w, h, p_bar);
    int track = h - 4;
    int thumb = track * vis / n_ents;
    if (thumb < 18) thumb = 18;
    int max_scroll = n_ents - vis;
    int off = max_scroll > 0 ? (track - thumb) * scroll / max_scroll : 0;
    fill_rect(x + 1, y + 2 + off, w - 2, thumb, p_dim);
}

static void paint(void) {
    if (!win) return;

    if (!backbuf || bb_w != W || bb_h != H) {
        if (backbuf) { if (xdraw) { XftDrawDestroy(xdraw); xdraw = NULL; } XFreePixmap(dpy, backbuf); backbuf = 0; }
        backbuf = XCreatePixmap(dpy, win, (unsigned)W, (unsigned)H,
                                (unsigned)DefaultDepth(dpy, scr));
        bb_w = W; bb_h = H;
        if (have_xft) xdraw = XftDrawCreate(dpy, backbuf, vis, cmap);
    }
    if (!backbuf) return;

    fill_rect(0, 0, W, H, p_bg);

    /* ---- top bar: current path on the left, entry count on the right */
    fill_rect(0, 0, W, TOPBAR_H, p_bar);
    hline(0, TOPBAR_H - 1, W, p_line);
    {
        char right[64];
        const char *nm = cur_path;
        int rw;
        snprintf(right, sizeof right, "%d item%s", n_ents,
                 n_ents == 1 ? "" : "s");
        rw = text_width(right) + 2 * PAD;
        draw_icon(PAD, 7, K_DIR);
        draw_text_clip(PAD + ICON_SZ + 8, (TOPBAR_H + font_ascent()) / 2 - 2,
                       W - rw - (PAD + ICON_SZ + 16), nm, 0);
        draw_text_dim(W - rw + PAD / 2, (TOPBAR_H + font_ascent()) / 2 - 2, right);
    }

    /* ---- list */
    int vis = visible_rows();
    int list_y = TOPBAR_H;
    int list_h = H - TOPBAR_H - STATBAR_H;
    int bar_w = 10;
    int text_right = W - bar_w - PAD;

    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx >= n_ents) break;
        int y = list_y + i * ROW_H;
        struct ent *e = &ents[idx];
        if (idx == sel) {
            fill_rect(0, y, W, ROW_H, p_sel);
            fill_rect(0, y, 3, ROW_H, p_accent);
        }
        draw_icon(PAD, y + (ROW_H - ICON_SZ) / 2, e->kind);
        int base = y + (ROW_H + font_ascent()) / 2 - 2;

        char sz[32];
        if (!e->dir) fmt_size(sz, sizeof sz, e->size); else sz[0] = '\0';
        int szw = e->dir ? 0 : text_width(sz);

        int name_x = PAD + ICON_SZ + 8;
        int name_max = text_right - name_x - (szw ? szw + 2 * PAD : 0);
        draw_text_clip(name_x, base, name_max, e->name, idx == sel);
        if (szw) {
            if (idx == sel) draw_text_sel(text_right - szw, base, sz);
            else            draw_text_dim(text_right - szw, base, sz);
        }
    }
    if (n_ents == 0) {
        const char *msg = "empty directory";
        draw_text_dim(PAD + ICON_SZ + 8, list_y + 20, msg);
    }

    paint_scrollbar(W - bar_w, list_y, bar_w, list_h);

    /* ---- status bar */
    fill_rect(0, H - STATBAR_H, W, STATBAR_H, p_bar);
    hline(0, H - STATBAR_H, W, p_line);
    {
        char left[512];
        int base = H - STATBAR_H + (STATBAR_H + font_ascent()) / 2 - 2;
        if (status[0]) {
            snprintf(left, sizeof left, "%s", status);
        } else if (n_ents > 0 && sel >= 0 && sel < n_ents) {
            struct ent *e = &ents[sel];
            if (e->dir) {
                snprintf(left, sizeof left, "%s  -  %s", e->name,
                         e->kind == K_PARENT ? "parent directory" : "directory");
            } else {
                char sz[32];
                fmt_size(sz, sizeof sz, e->size);
                snprintf(left, sizeof left, "%s  -  %s, %s", e->name,
                         kind_label(e->kind, 0, e->name), sz);
            }
        } else {
            snprintf(left, sizeof left, "%s", "up/down select  enter open  backspace up  / path  n new  d delete  ? help");
        }
        draw_text_dim(PAD, base, left);

        char pos[48];
        if (n_ents > 0) snprintf(pos, sizeof pos, "%d/%d", sel + 1, n_ents);
        else            snprintf(pos, sizeof pos, "0/0");
        draw_text_dim(W - text_width(pos) - PAD, base, pos);
    }

    /* ---- prompt / help overlay */
    if (prompt_mode != P_NONE) {
        if (prompt_mode == P_HELP) {
            int bw = 460, bh = 250;
            int bx = (W - bw) / 2, by = (H - bh) / 2;
            fill_rect(bx, by, bw, bh, p_bar);
            fill_rect(bx, by, bw, 1, p_accent);
            fill_rect(bx, by + bh - 1, bw, 1, p_accent);
            int y = by + 26;
            draw_text(bx + 16, y, "Files (X11) - keys");
            y += 8;
            static const char *lines[] = {
                "up / down        move selection",
                "enter, right     open directory or file",
                "left, backspace  go to parent directory",
                "home / end       first / last entry",
                "page up/down     scroll a page",
                "mouse wheel      scroll",
                "/                type a path",
                "n                new folder",
                "d                delete selected",
                "f2               rename selected",
                "s                sort: name, size, time",
                "h                show/hide dot files",
                "r                refresh",
                "?                this help",
                "esc, q           close help / quit",
            };
            for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) {
                y += 14;
                draw_text_dim(bx + 16, y, lines[i]);
            }
        } else {
            int bh = 34;
            int by = H - STATBAR_H - bh - 6;
            fill_rect(0, by, W, bh, p_bar);
            hline(0, by, W, p_accent);
            int base = by + (bh + font_ascent()) / 2 - 2;
            char line[PATH_MAX + 64];
            snprintf(line, sizeof line, "%s%s", prompt_label, prompt_buf);
            draw_text(PAD, base, line);
            int cx = PAD + text_width(prompt_label) + text_width(prompt_buf) + 2;
            fill_rect(cx, base - font_ascent(), 2, font_ascent() + 3, p_accent);
        }
    }

    XCopyArea(dpy, backbuf, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
    XFlush(dpy);
}

/* ------------------------------------------------------------------ dir model */

static int kind_of(const char *name, int is_dir) {
    if (is_dir) return K_DIR;
    const char *dot = strrchr(name, '.');
    if (!dot) return K_FILE;
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".md") || !strcasecmp(dot, ".log") ||
        !strcasecmp(dot, ".c")   || !strcasecmp(dot, ".h")  || !strcasecmp(dot, ".ini") ||
        !strcasecmp(dot, ".cfg") || !strcasecmp(dot, ".conf") || !strcasecmp(dot, ".sh"))
        return K_TEXT;
    if (!strcasecmp(dot, ".bin") || !strcasecmp(dot, ".elf") || !strcasecmp(dot, ".exe"))
        return K_PROGRAM;
    if (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".bmp") ||
        !strcasecmp(dot, ".ppm") || !strcasecmp(dot, ".gif"))
        return K_IMAGE;
    if (!strcasecmp(dot, ".zip") || !strcasecmp(dot, ".tar") || !strcasecmp(dot, ".gz") ||
        !strcasecmp(dot, ".xz") || !strcasecmp(dot, ".bz2"))
        return K_ARCHIVE;
    return K_FILE;
}

static int cmp_ents(const void *va, const void *vb) {
    const struct ent *a = va, *b = vb;
    if (a->kind == K_PARENT) return -1;
    if (b->kind == K_PARENT) return 1;
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    switch (sort_mode) {
    case 1:
        if (a->size != b->size) return a->size > b->size ? -1 : 1;
        break;
    case 2:
        if (a->mtime != b->mtime) return a->mtime > b->mtime ? -1 : 1;
        break;
    default:
        break;
    }
    return strcasecmp(a->name, b->name);
}

static void free_ents(void) {
    for (int i = 0; i < n_ents; i++) free(ents[i].name);
    free(ents);
    ents = NULL;
    n_ents = cap_ents = 0;
}

static void add_ent(const char *name, int dir, long size, long mtime, int kind) {
    if (n_ents == cap_ents) {
        cap_ents = cap_ents ? cap_ents * 2 : 64;
        struct ent *ne = realloc(ents, (size_t)cap_ents * sizeof *ne);
        if (!ne) { set_status("out of memory"); return; }
        ents = ne;
    }
    ents[n_ents].name  = xstrdup(name);
    ents[n_ents].dir   = dir;
    ents[n_ents].size  = size;
    ents[n_ents].mtime = mtime;
    ents[n_ents].kind  = kind;
    n_ents++;
}

static void reload(int keep_selection) {
    char keep[PATH_MAX];
    keep[0] = '\0';
    if (keep_selection && sel >= 0 && sel < n_ents)
        snprintf(keep, sizeof keep, "%s", ents[sel].name);

    free_ents();

    DIR *d = opendir(".");
    if (!d) {
        set_status("cannot open directory (%s)", strerror(errno));
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (!nm[0]) continue;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (!show_hidden && nm[0] == '.') continue;
        struct stat st;
        if (stat(nm, &st) != 0) continue;
        int isdir = S_ISDIR(st.st_mode);
        add_ent(nm, isdir, (long)st.st_size, (long)st.st_mtime,
                kind_of(nm, isdir));
    }
    closedir(d);

    if (strcmp(cur_path, "/") != 0)
        add_ent("..", 1, 0, 0, K_PARENT);

    qsort(ents, (size_t)n_ents, sizeof *ents, cmp_ents);

    sel = 0;
    if (keep[0]) {
        for (int i = 0; i < n_ents; i++)
            if (!strcmp(ents[i].name, keep)) { sel = i; break; }
    }
    clamp_view();
    dirty = 1;
}

static void select_name(const char *name) {
    for (int i = 0; i < n_ents; i++) {
        if (!strcmp(ents[i].name, name)) {
            sel = i;
            clamp_view();
            dirty = 1;
            return;
        }
    }
}

static int go_to(const char *path) {
    if (chdir(path) != 0) {
        set_status("cannot open %s (%s)", path, strerror(errno));
        fprintf(stderr, "xfiles: enter %s failed (%s)\n", path, strerror(errno));
        return 0;
    }
    if (!getcwd(cur_path, sizeof cur_path)) snprintf(cur_path, sizeof cur_path, "%s", path);
    reload(0);
    status[0] = '\0';
    fprintf(stderr, "xfiles: cwd %s\n", cur_path);
    return 1;
}

/* ------------------------------------------------------------------ actions */

static void track_child(pid_t p) {
    for (int i = 0; i < n_children; i++) if (children[i] == p) return;
    if (n_children < (int)(sizeof children / sizeof children[0])) children[n_children++] = p;
}

static void reap_children(void) {
    for (int i = 0; i < n_children; ) {
        int st = 0;
        pid_t r = waitpid(children[i], &st, WNOHANG);
        if (r == children[i] || r < 0) {
            children[i] = children[--n_children];
        } else {
            i++;
        }
    }
}

static int spawn(char *const argv[]) {
    pid_t p = fork();
    if (p < 0) { set_status("fork failed (%s)", strerror(errno)); return 0; }
    if (p == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    track_child(p);
    return 1;
}

static int is_text_file(const char *name) {
    return kind_of(name, 0) == K_TEXT;
}

static void open_selected(void) {
    if (n_ents == 0) return;
    struct ent *e = &ents[sel];
    if (e->kind == K_PARENT) { go_to(".."); return; }
    if (e->dir) { go_to(e->name); return; }

    char abs[PATH_MAX];
    if (cur_path[strlen(cur_path) - 1] == '/')
        snprintf(abs, sizeof abs, "%s%s", cur_path, e->name);
    else
        snprintf(abs, sizeof abs, "%s/%s", cur_path, e->name);

    if (e->kind == K_PROGRAM) {
        char *argv[3] = { (char *)"/HBHOST.BIN", abs, NULL };
        if (spawn(argv)) set_status("launching %s", e->name);
        fprintf(stderr, "xfiles: open %s as program\n", abs);
        return;
    }
    if (is_text_file(e->name)) {
        char *argv[4] = { (char *)"/HBHOST.BIN", (char *)"/EDITOR.BIN", abs, NULL };
        if (spawn(argv)) set_status("opening %s in the editor", e->name);
        fprintf(stderr, "xfiles: open %s in editor\n", abs);
        return;
    }
    set_status("no handler for %s", e->name);
    fprintf(stderr, "xfiles: open %s no handler\n", abs);
}

static void do_mkdir(const char *name) {
    if (!name[0]) return;
    if (mkdir(name, 0755) != 0) {
        set_status("mkdir %s failed (%s)", name, strerror(errno));
        fprintf(stderr, "xfiles: mkdir %s failed (%s)\n", name, strerror(errno));
    } else {
        set_status("created %s", name);
        fprintf(stderr, "xfiles: mkdir %s ok\n", name);
        reload(0);
        select_name(name);
    }
}

static void do_rename(const char *newname) {
    if (!newname[0] || sel < 0 || sel >= n_ents) return;
    const char *old = ents[sel].name;
    if (!strcmp(old, "..")) return;
    if (rename(old, newname) != 0) {
        set_status("rename failed (%s)", strerror(errno));
        fprintf(stderr, "xfiles: rename %s -> %s failed (%s)\n", old, newname, strerror(errno));
    } else {
        set_status("renamed to %s", newname);
        fprintf(stderr, "xfiles: rename %s -> %s ok\n", old, newname);
        reload(0);
        select_name(newname);
    }
}

static void do_delete(void) {  /* logs the outcome */
    if (sel < 0 || sel >= n_ents) return;
    struct ent *e = &ents[sel];
    if (!strcmp(e->name, "..")) return;
    char doomed[PATH_MAX];
    snprintf(doomed, sizeof doomed, "%s", e->name);
    int rc = e->dir ? rmdir(doomed) : unlink(doomed);
    if (rc != 0) {
        set_status("delete failed (%s)", strerror(errno));
        fprintf(stderr, "xfiles: delete %s failed (%s)\n", doomed, strerror(errno));
    } else {
        set_status("deleted %s", doomed);
        fprintf(stderr, "xfiles: delete %s ok\n", doomed);
        reload(0);
    }
}

static void move_sel(int delta) {
    sel += delta;
    clamp_view();
    dirty = 1;
}

static void sort_cycle(void) {
    sort_mode = (sort_mode + 1) % 3;
    static const char *names[] = { "name", "size", "time" };
    qsort(ents, (size_t)n_ents, sizeof *ents, cmp_ents);
    set_status("sorted by %s", names[sort_mode]);
    clamp_view();
}

static void type_ahead(int ch) {
    int start = sel + 1;
    for (int k = 0; k < n_ents; k++) {
        int i = (start + k) % n_ents;
        if (tolower((unsigned char)ents[i].name[0]) == tolower(ch)) {
            sel = i; clamp_view(); dirty = 1; return;
        }
    }
}

static void prompt_begin(int mode, const char *label, const char *initial) {
    static const char *const pm_name[] = { "none", "path", "mkdir", "rename",
                                           "delete", "help" };
    if (mode >= 0 && mode < (int)(sizeof pm_name / sizeof pm_name[0]))
        fprintf(stderr, "xfiles: prompt %s\n", pm_name[mode]);
    prompt_mode = mode;
    snprintf(prompt_label, sizeof prompt_label, "%s", label);
    snprintf(prompt_buf, sizeof prompt_buf, "%s", initial ? initial : "");
    prompt_len = (int)strlen(prompt_buf);
    dirty = 1;
}

static void prompt_finish(int accept) {
    int mode = prompt_mode;
    prompt_mode = P_NONE;
    if (!accept) { dirty = 1; return; }
    switch (mode) {
    case P_PATH:    if (prompt_buf[0]) go_to(prompt_buf); break;
    case P_MKDIR:   do_mkdir(prompt_buf); break;
    case P_RENAME:  do_rename(prompt_buf); break;
    case P_DELETE:
        if (prompt_buf[0] == 'y' || prompt_buf[0] == 'Y') do_delete();
        else set_status("delete cancelled");
        break;
    default: break;
    }
    dirty = 1;
}

/* ------------------------------------------------------------------ events */

static void on_key(XKeyEvent *ke) {
    char buf[32];
    KeySym ks = 0;
    int n = XLookupString(ke, buf, sizeof buf - 1, &ks, NULL);
    buf[n > 0 ? n : 0] = '\0';

    if (prompt_mode == P_HELP) {
        if (ks == XK_Escape || ks == XK_q || ks == XK_Return || ks == XK_space) {
            prompt_mode = P_NONE;
            dirty = 1;
        }
        return;
    }
    if (prompt_mode != P_NONE) {
        if (ks == XK_Escape) { prompt_finish(0); return; }
        if (ks == XK_Return || ks == XK_KP_Enter) { prompt_finish(1); return; }
        if (ks == XK_BackSpace) {
            if (prompt_len > 0) prompt_buf[--prompt_len] = '\0';
            dirty = 1; return;
        }
        if (n > 0 && isprint((unsigned char)buf[0]) && prompt_len + 1 < (int)sizeof prompt_buf) {
            prompt_buf[prompt_len++] = buf[0];
            prompt_buf[prompt_len] = '\0';
            dirty = 1;
        }
        return;
    }

    switch (ks) {
    case XK_Up:        move_sel(-1); break;
    case XK_Down:      move_sel(+1); break;
    case XK_Home:      sel = 0; clamp_view(); dirty = 1; break;
    case XK_End:       sel = n_ents - 1; clamp_view(); dirty = 1; break;
    case XK_Page_Up:   move_sel(-visible_rows()); break;
    case XK_Page_Down: move_sel(+visible_rows()); break;
    case XK_Return:
    case XK_KP_Enter:
    case XK_Right:     open_selected(); break;
    case XK_Left:
    case XK_BackSpace: go_to(".."); break;
    case XK_slash:     prompt_begin(P_PATH, "path: ", cur_path); break;
    case XK_n:         prompt_begin(P_MKDIR, "new folder: ", ""); break;
    case XK_d: {
        if (sel >= 0 && sel < n_ents && strcmp(ents[sel].name, "..")) {
            char label[PATH_MAX];
            snprintf(label, sizeof label, "delete %s? [y/N] ", ents[sel].name);
            prompt_begin(P_DELETE, label, "");
        }
        break;
    }
    case XK_F2: {
        if (sel >= 0 && sel < n_ents && strcmp(ents[sel].name, ".."))
            prompt_begin(P_RENAME, "rename to: ", ents[sel].name);
        break;
    }
    case XK_s:         sort_cycle(); break;
    case XK_h:         show_hidden = !show_hidden; reload(1);
                       set_status(show_hidden ? "showing dot files" : "hiding dot files"); break;
    case XK_r:         reload(1); set_status("refreshed"); break;
    case XK_F1:
    case XK_question:  prompt_mode = P_HELP; dirty = 1; break;
    case XK_Escape:    prompt_mode = P_NONE; status[0] = '\0'; dirty = 1; break;
    case XK_q:
    case XK_Q:         if (ke->state & ControlMask) { XDestroyWindow(dpy, win); }
                       else if (n > 0 && buf[0]) type_ahead(buf[0]);
                       break;
    default:
        if (n > 0 && isprint((unsigned char)buf[0])) type_ahead(buf[0]);
        break;
    }
}

static void on_button(XButtonEvent *be) {
    if (prompt_mode == P_HELP) { prompt_mode = P_NONE; dirty = 1; return; }

    if (be->button == Button4) { scroll -= 3; clamp_view(); dirty = 1; return; }
    if (be->button == Button5) { scroll += 3; clamp_view(); dirty = 1; return; }
    if (be->button != Button1) return;

    if (be->y < TOPBAR_H || be->y >= H - STATBAR_H) return;
    int row = (be->y - TOPBAR_H) / ROW_H;
    int idx = scroll + row;
    if (idx < 0 || idx >= n_ents) return;

    static Time last_time;
    static int  last_idx = -1;
    int dbl = (idx == last_idx && be->time - last_time < 400);

    sel = idx;
    clamp_view();
    dirty = 1;
    if (dbl) { open_selected(); last_idx = -1; }
    else     { last_idx = idx; last_time = be->time; }
}

static void on_configure(XConfigureEvent *ce) {
    if (ce->width != W || ce->height != H)
        fprintf(stderr, "xfiles: geom %dx%d+%d+%d\n", ce->width, ce->height,
                ce->x, ce->y);
    if (ce->width == W && ce->height == H) return;
    W = ce->width < MIN_W ? MIN_W : ce->width;
    H = ce->height < MIN_H ? MIN_H : ce->height;
    clamp_view();
    dirty = 1;
}

/* ------------------------------------------------------------------ main */

static void setup_colors(void) {
    p_bg       = alloc_pixel(dpy, scr, C_BG);
    p_bar      = alloc_pixel(dpy, scr, C_BAR);
    p_text     = alloc_pixel(dpy, scr, C_TEXT);
    p_dim      = alloc_pixel(dpy, scr, C_DIM);
    p_sel      = alloc_pixel(dpy, scr, C_SEL);
    p_accent   = alloc_pixel(dpy, scr, C_ACCENT);
    p_line     = alloc_pixel(dpy, scr, C_LINE);
    p_folder   = alloc_pixel(dpy, scr, C_FOLDER);
    p_folder_d = alloc_pixel(dpy, scr, C_FOLDER_D);
    p_file     = alloc_pixel(dpy, scr, C_FILE);
    p_file_d   = alloc_pixel(dpy, scr, C_FILE_D);
    p_exec     = alloc_pixel(dpy, scr, C_EXEC);
    p_img      = alloc_pixel(dpy, scr, C_IMG);
    p_arc      = alloc_pixel(dpy, scr, C_ARC);
    p_textf    = alloc_pixel(dpy, scr, C_TEXTF);

    if (have_xft) {
        XRenderColor rc;
#define ALLOC_XC(dst, rgb) do {                       \
        rc.red   = (unsigned short)((((rgb) >> 16) & 0xff) * 257); \
        rc.green = (unsigned short)((((rgb) >> 8) & 0xff) * 257);  \
        rc.blue  = (unsigned short)(((rgb) & 0xff) * 257);         \
        rc.alpha = 0xffff;                            \
        XftColorAllocValue(dpy, vis, cmap, &rc, &(dst)); \
    } while (0)
        ALLOC_XC(xc_text, C_TEXT);
        ALLOC_XC(xc_dim, C_DIM);
        ALLOC_XC(xc_sel_text, C_TEXT);
        ALLOC_XC(xc_accent, C_ACCENT);
#undef ALLOC_XC
    }
}

/* Open a font with grayscale antialiasing: subpixel RGB fringing looks
 * terrible on a dark background (and on a 32-bit XRender surface it is
 * pure noise), so ask fontconfig for FC_RGBA_NONE explicitly. */
static XftFont *open_font(const char *name) {
    FcPattern *pat = FcNameParse((const FcChar8 *)name);
    XftFont *f = NULL;
    if (!pat) return NULL;
    FcPatternAddInteger(pat, FC_RGBA, FC_RGBA_NONE);
    FcPatternAddBool(pat, FC_ANTIALIAS, FcTrue);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    f = XftFontOpenPattern(dpy, pat);
    if (!f) FcPatternDestroy(pat);
    return f;
}

static int init_text(void) {
    const char *pattern = getenv("XFILES_FONT");
    if (!pattern || !pattern[0]) pattern = "DejaVu Sans-12";

    FcInit();
    xfont = open_font(pattern);
    if (!xfont) xfont = open_font("DejaVu Sans:size=11");
    if (!xfont) xfont = open_font("sans-11");
    if (!xfont) xfont = XftFontOpenName(dpy, scr, "DejaVu Sans-11");
    if (!xfont) xfont = XftFontOpenName(dpy, scr, "sans-11");
    if (!xfont) xfont = XftFontOpenName(dpy, scr, "fixed");
    if (xfont) {
        have_xft = 1;
        fprintf(stderr, "xfiles: xft font ok (%dx%d)\n", xfont->max_advance_width, xfont->ascent);
        return 1;
    }
    fprintf(stderr, "xfiles: xft unavailable, falling back to the core font\n");
    cfont = XLoadQueryFont(dpy, "fixed");
    if (!cfont) cfont = XLoadQueryFont(dpy, "6x13");
    return cfont != NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1 && argv[1][0]) {
        if (chdir(argv[1]) != 0) {
            fprintf(stderr, "xfiles: cannot enter %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
    }
    if (!getcwd(cur_path, sizeof cur_path)) snprintf(cur_path, sizeof cur_path, "/");

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "xfiles: cannot open display\n"); return 1; }
    scr  = DefaultScreen(dpy);
    vis  = DefaultVisual(dpy, scr);
    cmap = DefaultColormap(dpy, scr);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 60, 60,
                              (unsigned)DEFAULT_W, (unsigned)DEFAULT_H, 0,
                              BlackPixel(dpy, scr), C_BG);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask |
                           ButtonReleaseMask | StructureNotifyMask |
                           PointerMotionMask);
    XStoreName(dpy, win, APP_NAME);

    net_wm_name   = XInternAtom(dpy, "_NET_WM_NAME", False);
    utf8_string   = XInternAtom(dpy, "UTF8_STRING", False);
    wm_delete     = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    {
        const char *t = APP_NAME;
        XChangeProperty(dpy, win, net_wm_name, utf8_string, 8, PropModeReplace,
                        (const unsigned char *)t, (int)strlen(t));
    }
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    {
        XSizeHints sh;
        memset(&sh, 0, sizeof sh);
        sh.flags = PMinSize;
        sh.min_width = MIN_W;
        sh.min_height = MIN_H;
        XSetWMNormalHints(dpy, win, &sh);
    }
    {
        XClassHint ch;
        ch.res_name = (char *)"xfiles";
        ch.res_class = (char *)"XFiles";
        XSetClassHint(dpy, win, &ch);
    }
    gc = XCreateGC(dpy, win, 0, NULL);

    if (!init_text()) { fprintf(stderr, "xfiles: no usable font\n"); return 1; }
    setup_colors();

    XMapWindow(dpy, win);
    XFlush(dpy);

    reload(0);
    fprintf(stderr, "xfiles: cwd %s\n", cur_path);
    snprintf(status, sizeof status, "%s", "");

    for (;;) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                dirty = 1;
                break;
            case KeyPress:
                on_key(&ev.xkey);
                break;
            case ButtonPress:
                on_button(&ev.xbutton);
                break;
            case ConfigureNotify:
                on_configure(&ev.xconfigure);
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete) {
                    XCloseDisplay(dpy);
                    return 0;
                }
                break;
            default:
                break;
            }
        }
        reap_children();
        if (dirty) { dirty = 0; paint(); }
        {
            XEvent ev;
            XNextEvent(dpy, &ev);          /* block for the next event */
            switch (ev.type) {
            case Expose:          dirty = 1; break;
            case KeyPress:        on_key(&ev.xkey); break;
            case ButtonPress:     on_button(&ev.xbutton); break;
            case ConfigureNotify: on_configure(&ev.xconfigure); break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete) {
                    XCloseDisplay(dpy);
                    return 0;
                }
                break;
            default: break;
            }
        }
    }
    return 0;
}