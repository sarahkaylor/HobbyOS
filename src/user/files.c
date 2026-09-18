/*
 * files.c - Files (file manager) for HobbyOS.
 *
 * A windowed text file manager over the FAT-16 volume:
 *   - Menu File: Open, Delete, Rename, New Folder, Refresh
 *   - Menu Go:   Up, Root, Home
 *   - Keys: Up/Down move the selection, Enter opens (directory: chdir,
 *     file: size dialog), 'd' deletes (with confirm), 'r' renames,
 *     'n' creates a folder, Backspace goes up one level, 'q' quits.
 *   - Mouse: a left click on an entry row selects that entry.
 *
 * Listing: read_dir("/", index, &ent) - HobbyOS resolves the path "/"
 * against the process cwd, so indices 0..N-1 are the children of the
 * current directory until read_dir() returns < 0.  Directories carry
 * attribute bit 0x10 (same convention as ls.c).  At most FS_MAX_ENTRIES
 * entries are kept.
 *
 * Rendering is deterministic: the whole screen is redrawn from fs_state
 * on every event (gui_clear() + one print()).  fs_render() writes the
 * screen into a caller supplied buffer, so the host unit test can assert
 * on the exact layout without capturing stdout.
 *
 * Fixed layout (content rows, 0-based):
 *   row 0       "=== Files ==="
 *   row 1       "Path: <cwd>" ... right-aligned "Free: <gui_size_str(free)>"
 *   row 2       gui_rule separator
 *   rows 3..20  up to 18 entry rows (scroll window driven by struct gui_list)
 *   row 21      key legend
 *
 * Host test seams: the three modal dialogs block on stdin, which a unit
 * test cannot satisfy, so the app reaches them through the fs_dlg_*
 * function pointers (initialized to the real dialog library).  The host
 * test swaps in non-blocking mocks; the ARM build always calls the real
 * dialogs.  Free space is probed once per refresh (sysinfo cmd 7) and the
 * cached result is what fs_render() shows.
 */

#include "libc.h"
#include "gui.h"
#include "dialog.h"

/* ---- Layout and limits ---- */

#define FS_ATTR_DIR      0x10   /* sys_dirent.attr bit: entry is a directory  */
#define FS_MAX_ENTRIES   64     /* hard cap on entries read per directory     */
#define FS_VISIBLE       18     /* entry rows on screen                       */
#define FS_ROW_W         70     /* width of every rendered line, in columns   */
#define FS_SIZE_COL      10     /* size right-aligned in the last 10 columns  */
#define FS_NAME_MAX      32     /* matches sys_dirent.name                    */
#define FS_PATH_MAX      128    /* cwd buffer                                 */
#define FS_SIZE_MAX      24     /* gui_size_str output buffer                 */
#define FS_MSG_MAX       96     /* dialog message buffer                      */
#define FS_SCREEN_MAX    1800   /* full screen buffer (window holds 2048)     */

/* Content rows (0-based).  The legend always lands on FS_ROW_LEGEND, and
 * the entry rows start at FS_ROW_ENTRY0 for the mouse row mapping. */
#define FS_ROW_TITLE     0
#define FS_ROW_PATH      1
#define FS_ROW_RULE      2
#define FS_ROW_ENTRY0    3
#define FS_ROW_LEGEND    (FS_ROW_ENTRY0 + FS_VISIBLE)

#define FS_LEGEND "Enter=open  d=del  r=rename  n=new  Bksp=up  q=quit"

/* fs_handle_event() results (bit flags) */
#define FS_ACT_NONE      0
#define FS_ACT_REDRAW    1
#define FS_ACT_QUIT      2

/* ---- State ---- */

struct fs_entry {
    char     name[FS_NAME_MAX];
    uint8_t  attr;
    uint32_t size;
};

struct fs_state {
    struct fs_entry entries[FS_MAX_ENTRIES];
    int             count;       /* number of valid entries                  */
    struct gui_list list;        /* selection + scroll window                */
    char            cwd[FS_PATH_MAX];
    uint64_t        free_bytes;  /* cached at refresh time                   */
    int             free_ok;     /* 0 -> "Free: ?"                           */
};

static struct fs_state FS;

/* ---- Host-test seams (see file header) ----
 * The ARM build runs with these pointing at the real dialog library. */
static int  (*fs_dlg_confirm)(const char *title, const char *msg) = dialog_confirm;
static int  (*fs_dlg_prompt)(const char *title, const char *msg, char *buf, int max) = dialog_prompt;
static void (*fs_dlg_message)(const char *title, const char *msg) = dialog_message;

/* ====================================================================== */
/* Small string helpers (HobbyOS libc has almost none)                    */
/* ====================================================================== */

static int fs_slen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Copy src into dst (cap bytes including NUL); always NUL-terminates. */
static void fs_scopy(char *dst, int cap, const char *src) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Append src to dst (cap bytes including NUL).  Returns the new length. */
static int fs_sappend(char *dst, int cap, const char *src) {
    int n = fs_slen(dst);
    int i = 0;
    if (cap <= 0) return n;
    if (n > cap - 1) n = cap - 1;
    while (src[i] && n < cap - 1) { dst[n] = src[i]; i++; n++; }
    dst[n] = '\0';
    return n;
}

/* ---- buffer writers: keep a running length, truncate at cap, always
 * NUL-terminated.  They return the running length, which may be larger
 * than what was actually stored. ---- */

static int fs_putc(char *out, int cap, int len, int ch) {
    if (len < cap - 1) { out[len] = (char)ch; out[len + 1] = '\0'; }
    return len + 1;
}

static int fs_put(char *out, int cap, int len, const char *s) {
    for (int i = 0; s[i]; i++) len = fs_putc(out, cap, len, s[i]);
    return len;
}

static int fs_putn(char *out, int cap, int len, const char *s, int n) {
    for (int i = 0; i < n && s[i]; i++) len = fs_putc(out, cap, len, s[i]);
    return len;
}

/* Pad with spaces up to column `col`. */
static int fs_putpad(char *out, int cap, int len, int col) {
    while (len < col) len = fs_putc(out, cap, len, ' ');
    return len;
}

/* ====================================================================== */
/* Formatting helpers (pure: no state)                                    */
/* ====================================================================== */

static int fs_is_dir(const struct fs_entry *e) {
    return (e->attr & FS_ATTR_DIR) != 0;
}

/* Right-align the human readable size in exactly `width` columns.
 * Directories get a blank cell (their size is meaningless in FAT-16).
 * Writes a NUL-terminated string and returns its length. */
static int fs_size_cell(char *out, int cap, int width, int is_dir, uint32_t size) {
    int len = 0;
    if (width < 0) width = 0;
    if (is_dir) {
        for (int i = 0; i < width; i++) len = fs_putc(out, cap, len, ' ');
        return len;
    }
    char sz[FS_SIZE_MAX];
    gui_size_str((uint64_t)size, sz);
    /* A size wider than `width` simply overflows it (width >= FS_SIZE_COL
     * holds for every layout this app uses). */
    for (int i = fs_slen(sz); i < width; i++) len = fs_putc(out, cap, len, ' ');
    len = fs_put(out, cap, len, sz);
    return len;
}

/* One entry row: "> " or "  ", then "[DIR] " for directories, the clipped
 * name, space padding, and the size right-aligned in FS_SIZE_COL columns.
 * Selected rows are prefixed with "> ", others with two spaces. */
static int fs_row(char *out, int cap, int width, const struct fs_entry *e, int selected) {
    int len = 0;
    int is_dir = fs_is_dir(e);
    int name_max;

    len = fs_putc(out, cap, len, selected ? '>' : ' ');
    len = fs_putc(out, cap, len, ' ');
    if (is_dir) len = fs_put(out, cap, len, "[DIR] ");

    name_max = width - FS_SIZE_COL - len;       /* columns left for the name */
    if (name_max < 0) name_max = 0;
    len = fs_putn(out, cap, len, e->name, name_max);

    len = fs_putpad(out, cap, len, width - FS_SIZE_COL);

    char cell[FS_SIZE_COL + 1];
    cell[0] = '\0';
    fs_size_cell(cell, sizeof cell, FS_SIZE_COL, is_dir, e->size);
    len = fs_put(out, cap, len, cell);
    return len;
}

/* "Free: 40.0M" or "Free: ?" when the sysinfo probe failed. */
static void fs_free_text(char *out, int cap, int ok, uint64_t bytes) {
    if (!ok) { fs_scopy(out, cap, "Free: ?"); return; }
    char sz[FS_SIZE_MAX];
    gui_size_str(bytes, sz);
    fs_scopy(out, cap, "Free: ");
    fs_sappend(out, cap, sz);
}

/* "Path: <cwd>" with "Free: ..." right-aligned on the same line.
 * A cwd that does not fit is clipped to its tail and prefixed with "..."
 * (the tail is the informative part of a path). */
static int fs_path_line(char *out, int cap, int width, const char *cwd,
                        const char *free_text) {
    int len = 0;
    int flen = fs_slen(free_text);
    int clen = fs_slen(cwd);
    int avail = width - flen - 6;               /* 6 = "Path: " */

    if (avail < 0) avail = 0;
    len = fs_put(out, cap, len, "Path: ");
    if (clen <= avail) {
        len = fs_putn(out, cap, len, cwd, clen);
    } else if (avail >= 4) {
        len = fs_put(out, cap, len, "...");
        len = fs_putn(out, cap, len, cwd + (clen - (avail - 3)), avail - 3);
    } else {
        len = fs_putn(out, cap, len, cwd + (clen - avail), avail);
    }
    len = fs_putpad(out, cap, len, width - flen);
    len = fs_put(out, cap, len, free_text);
    return len;
}

/* Message for Enter on a file: "NAME  <size> (<bytes> bytes)". */
static void fs_file_info_msg(char *out, int cap, const char *name, uint64_t size) {
    char sz[FS_SIZE_MAX];
    char num[24];
    gui_size_str(size, sz);
    gui_uitoa((unsigned long)size, num);
    fs_scopy(out, cap, name);
    fs_sappend(out, cap, "  ");
    fs_sappend(out, cap, sz);
    fs_sappend(out, cap, " (");
    fs_sappend(out, cap, num);
    fs_sappend(out, cap, " bytes)");
}

/* Delete confirmation question: "Delete NAME?" */
static void fs_confirm_msg(char *out, int cap, const char *name) {
    fs_scopy(out, cap, "Delete ");
    fs_sappend(out, cap, name);
    fs_sappend(out, cap, "?");
}

/* ====================================================================== */
/* Directory loading / navigation                                         */
/* ====================================================================== */

/* Read the current directory (read_dir with "/" == cwd) into st, honouring
 * the FS_MAX_ENTRIES cap, then clamp the selection and scroll window. */
static int fs_load(struct fs_state *st) {
    struct sys_dirent ent;

    st->count = 0;
    for (int i = 0; i < FS_MAX_ENTRIES; i++) {
        if (read_dir("/", i, &ent) < 0) break;
        fs_scopy(st->entries[st->count].name, FS_NAME_MAX, ent.name);
        st->entries[st->count].attr = ent.attr;
        st->entries[st->count].size = ent.size;
        st->count++;
    }
    if (st->list.selected > st->count - 1) st->list.selected = st->count - 1;
    if (st->list.selected < 0) st->list.selected = 0;
    st->list.count = st->count;
    st->list.visible = FS_VISIBLE;
    gui_list_ensure_visible(&st->list);
    return st->count;
}

/* Refresh cwd, the cached free-space figure and the listing. */
static void fs_refresh(struct fs_state *st) {
    char buf[FS_PATH_MAX];
    struct sys_fsinfo fsinfo;

    if (getcwd(buf, sizeof buf) == 0) buf[0] = '\0';
    fs_scopy(st->cwd, FS_PATH_MAX, buf);

    if (sysinfo(7, &fsinfo, sizeof fsinfo) == 0) {
        st->free_bytes = fsinfo.free_bytes;
        st->free_ok = 1;
    } else {
        st->free_bytes = 0;
        st->free_ok = 0;
    }
    fs_load(st);
}

/* Reset the state and load the current directory (startup). */
static void fs_init(struct fs_state *st) {
    st->count = 0;
    st->list.selected = 0;
    st->list.top = 0;
    st->list.count = 0;
    st->list.visible = FS_VISIBLE;
    st->cwd[0] = '\0';
    st->free_bytes = 0;
    st->free_ok = 0;
    for (int i = 0; i < FS_MAX_ENTRIES; i++) st->entries[i].name[0] = '\0';
    fs_refresh(st);
}

/* Selected index, or -1 when the listing is empty / out of range. */
static int fs_selected(const struct fs_state *st) {
    if (st->count <= 0) return -1;
    if (st->list.selected < 0 || st->list.selected >= st->count) return -1;
    return st->list.selected;
}

static void fs_move(struct fs_state *st, int delta) {
    gui_list_move(&st->list, delta);
}

/* chdir + reload, shared by Backspace, the Go menu and Enter on a dir. */
static void fs_chdir_go(struct fs_state *st, const char *path) {
    if (chdir(path) != 0) {
        fs_dlg_message("Error", "Cannot open directory.");
        return;
    }
    fs_refresh(st);
}

/* Enter: a directory is entered, a file shows its size. */
static void fs_open_selected(struct fs_state *st) {
    int idx = fs_selected(st);
    char msg[FS_MSG_MAX];
    if (idx < 0) return;
    if (fs_is_dir(&st->entries[idx])) {
        fs_chdir_go(st, st->entries[idx].name);
    } else {
        fs_file_info_msg(msg, sizeof msg, st->entries[idx].name,
                         (uint64_t)st->entries[idx].size);
        fs_dlg_message("File", msg);
    }
}

/* 'd': confirm, then unlink and reload. */
static void fs_delete_selected(struct fs_state *st) {
    int idx = fs_selected(st);
    char msg[FS_MSG_MAX];
    int rc;
    if (idx < 0) return;
    fs_confirm_msg(msg, sizeof msg, st->entries[idx].name);
    if (!fs_dlg_confirm("Delete", msg)) return;
    rc = unlink(st->entries[idx].name);
    fs_refresh(st);
    if (rc != 0) fs_dlg_message("Error", "Cannot delete file.");
}

/* 'r': prompt for the new name, then rename and reload. */
static void fs_rename_selected(struct fs_state *st) {
    int idx = fs_selected(st);
    char name[FS_NAME_MAX];
    int rc;
    if (idx < 0) return;
    name[0] = '\0';
    if (!fs_dlg_prompt("Rename", "New name", name, sizeof name)) return;
    if (name[0] == '\0') return;                /* empty answer = cancel    */
    rc = rename(st->entries[idx].name, name);
    fs_refresh(st);
    if (rc != 0) fs_dlg_message("Error", "Cannot rename file.");
}

/* 'n': prompt for a folder name, then mkdir and reload. */
static void fs_new_folder(struct fs_state *st) {
    char name[FS_NAME_MAX];
    int rc;
    name[0] = '\0';
    if (!fs_dlg_prompt("New Folder", "Folder name", name, sizeof name)) return;
    if (name[0] == '\0') return;
    rc = mkdir(name);
    fs_refresh(st);
    if (rc != 0) fs_dlg_message("Error", "Cannot create folder.");
}

/* ====================================================================== */
/* Rendering                                                              */
/* ====================================================================== */

/* Write the whole screen into out (cap bytes).  Returns the length. */
static int fs_render(char *out, int cap, const struct fs_state *st) {
    int len = 0;
    char line[FS_ROW_W + 1];
    char free_text[FS_SIZE_MAX + 8];

    len = fs_put(out, cap, len, "=== Files ===\n");

    fs_free_text(free_text, sizeof free_text, st->free_ok, st->free_bytes);
    fs_path_line(line, sizeof line, FS_ROW_W, st->cwd, free_text);
    len = fs_put(out, cap, len, line);
    len = fs_putc(out, cap, len, '\n');

    gui_rule(line, FS_ROW_W);
    len = fs_put(out, cap, len, line);
    len = fs_putc(out, cap, len, '\n');

    if (st->count == 0) {
        len = fs_put(out, cap, len, "  (empty)\n");
    } else {
        for (int i = 0; i < FS_VISIBLE; i++) {
            int idx = st->list.top + i;
            if (idx < 0 || idx >= st->count) break;
            fs_row(line, sizeof line, FS_ROW_W, &st->entries[idx],
                   idx == st->list.selected);
            len = fs_put(out, cap, len, line);
            len = fs_putc(out, cap, len, '\n');
        }
    }

    len = fs_put(out, cap, len, FS_LEGEND);
    len = fs_putc(out, cap, len, '\n');
    return len;
}

/* gui_clear() + print the complete screen (the desktop captures print()). */
static void fs_redraw(void) {
    static char screen[FS_SCREEN_MAX];
    gui_clear();
    fs_render(screen, (int)sizeof screen, &FS);
    print(screen);
}

/* ====================================================================== */
/* Events                                                                 */
/* ====================================================================== */

static int fs_handle_menu(struct fs_state *st, int menu, int item) {
    if (menu == 0) {                            /* File */
        switch (item) {
        case 0: fs_open_selected(st);   return FS_ACT_REDRAW;   /* Open       */
        case 1: fs_delete_selected(st); return FS_ACT_REDRAW;   /* Delete     */
        case 2: fs_rename_selected(st); return FS_ACT_REDRAW;   /* Rename     */
        case 3: fs_new_folder(st);      return FS_ACT_REDRAW;   /* New Folder */
        case 4: fs_refresh(st);         return FS_ACT_REDRAW;   /* Refresh    */
        default: return FS_ACT_NONE;
        }
    }
    if (menu == 1) {                            /* Go */
        if (item == 0) { fs_chdir_go(st, "..");    return FS_ACT_REDRAW; } /* Up   */
        if (item == 1) { fs_chdir_go(st, "/");     return FS_ACT_REDRAW; } /* Root */
        if (item == 2) { fs_chdir_go(st, "/home"); return FS_ACT_REDRAW; } /* Home */
    }
    return FS_ACT_NONE;
}

/* Apply one event.  Returns FS_ACT_REDRAW and/or FS_ACT_QUIT. */
static int fs_handle_event(struct fs_state *st, const struct gui_event *ev) {
    switch (ev->type) {
    case GUI_EV_CHAR:
        switch (ev->ch) {
        case 'q':
            return FS_ACT_QUIT;
        case '\n':
        case '\r':
            fs_open_selected(st);
            return FS_ACT_REDRAW;
        case 'd':
            fs_delete_selected(st);
            return FS_ACT_REDRAW;
        case 'r':
            fs_rename_selected(st);
            return FS_ACT_REDRAW;
        case 'n':
            fs_new_folder(st);
            return FS_ACT_REDRAW;
        case '\b':
        case 127:                               /* DEL behaves like Bksp */
            fs_chdir_go(st, "..");
            return FS_ACT_REDRAW;
        default:
            return FS_ACT_NONE;
        }

    case GUI_EV_UP:
        fs_move(st, -1);
        return FS_ACT_REDRAW;
    case GUI_EV_DOWN:
        fs_move(st, 1);
        return FS_ACT_REDRAW;

    case GUI_EV_MENU:
        return fs_handle_menu(st, ev->menu, ev->item);

    case GUI_EV_MOUSE: {
        int idx;
        if (ev->button != 1) return FS_ACT_NONE;
        idx = gui_list_click_row(&st->list, ev->y, FS_ROW_ENTRY0);
        if (idx < 0 || idx == st->list.selected) return FS_ACT_NONE;
        st->list.selected = idx;
        gui_list_ensure_visible(&st->list);
        return FS_ACT_REDRAW;
    }

    default:
        return FS_ACT_NONE;
    }
}

/* ====================================================================== */
/* Entry point                                                            */
/* ====================================================================== */

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
    gui_set_title("Files");
    print_console("[APP] FILES started\n");
    gui_add_menu(0, "File", "Open,Delete,Rename,New Folder,Refresh");
    gui_add_menu(1, "Go", "Up,Root,Home");
    gui_enable_mouse();

    fs_init(&FS);
    fs_redraw();

    for (;;) {
        struct gui_event ev;
        if (!gui_read_event(&ev)) continue;
        int act = fs_handle_event(&FS, &ev);
        if (act & FS_ACT_QUIT) break;
        if (act & FS_ACT_REDRAW) fs_redraw();
    }
    exit(0);

#ifdef HOST_TEST
    return 0;   /* not reached: main is renamed and never called by tests */
#endif
}
