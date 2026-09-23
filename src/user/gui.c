/*
 * gui.c - Shared toolkit for HobbyOS windowed applications.
 *
 * Compiled for both the ARM userland (aarch64-none-elf) and the host test
 * environment (HOST_TEST). See gui.h for the full API contract.
 */

#include "gui.h"
#include "libc.h"

/* ================================================================== */
/* Window integration                                                  */
/* ================================================================== */

void gui_set_title(const char *title) {
    print("\033]T");
    print(title);
    print("~");
}

void gui_enable_mouse(void) {
    print("\033]P1~");
}

void gui_clear(void) {
    print("\f");
}

/* ================================================================== */
/* String helpers                                                      */
/* ================================================================== */

int gui_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

void gui_strcpy(char *dst, const char *src) {
    int i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void gui_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int gui_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int gui_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

int gui_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

int gui_append(char *dst, const char *src) {
    int n = gui_strlen(dst);
    int i = 0;
    while (src[i]) { dst[n + i] = src[i]; i++; }
    dst[n + i] = '\0';
    return n + i;
}

static int ci_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

int gui_ci_find(const char *hay, const char *needle) {
    if (!needle[0]) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] &&
               ci_lower((unsigned char)hay[i + j]) == ci_lower((unsigned char)needle[j])) {
            j++;
        }
        if (!needle[j]) return i;
    }
    return -1;
}

void gui_upper(char *dst, const char *src, int max) {
    int i = 0;
    for (; src[i] && i < max - 1; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = c - 32;
        dst[i] = c;
    }
    dst[i] = '\0';
}

/* ================================================================== */
/* Number formatting                                                   */
/* ================================================================== */

int gui_uitoa(unsigned long v, char *buf) {
    char tmp[24];
    int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    while (v > 0) { tmp[i++] = '0' + (int)(v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return j;
}

int gui_itoa(long v, char *buf) {
    if (v < 0) {
        unsigned long uv = (unsigned long)(-(v + 1)) + 1UL; /* safe for LONG_MIN */
        buf[0] = '-';
        return 1 + gui_uitoa(uv, buf + 1);
    }
    return gui_uitoa((unsigned long)v, buf);
}

int gui_uitoa_z(unsigned long v, char *buf, int digits) {
    char tmp[24];
    int n = gui_uitoa(v, tmp);
    int j = 0;
    for (int i = n; i < digits; i++) buf[j++] = '0';
    for (int i = 0; i < n; i++) buf[j++] = tmp[i];
    buf[j] = '\0';
    return j;
}

int gui_itoa_pad(long v, char *out, int w) {
    char tmp[24];
    int n = gui_itoa(v, tmp);
    int j = 0;
    for (int i = n; i < w; i++) out[j++] = ' ';
    for (int i = 0; i < n; i++) out[j++] = tmp[i];
    out[j] = '\0';
    return j;
}

int gui_size_str(uint64_t bytes, char *buf) {
    if (bytes < 1024ULL) {
        int n = gui_uitoa((unsigned long)bytes, buf);
        buf[n] = 'B';
        buf[n + 1] = '\0';
        return n + 1;
    }
    const char *suffix;
    uint64_t base;
    if (bytes < 1024ULL * 1024ULL) { suffix = "K"; base = 1024ULL; }
    else if (bytes < 1024ULL * 1024ULL * 1024ULL) { suffix = "M"; base = 1024ULL * 1024ULL; }
    else { suffix = "G"; base = 1024ULL * 1024ULL * 1024ULL; }

    /* One decimal place, e.g. "1.5K", "64.0M", "2.0G". */
    uint64_t whole = bytes / base;
    uint64_t frac = (bytes % base) * 10ULL / base;
    int j = gui_uitoa((unsigned long)whole, buf);
    buf[j++] = '.';
    buf[j++] = '0' + (int)frac;
    buf[j++] = suffix[0];
    buf[j] = '\0';
    return j;
}

/* ================================================================== */
/* Text canvas helpers                                                 */
/* ================================================================== */

void gui_rule(char *out, int width) {
    if (width < 4) width = 4;
    int j = 0;
    out[j++] = '+';
    for (int i = 0; i < width - 2; i++) out[j++] = '-';
    out[j++] = '+';
    out[j] = '\0';
}

void gui_box_row(char *out, int width, const char *title) {
    if (width < 6) width = 6;
    int j = 0;
    out[j++] = '+';
    out[j++] = '-';
    out[j++] = '-';
    out[j++] = ' ';
    int tlen = gui_strlen(title);
    if (tlen > width - 8) tlen = width - 8;
    for (int i = 0; i < tlen; i++) out[j++] = title[i];
    out[j++] = ' ';
    int rest = width - 2 - (j - 1); /* fill until second-to-last column */
    for (int i = 0; i < rest; i++) out[j++] = '-';
    out[j++] = '+';
    out[j] = '\0';
}

int gui_bar(char *out, int width, int percent) {
    /* Layout: "[" + inner + "]" + pct4
     * inner = width - 6; pct4 is always 4 chars: "  5%", " 50%", "100%". */
    if (width < 10) width = 10;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int inner = width - 6;
    int fill = inner * percent / 100;
    int j = 0;
    out[j++] = '[';
    for (int i = 0; i < fill; i++) out[j++] = '#';
    for (int i = fill; i < inner; i++) out[j++] = ' ';
    out[j++] = ']';
    if (percent == 100) { out[j++] = '1'; out[j++] = '0'; out[j++] = '0'; out[j++] = '%'; }
    else {
        out[j++] = ' ';
        if (percent < 10) out[j++] = ' ';
        out[j++] = '0' + (percent >= 10 ? percent / 10 : percent % 10);
        if (percent >= 10) {
            out[j++] = '0' + percent % 10;
            /* out already advanced for tens; rewrite: handled below */
        }
        out[j++] = '%';
    }
    out[j] = '\0';
    return j;
}

void gui_fit(char *out, int width, const char *text) {
    int i = 0;
    for (; i < width && text[i]; i++) out[i] = text[i];
    for (; i < width; i++) out[i] = ' ';
    out[width] = '\0';
}

void gui_center(char *out, int width, const char *text) {
    int tlen = gui_strlen(text);
    if (tlen > width) tlen = width;
    int left = (width - tlen) / 2;
    int j = 0;
    for (int i = 0; i < left; i++) out[j++] = ' ';
    for (int i = 0; i < tlen; i++) out[j++] = text[i];
    while (j < width) out[j++] = ' ';
    out[j] = '\0';
}

/* ================================================================== */
/* Scrollable list state                                               */
/* ================================================================== */

void gui_list_ensure_visible(struct gui_list *l) {
    if (l->visible <= 0) l->visible = 1;
    if (l->selected < l->top) l->top = l->selected;
    if (l->selected >= l->top + l->visible) l->top = l->selected - l->visible + 1;
    if (l->top < 0) l->top = 0;
    if (l->top > l->count - l->visible && l->count > l->visible)
        l->top = l->count - l->visible;
    if (l->top < 0) l->top = 0;
}

void gui_list_move(struct gui_list *l, int delta) {
    l->selected += delta;
    if (l->selected < 0) l->selected = 0;
    if (l->selected > l->count - 1) l->selected = l->count - 1;
    if (l->selected < 0) l->selected = 0;
    gui_list_ensure_visible(l);
}

int gui_list_click_row(const struct gui_list *l, int y, int first_row) {
    int idx = l->top + (y - first_row);
    if (y < first_row || idx < l->top || idx >= l->count) return -1;
    if (idx >= l->top + l->visible) return -1;
    return idx;
}

/* ================================================================== */
/* Event decoding                                                      */
/* ================================================================== */

/* Parse an unsigned decimal from s (up to maxlen bytes, stops at non-digit).
 * Returns 0 on success (with *out and *used set), 1 if more digits may come
 * (caller should read more), -1 on syntax error. */
static int parse_uint(const char *s, int maxlen, int *out, int *used) {
    int v = 0;
    int i = 0;
    if (maxlen <= 0) return 1;
    while (i < maxlen && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        if (v > 100000) return -1;
        i++;
    }
    if (i == 0) return -1;
    *out = v;
    *used = i;
    return 0;
}

int gui_decode(const char *buf, int len, struct gui_event *ev) {
    ev->type = GUI_EV_NONE;
    if (len <= 0) return 0;

    unsigned char c0 = (unsigned char)buf[0];
    if (c0 != 27) {
        ev->type = GUI_EV_CHAR;
        ev->ch = c0;
        return 1;
    }
    if (len < 2) return 0;
    if (buf[1] != '[') {
        /* Not a CSI sequence; treat the ESC itself as an event and let the
         * caller re-process the next byte. */
        ev->type = GUI_EV_ESC;
        return 1;
    }
    if (len < 3) return 0;
    char k = buf[2];

    if (k == 'A' || k == 'B' || k == 'C' || k == 'D') {
        ev->type = (k == 'A') ? GUI_EV_UP : (k == 'B') ? GUI_EV_DOWN
                 : (k == 'C') ? GUI_EV_RIGHT : GUI_EV_LEFT;
        return 3;
    }

    /* Menu selection: ESC [ M <menu> ; <item> ~  (payload digits, then '~')
     * Mouse events:   ESC [ P/G/R <col> ; <row> ; <btn> ~
     *   P = press, G = drag (button held), R = release. */
    if (k == 'M' || k == 'P' || k == 'G' || k == 'R') {
        int p[3] = {0, 0, 0};
        int np = (k == 'M') ? 2 : 3;
        int i = 3; /* index into buf */
        for (int n = 0; n < np; n++) {
            int used = 0;
            int r = parse_uint(buf + i, len - i, &p[n], &used);
            if (r == -1) return -1;          /* bad syntax */
            if (r == 1) {
                if (len - i >= 12) return -1; /* runaway */
                return 0;                     /* need more bytes */
            }
            i += used;
            if (n < np - 1) {
                if (i >= len) return 0;       /* need separator */
                if (buf[i] != ';') return -1;
                i++;
            }
        }
        if (i >= len) return 0;               /* need terminator */
        if (buf[i] != '~') return -1;
        i++;
        if (k == 'M') {
            ev->type = GUI_EV_MENU;
            ev->menu = p[0];
            ev->item = p[1];
        } else {
            ev->type = GUI_EV_MOUSE;
            ev->x = p[0];
            ev->y = p[1];
            ev->button = p[2];
            ev->state = (k == 'P') ? GUI_MOUSE_PRESS
                      : (k == 'G') ? GUI_MOUSE_DRAG
                      :              GUI_MOUSE_RELEASE;
        }
        return i;
    }

    return -1;
}

/* Internal read loop. block_ms < 0 = block forever; 0 = poll only. */
static int gui_read_internal(struct gui_event *ev, int block_ms) {
    static char rxbuf[48];
    static int rxlen = 0;

    int waited = 0;
    for (;;) {
        if (rxlen > 0) {
            int n = gui_decode(rxbuf, rxlen, ev);
            if (n > 0) {
                for (int i = n; i < rxlen; i++) rxbuf[i - n] = rxbuf[i];
                rxlen -= n;
                return 1;
            }
            if (n < 0) {
                /* Invalid sequence: drop one byte and retry. */
                for (int i = 1; i < rxlen; i++) rxbuf[i - 1] = rxbuf[i];
                rxlen--;
                continue;
            }
            /* n == 0: incomplete, need more bytes below. */
        }

        int avail = available(0);
        if (avail > 0) {
            int want = 48 - rxlen;
            if (want <= 0) { rxlen = 0; want = 48; } /* should not happen */
            if (avail > want) avail = want;
            char tmp[48];
            int r = read(0, tmp, avail);
            if (r > 0) {
                for (int i = 0; i < r; i++) rxbuf[rxlen + i] = tmp[i];
                rxlen += r;
                continue;
            }
            /* read returned <=0 with data reported: retry */
            avail = 0;
        }

        /* No data available. */
        if (rxlen == 1 && (unsigned char)rxbuf[0] == 27) {
            /* A lone ESC: give the desktop a grace period in case the rest
             * of a sequence is still in flight. */
            int grace = 0;
            while (available(0) <= 0 && grace < 40) { usleep(1000); grace++; }
            if (available(0) <= 0) {
                rxlen = 0;
                ev->type = GUI_EV_ESC;
                return 1;
            }
            continue;
        }
        if (rxlen > 0 && (unsigned char)rxbuf[0] == 27) {
            /* Partial CSI still short: wait a little longer for the rest. */
            int grace = 0;
            while (available(0) <= 0 && grace < 40) { usleep(1000); grace++; }
            if (available(0) <= 0) { rxlen = 0; continue; }
            continue;
        }
        if (rxlen > 0) { rxlen = 0; continue; }

        /* Nothing buffered: honour blocking semantics. */
        if (block_ms == 0) return 0;
        if (block_ms > 0 && waited >= block_ms) return 0;
        usleep(2000);
        waited += 2;
        if (block_ms > 0 && waited >= block_ms) {
            return 0;
        }
    }
}

int gui_read_event(struct gui_event *ev) {
    return gui_read_internal(ev, -1);
}

int gui_read_event_timeout(struct gui_event *ev, int ms) {
    if (ms < 0) return gui_read_internal(ev, -1);
    return gui_read_internal(ev, ms);
}

int gui_poll_event(struct gui_event *ev) {
    return gui_read_internal(ev, 0);
}
