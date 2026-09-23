/*
 * hex.c - Hex Viewer for HobbyOS.
 *
 * Browse any file as a hex + ASCII dump: 18 rows of 16 bytes per screen,
 * with scrolling (up/down one row, left/right one page of 16 rows) and a
 * Go To Offset prompt. Files are chosen through the standard file open
 * dialog and are capped at 32768 bytes; the header shows when a file was
 * truncated.
 *
 * The screen is built by hex_render(), a deterministic function that
 * writes the whole window text into a caller-provided buffer, so the host
 * unit test (src/host/hex_test.c) can check the layout without a desktop.
 * All formatting helpers (offset digits, ASCII filter, row/offset
 * formatting, clamping, offset parsing) are pure and separately tested.
 */

#include "libc.h"
#include "gui.h"
#include "dialog.h"
#include "filedialog.h"

/* ---- Constants ---- */

#define HEX_MAX_BYTES  32768   /* Hard cap on loaded file size          */
#define HEX_ROW_BYTES     16   /* Bytes per dump row                    */
#define HEX_VIS_ROWS      18   /* Dump rows visible on one screen       */
#define HEX_PAGE_ROWS     16   /* Rows moved by Left/Right (page step)  */
#define HEX_NAME_MAX      64   /* Filename buffer size (incl. NUL)      */
#define HEX_ROW_LINE      96   /* One formatted row (77 chars) + NUL    */
#define HEX_SCREEN_MAX  2048   /* Window text buffer size               */

/* hex_handle_event() results */
#define HEX_ACT_NONE     0     /* nothing happened, no redraw needed    */
#define HEX_ACT_REDRAW   1     /* state changed, redraw the window      */
#define HEX_ACT_QUIT  (-1)     /* 'q' pressed: caller should exit(0)    */

/* ---- State ---- */

static unsigned char hex_data[HEX_MAX_BYTES]; /* the loaded bytes        */
static int  hex_loaded = 0;                   /* 1 = a file is open      */
static int  hex_len = 0;                      /* bytes actually loaded   */
static int  hex_truncated = 0;                /* file was longer than cap*/
static char hex_fname[HEX_NAME_MAX];          /* name shown in the header*/
static int  hex_top_row = 0;                  /* first visible row       */
static char hex_screen[HEX_SCREEN_MAX];       /* render target           */

/* ---- Bounded string builder (never overflows `max`) ---- */

struct hex_out {
    char *buf;
    int   len;
    int   max;
};

/* Append one character; drops what does not fit, always leaves room for
 * the terminating NUL. */
static void ho_ch(struct hex_out *o, char c) {
    if (o->len < o->max - 1) o->buf[o->len++] = c;
}

static void ho_str(struct hex_out *o, const char *s) {
    for (int i = 0; s[i]; i++) ho_ch(o, s[i]);
}

static void ho_uint(struct hex_out *o, unsigned long v) {
    char tmp[24];
    gui_uitoa(v, tmp);
    ho_str(o, tmp);
}

/* ---- Pure helpers (unit-tested) ---- */

/* Uppercase hex digit for 0..15; '?' for anything out of range. */
static char hex_digit(int nibble) {
    if (nibble >= 0 && nibble <= 9) return (char)('0' + nibble);
    if (nibble >= 10 && nibble <= 15) return (char)('A' + (nibble - 10));
    return '?';
}

static void ho_hex8(struct hex_out *o, unsigned long v) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        ho_ch(o, hex_digit((int)((v >> shift) & 0xFu)));
    }
}

/* Terminate the builder's buffer and return the length written. */
static int ho_finish(struct hex_out *o) {
    o->buf[o->len] = '\0';
    return o->len;
}

/* ASCII column filter: bytes 32..126 (' '..'~') print as-is, everything
 * else (controls, DEL, high bytes) becomes '.'. */
static char hex_ascii(unsigned char b) {
    if (b >= 32 && b <= 126) return (char)b;
    return '.';
}

/* Number of 16-byte rows needed for `len` bytes; 0 for an empty buffer. */
static int hex_total_rows(int len) {
    if (len <= 0) return 0;
    return (len + HEX_ROW_BYTES - 1) / HEX_ROW_BYTES;
}

/* Clamp a row index into [0, total-1]; always 0 when there are no rows. */
static int hex_clamp_row(int row, int total) {
    if (total <= 0) return 0;
    if (row < 0) return 0;
    if (row > total - 1) return total - 1;
    return row;
}

/* Clamp a byte offset to the start of the last row; 0 when empty. */
static long hex_clamp_offset(long off, int total) {
    if (total <= 0) return 0;
    long last = (long)(total - 1) * HEX_ROW_BYTES;
    if (off < 0) return 0;
    if (off > last) return last;
    return off;
}

/* Parse a hex byte offset: optional "0x"/"0X" prefix, 1..8 hex digits,
 * surrounding spaces allowed. Returns the offset, or -1 when the text is
 * empty, malformed, or has more than 8 digits. */
static long hex_parse_offset(const char *text) {
    if (!text) return -1;
    int i = 0;
    while (text[i] == ' ') i++;
    if (text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) i += 2;

    long value = 0;
    int digits = 0;
    for (;;) {
        char c = text[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (digits >= 8) return -1;  /* too long to be a 32-bit offset */
        value = value * 16 + d;
        digits++;
        i++;
    }
    if (digits == 0) return -1;
    while (text[i] == ' ') i++;
    if (text[i] != '\0') return -1;
    return value;
}

/* Format one dump row starting at byte `offset`:
 *   8 uppercase offset digits, two spaces, 16 "XX " byte slots (missing
 *   bytes become three spaces so the ASCII column stays aligned), then
 *   " |", 16 ASCII characters and a closing '|'.
 * Writes a NUL-terminated string into `out` (needs 78+ bytes) and returns
 * its length (77 for a complete row). */
static int hex_format_row(char *out, int max, const unsigned char *data,
                          int len, int offset) {
    if (max <= 0) return 0;
    struct hex_out o = { out, 0, max };
    unsigned long off = (offset < 0) ? 0UL : (unsigned long)offset;

    ho_hex8(&o, off);
    ho_ch(&o, ' ');
    ho_ch(&o, ' ');

    for (int i = 0; i < HEX_ROW_BYTES; i++) {
        int idx = offset + i;
        if (idx >= 0 && idx < len) {
            ho_ch(&o, hex_digit((data[idx] >> 4) & 0xF));
            ho_ch(&o, hex_digit(data[idx] & 0xF));
        } else {
            ho_ch(&o, ' ');
            ho_ch(&o, ' ');
        }
        ho_ch(&o, ' ');
    }

    ho_ch(&o, ' ');
    ho_ch(&o, '|');
    for (int i = 0; i < HEX_ROW_BYTES; i++) {
        int idx = offset + i;
        ho_ch(&o, (idx >= 0 && idx < len) ? hex_ascii(data[idx]) : ' ');
    }
    ho_ch(&o, '|');
    return ho_finish(&o);
}

/* Format the status line:
 *   "Offset: 0xXXXXXXXX (row R/T)  ^v=scroll  <> =page  g=goto  c=close"
 * XXXXXXXX is the byte offset of the first visible row, R its 1-based
 * number (0 when the buffer is empty) and T the total number of rows. */
static int hex_format_offset_line(char *out, int max, int top_row, int total) {
    if (max <= 0) return 0;
    struct hex_out o = { out, 0, max };
    int row = hex_clamp_row(top_row, total);

    ho_str(&o, "Offset: 0x");
    ho_hex8(&o, (unsigned long)row * HEX_ROW_BYTES);
    ho_str(&o, " (row ");
    ho_uint(&o, (total > 0) ? (unsigned long)(row + 1) : 0UL);
    ho_ch(&o, '/');
    ho_uint(&o, (unsigned long)total);
    ho_str(&o, ")  ^v=scroll  <> =page  g=goto  c=close");
    return ho_finish(&o);
}

/* ---- Screen rendering ---- */

/* Render the complete viewer screen into `out` (NUL-terminated) and return
 * its length. Deterministic: identical state gives identical bytes.
 * Layout with a file open: header line, file line, 18 dump rows (blank
 * past end of file) and the offset line. Without a file: two lines only. */
static int hex_render(char *out, int max) {
    if (max <= 0) return 0;
    struct hex_out o = { out, 0, max };
    out[0] = '\0';

    ho_str(&o, "=== Hex Viewer ===\n");

    if (!hex_loaded) {
        ho_str(&o, "No file open - File > Open (or press o)\n");
        return ho_finish(&o);
    }

    ho_str(&o, "File: ");
    ho_str(&o, hex_fname);
    if (hex_truncated) {
        ho_str(&o, "  (truncated at ");
        ho_uint(&o, HEX_MAX_BYTES);
        ho_str(&o, ")\n");
    } else {
        ho_str(&o, "  (");
        ho_uint(&o, (unsigned long)hex_len);
        ho_str(&o, " bytes)\n");
    }

    int total = hex_total_rows(hex_len);
    char row[HEX_ROW_LINE];
    for (int i = 0; i < HEX_VIS_ROWS; i++) {
        int r = hex_top_row + i;
        if (r < total) {
            hex_format_row(row, HEX_ROW_LINE, hex_data, hex_len,
                           r * HEX_ROW_BYTES);
            ho_str(&o, row);
        }
        ho_ch(&o, '\n');
    }

    hex_format_offset_line(row, HEX_ROW_LINE, hex_top_row, total);
    ho_str(&o, row);
    ho_ch(&o, '\n');
    return ho_finish(&o);
}

/* Clear the window and print the whole screen; the desktop captures
 * print() output into the window's text buffer. */
static void hex_redraw(void) {
    hex_render(hex_screen, (int)sizeof(hex_screen));
    gui_clear();
    print(hex_screen);
}

/* ---- File state ---- */

/* Load `name` into the dump buffer. Returns 1 on success, 0 when the file
 * could not be opened (the caller reports that with dialog_message).
 * Reading stops at HEX_MAX_BYTES; one extra byte is probed so the header
 * can say whether the file was truncated. */
static int hex_load(const char *name) {
    if (!name || !name[0]) return 0;

    int fd = open(name, 0);
    if (fd < 0) return 0;

    int n = read(fd, hex_data, HEX_MAX_BYTES);
    if (n < 0) n = 0;
    int truncated = 0;
    if (n == HEX_MAX_BYTES) {
        unsigned char extra;
        if (read(fd, &extra, 1) > 0) truncated = 1;
    }
    close(fd);

    hex_len = n;
    hex_truncated = truncated;
    hex_loaded = 1;
    hex_top_row = 0;
    gui_strncpy(hex_fname, name, HEX_NAME_MAX);
    return 1;
}

/* Drop the loaded file (File > Close, or 'c'). */
static void hex_close_file(void) {
    hex_loaded = 0;
    hex_len = 0;
    hex_truncated = 0;
    hex_top_row = 0;
    hex_fname[0] = '\0';
}

/* ---- Navigation ---- */

/* Scroll the view by `rows` dump rows (negative = up), clamped. */
static void hex_scroll(int rows) {
    if (!hex_loaded) { hex_top_row = 0; return; }
    hex_top_row = hex_clamp_row(hex_top_row + rows, hex_total_rows(hex_len));
}

/* Jump to the row containing byte `off` (clamped to the last row). */
static void hex_goto_offset(long off) {
    if (!hex_loaded) { hex_top_row = 0; return; }
    int total = hex_total_rows(hex_len);
    off = hex_clamp_offset(off, total);
    hex_top_row = hex_clamp_row((int)(off / HEX_ROW_BYTES), total);
}

/* ---- Prompts ---- */

/* File > Open / 'o': pick a file, then load it. */
static void hex_prompt_open(void) {
    char name[HEX_NAME_MAX];
    name[0] = '\0';
    if (!file_open_dialog(name, HEX_NAME_MAX)) return;
    if (!hex_load(name)) dialog_message("Hex Viewer", "Cannot open file.");
}

/* 'g': ask for a hex offset (optional 0x prefix) and jump there. */
static void hex_prompt_goto(void) {
    if (!hex_loaded) return;
    char text[24];
    text[0] = '\0';
    if (!dialog_prompt("Go To Offset", "Hex offset:", text,
                       (int)sizeof(text))) {
        return;
    }
    long off = hex_parse_offset(text);
    if (off < 0) {
        dialog_message("Go To Offset", "Invalid hex offset.");
        return;
    }
    hex_goto_offset(off);
}

/* ---- Event handling ---- */

/* Apply one GUI event to the viewer state.
 * Returns HEX_ACT_QUIT to quit, HEX_ACT_REDRAW when the screen changed and
 * HEX_ACT_NONE when the event was ignored. */
static int hex_handle_event(const struct gui_event *ev) {
    switch (ev->type) {
    case GUI_EV_CHAR:
        switch (ev->ch) {
        case 'q': case 'Q': return HEX_ACT_QUIT;
        case 'o': case 'O': hex_prompt_open(); return HEX_ACT_REDRAW;
        case 'c': case 'C': hex_close_file();  return HEX_ACT_REDRAW;
        case 'g': case 'G': hex_prompt_goto(); return HEX_ACT_REDRAW;
        default: return HEX_ACT_NONE;
        }
    case GUI_EV_UP:    hex_scroll(-1);             return HEX_ACT_REDRAW;
    case GUI_EV_DOWN:  hex_scroll(1);              return HEX_ACT_REDRAW;
    case GUI_EV_LEFT:  hex_scroll(-HEX_PAGE_ROWS); return HEX_ACT_REDRAW;
    case GUI_EV_RIGHT: hex_scroll(HEX_PAGE_ROWS);  return HEX_ACT_REDRAW;
    case GUI_EV_MENU:
        if (ev->menu == 0) {
            if (ev->item == 0) { hex_prompt_open(); return HEX_ACT_REDRAW; }
            if (ev->item == 1) { hex_close_file();  return HEX_ACT_REDRAW; }
        }
        return HEX_ACT_NONE;
    default:
        return HEX_ACT_NONE;
    }
}

/* ---- Entry point ---- */

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
    gui_add_menu(0, "File", "Open,Close");
    gui_set_title("Hex Viewer");
    print_console("[APP] HEX started\n");

    hex_redraw();

    for (;;) {
        struct gui_event ev;
        if (!gui_read_event(&ev)) continue;
        int action = hex_handle_event(&ev);
        if (action == HEX_ACT_QUIT) exit(0);
        if (action == HEX_ACT_REDRAW) hex_redraw();
    }
}
