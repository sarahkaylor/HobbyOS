#ifndef GUI_H
#define GUI_H

/*
 * gui.h - Shared toolkit for HobbyOS windowed applications.
 *
 * HobbyOS GUI apps are text-mode programs. They draw by printing text to
 * stdout (captured by the desktop window manager into the window's text
 * buffer) and read input from stdin (fed by the desktop).
 *
 * This library provides:
 *   - Event parsing: arrow keys, ESC-alone, menu selections, mouse clicks
 *   - Window integration: set the window title, opt in to mouse events
 *   - Formatting helpers: integer to string, padded numbers, bars, boxes
 *   - List navigation state helper (selection + scroll window)
 *   - Small string helpers (HobbyOS libc has almost none)
 *
 * All functions are safe to call from any GUI app. There is no global state
 * that survives between events; apps keep their own state.
 *
 * PROTOCOL NOTES (implemented by the desktop window manager):
 *   - Menu registration (existing): gui_add_menu(idx, name, items) from libc.h
 *   - Window title:    ESC ] T <title> ~        (gui_set_title)
 *   - Mouse enable:    ESC ] P 1 ~              (gui_enable_mouse)
 *   - Mouse press arrives as:    ESC [ P <col> ; <row> ; <btn> ~
 *     where col/row are 0-based text-cell coordinates of the window content
 *     area (same grid print() draws into), btn: 1=left, 2=right.
 *   - Mouse drag (button held, pointer moved) arrives as:
 *     ESC [ G <col> ; <row> ; <btn> ~     (ev.state = GUI_MOUSE_DRAG)
 *   - Mouse release arrives as:
 *     ESC [ R <col> ; <row> ; <btn> ~     (ev.state = GUI_MOUSE_RELEASE)
 *     Coordinates are clamped to the window content area, so a release
 *     outside the window is delivered at the nearest content cell. Apps
 *     that never press get no G/R events.
 *   - Arrow keys arrive as: ESC [ A/B/C/D
 *   - Menu choices arrive as: ESC [ M <menuIdx> ; <itemIdx> ~
 */

#include <stdint.h>

/* ---- Event types returned by gui_read_event() ---- */
#define GUI_EV_NONE   0  /* nothing decoded (internal; keep looping) */
#define GUI_EV_CHAR   1  /* printable char in ev.ch; also '\n' and '\b' */
#define GUI_EV_UP     2
#define GUI_EV_DOWN   3
#define GUI_EV_LEFT   4
#define GUI_EV_RIGHT  5
#define GUI_EV_ESC    6  /* ESC pressed alone (command mode toggle) */
#define GUI_EV_MENU   7  /* menu selection: ev.menu, ev.item */
#define GUI_EV_MOUSE  8  /* mouse press/drag/release: ev.x, ev.y, ev.button, ev.state */

/* ev.state values for GUI_EV_MOUSE: which half of a click this is. */
#define GUI_MOUSE_PRESS   0  /* button went down on this cell          */
#define GUI_MOUSE_DRAG    1  /* button held, pointer moved to this cell */
#define GUI_MOUSE_RELEASE 2  /* button went up on this cell             */

struct gui_event {
    int type;
    int ch;      /* GUI_EV_CHAR: the character */
    int menu;    /* GUI_EV_MENU: menu index (0-based) */
    int item;    /* GUI_EV_MENU: item index (0-based) */
    int x;       /* GUI_EV_MOUSE: column (text cells, window-relative) */
    int y;       /* GUI_EV_MOUSE: row (text cells, window-relative) */
    int button;  /* GUI_EV_MOUSE: 1=left, 2=right */
    int state;   /* GUI_EV_MOUSE: GUI_MOUSE_PRESS / _DRAG / _RELEASE */
};

/* ---- Window integration ---- */

/* Set the window title shown in the title bar. Sends the escape sequence. */
void gui_set_title(const char *title);

/* Ask the desktop to forward mouse clicks in the window content area. */
void gui_enable_mouse(void);

/* ---- Input ---- */

/* Blocking read of the next input event from stdin.
 * Returns 1 when ev was filled in, 0 if a partial/unknown sequence was
 * consumed (caller simply loops again). Never blocks forever: unknown
 * sequences are consumed and reported as 0. */
int gui_read_event(struct gui_event *ev);

/* Like gui_read_event but waits at most `ms` milliseconds; returns 0 on
 * timeout. ms < 0 blocks forever. Useful for live-updating apps (clock,
 * system monitor) that also need to refresh on a timer. */
int gui_read_event_timeout(struct gui_event *ev, int ms);

/* Non-blocking: returns 1 if an event was ready, 0 if not. */
int gui_poll_event(struct gui_event *ev);

/* Pure decoder: parse one event from `buf` (len bytes available).
 * Returns number of bytes consumed (>0), 0 if the sequence is incomplete,
 * or -1 if it is invalid/unknown (consume 1 byte and retry is caller's job).
 * Exposed for host unit tests. */
int gui_decode(const char *buf, int len, struct gui_event *ev);

/* ---- Output helpers ---- */

/* Clear the window (the desktop honors '\f' as clear-screen). */
void gui_clear(void);

/* ---- Number formatting ----
 * All of these write a NUL-terminated string and return its length.
 * Buffers must be at least 24 bytes unless documented otherwise. */

int gui_itoa(long v, char *buf);            /* "-123" */
int gui_uitoa(unsigned long v, char *buf);  /* "123"  */
/* Zero-padded to at least `digits` chars: (7,2) -> "07", (123,2) -> "123" */
int gui_uitoa_z(unsigned long v, char *buf, int digits);
/* Right-align `v` in exactly `w` columns (space padded, may overflow w). */
int gui_itoa_pad(long v, char *out, int w);
/* Human-readable byte size: 0..1023 -> "512B", else "1.2K"/"63.9M"/"1.0G".
 * buf >= 16 bytes. Returns length. */
int gui_size_str(uint64_t bytes, char *buf);

/* ---- Text canvas helpers (all write NUL-terminated strings) ---- */

/* Horizontal rule of `width` chars: "+" + '-'*(width-2) + "+" (width>=4). */
void gui_rule(char *out, int width);
/* Title bar row: "+-- Title " + '-'*rest + "+" (width>=6). Title clipped. */
void gui_box_row(char *out, int width, const char *title);
/* Progress bar exactly `width` chars total: "[" + inner + "]" + " NN%"
 * where inner = width - 6 and the percent is right-aligned in 4 columns
 * (e.g. "  5%", " 50%", "100%"). percent clamped 0..100. */
int gui_bar(char *out, int width, int percent);
/* Copy `text` squeezed/ padded to exactly `width` chars into out. */
void gui_fit(char *out, int width, const char *text);
/* Center `text` within `width` columns (space padded, clipped if longer). */
void gui_center(char *out, int width, const char *text);

/* ---- Scrollable list state ---- */

struct gui_list {
    int selected; /* current selection index, 0-based */
    int top;      /* first visible index */
    int count;    /* total items */
    int visible;  /* visible rows */
};

/* Fix l->top so that l->selected is inside [top, top+visible). */
void gui_list_ensure_visible(struct gui_list *l);
/* Move selection by `delta`, clamp to [0, count-1], keep visible. */
void gui_list_move(struct gui_list *l, int delta);
/* Map a click at content row `y` to an item index given the list's first
 * drawn row `first_row`; returns index or -1 if outside. */
int gui_list_click_row(const struct gui_list *l, int y, int first_row);

/* ---- String helpers (HobbyOS libc has none of these) ---- */

int gui_strlen(const char *s);
void gui_strcpy(char *dst, const char *src);
void gui_strncpy(char *dst, const char *src, int n); /* always NUL-terminates */
int gui_strcmp(const char *a, const char *b);
int gui_strncmp(const char *a, const char *b, int n);
int gui_starts_with(const char *s, const char *prefix);
/* Append src to dst (dst must have room); returns new length. */
int gui_append(char *dst, const char *src);
/* Case-insensitive substring search: returns index or -1. */
int gui_ci_find(const char *hay, const char *needle);
/* Uppercase a copy of src into dst (8.3-friendly). */
void gui_upper(char *dst, const char *src, int max);

#endif /* GUI_H */
