/*
 * diff.c - Diff (file comparer) for HobbyOS.
 *
 * A classic line-diff utility for the HobbyOS desktop:
 *   - File menu: Open A / Open B  (file_open_dialog, then load)
 *   - Diff menu: Compare / Next Change
 *   - Keys: 'o'/'O' open A/B, 'c' compare, 'n' next change, Up/Down scroll
 *
 * How it works
 *   Each loaded file is split into lines (<= 100 chars each, at most 400
 *   lines kept) and every line is reduced to a 32-bit FNV-1a hash at load
 *   time. "Compare" runs a longest-common-subsequence (LCS) DP over the two
 *   hash arrays and classifies each line as SAME / AONLY ('-') / BONLY ('+').
 *   Only the hashes are compared; equal hashes are treated as equal lines.
 *
 * HobbyOS constraints
 *   Freestanding aarch64 C, no libc, integer math only. The whole screen is
 *   rendered into one buffer and printed after gui_clear() so a redraw is a
 *   single write ("\f" clears the window text buffer, 2048 bytes).
 */

#include "libc.h"
#include "gui.h"
#include "filedialog.h"

/* ---- Limits ---------------------------------------------------------- */

#define DIFF_MAX_FILESIZE 16384  /* read at most 16 KB of each file       */
#define DIFF_MAX_LINE     100    /* lines longer than this are cut        */
#define DIFF_MAX_LINES    400    /* at most 400 lines kept per file       */
#define DIFF_MAX_ROWS     600    /* at most 600 diff rows stored          */
#define DIFF_VIEW_ROWS    18     /* diff rows shown on screen             */
#define DIFF_NAME_MAX     64     /* filename buffer size                  */
#define DIFF_NAME_SHOW    18     /* filename chars shown in the header    */
#define DIFF_SLOT_MAX     80     /* buffer for one "A: name (n lines)"    */
#define DIFF_LINE2_MAX    69     /* max columns for header line 2         */
#define DIFF_LINE3_MAX    69     /* max columns for status line 3         */
#define DIFF_TEXT_COLS    70     /* text columns per rendered diff row    */
#define DIFF_ROW_MAX      (3 + DIFF_TEXT_COLS)
#define DIFF_SCREEN_MAX   2048   /* whole rendered screen (1 print call)  */

/* ---- Row types (diff row classes) ---- */
#define DIFF_ROW_SAME  0         /* line present on both sides            */
#define DIFF_ROW_AONLY 1         /* line only in A -> rendered " - text"  */
#define DIFF_ROW_BONLY 2         /* line only in B -> rendered " + text"  */

/* ---- Commands (keys/menu items map to these) ---- */
#define DIFF_CMD_NONE    0
#define DIFF_CMD_OPEN_A  1
#define DIFF_CMD_OPEN_B  2
#define DIFF_CMD_COMPARE 3
#define DIFF_CMD_NEXT    4

/* ---- Data types ------------------------------------------------------ */

struct diff_line {
    char     text[DIFF_MAX_LINE + 1]; /* NUL-terminated, <= 100 chars     */
    uint32_t hash;                    /* FNV-1a 32-bit of text            */
};

struct diff_file {
    char     name[DIFF_NAME_MAX];     /* "" -> shown as (none)            */
    int      loaded;                  /* 1 after a successful load        */
    int      count;                   /* lines kept  (0..DIFF_MAX_LINES)  */
    int      total;                   /* lines seen  (may exceed count)   */
    int      dropped;                 /* total - count (400-line cap)     */
    int      long_cut;                /* # lines shortened to 100 chars   */
    int      size_cap;                /* read stopped at the 16 KB cap    */
    struct diff_line lines[DIFF_MAX_LINES];
};

/* One rendered diff row. a_index/b_index are -1 when the line does not
 * exist on that side; text points at the owning line's text[] (both line
 * arrays are static, so the pointer stays valid for the life of the app). */
struct diff_row {
    int         type;                 /* DIFF_ROW_*                       */
    int         a_index;              /* index in A lines, or -1          */
    int         b_index;              /* index in B lines, or -1          */
    const char *text;
};

/* Test hook: when set, diff_build() behaves as if malloc() failed so the
 * fallback path can be exercised on the host. Always 0 in the app. */
int diff_force_fallback = 0;

/* ---- App state ------------------------------------------------------- */

static struct diff_file a_file;
static struct diff_file b_file;

static struct diff_row diff_rows[DIFF_MAX_ROWS];
static int diff_row_count;      /* rows stored (<= DIFF_MAX_ROWS)       */
static int diff_valid;          /* 1 once Compare has run               */
static int diff_row_capped;     /* rows hit the DIFF_MAX_ROWS cap       */
static int diff_fallback;       /* malloc failed -> A-'/B+ raw dump     */
static int diff_aonly;          /* # AONLY rows stored (line 3 "-" count) */
static int diff_bonly;          /* # BONLY rows stored (line 3 "+" count) */
static int view_top;            /* first displayed diff row             */

static char file_buf[DIFF_MAX_FILESIZE + 1]; /* read scratch            */
static char screen_buf[DIFF_SCREEN_MAX];     /* rendered screen         */

/* ---- Small bounded string writer ------------------------------------- */

struct buf_w {
    char *buf;
    int   cap;   /* total bytes incl. NUL */
    int   len;
};

static void bw_ch(struct buf_w *w, char c) {
    if (w->len < w->cap - 1) {
        w->buf[w->len++] = c;
        w->buf[w->len] = '\0';
    }
}

static void bw_str(struct buf_w *w, const char *s) {
    while (*s && w->len < w->cap - 1) {
        w->buf[w->len++] = *s++;
    }
    w->buf[w->len] = '\0';
}

static void bw_nchars(struct buf_w *w, const char *s, int max) {
    int i;
    for (i = 0; i < max && s[i]; i++) bw_ch(w, s[i]);
}

static void bw_int(struct buf_w *w, int v) {
    char tmp[12];
    int n = 0, i;
    unsigned int uv;
    if (v < 0) {
        bw_ch(w, '-');
        uv = (unsigned int)(-(long)v);
    } else {
        uv = (unsigned int)v;
    }
    if (uv == 0) tmp[n++] = '0';
    while (uv > 0) {
        tmp[n++] = (char)('0' + (int)(uv % 10));
        uv /= 10;
    }
    for (i = n - 1; i >= 0; i--) bw_ch(w, tmp[i]);
}

/* ---- Hashing --------------------------------------------------------- */

/* FNV-1a 32-bit over `len` bytes. Stable and dependency-free. */
static uint32_t diff_fnv1a(const char *s, int len) {
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < len; i++) {
        h ^= (uint32_t)(unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/* ---- Line splitting -------------------------------------------------- */

/*
 * Split `len` bytes of `buf` into lines stored in `f`:
 *   - lines are separated by '\n'; a trailing '\n' does not create an
 *     extra empty line, an interior empty line is kept
 *   - a trailing '\r' is stripped (CRLF files) and documented as such
 *   - a line longer than DIFF_MAX_LINE is cut to DIFF_MAX_LINE chars and
 *     counted in f->long_cut
 *   - at most DIFF_MAX_LINES lines are stored; the rest are counted in
 *     f->total so f->dropped reports how many were dropped
 * Each kept line gets its FNV-1a hash. f->count/total/dropped/long_cut are
 * always reset, loaded/name are left alone.
 */
static void diff_split_lines(const char *buf, int len, struct diff_file *f) {
    int i = 0;
    f->count = 0;
    f->total = 0;
    f->long_cut = 0;

    while (i < len) {
        int start = i;
        int line_len, keep, k;

        while (i < len && buf[i] != '\n') i++;
        line_len = i - start;                       /* without the '\n'    */
        if (line_len > 0 && buf[start + line_len - 1] == '\r') line_len--;

        keep = line_len > DIFF_MAX_LINE ? DIFF_MAX_LINE : line_len;
        if (f->total < DIFF_MAX_LINES) {
            struct diff_line *ln = &f->lines[f->count];
            for (k = 0; k < keep; k++) ln->text[k] = buf[start + k];
            ln->text[keep] = '\0';
            ln->hash = diff_fnv1a(ln->text, keep);
            f->count++;
        }
        if (line_len > keep) f->long_cut++;
        f->total++;
        i++;                                        /* past the '\n'       */
    }

    f->dropped = f->total - f->count;
    if (f->dropped < 0) f->dropped = 0;
}

/* ---- File loading ---------------------------------------------------- */

/*
 * Read at most DIFF_MAX_FILESIZE bytes of `name` into file_buf.
 * Returns the byte count (>= 0), or -1 if the file cannot be opened/read.
 * *size_cap is set to 1 when the file is longer than the cap (probed with
 * one extra byte, so a file of exactly 16384 bytes does not set it).
 */
static int diff_read_file(const char *name, int *size_cap) {
    int fd, total = 0, n;
    *size_cap = 0;

    fd = open(name, 0);
    if (fd < 0) return -1;
    while (total < DIFF_MAX_FILESIZE) {
        n = read(fd, file_buf + total, DIFF_MAX_FILESIZE - total);
        if (n <= 0) break;
        total += n;
    }
    if (total == DIFF_MAX_FILESIZE) {
        char probe;
        if (read(fd, &probe, 1) > 0) *size_cap = 1;
    }
    close(fd);
    file_buf[total] = '\0';
    return total;
}

/* Drop any computed diff (called whenever a file changes). */
static void diff_invalidate(void) {
    diff_valid = 0;
    diff_row_count = 0;
    diff_row_capped = 0;
    diff_fallback = 0;
    diff_aonly = 0;
    diff_bonly = 0;
    view_top = 0;
}

/*
 * Load `name` into slot 0 (A) or 1 (B).
 * Returns 1 on success. On failure the slot is emptied and shown as
 * "(none)" (a failed open never leaves stale lines behind).
 */
static int diff_load_slot(int slot, const char *name) {
    struct diff_file *f = (slot == 0) ? &a_file : &b_file;
    int size_cap = 0;
    int n = diff_read_file(name, &size_cap);

    if (n < 0) {
        f->loaded = 0;
        f->name[0] = '\0';
        f->count = 0;
        f->total = 0;
        f->dropped = 0;
        f->long_cut = 0;
        f->size_cap = 0;
        diff_invalidate();
        return 0;
    }

    {
        struct buf_w w;
        w.buf = f->name;
        w.cap = DIFF_NAME_MAX;
        w.len = 0;
        f->name[0] = '\0';
        bw_nchars(&w, name, DIFF_NAME_MAX - 1);
    }
    f->size_cap = size_cap;
    diff_split_lines(file_buf, n, f);
    f->loaded = 1;
    diff_invalidate();
    return 1;
}

/* ---- The diff itself ------------------------------------------------- */

/* Append the "raw" diff (all A lines '-' then all B lines '+') to `rev`.
 * Used for empty inputs and as the malloc-failure fallback.
 * Like the LCS backtrack, `rev` is filled bottom-to-top (B block first,
 * each block reversed) so the shared copy-out below yields top-to-bottom. */
static int diff_raw_rows(struct diff_row *rev, const struct diff_line *A, int na,
                         const struct diff_line *B, int nb) {
    int total = 0, i;
    for (i = nb - 1; i >= 0; i--) {
        rev[total].type = DIFF_ROW_BONLY;
        rev[total].a_index = -1;
        rev[total].b_index = i;
        rev[total].text = B[i].text;
        total++;
    }
    for (i = na - 1; i >= 0; i--) {
        rev[total].type = DIFF_ROW_AONLY;
        rev[total].a_index = i;
        rev[total].b_index = -1;
        rev[total].text = A[i].text;
        total++;
    }
    return total;
}

/*
 * Build the diff rows for line arrays A[0..na) and B[0..nb).
 * `out` receives at most max_rows rows, in top-to-bottom order; the return
 * value is how many rows were written.
 *   *capped   <- 1 when the full diff had more rows than max_rows
 *   *fallback <- 1 when the DP allocation failed and the raw dump was used
 * force_fallback: test hook (same effect as a failed malloc).
 *
 * The DP table is malloc'd: (na+1)*(nb+1) uint16 values. na,nb <= 400 so
 * the LCS length fits a uint16 comfortably ((400+1)^2 * 2 = 321 KB max).
 */
static int diff_build(const struct diff_line *A, int na,
                      const struct diff_line *B, int nb,
                      struct diff_row *out, int max_rows,
                      int *capped, int *fallback, int force_fallback) {
    static struct diff_row rev[DIFF_MAX_LINES * 2];  /* reversed scratch   */
    int total = 0, i, j, k, written;

    *capped = 0;
    *fallback = 0;
    if (na < 0) na = 0;
    if (nb < 0) nb = 0;

    if (na == 0 || nb == 0) {
        /* Nothing to align: one side is empty. No allocation needed. */
        total = diff_raw_rows(rev, A, na, B, nb);
    } else {
        uint16_t *dp = NULL;
        int m1 = nb + 1;
        if (!force_fallback) {
            size_t cells = (size_t)(na + 1) * (size_t)m1;
            dp = (uint16_t *)malloc(cells * sizeof(uint16_t));
        }
        if (!dp) {
            /* No memory: fall back to marking every A line '-' and every
             * B line '+' (classic "no common lines assumed" dump). */
            *fallback = 1;
            total = diff_raw_rows(rev, A, na, B, nb);
        } else {
            /* dp[i*m1 + j] = LCS length of A[0..i) and B[0..j) */
            for (j = 0; j <= nb; j++) dp[j] = 0;
            for (i = 1; i <= na; i++) {
                dp[i * m1] = 0;
                for (j = 1; j <= nb; j++) {
                    if (A[i - 1].hash == B[j - 1].hash) {
                        dp[i * m1 + j] = (uint16_t)(dp[(i - 1) * m1 + (j - 1)] + 1);
                    } else {
                        uint16_t up = dp[(i - 1) * m1 + j];
                        uint16_t lf = dp[i * m1 + (j - 1)];
                        dp[i * m1 + j] = (up >= lf) ? up : lf;
                    }
                }
            }
            /* Walk back from (na,nb), appending rows bottom-to-top. At equal
             * LCS scores the B line is consumed first: rows are emitted
             * bottom-up, so this is what puts the '-' line directly above
             * the '+' line of a changed/replaced line after the reversal. */
            i = na;
            j = nb;
            while (i > 0 || j > 0) {
                if (i > 0 && j > 0 && A[i - 1].hash == B[j - 1].hash) {
                    rev[total].type = DIFF_ROW_SAME;
                    rev[total].a_index = i - 1;
                    rev[total].b_index = j - 1;
                    rev[total].text = A[i - 1].text;
                    total++;
                    i--; j--;
                } else if (i == 0) {
                    rev[total].type = DIFF_ROW_BONLY;
                    rev[total].a_index = -1;
                    rev[total].b_index = j - 1;
                    rev[total].text = B[j - 1].text;
                    total++;
                    j--;
                } else if (j == 0) {
                    rev[total].type = DIFF_ROW_AONLY;
                    rev[total].a_index = i - 1;
                    rev[total].b_index = -1;
                    rev[total].text = A[i - 1].text;
                    total++;
                    i--;
                } else if (dp[i * m1 + (j - 1)] >= dp[(i - 1) * m1 + j]) {
                    rev[total].type = DIFF_ROW_BONLY;
                    rev[total].a_index = -1;
                    rev[total].b_index = j - 1;
                    rev[total].text = B[j - 1].text;
                    total++;
                    j--;
                } else {
                    rev[total].type = DIFF_ROW_AONLY;
                    rev[total].a_index = i - 1;
                    rev[total].b_index = -1;
                    rev[total].text = A[i - 1].text;
                    total++;
                    i--;
                }
            }
            free(dp);
        }
    }

    written = total > max_rows ? max_rows : total;
    for (k = 0; k < written; k++) {
        out[k] = rev[total - 1 - k];   /* undo the bottom-to-top order */
    }
    *capped = (total > max_rows) ? 1 : 0;
    return written;
}

/* Diff>Compare: rebuild the rows from the currently loaded files. */
static void diff_compare(void) {
    int capped = 0, fallback = 0;
    int na = a_file.loaded ? a_file.count : 0;
    int nb = b_file.loaded ? b_file.count : 0;
    int k;

    diff_row_count = diff_build(a_file.lines, na, b_file.lines, nb,
                                diff_rows, DIFF_MAX_ROWS,
                                &capped, &fallback, diff_force_fallback);
    diff_row_capped = capped;
    diff_fallback = fallback;

    diff_aonly = 0;
    diff_bonly = 0;
    for (k = 0; k < diff_row_count; k++) {
        if (diff_rows[k].type == DIFF_ROW_AONLY) diff_aonly++;
        else if (diff_rows[k].type == DIFF_ROW_BONLY) diff_bonly++;
    }
    diff_valid = 1;
    view_top = 0;
}

/* ---- Scrolling / navigation ------------------------------------------ */

/* Diff>Next Change ('n'): scroll so the first non-SAME row below the
 * current top lands on the top line. No wraparound. */
static void diff_next_change(void) {
    int k;
    if (!diff_valid) return;
    for (k = view_top + 1; k < diff_row_count; k++) {
        if (diff_rows[k].type != DIFF_ROW_SAME) {
            view_top = k;
            return;
        }
    }
}

static void diff_scroll_up(void) {
    if (view_top > 0) view_top--;
}

static void diff_scroll_down(void) {
    int max_top = diff_row_count - DIFF_VIEW_ROWS;
    if (max_top < 0) max_top = 0;
    if (view_top < max_top) view_top++;
}

/* ---- Key / menu mapping ---------------------------------------------- */

/* 'o'/'O' open A/B, 'c'/'C' compare, 'n'/'N' next change. Else NONE. */
static int diff_key_command(int ch) {
    switch (ch) {
        case 'o': return DIFF_CMD_OPEN_A;
        case 'O': return DIFF_CMD_OPEN_B;
        case 'c': case 'C': return DIFF_CMD_COMPARE;
        case 'n': case 'N': return DIFF_CMD_NEXT;
        default:  return DIFF_CMD_NONE;
    }
}

/* File(0): 0=Open A 1=Open B   Diff(1): 0=Compare 1=Next Change */
static int diff_menu_command(int menu, int item) {
    if (menu == 0 && item == 0) return DIFF_CMD_OPEN_A;
    if (menu == 0 && item == 1) return DIFF_CMD_OPEN_B;
    if (menu == 1 && item == 0) return DIFF_CMD_COMPARE;
    if (menu == 1 && item == 1) return DIFF_CMD_NEXT;
    return DIFF_CMD_NONE;
}

/* Pick a file with the modal dialog and load it into a slot. */
static void diff_open_slot(int slot) {
    char name[DIFF_NAME_MAX];
    name[0] = '\0';
    if (file_open_dialog(name, DIFF_NAME_MAX)) {
        diff_load_slot(slot, name);
    }
}

static void diff_run_command(int cmd) {
    switch (cmd) {
        case DIFF_CMD_OPEN_A:  diff_open_slot(0); break;
        case DIFF_CMD_OPEN_B:  diff_open_slot(1); break;
        case DIFF_CMD_COMPARE: diff_compare();    break;
        case DIFF_CMD_NEXT:    diff_next_change(); break;
        default: break;
    }
}

/* Handle one event. Returns 1 when the screen should be redrawn. */
static int diff_handle_event(const struct gui_event *ev) {
    int cmd;
    if (ev->type == GUI_EV_CHAR) {
        cmd = diff_key_command(ev->ch);
        if (cmd != DIFF_CMD_NONE) {
            diff_run_command(cmd);
            return 1;
        }
    } else if (ev->type == GUI_EV_MENU) {
        cmd = diff_menu_command(ev->menu, ev->item);
        if (cmd != DIFF_CMD_NONE) {
            diff_run_command(cmd);
            return 1;
        }
    } else if (ev->type == GUI_EV_UP) {
        diff_scroll_up();
        return 1;
    } else if (ev->type == GUI_EV_DOWN) {
        diff_scroll_down();
        return 1;
    }
    return 0;
}

/* ---- Screen formatting ----------------------------------------------- */

/*
 * Header line 2 slot: "A: NAME (N lines)" with truncation notes:
 *   dropped lines -> "(400 of 512 lines)"
 *   cut lines     -> ", 3 cut"
 *   16 KB cap     -> ", size cap"
 * Unloaded slots render as "A: (none)".
 * `out` must hold DIFF_SLOT_MAX bytes. Returns the length written.
 */
static int diff_format_slot(char *out, char label, const struct diff_file *f) {
    struct buf_w w;
    w.buf = out;
    w.cap = DIFF_SLOT_MAX;
    w.len = 0;
    out[0] = '\0';

    bw_ch(&w, label);
    bw_ch(&w, ':');
    bw_ch(&w, ' ');
    if (!f->loaded) {
        bw_str(&w, "(none)");
        return w.len;
    }
    bw_nchars(&w, f->name, DIFF_NAME_SHOW);
    bw_str(&w, " (");
    bw_int(&w, f->count);
    if (f->dropped > 0) {
        bw_str(&w, " of ");
        bw_int(&w, f->total);
    }
    bw_str(&w, " lines");
    if (f->long_cut > 0) {
        bw_str(&w, ", ");
        bw_int(&w, f->long_cut);
        bw_str(&w, " cut");
    }
    if (f->size_cap) bw_str(&w, ", size cap");
    bw_ch(&w, ')');
    return w.len;
}

/* Header line 2: both slots, clipped to DIFF_LINE2_MAX columns.
 * `out` must hold DIFF_LINE2_MAX + 1 bytes. */
static void diff_format_line2(char *out) {
    char a[DIFF_SLOT_MAX];
    char b[DIFF_SLOT_MAX];
    struct buf_w w;

    diff_format_slot(a, 'A', &a_file);
    diff_format_slot(b, 'B', &b_file);

    w.buf = out;
    w.cap = DIFF_LINE2_MAX + 1;
    w.len = 0;
    out[0] = '\0';

    bw_nchars(&w, a, DIFF_LINE2_MAX);
    if (b[0]) {
        bw_nchars(&w, "   ", DIFF_LINE2_MAX - w.len);
        bw_nchars(&w, b, DIFF_LINE2_MAX - w.len);
    }
}

/* Status line 3: "+added -removed" once compared, else a hint, plus notes
 * for the fallback/capped cases. `out` must hold DIFF_LINE3_MAX + 1. */
static void diff_format_line3(char *out) {
    struct buf_w w;
    w.buf = out;
    w.cap = DIFF_LINE3_MAX + 1;
    w.len = 0;
    out[0] = '\0';

    if (!diff_valid) {
        bw_str(&w, "Press Compare to diff");
        return;
    }
    bw_ch(&w, '+');
    bw_int(&w, diff_bonly);
    bw_str(&w, " -");
    bw_int(&w, diff_aonly);
    if (diff_fallback) bw_str(&w, " (fallback: no memory)");
    if (diff_row_capped) bw_str(&w, " (600 row cap)");
}

/* One rendered diff row: 3-char prefix + text (max 70 cols).
 * `out` must hold DIFF_ROW_MAX + 1 bytes. Returns the length written. */
static int diff_format_row(char *out, const struct diff_row *r) {
    struct buf_w w;
    w.buf = out;
    w.cap = DIFF_ROW_MAX + 1;
    w.len = 0;
    out[0] = '\0';

    if (r->type == DIFF_ROW_AONLY) bw_str(&w, " - ");
    else if (r->type == DIFF_ROW_BONLY) bw_str(&w, " + ");
    else bw_str(&w, "   ");
    if (r->text) bw_nchars(&w, r->text, DIFF_TEXT_COLS);
    return w.len;
}

/* Render the whole screen into `out` (cap bytes, NUL-terminated). */
static void diff_render_to(char *out, int cap) {
    struct buf_w w;
    char tmp[DIFF_SLOT_MAX];
    int k;

    w.buf = out;
    w.cap = cap;
    w.len = 0;
    out[0] = '\0';

    bw_str(&w, "=== Diff ===\n");

    diff_format_line2(tmp);
    bw_str(&w, tmp);
    bw_ch(&w, '\n');

    diff_format_line3(tmp);
    bw_str(&w, tmp);
    bw_ch(&w, '\n');

    for (k = 0; k < DIFF_VIEW_ROWS; k++) {
        int idx = view_top + k;
        if (diff_valid && idx < diff_row_count) {
            diff_format_row(tmp, &diff_rows[idx]);
            bw_str(&w, tmp);
        }
        bw_ch(&w, '\n');
    }

    bw_str(&w, "o=open A  O=open B  c=compare  n=next change  arrows=scroll\n");
}

/* Print the full screen (one write after the clear). */
static void diff_render(void) {
    gui_clear();
    diff_render_to(screen_buf, DIFF_SCREEN_MAX);
    print(screen_buf);
}

/* ---- Entry point ----------------------------------------------------- */

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
    gui_set_title("Diff");
    gui_add_menu(0, "File", "Open A,Open B");
    gui_add_menu(1, "Diff", "Compare,Next Change");
    gui_enable_mouse();
    print_console("[APP] DIFF started\n");

    diff_invalidate();
    diff_render();

    for (;;) {
        struct gui_event ev;
        if (gui_read_event(&ev)) {
            if (diff_handle_event(&ev)) diff_render();
        }
    }
}
