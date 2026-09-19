/*
 * files.c - Files (file manager) for HobbyOS.
 *
 * A windowed file manager over the FAT-16 volume and any mounted NFS
 * export, with real icons, mouse drag & drop, and type-aware opening:
 *
 *   - Every row starts with an 8x8 icon byte (src/user_include/graphics/
 *     icons.h): folder / text / program / image / archive / generic file,
 *     a network globe for mount points, an up-arrow for ".." and a drive
 *     glyph in the title.
 *   - File type handling on Enter: directories are entered, .TXT-style
 *     documents open in EDITOR.BIN (with the absolute path as an argument
 *     via the desktop's ESC ] R <bin>;<args> ~ protocol), .BIN programs run
 *     in a new window, anything else shows an info dialog.
 *   - Drag & drop: press a file row, drag over a folder row (or "..") and
 *     release to move the file there. The pressed row shows a '*' marker,
 *     the hovered drop target an '=' marker plus a "[drop]" suffix; the
 *     move is a real FAT-16 rename into the target directory.
 *   - Mounts view ('m' or the Mount menu): lists active mounts
 *     (point <- source), Enter jumps into one, 'u' unmounts the selected
 *     mount, and "Mount NFS..." prompts for server:/export and a mount
 *     point and calls the mount() syscall. Inside an NFS mount the status
 *     badge shows NFS, delete/rename/move are refused (read-only client)
 *     and the free-space figure comes from the server (FSSTAT).
 *
 * Listing: read_dir(cwd, index, &ent) - the FAT driver resolves the process
 * cwd's canonical path (and the VFS routes it to NFS when it sits inside a
 * mounted export), so indices 0..N-1 are the children of the current
 * directory until read_dir() returns < 0.  Directories carry
 * attribute bit 0x10 (same convention as ls.c).  "." and ".." entries are
 * skipped and a synthesized ".." row (up-arrow icon) is pinned at the top
 * of every directory except "/".
 *
 * Rendering is deterministic: the whole screen is redrawn from fs_state
 * on every event (gui_clear() + one print()).  fs_render() writes the
 * screen into a caller supplied buffer, so the host unit test can assert
 * on the exact layout without capturing stdout.
 *
 * Fixed layout (content rows, 0-based), classic list view:
 *   row 0       icon + " Files" ... right-aligned badge ("NFS" in a mount)
 *   row 1       "Path: <cwd>" ... right-aligned "Free: <gui_size_str>"
 *   row 2       gui_rule separator
 *   rows 3..20  up to 18 entry rows (scroll window driven by struct gui_list)
 *   row 21      key legend
 * Mounts view keeps the same frame; rows show "point <- source" instead.
 *
 * Entry row (70 columns):
 *   col 0       marker: '>' selected, '*' drag source, '=' drop target, ' '
 *   col 2       icon byte
 *   col 4..53   name (clipped, space padded)
 *   col 54..59  type tag ("[DIR]" "[TXT]" "[EXE]" "[IMG]" "[ARC]", blank
 *               for unknown files)
 *   col 60..69  size right-aligned in 10 columns (blank for directories)
 *
 * Host test seams: the three modal dialogs block on stdin, which a unit
 * test cannot satisfy, so the app reaches them through the fs_dlg_*
 * function pointers (initialized to the real dialog library).  mount() and
 * umount() go through fs_sys_* so tests can intercept them.  The host test
 * swaps in non-blocking mocks; the ARM build always calls the real ones.
 * Free space is probed once per refresh (sysinfo cmd 7) and the mount
 * table via sysinfo cmd 8; the cached results are what fs_render() shows.
 */

#include "libc.h"
#include "gui.h"
#include "dialog.h"
#include "icons.h"

/* ---- Layout and limits ---- */

#define FS_ATTR_DIR      0x10   /* sys_dirent.attr bit: entry is a directory  */
#define FS_MAX_ENTRIES   64     /* hard cap on entries read per directory     */
#define FS_VISIBLE       18     /* entry rows on screen                       */
#define FS_ROW_W         70     /* width of every rendered line, in columns   */
#define FS_SIZE_COL      10     /* size right-aligned in the last 10 columns  */
#define FS_TAG_COL       6      /* type tag column ("[DIR]" etc)              */
#define FS_NAME_MAX      32     /* matches sys_dirent.name                    */
#define FS_PATH_MAX      128    /* cwd buffer                                 */
#define FS_SIZE_MAX      24     /* gui_size_str output buffer                 */
#define FS_MSG_MAX       96     /* dialog message buffer                      */
#define FS_SCREEN_MAX    1800   /* full screen buffer (window holds 2048)     */
#define FS_MAX_MOUNTS    8      /* mount table snapshot cap                   */
#define FS_MNT_TEXT      68     /* mount point / source string buffer         */
#define FS_REQ_MAX       120    /* ESC ] R launch request buffer              */

/* Entry flags */
#define FS_F_DOTDOT      1      /* synthesized ".." row                       */
#define FS_F_MOUNT       2      /* row is a mount point (list or mounts view) */

/* Views */
#define FS_VIEW_LIST     0
#define FS_VIEW_MOUNTS   1

/* Content rows (0-based).  The legend always lands on FS_ROW_LEGEND, and
 * the entry rows start at FS_ROW_ENTRY0 for the mouse row mapping. */
#define FS_ROW_TITLE     0
#define FS_ROW_PATH      1
#define FS_ROW_RULE      2
#define FS_ROW_ENTRY0    3
#define FS_ROW_LEGEND    (FS_ROW_ENTRY0 + FS_VISIBLE)

#define FS_LEGEND_LIST   "Enter=open d=del r=rename n=new m=mounts Bksp=up q=quit"
#define FS_LEGEND_MOUNTS "Enter=open u=unmount m=back q=quit"

/* fs_handle_event() results (bit flags) */
#define FS_ACT_NONE      0
#define FS_ACT_REDRAW    1
#define FS_ACT_QUIT      2

/* What Enter does with an entry (see fs_open_kind). */
#define FS_OPEN_DIR      0      /* chdir into it                              */
#define FS_OPEN_EDIT     1      /* open in EDITOR.BIN                         */
#define FS_OPEN_RUN      2      /* run the .BIN in a new window               */
#define FS_OPEN_INFO     3      /* no handler - show an info dialog           */

/* ---- State ---- */

struct fs_entry {
    char     name[FS_NAME_MAX];
    char     info[FS_MNT_TEXT]; /* mount source (mounts view)              */
    uint8_t  attr;
    uint32_t size;
    int      flags;
};

struct fs_mount {
    char point[FS_MNT_TEXT];
    char source[FS_MNT_TEXT];
    int  type;                  /* 0 = FAT16 (local), 1 = NFS              */
};

struct fs_state {
    struct fs_entry entries[FS_MAX_ENTRIES];
    int             count;       /* number of valid entries                  */
    struct gui_list list;        /* selection + scroll window                */
    char            cwd[FS_PATH_MAX];
    uint64_t        free_bytes;  /* cached at refresh time                   */
    int             free_ok;     /* 0 -> "Free: ?"                           */
    int             view;        /* FS_VIEW_LIST / FS_VIEW_MOUNTS            */
    struct fs_mount mounts[FS_MAX_MOUNTS];
    int             mount_count;
    /* Drag session */
    int             drag_from;   /* entry index the press started on (-1)    */
    int             drag_hover;  /* current drop target (-1 = none)          */
};

static struct fs_state FS;

/* ---- Host-test seams (see file header) ----
 * The ARM build runs with these pointing at the real dialog library and
 * the real mount syscalls. */
static int  (*fs_dlg_confirm)(const char *title, const char *msg) = dialog_confirm;
static int  (*fs_dlg_prompt)(const char *title, const char *msg, char *buf, int max) = dialog_prompt;
static void (*fs_dlg_message)(const char *title, const char *msg) = dialog_message;
static int  (*fs_sys_mount)(const char *source, const char *target) = mount;
static int  (*fs_sys_umount)(const char *target) = umount;

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

static int fs_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/* Case-insensitive equality (FAT-16 names are upper case on disk). */
static int fs_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (fs_lower((unsigned char)*a) != fs_lower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Does `name` end with ".<ext>"?  ext may be any length ("BIN", "TXT",
 * "MD", "GZ"); the match is case-insensitive and must sit at the very end
 * of the name (unless it IS the name). */
static int fs_ext_eq(const char *name, const char *ext) {
    int nlen = fs_slen(name);
    int elen = fs_slen(ext);
    if (elen == 0 || nlen < elen + 2) return 0;   /* name + '.' + ext */
    if (name[nlen - elen - 1] != '.') return 0;
    for (int i = 0; i < elen; i++) {
        if (fs_lower((unsigned char)name[nlen - elen + i]) !=
            fs_lower((unsigned char)ext[i])) return 0;
    }
    return 1;
}

/* ".."? (exact)  "." and ".." entries read back from disk are skipped. */
static int fs_name_is_dot(const char *name) {
    if (name[0] != '.') return 0;
    if (name[1] == '\0') return 1;
    if (name[1] == '.' && name[2] == '\0') return 1;
    return 0;
}

/* Join cwd and name into an absolute path ("/" cwd handled without "//"). */
static void fs_join_abs(char *out, int cap, const char *cwd, const char *name) {
    if (cwd[0] == '/' && cwd[1] == '\0') {
        out[0] = '/';
        out[1] = '\0';
        fs_sappend(out, cap, name);
    } else {
        fs_scopy(out, cap, cwd);
        fs_sappend(out, cap, "/");
        fs_sappend(out, cap, name);
    }
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
/* Classification helpers (pure: no state)                                */
/* ====================================================================== */

static int fs_is_dir(const struct fs_entry *e) {
    return (e->attr & FS_ATTR_DIR) != 0;
}

/* Icon byte for an entry (see icons.h). */
static int fs_entry_icon(const struct fs_entry *e) {
    if (e->flags & FS_F_DOTDOT) return ICON_UP;
    if (e->flags & FS_F_MOUNT)  return ICON_NET;
    if (fs_is_dir(e))           return ICON_FOLDER;
    if (fs_ext_eq(e->name, "TXT") || fs_ext_eq(e->name, "LOG") ||
        fs_ext_eq(e->name, "MD")  || fs_ext_eq(e->name, "INI") ||
        fs_ext_eq(e->name, "CFG")) return ICON_TEXT;
    if (fs_ext_eq(e->name, "BIN")) return ICON_PROG;
    if (fs_ext_eq(e->name, "IMG") || fs_ext_eq(e->name, "PPM") ||
        fs_ext_eq(e->name, "PNG") || fs_ext_eq(e->name, "BMP")) return ICON_IMAGE;
    if (fs_ext_eq(e->name, "ZIP") || fs_ext_eq(e->name, "ARC") ||
        fs_ext_eq(e->name, "TAR") || fs_ext_eq(e->name, "GZ")) return ICON_ARCH;
    return ICON_FILE;
}

/* Type tag: "[DIR]" for directories, "[TXT]"/"[EXE]"/"[IMG]"/"[ARC]" for
 * the known file families, empty for unknown files and "..". */
static void fs_entry_tag(char *out, int cap, const struct fs_entry *e) {
    const char *t = "";
    if (e->flags & FS_F_DOTDOT) {
        t = "";
    } else if (fs_is_dir(e)) {
        t = "[DIR]";
    } else if (fs_ext_eq(e->name, "TXT") || fs_ext_eq(e->name, "LOG") ||
               fs_ext_eq(e->name, "MD")  || fs_ext_eq(e->name, "INI") ||
               fs_ext_eq(e->name, "CFG")) {
        t = "[TXT]";
    } else if (fs_ext_eq(e->name, "BIN")) {
        t = "[EXE]";
    } else if (fs_ext_eq(e->name, "IMG") || fs_ext_eq(e->name, "PPM") ||
               fs_ext_eq(e->name, "PNG") || fs_ext_eq(e->name, "BMP")) {
        t = "[IMG]";
    } else if (fs_ext_eq(e->name, "ZIP") || fs_ext_eq(e->name, "ARC") ||
               fs_ext_eq(e->name, "TAR") || fs_ext_eq(e->name, "GZ")) {
        t = "[ARC]";
    }
    fs_scopy(out, cap, t);
}

/* What does Enter do with this entry? */
static int fs_open_kind(const struct fs_entry *e) {
    if (e->flags & FS_F_DOTDOT) return FS_OPEN_DIR;
    if (fs_is_dir(e)) return FS_OPEN_DIR;
    if (fs_ext_eq(e->name, "TXT") || fs_ext_eq(e->name, "LOG") ||
        fs_ext_eq(e->name, "MD")  || fs_ext_eq(e->name, "INI") ||
        fs_ext_eq(e->name, "CFG")) return FS_OPEN_EDIT;
    if (fs_ext_eq(e->name, "BIN")) return FS_OPEN_RUN;
    return FS_OPEN_INFO;
}

/* Can a drag be dropped on this entry?  (".." and directories only.) */
static int fs_entry_droppable(const struct fs_entry *e) {
    if (e->flags & FS_F_MOUNT) return 0;         /* mount internals: none */
    if (e->flags & FS_F_DOTDOT) return 1;
    return fs_is_dir(e);
}

/* Build the desktop run request for launching `bin` (with optional args):
 * ESC ] R <bin>[;<args>] ~ .  The desktop spawns the program into a new
 * window; the editor uses args as the file to open.  Writes a
 * NUL-terminated string into out and returns its length. */
static int fs_build_run_req(char *out, int cap, const char *bin, const char *args) {
    int len = 0;
    len = fs_put(out, cap, len, "\033]R");
    len = fs_put(out, cap, len, bin);
    if (args && args[0]) {
        len = fs_putc(out, cap, len, ';');
        len = fs_put(out, cap, len, args);
    }
    len = fs_putc(out, cap, len, '~');
    return len;
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

/* One list-view entry row (see the file header for the column map). */
static int fs_row(char *out, int cap, int width, const struct fs_entry *e, char marker) {
    int len = 0;
    int is_dir = fs_is_dir(e) || (e->flags & FS_F_DOTDOT);
    char tag[8];
    char cell[FS_SIZE_COL + 1];

    len = fs_putc(out, cap, len, marker);
    len = fs_putc(out, cap, len, ' ');
    len = fs_putc(out, cap, len, (char)fs_entry_icon(e));
    len = fs_putc(out, cap, len, ' ');

    int name_max = width - 4 - FS_TAG_COL - FS_SIZE_COL;
    if (name_max < 0) name_max = 0;
    len = fs_putn(out, cap, len, e->name, name_max);
    len = fs_putpad(out, cap, len, width - FS_TAG_COL - FS_SIZE_COL);

    tag[0] = '\0';
    fs_entry_tag(tag, sizeof tag, e);
    len = fs_putn(out, cap, len, tag, FS_TAG_COL);
    len = fs_putpad(out, cap, len, width - FS_SIZE_COL);

    cell[0] = '\0';
    fs_size_cell(cell, sizeof cell, FS_SIZE_COL, is_dir, e->size);
    len = fs_put(out, cap, len, cell);
    return len;
}

/* One mounts-view row:  marker icon ' ' point " <- " source ... */
static int fs_mount_row(char *out, int cap, int width, const struct fs_entry *e, char marker) {
    int len = 0;
    int point_cols = 20;

    len = fs_putc(out, cap, len, marker);
    len = fs_putc(out, cap, len, ' ');
    len = fs_putc(out, cap, len, (char)fs_entry_icon(e));
    len = fs_putc(out, cap, len, ' ');

    len = fs_putn(out, cap, len, e->name, point_cols);
    len = fs_putpad(out, cap, len, 4 + point_cols);
    len = fs_put(out, cap, len, " <- ");
    len = fs_putn(out, cap, len, e->info, width - (4 + point_cols + 4));
    len = fs_putpad(out, cap, len, width);
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

/* "Path: <cwd>" with `right` text ("Free: ...") right-aligned on the same
 * line.  A cwd that does not fit is clipped to its tail and prefixed with
 * "..." (the tail is the informative part of a path). */
static int fs_path_line(char *out, int cap, int width, const char *cwd,
                        const char *right) {
    int len = 0;
    int rlen = fs_slen(right);
    int clen = fs_slen(cwd);
    int avail = width - rlen;

    if (avail < 0) avail = 0;
    len = fs_put(out, cap, len, "Path: ");
    avail -= 6;                                  /* 6 = "Path: " */
    if (avail < 0) avail = 0;
    if (clen <= avail) {
        len = fs_putn(out, cap, len, cwd, clen);
    } else if (avail >= 4) {
        len = fs_put(out, cap, len, "...");
        len = fs_putn(out, cap, len, cwd + (clen - (avail - 3)), avail - 3);
    } else {
        len = fs_putn(out, cap, len, cwd + (clen - avail), avail);
    }
    len = fs_putpad(out, cap, len, width - rlen);
    len = fs_put(out, cap, len, right);
    return len;
}

/* Message for Enter on a file with no handler: "NAME  <size> (<bytes> bytes)". */
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
    fs_sappend(out, cap, " bytes)\nNo viewer for this file type.");
}

/* Delete confirmation question: "Delete NAME?" */
static void fs_confirm_msg(char *out, int cap, const char *name) {
    fs_scopy(out, cap, "Delete ");
    fs_sappend(out, cap, name);
    fs_sappend(out, cap, "?");
}

/* ====================================================================== */
/* Mount table helpers                                                    */
/* ====================================================================== */

/* Is `path` the mount point itself or inside it? (case-insensitive) */
static int fs_path_in_mount(const char *path, const char *point) {
    int i = 0;
    while (point[i]) {
        if (fs_lower((unsigned char)path[i]) != fs_lower((unsigned char)point[i])) return 0;
        i++;
    }
    if (path[i] == '\0') return 1;               /* exact match */
    return path[i] == '/';
}

/* Index of the mount whose point contains cwd, or -1. */
static int fs_cwd_mount(const struct fs_state *st) {
    for (int i = 0; i < st->mount_count; i++) {
        if (fs_path_in_mount(st->cwd, st->mounts[i].point)) return i;
    }
    return -1;
}

/* Snapshot the kernel mount table (sysinfo cmd 8). */
static void fs_load_mounts(struct fs_state *st) {
    struct sys_mountinfo m[FS_MAX_MOUNTS];
    int n = sysinfo(8, m, sizeof m);
    st->mount_count = 0;
    if (n <= 0) return;
    if (n > FS_MAX_MOUNTS) n = FS_MAX_MOUNTS;
    for (int i = 0; i < n; i++) {
        fs_scopy(st->mounts[i].point, FS_MNT_TEXT, m[i].point);
        fs_scopy(st->mounts[i].source, FS_MNT_TEXT, m[i].source);
        st->mounts[i].type = m[i].type;
    }
    st->mount_count = n;
}

/* ====================================================================== */
/* Directory loading / navigation                                         */
/* ====================================================================== */

/* Read the current directory (read_dir on the canonical cwd path) into st,
 * honouring the FS_MAX_ENTRIES cap, skipping "."/"..", synthesizing a ".."
 * row at the top when cwd != "/", then clamp the selection and scroll
 * window. */
static int fs_load(struct fs_state *st) {
    struct sys_dirent ent;
    int n = 0;
    int base = (st->cwd[0] == '/' && st->cwd[1] == '\0') ? 0 : 1;

    for (int i = 0; i < FS_MAX_ENTRIES; i++) {
        if (read_dir(st->cwd, i, &ent) < 0) break;
        if (fs_name_is_dot(ent.name)) continue;
        if (n + base >= FS_MAX_ENTRIES) break;
        struct fs_entry *e = &st->entries[n + base];
        fs_scopy(e->name, FS_NAME_MAX, ent.name);
        e->info[0] = '\0';
        e->attr = ent.attr;
        e->size = ent.size;
        e->flags = 0;
        n++;
    }
    if (base) {
        struct fs_entry *e = &st->entries[0];
        fs_scopy(e->name, FS_NAME_MAX, "..");
        e->info[0] = '\0';
        e->attr = FS_ATTR_DIR;
        e->size = 0;
        e->flags = FS_F_DOTDOT;
    }
    st->count = n + base;

    /* Mark entries that are mount points so they get the network icon. */
    for (int i = 0; i < st->count; i++) {
        struct fs_entry *e = &st->entries[i];
        char abs[FS_PATH_MAX + FS_NAME_MAX];
        if (e->flags & FS_F_DOTDOT) continue;
        fs_join_abs(abs, sizeof abs, st->cwd, e->name);
        for (int m = 0; m < st->mount_count; m++) {
            if (fs_eq_ci(abs, st->mounts[m].point)) {
                /* leave FS_F_DOTDOT alone; add the mount flag */
                e->flags |= FS_F_MOUNT;
                break;
            }
        }
    }

    if (st->list.selected > st->count - 1) st->list.selected = st->count - 1;
    if (st->list.selected < 0) st->list.selected = 0;
    st->list.count = st->count;
    st->list.visible = FS_VISIBLE;
    gui_list_ensure_visible(&st->list);
    return st->count;
}

/* Fill the entry rows from the mount table (mounts view). */
static void fs_load_mounts_view(struct fs_state *st) {
    st->count = st->mount_count;
    for (int i = 0; i < st->mount_count; i++) {
        struct fs_entry *e = &st->entries[i];
        fs_scopy(e->name, FS_NAME_MAX, st->mounts[i].point);
        fs_scopy(e->info, FS_MNT_TEXT, st->mounts[i].source);
        e->attr = FS_ATTR_DIR;
        e->size = 0;
        e->flags = FS_F_MOUNT;
    }
    if (st->list.selected > st->count - 1) st->list.selected = st->count - 1;
    if (st->list.selected < 0) st->list.selected = 0;
    st->list.count = st->count;
    st->list.visible = FS_VISIBLE;
    gui_list_ensure_visible(&st->list);
}

/* Refresh cwd, the mount table, the cached free-space figure and the
 * listing for the active view. */
static void fs_refresh(struct fs_state *st) {
    char buf[FS_PATH_MAX];
    struct sys_fsinfo fsinfo;

    if (getcwd(buf, sizeof buf) == 0) buf[0] = '\0';
    fs_scopy(st->cwd, FS_PATH_MAX, buf);

    fs_load_mounts(st);

    if (sysinfo(7, &fsinfo, sizeof fsinfo) == 0) {
        st->free_bytes = fsinfo.free_bytes;
        st->free_ok = 1;
    } else {
        st->free_bytes = 0;
        st->free_ok = 0;
    }

    if (st->view == FS_VIEW_MOUNTS) fs_load_mounts_view(st);
    else fs_load(st);

    st->drag_from = -1;
    st->drag_hover = -1;
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
    st->view = FS_VIEW_LIST;
    st->mount_count = 0;
    st->drag_from = -1;
    st->drag_hover = -1;
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
    st->view = FS_VIEW_LIST;
    fs_refresh(st);
}

/* Select the first real entry (skip the synthesized ".." row). */
static void fs_select_first_real(struct fs_state *st) {
    st->list.selected = 0;
    if (st->count > 0 && (st->entries[0].flags & FS_F_DOTDOT)) st->list.selected = 1;
    if (st->list.selected > st->count - 1) st->list.selected = 0;
    gui_list_ensure_visible(&st->list);
}

/* Enter on a mount row (mounts view): jump into the mount point. */
static void fs_open_mount(struct fs_state *st, int idx) {
    if (idx < 0 || idx >= st->count) return;
    fs_chdir_go(st, st->entries[idx].name);
    fs_select_first_real(st);
}

/* Enter: a directory is entered, text opens in the editor, a .BIN runs,
 * everything else shows an info dialog. */
static void fs_open_selected(struct fs_state *st) {
    int idx = fs_selected(st);
    char msg[FS_MSG_MAX];
    if (idx < 0) return;

    if (st->view == FS_VIEW_MOUNTS) { fs_open_mount(st, idx); return; }

    struct fs_entry *e = &st->entries[idx];
    int kind = fs_open_kind(e);

    if (kind == FS_OPEN_DIR) {
        fs_chdir_go(st, (e->flags & FS_F_DOTDOT) ? ".." : e->name);
        fs_select_first_real(st);
    } else if (kind == FS_OPEN_EDIT) {
        char abs[FS_PATH_MAX + FS_NAME_MAX];
        char req[FS_REQ_MAX];
        fs_join_abs(abs, sizeof abs, st->cwd, e->name);
        fs_build_run_req(req, sizeof req, "EDITOR.BIN", abs);
        print(req);
    } else if (kind == FS_OPEN_RUN) {
        if (fs_cwd_mount(st) >= 0) {
            fs_dlg_message("Run", "Cannot run programs from an NFS mount.");
        } else {
            char abs[FS_PATH_MAX + FS_NAME_MAX];
            char req[FS_REQ_MAX];
            fs_join_abs(abs, sizeof abs, st->cwd, e->name);
            fs_build_run_req(req, sizeof req, abs, "");
            print(req);
        }
    } else {
        fs_file_info_msg(msg, sizeof msg, e->name, (uint64_t)e->size);
        fs_dlg_message("File", msg);
    }
}

/* 'd': confirm, then unlink and reload. */
static void fs_delete_selected(struct fs_state *st) {
    int idx = fs_selected(st);
    char msg[FS_MSG_MAX];
    int rc;
    if (idx < 0) return;
    if (st->view == FS_VIEW_MOUNTS) return;
    if (st->entries[idx].flags & FS_F_DOTDOT) return;
    if (fs_cwd_mount(st) >= 0) {
        fs_dlg_message("Delete", "NFS mounts are read-only.");
        return;
    }
    if (fs_is_dir(&st->entries[idx])) {
        fs_dlg_message("Delete", "Cannot delete folders.");
        return;
    }
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
    if (st->view == FS_VIEW_MOUNTS) return;
    if (st->entries[idx].flags & FS_F_DOTDOT) return;
    if (fs_cwd_mount(st) >= 0) {
        fs_dlg_message("Rename", "NFS mounts are read-only.");
        return;
    }
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
    if (fs_cwd_mount(st) >= 0) {
        fs_dlg_message("New Folder", "NFS mounts are read-only.");
        return;
    }
    name[0] = '\0';
    if (!fs_dlg_prompt("New Folder", "Folder name", name, sizeof name)) return;
    if (name[0] == '\0') return;
    rc = mkdir(name);
    fs_refresh(st);
    if (rc != 0) fs_dlg_message("Error", "Cannot create folder.");
}

/* Drag & drop release: move entry `from` into the directory `to` (or the
 * parent for the ".." row) with a real rename. */
static void fs_move_entry(struct fs_state *st, int from, int to) {
    char newname[FS_PATH_MAX + FS_NAME_MAX];
    int rc;

    if (from < 0 || from >= st->count || to < 0 || to >= st->count) return;
    if (from == to) return;
    struct fs_entry *s = &st->entries[from];
    struct fs_entry *d = &st->entries[to];

    if (s->flags & FS_F_DOTDOT) return;
    if (fs_is_dir(s)) {
        fs_dlg_message("Move", "Cannot move folders.");
        return;
    }
    if (fs_cwd_mount(st) >= 0) {
        fs_dlg_message("Move", "NFS mounts are read-only.");
        return;
    }
    if (d->flags & FS_F_MOUNT) {
        fs_dlg_message("Move", "Cannot move into a mounted filesystem.");
        return;
    }

    if (d->flags & FS_F_DOTDOT) {
        fs_scopy(newname, sizeof newname, "../");
        fs_sappend(newname, sizeof newname, s->name);
    } else if (fs_is_dir(d)) {
        fs_scopy(newname, sizeof newname, d->name);
        fs_sappend(newname, sizeof newname, "/");
        fs_sappend(newname, sizeof newname, s->name);
    } else {
        return;                                  /* dropped onto a file */
    }

    rc = rename(s->name, newname);
    fs_refresh(st);
    if (rc != 0) fs_dlg_message("Move", "Cannot move file.");
}

/* ---- Mounts view actions ---- */

/* "Mount NFS...": prompt for server:/export and a mount point, then call
 * the mount() syscall. */
static void fs_mount_prompt(struct fs_state *st) {
    char src[FS_MNT_TEXT];
    char tgt[64];
    int rc;

    src[0] = '\0';
    if (!fs_dlg_prompt("Mount NFS", "Server:/export", src, sizeof src)) return;
    if (src[0] == '\0') return;

    fs_scopy(tgt, sizeof tgt, "/nfs");
    if (!fs_dlg_prompt("Mount at", "Mount point", tgt, sizeof tgt)) return;
    if (tgt[0] == '\0') return;

    rc = fs_sys_mount(src, tgt);
    fs_refresh(st);
    if (rc != 0) fs_dlg_message("Mount", "Mount failed.");
    else fs_dlg_message("Mount", "Mounted. Use Mounts to open it.");
}

/* 'u' / Mount menu: unmount the selected mount (or the mount containing
 * the cwd when in the list view). */
static void fs_unmount_selected(struct fs_state *st) {
    char point[FS_MNT_TEXT];
    char msg[FS_MSG_MAX];
    int rc;

    point[0] = '\0';
    if (st->view == FS_VIEW_MOUNTS) {
        int idx = fs_selected(st);
        if (idx < 0) return;
        fs_scopy(point, sizeof point, st->entries[idx].name);
    } else {
        int m = fs_cwd_mount(st);
        if (m < 0) {
            fs_dlg_message("Unmount", "No mount selected. Use the Mounts view.");
            return;
        }
        fs_scopy(point, sizeof point, st->mounts[m].point);
    }

    fs_scopy(msg, sizeof msg, "Unmount ");
    fs_sappend(msg, sizeof msg, point);
    fs_sappend(msg, sizeof msg, "?");
    if (!fs_dlg_confirm("Unmount", msg)) return;

    rc = fs_sys_umount(point);
    if (rc == 0 && fs_path_in_mount(st->cwd, point)) {
        chdir("/");                              /* leave the removed tree */
    }
    fs_refresh(st);
    if (rc != 0) fs_dlg_message("Unmount", "Cannot unmount.");
}

static void fs_show_mounts(struct fs_state *st) {
    st->view = FS_VIEW_MOUNTS;
    st->list.selected = 0;
    st->list.top = 0;
    fs_refresh(st);
}

static void fs_show_list(struct fs_state *st) {
    st->view = FS_VIEW_LIST;
    st->list.selected = 0;
    st->list.top = 0;
    fs_refresh(st);
}

/* ====================================================================== */
/* Rendering                                                              */
/* ====================================================================== */

/* Write the whole screen into out (cap bytes).  Returns the length. */
static int fs_render(char *out, int cap, const struct fs_state *st) {
    int len = 0;
    /* Row buffer must fit a full row plus the drag suffixes (" [moving]"). */
    char line[FS_ROW_W + 16];
    char free_text[FS_SIZE_MAX + 8];

    /* Title row: icon + " Files" + right-aligned badge. */
    len = fs_putc(out, cap, len, (char)ICON_FOLDER);
    len = fs_put(out, cap, len, " Files");
    if (st->view == FS_VIEW_MOUNTS) {
        len = fs_putpad(out, cap, len, FS_ROW_W - 6);
        len = fs_put(out, cap, len, "MOUNTS");
    } else if (fs_cwd_mount(st) >= 0) {
        len = fs_putpad(out, cap, len, FS_ROW_W - 3);
        len = fs_put(out, cap, len, "NFS");
    } else {
        len = fs_putpad(out, cap, len, FS_ROW_W - 3);
        len = fs_put(out, cap, len, "FAT");
    }
    len = fs_putc(out, cap, len, '\n');

    /* Path / mounts summary row with the right-aligned info column. */
    if (st->view == FS_VIEW_MOUNTS) {
        char cnt[24];
        cnt[0] = '\0';
        fs_scopy(cnt, sizeof cnt, "Active mounts: ");
        char num[16];
        gui_uitoa((unsigned long)st->mount_count, num);
        fs_sappend(cnt, sizeof cnt, num);
        len = fs_put(out, cap, len, cnt);
        len = fs_putpad(out, cap, len, FS_ROW_W);
    } else {
        fs_free_text(free_text, sizeof free_text, st->free_ok, st->free_bytes);
        fs_path_line(line, sizeof line, FS_ROW_W, st->cwd, free_text);
        len = fs_put(out, cap, len, line);
    }
    len = fs_putc(out, cap, len, '\n');

    gui_rule(line, FS_ROW_W);
    len = fs_put(out, cap, len, line);
    len = fs_putc(out, cap, len, '\n');

    /* "(empty)" shows when there is nothing to list besides the ".." row. */
    if (st->count == 0 ||
        (st->view == FS_VIEW_LIST && st->count == 1 &&
         (st->entries[0].flags & FS_F_DOTDOT))) {
        len = fs_put(out, cap, len,
                     (st->view == FS_VIEW_MOUNTS) ? "  (no mounts)\n"
                                                  : "  (empty)\n");
    } else {
        for (int i = 0; i < FS_VISIBLE; i++) {
            int idx = st->list.top + i;
            char marker;
            if (idx < 0 || idx >= st->count) break;
            marker = ' ';
            if (idx == st->list.selected) marker = '>';
            if (st->drag_from >= 0 && idx == st->drag_from) marker = '*';
            if (st->drag_hover >= 0 && idx == st->drag_hover) marker = '=';
            if (st->view == FS_VIEW_MOUNTS) {
                fs_mount_row(line, sizeof line, FS_ROW_W, &st->entries[idx], marker);
            } else {
                fs_row(line, sizeof line, FS_ROW_W, &st->entries[idx], marker);
            }
            /* Drop-target / drag-source suffixes. */
            if (st->drag_hover >= 0 && idx == st->drag_hover) {
                fs_sappend(line, sizeof line, " [drop]");
            } else if (st->view == FS_VIEW_LIST && st->drag_from >= 0 &&
                       idx == st->drag_from) {
                fs_sappend(line, sizeof line, " [moving]");
            }
            len = fs_put(out, cap, len, line);
            len = fs_putc(out, cap, len, '\n');
        }
    }

    len = fs_put(out, cap, len,
                 (st->view == FS_VIEW_MOUNTS) ? FS_LEGEND_MOUNTS : FS_LEGEND_LIST);
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

/* Map a content row to an entry index (mouse). */
static int fs_entry_at_row(struct fs_state *st, int row) {
    return gui_list_click_row(&st->list, row, FS_ROW_ENTRY0);
}

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
        if (item == 3) { fs_show_mounts(st);       return FS_ACT_REDRAW; } /* Mounts */
    }
    if (menu == 2) {                            /* Mount */
        if (item == 0) { fs_mount_prompt(st);     return FS_ACT_REDRAW; } /* Mount NFS  */
        if (item == 1) { fs_unmount_selected(st); return FS_ACT_REDRAW; } /* Unmount    */
        if (item == 2) { fs_show_mounts(st);      return FS_ACT_REDRAW; } /* Mounts     */
    }
    return FS_ACT_NONE;
}

/* Mouse handling: press selects (and arms a drag in the list view), drag
 * tracks the drop target, release performs the move. */
static int fs_handle_mouse(struct fs_state *st, const struct gui_event *ev) {
    int idx;
    if (ev->button != 1) return FS_ACT_NONE;

    if (ev->state == GUI_MOUSE_PRESS) {
        int was = st->list.selected;
        int had_drag = (st->drag_from >= 0 || st->drag_hover >= 0);
        st->drag_from = -1;                      /* any press resets the drag */
        st->drag_hover = -1;
        idx = fs_entry_at_row(st, ev->y);
        if (idx < 0) return had_drag ? FS_ACT_REDRAW : FS_ACT_NONE;
        st->list.selected = idx;
        gui_list_ensure_visible(&st->list);
        if (st->view == FS_VIEW_LIST && !(st->entries[idx].flags & FS_F_DOTDOT)) {
            st->drag_from = idx;                 /* arm a drag session */
        }
        return (idx != was || st->drag_from >= 0 || had_drag) ? FS_ACT_REDRAW
                                                              : FS_ACT_NONE;
    }

    if (ev->state == GUI_MOUSE_DRAG) {
        int hover = -1;
        if (st->drag_from >= 0) {
            idx = fs_entry_at_row(st, ev->y);
            if (idx >= 0 && idx != st->drag_from && fs_entry_droppable(&st->entries[idx]))
                hover = idx;
        }
        if (hover != st->drag_hover) {
            st->drag_hover = hover;
            return FS_ACT_REDRAW;
        }
        return FS_ACT_NONE;
    }

    if (ev->state == GUI_MOUSE_RELEASE) {
        int from = st->drag_from;
        int hover = st->drag_hover;
        st->drag_from = -1;
        st->drag_hover = -1;
        if (from >= 0) {
            idx = fs_entry_at_row(st, ev->y);
            if (idx < 0) idx = hover;            /* released off-grid: use hover */
            if (idx >= 0) fs_move_entry(st, from, idx);
        }
        return FS_ACT_REDRAW;
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
        case 'm':
            if (st->view == FS_VIEW_MOUNTS) fs_show_list(st);
            else fs_show_mounts(st);
            return FS_ACT_REDRAW;
        case 'u':
            fs_unmount_selected(st);
            return FS_ACT_REDRAW;
        case '\b':
        case 127:                               /* DEL behaves like Bksp */
            if (st->view == FS_VIEW_MOUNTS) fs_show_list(st);
            else fs_chdir_go(st, "..");
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

    case GUI_EV_MOUSE:
        return fs_handle_mouse(st, ev);

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
    gui_add_menu(1, "Go", "Up,Root,Home,Mounts");
    gui_add_menu(2, "Mount", "Mount NFS,Unmount,Mounts");
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
