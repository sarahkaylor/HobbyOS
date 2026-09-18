/*
 * files_test.c - Host unit tests for src/user/files.c (HobbyOS "Files" app).
 *
 * The app source is included directly (entry point renamed to
 * files_app_main) so the tests drive the real state, formatters and event
 * handler.  fs_render() writes the whole screen into a buffer, so the
 * layout can be asserted exactly; one fs_redraw() test captures fd 1
 * through a pipe to prove the print() path emits gui_clear() + that screen.
 *
 * The three modal dialogs block on stdin, so the tests install
 * non-blocking mocks through the app's fs_dlg_* seams and record what the
 * app asked for.  compat.c supplies: read_dir (7 fixed files, then -1), an
 * in-memory cwd for chdir/getcwd (chdir stores the path verbatim - it does
 * not resolve ".." or absolute paths, hence the literal ".." expectations
 * below), sysinfo(7) -> 40.0M free, and inert mkdir/unlink/rename whose
 * mock_*_result globals flip them into failures.
 *
 * Covered: string/buffer writers (length counting, truncation, caps), size
 * cell alignment from 0B to 3.9G, row rendering (selected vs not, [DIR] vs
 * file, blank size cell for dirs, padding, clipping at narrow widths,
 * 12-char 8.3 names, 31-char names), the path/free line (short, 45-char,
 * exactly-fitting and 86-char cwd), the dialog message builders, directory
 * loading (end of list, cap, stale entries, selection clamping), selection
 * and scroll-window math through gui_list, every key and menu action with
 * success and failure paths, the quit flag, mouse row mapping, and
 * full-screen rendering (exact snapshot, empty listing, "Free: ?" fallback,
 * screen budget, determinism, no line wider than the row width).
 *
 * Not covered on the host: a chdir() failure (compat's mock always
 * succeeds), a live in-OS directory tree, and the real blocking dialogs -
 * the app reaches those through fs_dlg_*, which is exactly what the seam
 * exists for.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#define main files_app_main
#include "../user/files.c"
#undef main

/* compat.c's failure switches (no header provides them) */
extern int mock_mkdir_result;
extern int mock_unlink_result;
extern int mock_rename_result;

/* compat.c fd wrappers, used by the fd-1 capture helper below */
int ho_read(int fd, void *buf, int size);
int ho_close(int fd);
int ho_pipe(int fds[2]);

/* ====================================================================== */
/* check framework                                                        */
/* ====================================================================== */

static int checks_run = 0;
static int checks_failed = 0;

static void check(int cond, const char *name) {
    checks_run++;
    if (cond) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        checks_failed++;
    }
}

static int s_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static int has(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}
static int starts_with(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}
static int ends_with(const char *s, const char *suffix) {
    int ls = (int)strlen(s), lf = (int)strlen(suffix);
    if (lf > ls) return 0;
    return strcmp(s + (ls - lf), suffix) == 0;
}
static int all_spaces(const char *s) {
    for (int i = 0; s[i]; i++) if (s[i] != ' ') return 0;
    return 1;
}

/* Copy line `idx` (0-based) of buf into out (cap bytes); returns its length. */
static int get_line(const char *buf, int idx, char *out, int cap) {
    int line = 0, i = 0, j = 0;
    while (buf[i] != '\0' && line < idx) {
        if (buf[i] == '\n') line++;
        i++;
    }
    while (buf[i] != '\0' && buf[i] != '\n' && j < cap - 1) out[j++] = buf[i++];
    out[j] = '\0';
    return j;
}

static int count_lines(const char *buf) {
    int n = 0;
    for (int i = 0; buf[i]; i++) if (buf[i] == '\n') n++;
    return n;
}

/* Rendered lines that carry the selection marker. */
static int count_marked_lines(const char *buf) {
    int n = 0;
    for (int i = 0; buf[i]; i++) {
        if (buf[i] == '>' && (i == 0 || buf[i - 1] == '\n')) n++;
    }
    return n;
}

static int max_line_len(const char *buf) {
    int best = 0, cur = 0;
    for (int i = 0; buf[i]; i++) {
        if (buf[i] == '\n') { if (cur > best) best = cur; cur = 0; }
        else cur++;
    }
    if (cur > best) best = cur;
    return best;
}

static void fill(char *dst, int n, char c) {
    for (int i = 0; i < n; i++) dst[i] = c;
    dst[n] = '\0';
}

/* ====================================================================== */
/* non-blocking dialog mocks (the real dialogs read stdin)                */
/* ====================================================================== */

static int  mock_msg_calls;
static char mock_msg_title[64];
static char mock_msg_body[160];

static int  mock_confirm_calls;
static int  mock_confirm_result;
static char mock_confirm_title[64];
static char mock_confirm_body[160];

static int  mock_prompt_calls;
static int  mock_prompt_result;
static char mock_prompt_answer[64];
static char mock_prompt_title[64];
static char mock_prompt_body[64];

static void mock_message(const char *title, const char *msg) {
    mock_msg_calls++;
    snprintf(mock_msg_title, sizeof mock_msg_title, "%s", title);
    snprintf(mock_msg_body, sizeof mock_msg_body, "%s", msg);
}

static int mock_confirm(const char *title, const char *msg) {
    mock_confirm_calls++;
    snprintf(mock_confirm_title, sizeof mock_confirm_title, "%s", title);
    snprintf(mock_confirm_body, sizeof mock_confirm_body, "%s", msg);
    return mock_confirm_result;
}

static int mock_prompt(const char *title, const char *msg, char *buf, int max) {
    mock_prompt_calls++;
    snprintf(mock_prompt_title, sizeof mock_prompt_title, "%s", title);
    snprintf(mock_prompt_body, sizeof mock_prompt_body, "%s", msg);
    if (buf && max > 0) buf[0] = '\0';
    if (!mock_prompt_result) return 0;
    if (buf && max > 0) snprintf(buf, (size_t)max, "%s", mock_prompt_answer);
    return 1;
}

static void dialog_mocks_reset(void) {
    mock_msg_calls = 0;
    mock_msg_title[0] = '\0';
    mock_msg_body[0] = '\0';
    mock_confirm_calls = 0;
    mock_confirm_result = 0;
    mock_confirm_title[0] = '\0';
    mock_confirm_body[0] = '\0';
    mock_prompt_calls = 0;
    mock_prompt_result = 0;
    mock_prompt_answer[0] = '\0';
    mock_prompt_title[0] = '\0';
    mock_prompt_body[0] = '\0';
    mock_mkdir_result = 0;
    mock_unlink_result = 0;
    mock_rename_result = 0;
}

static void install_dialog_mocks(void) {
    fs_dlg_message = mock_message;
    fs_dlg_confirm = mock_confirm;
    fs_dlg_prompt = mock_prompt;
}

/* ====================================================================== */
/* state / event / layout helpers                                         */
/* ====================================================================== */

static void st_reset(struct fs_state *st) {
    memset(st, 0, sizeof *st);
    st->list.visible = FS_VISIBLE;
}

static void st_add(struct fs_state *st, const char *name, uint8_t attr,
                   uint32_t size) {
    struct fs_entry *e;
    if (st->count >= FS_MAX_ENTRIES) return;
    e = &st->entries[st->count];
    snprintf(e->name, FS_NAME_MAX, "%s", name);
    e->attr = attr;
    e->size = size;
    st->count++;
    st->list.count = st->count;
    gui_list_ensure_visible(&st->list);
}

static void st_addf(struct fs_state *st, uint8_t attr, uint32_t size,
                    const char *fmt, int v) {
    char name[FS_NAME_MAX];
    snprintf(name, sizeof name, fmt, v);
    st_add(st, name, attr, size);
}

/* A 30-entry listing (only the names matter). */
static void st_fill30(struct fs_state *st) {
    st_reset(st);
    for (int i = 0; i < 30; i++) st_addf(st, 0, 512, "F%02d.TXT", i);
}

/* A listing with a canonical cwd/free-space pair for render tests. */
static void st_plain(struct fs_state *st) {
    st_reset(st);
    strcpy(st->cwd, "/home");
    st->free_ok = 1;
    st->free_bytes = 40U * 1024U * 1024U;
}

static struct gui_event ev_char(int ch) {
    struct gui_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = GUI_EV_CHAR;
    ev.ch = ch;
    return ev;
}

static struct gui_event ev_type(int type) {
    struct gui_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    return ev;
}

static struct gui_event ev_menu(int menu, int item) {
    struct gui_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = GUI_EV_MENU;
    ev.menu = menu;
    ev.item = item;
    return ev;
}

static struct gui_event ev_mouse(int x, int y, int button) {
    struct gui_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = GUI_EV_MOUSE;
    ev.x = x;
    ev.y = y;
    ev.button = button;
    return ev;
}

/* ---- expected-layout builders (mirror the spec, not the app code) ---- */

static void pad_to(char *s, int col) {
    int n = (int)strlen(s);
    while (n < col) s[n++] = ' ';
    s[n] = '\0';
}

/* prefix padded to column FS_ROW_W-FS_SIZE_COL, then suffix right-aligned
 * in the final FS_SIZE_COL columns. */
static void mkrow(char *out, const char *prefix, const char *suffix) {
    int n = 0;
    while (prefix[n]) { out[n] = prefix[n]; n++; }
    while (n < FS_ROW_W - FS_SIZE_COL) out[n++] = ' ';
    int slen = (int)strlen(suffix);
    for (int i = slen; i < FS_SIZE_COL; i++) out[n++] = ' ';
    for (int i = 0; i < slen; i++) out[n++] = suffix[i];
    out[n] = '\0';
}

static void mkrule(char *out) {
    int n = 0;
    out[n++] = '+';
    for (int i = 0; i < FS_ROW_W - 2; i++) out[n++] = '-';
    out[n++] = '+';
    out[n] = '\0';
}

static void mkpath(char *out, const char *cwd, const char *free_text) {
    snprintf(out, FS_ROW_W + 1, "Path: %s", cwd);
    pad_to(out, FS_ROW_W - (int)strlen(free_text));
    strcat(out, free_text);
}

/* Capture what fs_redraw() writes to fd 1 (gui_clear + the whole screen). */
static int capture_redraw(char *out, int cap) {
    int fds[2];
    int saved, total = 0, n;
    if (ho_pipe(fds) != 0) return -1;
    fflush(stdout);
    saved = dup(1);
    if (saved < 0) { ho_close(fds[0]); ho_close(fds[1]); return -1; }
    dup2(fds[1], 1);
    fs_redraw();
    fflush(stdout);
    dup2(saved, 1);
    ho_close(saved);
    ho_close(fds[1]);
    while (total < cap - 1 &&
           (n = ho_read(fds[0], out + total, cap - 1 - total)) > 0) {
        total += n;
    }
    ho_close(fds[0]);
    out[total] = '\0';
    return total;
}

/* ====================================================================== */
/* main                                                                   */
/* ====================================================================== */

static const char *LEGEND_EXPECTED =
    "Enter=open  d=del  r=rename  n=new  Bksp=up  q=quit";

int main(void) {
    char buf[FS_SCREEN_MAX + 128];
    char buf2[FS_SCREEN_MAX + 128];
    char line[FS_ROW_W + 16];
    char line2[FS_ROW_W + 16];
    char cwd_buf[FS_PATH_MAX];
    struct fs_state st;
    struct gui_event ev;

    install_dialog_mocks();
    dialog_mocks_reset();

    printf("files_test: host unit tests for the HobbyOS Files app\n\n");

    /* ---- 1. layout constants ------------------------------------- */
    check(FS_MAX_ENTRIES == 64, "const: entry cap is 64");
    check(FS_VISIBLE == 18, "const: 18 visible entry rows");
    check(FS_SIZE_COL == 10, "const: size column is the last 10 columns");
    check(FS_ROW_ENTRY0 == 3, "const: first entry row is content row 3");
    check(FS_ROW_LEGEND == 21, "const: legend row is content row 21");
    check(FS_ROW_W == 70, "const: rows are 70 columns wide");
    check(s_eq(FS_LEGEND, LEGEND_EXPECTED), "const: key legend text");

    /* ---- 2. string helpers --------------------------------------- */
    check(fs_slen("") == 0, "str: slen of empty string");
    check(fs_slen("ABCDEFGH.IJK") == 12, "str: slen of a 12-char 8.3 name");

    fs_scopy(buf, 5, "ABCDEFGH");
    check(s_eq(buf, "ABCD"), "str: scopy truncates to cap-1");
    fs_scopy(buf, 1, "ABC");
    check(s_eq(buf, ""), "str: scopy with cap 1 stores only NUL");
    buf[0] = 'Q';
    fs_scopy(buf, 0, "ABC");
    check(buf[0] == 'Q', "str: scopy with cap 0 writes nothing");

    strcpy(buf, "ABC");
    check(fs_sappend(buf, 8, "XY") == 5 && s_eq(buf, "ABCXY"),
          "str: sappend appends and returns the new length");
    strcpy(buf, "ABCD");
    check(fs_sappend(buf, 6, "XYZW") == 5 && s_eq(buf, "ABCDX"),
          "str: sappend truncates at cap");
    strcpy(buf, "AB");
    check(fs_sappend(buf, 4, "") == 2 && s_eq(buf, "AB"),
          "str: sappend of an empty string is a no-op");

    memset(buf, 'Z', sizeof buf);
    {
        int len = 0;
        len = fs_put(buf, 4, len, "ABCDEF");
        check(len == 6, "put: counts every char even when truncated");
        check(s_eq(buf, "ABC"), "put: truncates at cap-1 and NUL-terminates");
        check(buf[4] == 'Z', "put: never writes past cap");
    }
    memset(buf, 'Z', sizeof buf);
    check(fs_putn(buf, 8, 0, "ABCDEF", 3) == 3 && s_eq(buf, "ABC"),
          "putn: copies at most n chars");
    check(fs_putn(buf, 8, 3, "ABCDEF", 2) == 5 && s_eq(buf, "ABCAB"),
          "putn: appends at the running length");
    check(fs_putpad(buf, 8, 5, 7) == 7 && s_eq(buf, "ABCAB  "),
          "putpad: pads with spaces up to the column");
    check(fs_putpad(buf, 8, 7, 3) == 7, "putpad: never shortens");
    memset(buf, 'Z', sizeof buf);
    check(fs_putc(buf, 2, 0, 'A') == 1 && buf[0] == 'A' && buf[1] == '\0',
          "putc: stores one char and terminates");
    check(fs_putc(buf, 8, 7, 'X') == 8 && buf[7] == 'Z',
          "putc: refuses to write at cap-1 (nothing past cap-1)");

    /* ---- 3. size cell -------------------------------------------- */
    fs_size_cell(buf, sizeof buf, 10, 0, 0);
    check(s_eq(buf, "        0B") && strlen(buf) == 10,
          "size: 0 bytes -> right-aligned \"0B\"");
    fs_size_cell(buf, sizeof buf, 10, 0, 512);
    check(s_eq(buf, "      512B"), "size: 512 -> \"512B\" in 10 columns");
    fs_size_cell(buf, sizeof buf, 10, 0, 1023);
    check(s_eq(buf, "     1023B"), "size: 1023 stays in bytes");
    fs_size_cell(buf, sizeof buf, 10, 0, 2048);
    check(s_eq(buf, "      2.0K"), "size: 2 KiB -> \"2.0K\"");
    fs_size_cell(buf, sizeof buf, 10, 0, 4096);
    check(s_eq(buf, "      4.0K"), "size: 4 KiB -> \"4.0K\"");
    fs_size_cell(buf, sizeof buf, 10, 0, (uint32_t)(1024U * 1024U));
    check(s_eq(buf, "      1.0M"), "size: 1 MiB -> \"1.0M\"");
    fs_size_cell(buf, sizeof buf, 10, 0, (uint32_t)(40U * 1024U * 1024U));
    check(s_eq(buf, "     40.0M"), "size: 40 MiB -> \"40.0M\"");
    fs_size_cell(buf, sizeof buf, 10, 0, (uint32_t)2147483648U);
    check(s_eq(buf, "      2.0G"), "size: 2 GiB -> \"2.0G\"");
    fs_size_cell(buf, sizeof buf, 10, 0, 0xFFFFFFFFU);
    check(s_eq(buf, "      3.9G") && strlen(buf) == 10,
          "size: max uint32 -> \"3.9G\", cell still 10 columns");

    {
        struct fs_entry d;
        memset(&d, 0, sizeof d);
        strcpy(d.name, "SUBDIR");
        d.attr = FS_ATTR_DIR;
        d.size = 4096;
        fs_size_cell(buf, sizeof buf, 10, 1, d.size);
        check(s_eq(buf, "          ") && strlen(buf) == 10,
              "size: directories get a blank cell even with size != 0");
        check(fs_is_dir(&d) == 1, "size: attr 0x10 marks a directory");
        d.attr = 0x20;
        check(fs_is_dir(&d) == 0, "size: attr 0x20 (archive) is a file");
    }

    /* ---- 4. row rendering ---------------------------------------- */
    {
        struct fs_entry f;
        memset(&f, 0, sizeof f);
        strcpy(f.name, "NOTES.TXT");
        f.size = 512;

        fs_row(line, sizeof line, FS_ROW_W, &f, 0);
        check(strlen(line) == FS_ROW_W, "row: file row is 70 columns");
        check(starts_with(line, "  NOTES.TXT"), "row: unselected prefix is \"  \"");
        check(line[2] == 'N' && line[2 + 9] == ' ',
              "row: the name starts at column 2 and is space padded");
        check(s_eq(line + (FS_ROW_W - FS_SIZE_COL), "      512B"),
              "row: file size right-aligned in the last 10 columns");

        fs_row(line, sizeof line, FS_ROW_W, &f, 1);
        check(starts_with(line, "> NOTES.TXT"), "row: selected prefix is \"> \"");
        check(strlen(line) == FS_ROW_W, "row: selected row is also 70 columns");
        check(s_eq(line + (FS_ROW_W - FS_SIZE_COL), "      512B"),
              "row: selection does not disturb the size column");

        strcpy(f.name, "BIG.BIN");
        f.size = 2048;
        fs_row(line, sizeof line, FS_ROW_W, &f, 0);
        check(ends_with(line, "      2.0K"), "row: 2 KiB file shows \"2.0K\"");

        /* a 12-char 8.3 name (the FAT maximum) must survive intact */
        strcpy(f.name, "ABCDEFGH.IJK");
        f.size = 512;
        fs_row(line, sizeof line, FS_ROW_W, &f, 0);
        check(strlen(line) == FS_ROW_W && has(line, "ABCDEFGH.IJK"),
              "row: 12-char 8.3 name is not clipped at width 70");
        check(line[2 + 12] == ' ' && line[FS_ROW_W - 1] == 'B',
              "row: name is padded before the size column");

        /* the widest name storable in struct fs_entry (31 chars) still fits */
        fill(f.name, 31, 'A');
        fs_row(line, sizeof line, FS_ROW_W, &f, 0);
        check(has(line, f.name) && strlen(line) == FS_ROW_W,
              "row: 31-char name fits at width 70 (58 columns available)");

        /* clipping: a narrow row width clips the name at the boundary */
        strcpy(f.name, "ABCDEFGH.IJKLMNOPQRSTUVWXYZ");
        fs_row(line, sizeof line, 20, &f, 0);
        check(s_eq(line, "  ABCDEFGH      512B") && strlen(line) == 20,
              "row: narrow width clips the name exactly at the size column");

        strcpy(f.name, "NOTES.TXT");
        fs_row(line, sizeof line, 24, &f, 1);
        check(strlen(line) == 24 && starts_with(line, "> NOTES.TXT") &&
                  ends_with(line, "      512B"),
              "row: narrow but sufficient width keeps name and size intact");
    }

    {
        struct fs_entry d;
        memset(&d, 0, sizeof d);
        strcpy(d.name, "SUBDIR");
        d.attr = FS_ATTR_DIR;
        d.size = 0;

        fs_row(line, sizeof line, FS_ROW_W, &d, 0);
        check(strlen(line) == FS_ROW_W, "row: directory row is 70 columns");
        check(starts_with(line, "  [DIR] SUBDIR"),
              "row: directory shows \"[DIR] NAME\"");
        check(all_spaces(line + (FS_ROW_W - FS_SIZE_COL)),
              "row: directory size column is blank");

        fs_row(line, sizeof line, FS_ROW_W, &d, 1);
        check(starts_with(line, "> [DIR] SUBDIR"), "row: selected directory prefix");

        fill(d.name, 31, 'D');
        fs_row(line, sizeof line, FS_ROW_W, &d, 0);
        check(has(line, d.name) && strlen(line) == FS_ROW_W,
              "row: 31-char directory name fits (54 columns available)");

        fs_row(line, sizeof line, 30, &d, 0);
        check(strlen(line) == 30 && starts_with(line, "  [DIR] ") &&
                  all_spaces(line + 30 - FS_SIZE_COL),
              "row: narrow directory row clips the name, keeps the blank cell");
    }

    /* ---- 5. free-space text -------------------------------------- */
    fs_free_text(buf, sizeof buf, 1, 40U * 1024U * 1024U);
    check(s_eq(buf, "Free: 40.0M"), "free: 40 MiB -> \"Free: 40.0M\"");
    fs_free_text(buf, sizeof buf, 1, 0);
    check(s_eq(buf, "Free: 0B"), "free: 0 -> \"Free: 0B\"");
    fs_free_text(buf, sizeof buf, 1, 123456);
    check(s_eq(buf, "Free: 120.5K"), "free: 123456 -> \"Free: 120.5K\"");
    fs_free_text(buf, sizeof buf, 0, 40U * 1024U * 1024U);
    check(s_eq(buf, "Free: ?"), "free: failed sysinfo probe -> \"Free: ?\"");
    fs_free_text(buf, sizeof buf, 0, 0);
    check(s_eq(buf, "Free: ?"), "free: failure with no bytes still shows \"?\"");

    /* ---- 6. path/free line --------------------------------------- */
    fs_path_line(buf, sizeof buf, FS_ROW_W, "/home", "Free: 40.0M");
    check(strlen(buf) == FS_ROW_W, "path: line is 70 columns");
    check(starts_with(buf, "Path: /home"), "path: starts with \"Path: <cwd>\"");
    check(buf[FS_ROW_W - (int)strlen("Free: 40.0M")] == 'F' &&
              s_eq(buf + (FS_ROW_W - (int)strlen("Free: 40.0M")), "Free: 40.0M"),
          "path: free-space text is right-aligned to the last column");

    fs_path_line(buf, sizeof buf, FS_ROW_W, "/", "Free: ?");
    check(starts_with(buf, "Path: /") && ends_with(buf, "Free: ?") &&
              strlen(buf) == FS_ROW_W,
          "path: root cwd with right-aligned free text");

    {
        char cwd[200];
        char filler[200];
        fill(filler, 39, 'a');
        snprintf(cwd, sizeof cwd, "/home/%s", filler);      /* 45 chars */
        fs_path_line(buf, sizeof buf, FS_ROW_W, cwd, "Free: 40.0M");
        check(strlen(buf) == FS_ROW_W && !has(buf, "...") && has(buf, cwd),
              "path: a 45-char cwd still fits (53 columns available)");
        check(ends_with(buf, "Free: 40.0M"),
              "path: 45-char cwd keeps the right-aligned free text");
    }

    {
        /* exactly 53 chars = 70 - 11 ("Free: 40.0M") - 6 ("Path: ") */
        char cwd[200];
        char filler[200];
        char exp[FS_ROW_W + 8];
        fill(filler, 47, 'b');
        snprintf(cwd, sizeof cwd, "/home/%s", filler);      /* 53 chars */
        fs_path_line(buf, sizeof buf, FS_ROW_W, cwd, "Free: 40.0M");
        snprintf(exp, sizeof exp, "Path: %s", cwd);
        pad_to(exp, FS_ROW_W - (int)strlen("Free: 40.0M"));
        strcat(exp, "Free: 40.0M");
        check(strlen(buf) == FS_ROW_W && !has(buf, "...") && s_eq(buf, exp),
              "path: exactly-fitting cwd is not truncated");
    }

    {
        /* 86 chars: clipped to the tail, prefixed with "..." */
        char cwd[200];
        char filler[200];
        int free_at = FS_ROW_W - (int)strlen("Free: 40.0M");
        fill(filler, 76, 'c');
        snprintf(cwd, sizeof cwd, "/aa%sTAILDIR", filler);  /* 86 chars */
        fs_path_line(buf, sizeof buf, FS_ROW_W, cwd, "Free: 40.0M");
        check(strlen(buf) == FS_ROW_W, "path: 86-char cwd still renders 70 columns");
        check(starts_with(buf, "Path: ..."),
              "path: long cwd is clipped with an ellipsis");
        check(has(buf, "TAILDIR"), "path: long cwd keeps its tail");
        check(!starts_with(buf, "Path: /aa"), "path: leading cwd text is dropped");
        check(s_eq(buf + free_at, "Free: 40.0M"),
              "path: free text stays right-aligned after cwd clipping");
    }

    /* ---- 7. dialog message builders ------------------------------ */
    fs_file_info_msg(buf, sizeof buf, "NOTES.TXT", 512);
    check(s_eq(buf, "NOTES.TXT  512B (512 bytes)"),
          "msg: file info shows name, human size and byte count");
    fs_file_info_msg(buf, sizeof buf, "BIG.BIN", 40U * 1024U * 1024U);
    check(s_eq(buf, "BIG.BIN  40.0M (41943040 bytes)"),
          "msg: file info for a large file");
    fs_file_info_msg(buf, 12, "NOTES.TXT", 512);
    check(strlen(buf) == 11 && starts_with(buf, "NOTES.TXT"),
          "msg: file info respects the caller's cap");
    fs_confirm_msg(buf, sizeof buf, "NOTES.TXT");
    check(s_eq(buf, "Delete NOTES.TXT?"), "msg: delete confirmation question");
    fs_confirm_msg(buf, sizeof buf, "A");
    check(s_eq(buf, "Delete A?"), "msg: delete confirmation with a short name");

    /* ---- 8. loading the directory -------------------------------- */
    chdir("/home");
    st_reset(&st);
    check(fs_load(&st) == 7, "load: read_dir yields the 7 mock entries");
    check(st.count == 7 && st.list.count == 7, "load: state tracks the count");
    check(s_eq(st.entries[0].name, "EDITOR.BIN"), "load: first entry name");
    check(s_eq(st.entries[6].name, "NOTES.TXT"), "load: last entry name");
    check(st.entries[0].attr == 0 && st.entries[0].size == 0,
          "load: attr/size copied from sys_dirent");
    check(st.entries[7].name[0] == '\0',
          "load: stops at read_dir's -1 (no ghost 8th entry)");
    {
        struct sys_dirent ent;
        memset(&ent, 0, sizeof ent);
        check(read_dir("/", 7, &ent) < 0,
              "load: mock read_dir reports end of list at 7");
        check(read_dir("/", 6, &ent) == 0 && s_eq(ent.name, "NOTES.TXT"),
              "load: mock read_dir still serves index 6");
    }
    check(st.list.selected == 0 && st.list.top == 0,
          "load: selection starts at the first entry");
    check(st.list.visible == FS_VISIBLE, "load: scroll window is 18 rows");

    /* stale entries beyond the new count are not rendered */
    st_reset(&st);
    for (int i = 0; i < FS_MAX_ENTRIES; i++) st_add(&st, "STALE9.TXT", 0, 4096);
    fs_render(buf, sizeof buf, &st);
    check(has(buf, "STALE9.TXT"), "load: preloaded entries render before a reload");
    fs_load(&st);
    fs_render(buf, sizeof buf, &st);
    check(st.count == 7 && !has(buf, "STALE"),
          "load: reload replaces the listing (stale entries gone)");

    /* selection clamping across a reload */
    st_fill30(&st);
    st.list.selected = 50;
    fs_load(&st);
    check(st.list.selected == 6, "load: selection clamps to the new last entry");
    st.list.selected = -3;
    fs_load(&st);
    check(st.list.selected == 0, "load: negative selection clamps to 0");

    /* fs_refresh: cwd + cached free space + listing */
    chdir("/home/dir");
    st_reset(&st);
    fs_refresh(&st);
    check(s_eq(st.cwd, "/home/dir"), "refresh: cwd comes from getcwd()");
    check(st.free_ok == 1 && st.free_bytes == 40ULL * 1024 * 1024,
          "refresh: free space cached from sysinfo(7)");
    check(st.count == 7, "refresh: listing reloaded");

    st_reset(&st);
    fs_init(&st);
    check(st.count == 7 && st.free_ok == 1 && s_eq(st.cwd, "/home/dir"),
          "init: fs_init loads cwd, free space and entries");
    check(st.list.visible == FS_VISIBLE && st.list.selected == 0,
          "init: fs_init sets up the scroll window");

    /* ---- 9. selection and scroll window -------------------------- */
    st_fill30(&st);
    check(st.count == 30 && fs_selected(&st) == 0, "sel: first entry selected");
    fs_move(&st, 1);
    check(st.list.selected == 1, "sel: Down moves the selection");
    fs_move(&st, -1);
    check(st.list.selected == 0, "sel: Up moves back");
    fs_move(&st, -1);
    check(st.list.selected == 0, "sel: Up at the top clamps to 0");
    fs_move(&st, 1000);
    check(st.list.selected == 29, "sel: Down clamps at the last entry");
    check(st.list.top == 12, "scroll: last entry scrolls top to 29-18+1");
    fs_move(&st, -5);
    check(st.list.selected == 24 && st.list.top == 12,
          "scroll: moving up inside the window leaves top alone");
    fs_move(&st, -20);
    check(st.list.selected == 4 && st.list.top == 4,
          "scroll: moving above the window pulls top up");
    fs_move(&st, -1000);
    check(st.list.selected == 0 && st.list.top == 0,
          "scroll: back at the top the window resets");
    st.list.top = 9;
    st.list.selected = 2;
    gui_list_ensure_visible(&st.list);
    check(st.list.top == 2, "scroll: ensure_visible pulls top back to selected");

    st_reset(&st);
    check(fs_selected(&st) == -1, "sel: empty listing has no selection");
    fs_move(&st, 5);
    check(st.list.selected == 0, "sel: no selection in an empty listing");
    st_reset(&st);
    for (int i = 0; i < 7; i++) st_addf(&st, 0, 512, "F%02d.TXT", i);
    fs_move(&st, 6);
    check(st.list.selected == 6 && st.list.top == 0,
          "scroll: 7 entries never scroll (window is 18 rows)");
    st.list.selected = 6;
    st.list.top = 0;
    check(gui_list_click_row(&st.list, FS_ROW_ENTRY0 + 3, FS_ROW_ENTRY0) == 3,
          "scroll: click row 6 maps to index 3 through the app's list");

    /* ---- 10. keys ------------------------------------------------ */
    chdir("/home");

    st_reset(&st);
    ev = ev_char('q');
    check(fs_handle_event(&st, &ev) == FS_ACT_QUIT,
          "key: 'q' returns the quit flag (no exit in tests)");
    ev = ev_char('x');
    check(fs_handle_event(&st, &ev) == FS_ACT_NONE,
          "key: unrelated characters are ignored");
    ev = ev_char('D');
    check(fs_handle_event(&st, &ev) == FS_ACT_NONE,
          "key: uppercase 'D' is not a shortcut");
    ev = ev_type(GUI_EV_ESC);
    check(fs_handle_event(&st, &ev) == FS_ACT_NONE, "key: ESC ignored");
    ev = ev_type(GUI_EV_LEFT);
    check(fs_handle_event(&st, &ev) == FS_ACT_NONE, "key: LEFT ignored");
    ev = ev_type(GUI_EV_RIGHT);
    check(fs_handle_event(&st, &ev) == FS_ACT_NONE, "key: RIGHT ignored");

    st_fill30(&st);
    ev = ev_type(GUI_EV_DOWN);
    check(fs_handle_event(&st, &ev) == FS_ACT_REDRAW, "key: Down redraws");
    check(st.list.selected == 1, "key: Down event moves the selection");
    ev = ev_type(GUI_EV_UP);
    fs_handle_event(&st, &ev);
    check(st.list.selected == 0, "key: Up event moves back");
    fs_handle_event(&st, &ev);
    check(st.list.selected == 0, "key: Up event clamps at the top");
    st.list.selected = 29;
    ev = ev_type(GUI_EV_DOWN);
    fs_handle_event(&st, &ev);
    check(st.list.selected == 29, "key: Down event clamps at the bottom");

    /* Enter on a file: message dialog, no chdir */
    dialog_mocks_reset();
    chdir("/home");
    st_reset(&st);
    st_add(&st, "NOTES.TXT", 0, 512);
    ev = ev_char('\n');
    check(fs_handle_event(&st, &ev) == FS_ACT_REDRAW, "key: Enter redraws");
    check(mock_msg_calls == 1 && s_eq(mock_msg_title, "File"),
          "key: Enter on a file opens the \"File\" message dialog");
    check(s_eq(mock_msg_body, "NOTES.TXT  512B (512 bytes)"),
          "key: file dialog body carries name and size");
    getcwd(cwd_buf, sizeof cwd_buf);
    check(s_eq(cwd_buf, "/home"), "key: Enter on a file does not chdir");
    check(mock_prompt_calls == 0 && mock_confirm_calls == 0,
          "key: Enter on a file asks no other question");
    check(st.count == 1, "key: Enter on a file does not reload the listing");
    ev = ev_char('\r');
    fs_handle_event(&st, &ev);
    check(mock_msg_calls == 2, "key: CR also opens the selected entry");

    /* Enter on a directory: chdir + refresh */
    dialog_mocks_reset();
    chdir("/home");
    st_reset(&st);
    st_add(&st, "SUBDIR", FS_ATTR_DIR, 0);
    ev = ev_char('\n');
    fs_handle_event(&st, &ev);
    getcwd(cwd_buf, sizeof cwd_buf);
    check(s_eq(cwd_buf, "SUBDIR"), "key: Enter on a directory chdirs into it");
    check(st.count == 7, "key: Enter on a directory reloads the listing");
    check(mock_msg_calls == 0, "key: entering a directory shows no dialog");

    /* Enter with an empty listing is a no-op */
    dialog_mocks_reset();
    st_reset(&st);
    ev = ev_char('\n');
    check(fs_handle_event(&st, &ev) == FS_ACT_REDRAW && mock_msg_calls == 0,
          "key: Enter on an empty listing does nothing");

    /* 'd' delete: confirmed, unlink succeeds */
    dialog_mocks_reset();
    chdir("/home");
    st_reset(&st);
    st_add(&st, "OLD.TXT", 0, 512);
    mock_confirm_result = 1;
    mock_unlink_result = 0;
    ev = ev_char('d');
    check(fs_handle_event(&st, &ev) == FS_ACT_REDRAW, "key: 'd' redraws");
    check(mock_confirm_calls == 1 && s_eq(mock_confirm_title, "Delete"),
          "del: confirmation dialog uses the \"Delete\" title");
    check(s_eq(mock_confirm_body, "Delete OLD.TXT?"),
          "del: confirmation question names the file");
    check(mock_msg_calls == 0, "del: successful unlink shows no error");
    check(st.count == 7, "del: listing reloaded after a delete");

    /* 'd' delete: unlink fails */
    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "OLD.TXT", 0, 512);
    mock_confirm_result = 1;
    mock_unlink_result = -1;
    ev = ev_char('d');
    fs_handle_event(&st, &ev);
    check(mock_msg_calls == 1 && s_eq(mock_msg_title, "Error"),
          "del: failed unlink shows the error dialog");
    check(s_eq(mock_msg_body, "Cannot delete file."), "del: failure message text");
    check(st.count == 7, "del: listing still refreshed after a failure");

    /* 'd' delete: user cancels */
    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "OLD.TXT", 0, 512);
    mock_confirm_result = 0;
    mock_unlink_result = -1;   /* would report an error if unlink were called */
    ev = ev_char('d');
    fs_handle_event(&st, &ev);
    check(mock_confirm_calls == 1 && mock_msg_calls == 0,
          "del: cancelled confirmation deletes nothing");
    check(st.count == 1, "del: cancelled confirmation leaves the listing alone");

    /* 'd' with an empty listing */
    dialog_mocks_reset();
    st_reset(&st);
    ev = ev_char('d');
    fs_handle_event(&st, &ev);
    check(mock_confirm_calls == 0 && mock_msg_calls == 0,
          "del: empty listing asks nothing");

    /* 'r' rename: succeeds */
    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "OLD.TXT", 0, 512);
    mock_prompt_result = 1;
    strcpy(mock_prompt_answer, "NEW.TXT");
    mock_rename_result = 0;
    ev = ev_char('r');
    check(fs_handle_event(&st, &ev) == FS_ACT_REDRAW, "key: 'r' redraws");
    check(mock_prompt_calls == 1 && s_eq(mock_prompt_title, "Rename"),
          "ren: asks the \"Rename\" prompt");
    check(mock_msg_calls == 0, "ren: successful rename shows no error");
    check(st.count == 7, "ren: listing reloaded after a rename");

    /* 'r' rename: rename() fails */
    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "OLD.TXT", 0, 512);
    mock_prompt_result = 1;
    strcpy(mock_prompt_answer, "NEW.TXT");
    mock_rename_result = -1;
    ev = ev_char('r');
    fs_handle_event(&st, &ev);
    check(mock_msg_calls == 1 && s_eq(mock_msg_body, "Cannot rename file."),
          "ren: failed rename reports an error");

    /* 'r' rename: prompt cancelled */
    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "OLD.TXT", 0, 512);
    mock_prompt_result = 0;
    mock_rename_result = -1;
    ev = ev_char('r');
    fs_handle_event(&st, &ev);
    check(mock_prompt_calls == 1 && mock_msg_calls == 0 && st.count == 1,
          "ren: cancelled prompt renames nothing");

    /* 'r' rename: empty answer is treated as cancel */
    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "OLD.TXT", 0, 512);
    mock_prompt_result = 1;
    mock_prompt_answer[0] = '\0';
    mock_rename_result = -1;
    ev = ev_char('r');
    fs_handle_event(&st, &ev);
    check(mock_msg_calls == 0 && st.count == 1, "ren: empty name renames nothing");

    /* 'r' with an empty listing */
    dialog_mocks_reset();
    st_reset(&st);
    ev = ev_char('r');
    fs_handle_event(&st, &ev);
    check(mock_prompt_calls == 0, "ren: empty listing asks no name");

    /* 'n' new folder: succeeds */
    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "A.TXT", 0, 512);
    mock_prompt_result = 1;
    strcpy(mock_prompt_answer, "MYDIR");
    mock_mkdir_result = 0;
    ev = ev_char('n');
    check(fs_handle_event(&st, &ev) == FS_ACT_REDRAW, "key: 'n' redraws");
    check(s_eq(mock_prompt_title, "New Folder"),
          "new: asks the \"New Folder\" prompt");
    check(mock_msg_calls == 0, "new: successful mkdir shows no error");
    check(st.count == 7, "new: listing reloaded after mkdir");

    /* 'n' new folder: mkdir fails */
    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "A.TXT", 0, 512);
    mock_prompt_result = 1;
    strcpy(mock_prompt_answer, "MYDIR");
    mock_mkdir_result = -1;
    ev = ev_char('n');
    fs_handle_event(&st, &ev);
    check(mock_msg_calls == 1 && s_eq(mock_msg_body, "Cannot create folder."),
          "new: failed mkdir reports an error");

    /* 'n' new folder: cancelled / empty */
    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "A.TXT", 0, 512);
    mock_prompt_result = 0;
    mock_mkdir_result = -1;
    ev = ev_char('n');
    fs_handle_event(&st, &ev);
    check(mock_msg_calls == 0 && st.count == 1,
          "new: cancelled prompt creates nothing");
    mock_prompt_result = 1;
    mock_prompt_answer[0] = '\0';
    fs_handle_event(&st, &ev);
    check(mock_msg_calls == 0 && st.count == 1, "new: empty name creates nothing");

    /* Backspace goes up one level */
    dialog_mocks_reset();
    chdir("/home/sub");
    st_reset(&st);
    st_add(&st, "A.TXT", 0, 512);
    ev = ev_char('\b');
    check(fs_handle_event(&st, &ev) == FS_ACT_REDRAW, "key: Backspace redraws");
    getcwd(cwd_buf, sizeof cwd_buf);
    check(s_eq(cwd_buf, ".."), "key: Backspace chdirs to \"..\" (mock is literal)");
    check(s_eq(st.cwd, ".."), "key: Backspace refreshes the shown path");
    check(st.count == 7, "key: Backspace reloads the parent listing");
    chdir("/home/sub");
    ev = ev_char(127);
    fs_handle_event(&st, &ev);
    getcwd(cwd_buf, sizeof cwd_buf);
    check(s_eq(cwd_buf, ".."), "key: DEL (127) behaves like Backspace");

    /* mouse: row mapping uses FS_ROW_ENTRY0 and the scroll window */
    chdir("/home");
    st_fill30(&st);
    ev = ev_mouse(4, FS_ROW_ENTRY0, 1);
    check(fs_handle_event(&st, &ev) == FS_ACT_NONE,
          "mouse: click on the already-selected row needs no redraw");
    check(st.list.selected == 0, "mouse: click on the first row keeps index 0");
    ev = ev_mouse(4, FS_ROW_ENTRY0 + 2, 1);
    check(fs_handle_event(&st, &ev) == FS_ACT_REDRAW, "mouse: click redraws");
    check(st.list.selected == 2, "mouse: click on row 5 selects index 2");
    ev = ev_mouse(4, FS_ROW_RULE, 1);
    fs_handle_event(&st, &ev);
    check(st.list.selected == 2, "mouse: click on the rule row is ignored");
    ev = ev_mouse(4, FS_ROW_LEGEND, 1);
    fs_handle_event(&st, &ev);
    check(st.list.selected == 2, "mouse: click on the legend row is ignored");
    ev = ev_mouse(4, FS_ROW_ENTRY0, 2);
    fs_handle_event(&st, &ev);
    check(st.list.selected == 2, "mouse: right-click does not select");
    st.list.top = 5;
    ev = ev_mouse(4, FS_ROW_ENTRY0, 1);
    fs_handle_event(&st, &ev);
    check(st.list.selected == 5 && st.list.top == 5,
          "mouse: clicks map through the scroll window");
    fs_handle_event(&st, &ev);
    check(st.list.selected == 5, "mouse: re-clicking the same row is a no-op");
    {
        struct fs_state empty;
        st_reset(&empty);
        ev = ev_mouse(4, FS_ROW_ENTRY0, 1);
        fs_handle_event(&empty, &ev);
        check(empty.list.selected == 0 && empty.count == 0,
              "mouse: clicking an empty listing is safe");
    }

    /* ---- 11. menus ------------------------------------------------ */
    dialog_mocks_reset();
    chdir("/home");

    st_fill30(&st);
    check(fs_handle_menu(&st, 0, 4) == FS_ACT_REDRAW,
          "menu: File>Refresh redraws");
    check(st.count == 7, "menu: File>Refresh reloads the listing");
    ev = ev_menu(0, 4);
    fs_handle_event(&st, &ev);
    check(st.count == 7, "menu: File>Refresh through an event works");

    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "NOTES.TXT", 0, 512);
    check(fs_handle_menu(&st, 0, 0) == FS_ACT_REDRAW && mock_msg_calls == 1 &&
              s_eq(mock_msg_title, "File"),
          "menu: File>Open on a file shows the size dialog");

    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "SUBDIR", FS_ATTR_DIR, 0);
    fs_handle_menu(&st, 0, 0);
    getcwd(cwd_buf, sizeof cwd_buf);
    check(s_eq(cwd_buf, "SUBDIR"), "menu: File>Open on a directory chdirs");

    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "OLD.TXT", 0, 512);
    mock_confirm_result = 1;
    check(fs_handle_menu(&st, 0, 1) == FS_ACT_REDRAW &&
              mock_confirm_calls == 1 && s_eq(mock_confirm_body, "Delete OLD.TXT?"),
          "menu: File>Delete confirms like 'd'");

    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "OLD.TXT", 0, 512);
    mock_prompt_result = 1;
    strcpy(mock_prompt_answer, "NEW.TXT");
    check(fs_handle_menu(&st, 0, 2) == FS_ACT_REDRAW &&
              s_eq(mock_prompt_title, "Rename"),
          "menu: File>Rename opens the rename prompt");

    dialog_mocks_reset();
    st_reset(&st);
    st_add(&st, "A.TXT", 0, 512);
    mock_prompt_result = 1;
    strcpy(mock_prompt_answer, "MYDIR");
    check(fs_handle_menu(&st, 0, 3) == FS_ACT_REDRAW &&
              s_eq(mock_prompt_title, "New Folder"),
          "menu: File>New Folder opens the folder prompt");

    check(fs_handle_menu(&st, 0, 9) == FS_ACT_NONE, "menu: unknown File item ignored");
    check(fs_handle_menu(&st, 2, 0) == FS_ACT_NONE, "menu: unknown menu index ignored");
    check(fs_handle_menu(&st, 1, 9) == FS_ACT_NONE, "menu: unknown Go item ignored");

    chdir("/home/sub");
    fs_handle_menu(&st, 1, 0);               /* Go > Up */
    getcwd(cwd_buf, sizeof cwd_buf);
    check(s_eq(cwd_buf, ".."), "menu: Go>Up chdirs to \"..\"");
    fs_handle_menu(&st, 1, 1);               /* Go > Root */
    getcwd(cwd_buf, sizeof cwd_buf);
    check(s_eq(cwd_buf, "/"), "menu: Go>Root chdirs to \"/\"");
    fs_handle_menu(&st, 1, 2);               /* Go > Home */
    getcwd(cwd_buf, sizeof cwd_buf);
    check(s_eq(cwd_buf, "/home"), "menu: Go>Home chdirs to \"/home\"");
    check(st.count == 7, "menu: Go items reload the listing");
    ev = ev_menu(1, 2);
    fs_handle_event(&st, &ev);
    getcwd(cwd_buf, sizeof cwd_buf);
    check(s_eq(cwd_buf, "/home"), "menu: Go>Home through an event works");

    /* ---- 12. full-screen rendering -------------------------------- */
    chdir("/home");
    st_plain(&st);
    st_add(&st, "SUBDIR", FS_ATTR_DIR, 0);
    st_add(&st, "NOTES.TXT", 0, 512);
    st_add(&st, "BIG.BIN", 0, 2048);
    st.list.selected = 1;
    {
        int len = fs_render(buf, sizeof buf, &st);
        char exp[1024];
        int el = 0;
        char l[FS_ROW_W + 8];

        el += snprintf(exp + el, sizeof exp - el, "=== Files ===\n");
        mkpath(l, "/home", "Free: 40.0M");
        el += snprintf(exp + el, sizeof exp - el, "%s\n", l);
        mkrule(l);
        el += snprintf(exp + el, sizeof exp - el, "%s\n", l);
        mkrow(l, "  [DIR] SUBDIR", "");
        el += snprintf(exp + el, sizeof exp - el, "%s\n", l);
        mkrow(l, "> NOTES.TXT", "512B");
        el += snprintf(exp + el, sizeof exp - el, "%s\n", l);
        mkrow(l, "  BIG.BIN", "2.0K");
        el += snprintf(exp + el, sizeof exp - el, "%s\n", l);
        el += snprintf(exp + el, sizeof exp - el, "%s\n", LEGEND_EXPECTED);

        check(len > 0 && s_eq(buf, exp), "render: exact 3-entry screen snapshot");
        check(count_lines(buf) == 7, "render: 3 entries -> 3 header + 3 rows + legend");
        get_line(buf, 0, line, sizeof line);
        check(s_eq(line, "=== Files ==="), "render: row 0 is the title");
        get_line(buf, 1, line, sizeof line);
        check(starts_with(line, "Path: /home") && ends_with(line, "Free: 40.0M") &&
                  strlen(line) == FS_ROW_W,
              "render: row 1 is the path/free line");
        get_line(buf, 2, line, sizeof line);
        check(strlen(line) == FS_ROW_W && line[0] == '+' &&
                  line[FS_ROW_W - 1] == '+' && line[1] == '-',
              "render: row 2 is a gui_rule separator");
        get_line(buf, 3, line, sizeof line);
        check(starts_with(line, "  [DIR] SUBDIR"), "render: row 3 is the first entry");
        get_line(buf, 4, line, sizeof line);
        check(starts_with(line, "> ") && has(line, "NOTES.TXT"),
              "render: the selected entry carries the \"> \" marker");
        get_line(buf, 5, line, sizeof line);
        check(starts_with(line, "  BIG.BIN"), "render: unselected entries use \"  \"");
        get_line(buf, 6, line, sizeof line);
        check(s_eq(line, LEGEND_EXPECTED), "render: the legend is the last row");
        check(count_marked_lines(buf) == 1, "render: exactly one selection marker");
    }

    /* empty directory */
    st_plain(&st);
    fs_render(buf, sizeof buf, &st);
    get_line(buf, 3, line, sizeof line);
    check(s_eq(line, "  (empty)"), "render: an empty directory shows \"(empty)\"");
    check(count_lines(buf) == 5, "render: empty directory is 5 rows");
    get_line(buf, 4, line, sizeof line);
    check(s_eq(line, LEGEND_EXPECTED), "render: empty listing still shows the legend");
    check(!has(buf, "> "), "render: empty listing has no selection marker");

    /* failed free-space probe */
    st.free_ok = 0;
    fs_render(buf, sizeof buf, &st);
    get_line(buf, 1, line, sizeof line);
    check(has(line, "Free: ?"), "render: failed probe renders \"Free: ?\"");
    check(strlen(line) == FS_ROW_W, "render: \"Free: ?\" line is still 70 columns");

    /* the render uses the cached figure (no live sysinfo call) */
    st.free_ok = 1;
    st.free_bytes = 123456;
    fs_render(buf, sizeof buf, &st);
    get_line(buf, 1, line, sizeof line);
    check(has(line, "Free: 120.5K"), "render: shows the cached free-space value");

    /* a long cwd is clipped on screen but keeps the free text */
    strcpy(st.cwd,
           "/very/long/path/that/keeps/going/and/going/and/going/past/the/limit/ENDDIR");
    fs_render(buf, sizeof buf, &st);
    get_line(buf, 1, line, sizeof line);
    check(strlen(line) == FS_ROW_W && ends_with(line, "Free: 120.5K"),
          "render: long cwd line still fits and keeps the free text");

    /* full 64-entry listing: 18 rows, budget, determinism */
    st_plain(&st);
    for (int i = 0; i < FS_MAX_ENTRIES; i++) {
        st_addf(&st, 0, (uint32_t)(i * 1024), "FILE%02d.TXT", i);
    }
    {
        int len = fs_render(buf, sizeof buf, &st);
        check(st.count == 64 && len > 0 && len <= FS_SCREEN_MAX,
              "render: 64 entries fit the screen budget");
        check(len < 2048, "render: screen fits the window's 2048-byte buffer");
        check(count_lines(buf) == 22, "render: 22 rows (3 header + 18 + legend)");
        get_line(buf, 3, line, sizeof line);
        check(has(line, "FILE00.TXT"), "render: the scroll window starts at top");
        get_line(buf, 20, line, sizeof line);
        check(has(line, "FILE17.TXT"), "render: the window shows entries top..top+17");
        get_line(buf, 21, line, sizeof line);
        check(s_eq(line, LEGEND_EXPECTED), "render: legend stays on row 21");
        check(!has(buf, "FILE18.TXT"), "render: entries past the window are not drawn");
        check(count_marked_lines(buf) == 1, "render: one marker in a full screen");
        check(max_line_len(buf) <= FS_ROW_W, "render: no line exceeds 70 columns");
        check(max_line_len(buf) <= 110, "render: no line exceeds the 110-col limit");

        st.list.selected = 63;
        gui_list_ensure_visible(&st.list);
        len = fs_render(buf, sizeof buf, &st);
        get_line(buf, 3, line, sizeof line);
        get_line(buf, 20, line2, sizeof line2);
        check(st.list.top == 46 && has(line, "FILE46.TXT") && has(line2, "FILE63.TXT"),
              "render: scrolled to the bottom shows 46..63");
        check(starts_with(line2, "> "), "render: marker follows the selection");
        check(count_lines(buf) == 22, "render: scrolled screen keeps 22 rows");

        st.list.selected = 0;
        gui_list_ensure_visible(&st.list);
        fs_render(buf, sizeof buf, &st);
        get_line(buf, 3, line, sizeof line);
        check(starts_with(line, "> FILE00.TXT"),
              "render: marker returns to the first entry");
    }

    /* determinism */
    fs_render(buf, sizeof buf, &st);
    fs_render(buf2, sizeof buf2, &st);
    check(s_eq(buf, buf2), "render: same state renders identically");
    st.list.selected = 5;
    fs_render(buf, sizeof buf, &st);
    check(!s_eq(buf, buf2), "render: a different selection changes the screen");
    check(starts_with(buf2, "=== Files ===\n"), "render: screens start with the title");

    /* ---- 13. fs_redraw (the print path) --------------------------- */
    st_plain(&st);
    st_add(&st, "SUBDIR", FS_ATTR_DIR, 0);
    st_add(&st, "NOTES.TXT", 0, 512);
    FS = st;                       /* drive the global the app redraws */
    {
        int n = capture_redraw(buf, sizeof buf);
        check(n > 0 && buf[0] == '\f', "redraw: gui_clear() clears first");
        check(has(buf, "=== Files ===") && has(buf, "> [DIR] SUBDIR") &&
                  has(buf, "NOTES.TXT") && has(buf, LEGEND_EXPECTED),
              "redraw: the whole screen is printed after the clear");
        check(has(buf, "Path: /home"), "redraw: path line printed");
        check(has(buf, "Free: 40.0M"), "redraw: cached free space printed");
        check(count_marked_lines(buf) == 1, "redraw: one selection marker printed");
        {
            char rendered[FS_SCREEN_MAX + 128];
            fs_render(rendered, sizeof rendered, &FS);
            check(s_eq(buf + 1, rendered), "redraw: output equals fs_render's screen");
        }
    }

    /* ---- summary -------------------------------------------------- */
    printf("\n=== Test Results ===\n");
    printf("Checks run:    %d\n", checks_run);
    printf("Checks failed: %d\n", checks_failed);
    if (checks_failed == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", checks_failed);
    return 1;
}
