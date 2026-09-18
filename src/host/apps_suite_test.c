/*
 * apps_suite_test.c - Cross-application host suite for the ten new HobbyOS
 * desktop apps: files, calc, clock, sysmon, hex, tasks, find, diff, notes
 * and unit.
 *
 * This is not a replacement for each app's own focused host test; it is the
 * one place where the ten apps are exercised TOGETHER, the way the desktop
 * runs them, so that per-app interactive behaviour and the invariants every
 * windowed app must satisfy are checked in one run.
 *
 * Two layers, both compiled into this translation unit:
 *
 *   A. In-process.  Every app source is #included here (its main() renamed),
 *      so the app's statics are reachable: state, host seams, render helpers
 *      and event handlers.  Dialogs are stubbed through the seams the apps
 *      already expose (files fs_dlg_*, tasks tasks_prompt_fn/tasks_confirm_fn,
 *      sysmon g_confirm/g_kill); apps without a seam (hex 'o'/'g', diff 'o',
 *      unit 'e', notes n/e/d) are only driven here through paths that cannot
 *      block on stdin - their dialog paths are covered end-to-end in layer B.
 *
 *   B. End-to-end.  Each app's real main() runs in a forked child whose
 *      stdin/stdout are pipes.  The harness writes a byte script and captures
 *      every byte the app printed, which pins the wire protocol: window title
 *      escape, startup marker, one complete frame per handled event, NO frame
 *      for ESC (a lone ESC is not a command for any of the ten apps) and
 *      byte-for-byte run-to-run determinism.
 *
 * Per app the suite checks:
 *   - 14 cross-cutting invariants: first render non-empty, banner line,
 *     legend line, two renders byte-identical, screen <= 2048 bytes (the
 *     desktop window text buffer), no line wider than 110 columns, and
 *     ESC / unknown key / unknown menu index / right click change neither
 *     the handler result nor the rendered screen.
 *   - 14 wire-protocol checks from the forked runs: title escape, startup
 *     marker, banner on the wire, frame count and frame geometry of the
 *     ESC-only run, "app still alive", determinism of two identical runs,
 *     frame count of the ignored-input run, the two scripted-interaction
 *     expectations, scripted frame count and exit behaviour.
 *   - a handful of app-specific interaction checks (the point of the app).
 *
 * Build (no make, no QEMU - the tree is owned by another process):
 *   clang -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics \
 *         -Isrc/include -DHOST_TEST src/host/apps_suite_test.c \
 *         src/host/compat.c src/user/gui.c src/user/dialog.c \
 *         src/user/filedialog.c -o /tmp/apps_suite
 * Run it from a scratch directory: tasks and notes read/write TODO.TXT and
 * NOTES.TXT in the cwd.
 *
 * Host mocks used from compat.c: the mock framebuffer, the 7-entry read_dir
 * listing (overridable), sysinfo cmd 1..7 (each overridable), and the inert
 * mkdir/unlink/rename wrappers plus their call recorders.
 */

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

/* ====================================================================== */
/* The ten application sources (entry points renamed)                     */
/* ====================================================================== */

#define main suite_files_main
#include "../user/files.c"
#undef main

#define main suite_calc_main
#include "../user/calc.c"
#undef main

#define main suite_clock_main
#include "../user/clock.c"
#undef main

#define main suite_sysmon_main
#include "../user/sysmon.c"
#undef main

#define main suite_hex_main
#include "../user/hex.c"
#undef main

#define main suite_tasks_main
#include "../user/tasks.c"
#undef main

#define main suite_find_main
#include "../user/find.c"
#undef main

#define main suite_diff_main
#include "../user/diff.c"
#undef main

#define main suite_notes_main
#include "../user/notes.c"
#undef main

#define main suite_unit_main
#include "../user/unit.c"
#undef main

/* ====================================================================== */
/* compat.c mock surface (no header provides it)                          */
/* ====================================================================== */

extern int  mock_read_dir_count;
extern char mock_read_dir_names[40][32];
extern unsigned char mock_read_dir_attr;
extern unsigned int  mock_read_dir_size;
void mock_read_dir_reset(void);

extern int  mock_mkdir_calls;
extern char mock_mkdir_last[64];
extern int  mock_unlink_calls;
extern char mock_unlink_last[64];
extern int  mock_rename_calls;
extern char mock_rename_last_old[64];
extern char mock_rename_last_new[64];

extern int mock_sysinfo_mem_enabled;
extern unsigned long long mock_sysinfo_mem_total, mock_sysinfo_mem_free;
extern int mock_sysinfo_cpu_enabled;
extern unsigned long long mock_sysinfo_cpu_uptime, mock_sysinfo_cpu_idle;
extern int mock_sysinfo_num_cpus;
extern int mock_sysinfo_time_enabled;
extern struct sys_time mock_sysinfo_time;
void mock_sysinfo_override_reset(void);

/* ====================================================================== */
/* check framework                                                        */
/* ====================================================================== */

static int checks_run = 0;
static int checks_failed = 0;

static void suite_check(int ok, const char *what, int line) {
    checks_run++;
    if (ok) {
        printf("  PASS %s\n", what);
    } else {
        printf("  FAIL %s  (line %d)\n", what, line);
        checks_failed++;
    }
}

/* Formatted variant; `what_fmt` may contain printf conversions. */
static void suite_checkf(int ok, int line, const char *what_fmt, ...) {
    char name[512];
    va_list ap;
    va_start(ap, what_fmt);
    vsnprintf(name, sizeof name, what_fmt, ap);
    va_end(ap);
    suite_check(ok, name, line);
}

#define CHECK(cond, what)      suite_check((cond) ? 1 : 0, (what), __LINE__)
#define CHECKF(cond, ...)      suite_checkf((cond) ? 1 : 0, __LINE__, __VA_ARGS__)

static void section(const char *name) { printf("\n-- %s\n", name); }

/* ---- tiny string helpers (HobbyOS libc has none) ---------------------- */

static int has(const char *hay, const char *needle) {
    return needle != 0 && strstr(hay, needle) != 0;
}
static int starts_with(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}
static int line_containing(const char *text, const char *needle,
                           char *out, int cap) {
    const char *p = text;
    out[0] = '\0';
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len < cap) {
            memcpy(out, p, (size_t)len);
            out[len] = '\0';
            if (strstr(out, needle) != 0) return 1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}
static int max_line_width(const char *s, int len) {
    int max = 0, cur = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '\n') {
            if (cur > max) max = cur;
            cur = 0;
        } else {
            cur++;
        }
    }
    if (cur > max) max = cur;
    return max;
}

/* A frame is one full screen: the bytes after a '\f' up to the next one. */
static int count_frames(const char *s, int len) {
    int n = 0;
    for (int i = 0; i < len; i++) if (s[i] == '\f') n++;
    return n;
}
/* 0-based frame i -> [*off, *flen); returns 0 when the frame does not exist. */
static int frame_at(const char *s, int len, int i, int *off, int *flen) {
    int seen = -1, start = 0;
    for (int k = 0; k < len; k++) {
        if (s[k] == '\f') {
            if (seen == i) { *off = start; *flen = k - start; return 1; }
            seen++;
            start = k + 1;
        }
    }
    if (seen == i) { *off = start; *flen = len - start; return 1; }
    return 0;
}

/* ---- file fixtures (stdio, so the inert mkdir/unlink mocks are bypassed) */

static void file_write_n(const char *path, const void *data, int len) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (len > 0) fwrite(data, 1, (size_t)len, f);
    fclose(f);
}
static void file_write(const char *path, const char *text) {
    file_write_n(path, text, (int)strlen(text));
}
static int file_size_of(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return (int)n;
}

/* ---- stdout capture (for the print()-based renders) ------------------- */

#define CAP_MAX 32768
static char cap_buf[CAP_MAX];
static int  cap_pipe_fds[2] = { -1, -1 };
static int  cap_saved_stdout = -1;

static int capture_begin(void) {
    fflush(NULL);
    if (pipe(cap_pipe_fds) != 0) return 0;
    cap_saved_stdout = dup(1);
    if (cap_saved_stdout < 0) {
        close(cap_pipe_fds[0]); close(cap_pipe_fds[1]);
        cap_pipe_fds[0] = cap_pipe_fds[1] = -1;
        return 0;
    }
    if (dup2(cap_pipe_fds[1], 1) < 0) {
        close(cap_pipe_fds[0]); close(cap_pipe_fds[1]);
        close(cap_saved_stdout);
        cap_pipe_fds[0] = cap_pipe_fds[1] = -1;
        cap_saved_stdout = -1;
        return 0;
    }
    close(cap_pipe_fds[1]);
    cap_pipe_fds[1] = -1;
    return 1;
}

static int capture_end(void) {
    int total = 0;
    fflush(NULL);
    if (cap_saved_stdout >= 0) {
        dup2(cap_saved_stdout, 1);
        close(cap_saved_stdout);
        cap_saved_stdout = -1;
    }
    if (cap_pipe_fds[0] >= 0) {
        int n;
        while (total < CAP_MAX - 1 &&
               (n = (int)read(cap_pipe_fds[0], cap_buf + total,
                              CAP_MAX - 1 - total)) > 0) {
            total += n;
        }
        close(cap_pipe_fds[0]);
        cap_pipe_fds[0] = -1;
    }
    cap_buf[total] = '\0';
    return total;
}

/* Run a print()-based render into the caller's buffer, dropping the leading
 * gui_clear() byte so the caller always sees the screen text. */
static int capture_into(char *out, int cap, void (*fn)(void)) {
    int n, skip = 0;
    if (!capture_begin()) {
        if (cap > 0) out[0] = '\0';
        return 0;
    }
    fn();
    n = capture_end();
    if (n > 0 && cap_buf[0] == '\f') skip = 1;
    n -= skip;
    if (n > cap - 1) n = cap - 1;
    if (n < 0) n = 0;
    memcpy(out, cap_buf + skip, (size_t)n);
    out[n] = '\0';
    return n;
}

/* ====================================================================== */
/* event helpers                                                          */
/* ====================================================================== */

/* Events are compound literals, so &EV_CHAR('x') is a valid lvalue in C.
 * Parameter names must differ from the field names or the preprocessor
 * rewrites the designators themselves. */
#define EV_TYPE(typ)      ((struct gui_event){ .type = (typ) })
#define EV_CHAR(c)        ((struct gui_event){ .type = GUI_EV_CHAR, .ch = (c) })
#define EV_MENU(menu_idx, item_idx) \
    ((struct gui_event){ .type = GUI_EV_MENU, .menu = (menu_idx), .item = (item_idx) })
#define EV_MOUSE(cx, cy, btn) \
    ((struct gui_event){ .type = GUI_EV_MOUSE, .x = (cx), .y = (cy), .button = (btn) })

/* ====================================================================== */
/* per-app adapters                                                       */
/* ====================================================================== */

/* How a handler's return value maps to "a frame is coming". */
#define MODE_RC     0   /* 1 == redraw (calc, clock, sysmon, hex, find,  */
                        /*             diff, notes, unit)                */
#define MODE_BITS   1   /* files: FS_ACT_REDRAW == bit 0                 */
#define MODE_ALWAYS 2   /* tasks: the loop re-renders after every event  */

struct app_ops {
    const char *name;         /* short name used in check labels          */
    const char *title;        /* gui_set_title() argument                 */
    const char *marker;       /* startup console marker                   */
    const char *banner;       /* first rendered line (with '\n')          */
    const char *legend;       /* contract line present in every screen    */
    int  (*entry)(void);      /* the app's renamed main()                 */
    int  (*send)(const struct gui_event *ev);
    void (*reset)(void);
    int  (*render)(char *out, int cap);
    void (*pre_script)(void); /* state the scripted run inherits (or 0)   */
    int    mode;
    int    unknown_ch;        /* a key the app must ignore                */
    const char *script;       /* stdin script for the end-to-end run      */
    const char *expect0;      /* scripted run: must appear in the capture */
    const char *expect1;
    int    script_frames;
    int    script_quits;
    int    esc_frames;
    int    ignored_frames;
};

/* ---- handler shims --------------------------------------------------- */

static int send_files(const struct gui_event *ev)  { return fs_handle_event(&FS, ev); }
static int send_calc(const struct gui_event *ev)   { return calc_handle_event(ev); }
static int send_clock(const struct gui_event *ev)  { return clk_handle_event(ev); }
static int send_sysmon(const struct gui_event *ev) { return sm_handle_event(ev); }
static int send_hex(const struct gui_event *ev)    { return hex_handle_event(ev); }
static int send_tasks(const struct gui_event *ev)  { return tasks_handle_event(&tasks_state, &tasks_list, ev); }
static int send_find(const struct gui_event *ev)   { return find_handle_event(&FIND, ev); }
static int send_diff(const struct gui_event *ev)   { return diff_handle_event(ev); }
static int send_notes(const struct gui_event *ev)  { return notes_handle_event(ev); }
static int send_unit(const struct gui_event *ev)   { return unit_handle_event(ev); }

/* "The event must not ask for a frame." */
static int no_redraw_requested(const struct app_ops *o, int rc) {
    if (o->mode == MODE_BITS)   return (rc & FS_ACT_REDRAW) == 0;
    if (o->mode == MODE_ALWAYS) return rc == 0;   /* here 1 means "quit" */
    return rc == 0;
}

/* ---- render shims (fill a NUL-terminated buffer, return its length) ---- */

static void render_tasks_now(void) { tasks_render(&tasks_state, &tasks_list); }

static int rend_files(char *o, int c)  { return fs_render(o, c, &FS); }
static int rend_calc(char *o, int c)   { return calc_render(o, c); }
static int rend_clock(char *o, int c)  { return clk_render(o, c); }
static int rend_sysmon(char *o, int c) { return sm_render(o, c); }
static int rend_hex(char *o, int c)    { return hex_render(o, c); }
static int rend_tasks(char *o, int c)  { return capture_into(o, c, render_tasks_now); }
static int rend_find(char *o, int c)   { return find_render(o, c, &FIND); }
static int rend_diff(char *o, int c)   { diff_render_to(o, c); return (int)strlen(o); }
static int rend_notes(char *o, int c)  { return capture_into(o, c, notes_render); }
static int rend_unit(char *o, int c)   { return unit_render_to(o, c); }

/* ---- resets (a fresh, deterministic state for every scenario) -------- */

static void reset_files(void) {
    chdir("/home");                    /* compat's mock cwd */
    mock_read_dir_reset();
    fs_init(&FS);
}
static void reset_calc(void) {
    calc_clear_calculation();
    calc_memory = 0;
}
static void reset_clock(void) { clk_init(); }
static void reset_sysmon(void) {
    g_list.selected = 0;
    g_list.top = 0;
    g_sort_mode = SYSMON_SORT_PID;
    g_have_prev = 0;
    sm_refresh();
}
static void reset_hex(void) { hex_close_file(); }
static void reset_tasks(void) {
    memset(&tasks_state, 0, sizeof tasks_state);
    memset(&tasks_list, 0, sizeof tasks_list);
    tasks_list.visible = TASKS_VISIBLE;
    tasks_load(&tasks_state);
    tasks_list_sync(&tasks_state, &tasks_list);
}
static void reset_find(void) { find_init(&FIND); }
static void reset_diff(void) {
    memset(&a_file, 0, sizeof a_file);
    memset(&b_file, 0, sizeof b_file);
    diff_force_fallback = 0;
    diff_invalidate();
}
static void reset_notes(void) { notes_reset(); }
static void reset_unit(void) { unit_reset(); }

/* ---- fixtures shared by the wire runs --------------------------------- */

#define HEX_FIXTURE   "/tmp/apps_suite_hex.bin"
#define HEX_EMPTY     "/tmp/apps_suite_hex_empty.bin"
#define DIFF_A        "/tmp/as_a.txt"
#define DIFF_B        "/tmp/as_b.txt"
#define DIFF_B2       "/tmp/as_b2.txt"
#define DIFF_LONG_A   "/tmp/as_long_a.txt"
#define DIFF_LONG_B   "/tmp/as_long_b.txt"
#define TASKS_FIXTURE "TODO.TXT"
#define NOTES_FIXTURE "NOTES.TXT"

static const unsigned char HEX_T20[20] = {
    'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd', '!', 0x0A,
    'B', 'C', 'D', 'E', 'F', 'G', 'H'
};

static void write_tasks_fixture(void) {
    file_write(TASKS_FIXTURE, "[ ] alpha\n[x] beta\n");
}
static void write_notes_fixture(void) {
    file_write(NOTES_FIXTURE, "### First\nbody one\n### Second\nbody two\n");
}

/* A 24-entry directory: the files app window holds 18 rows, so the listing
 * scrolls.  Inherited by the forked files runs. */
static void install_long_listing(void) {
    mock_read_dir_count = 24;
    for (int i = 0; i < 24; i++)
        snprintf(mock_read_dir_names[i], 32, "FILE%02d.DAT", i);
    mock_read_dir_size = 1024;
}

/* ---- state the scripted runs inherit --------------------------------- */

static void files_script_setup(void) {
    chdir("/home");
    install_long_listing();
    fs_refresh(&FS);
}
static void hex_script_setup(void) {
    file_write_n(HEX_FIXTURE, HEX_T20, 20);
    hex_load(HEX_FIXTURE);
}
static void diff_script_setup(void) {
    file_write(DIFF_A, "one\n");
    file_write(DIFF_B, "uno\n");
    diff_load_slot(0, DIFF_A);
    diff_load_slot(1, DIFF_B);
}

/* ---- the table ------------------------------------------------------- */

static const struct app_ops APPS[] = {
    {   /* 0: files */
        "files", "Files", "[APP] FILES started\n",
        "=== Files ===\n",
        "Enter=open  d=del  r=rename  n=new  Bksp=up  q=quit",
        suite_files_main, send_files, reset_files, rend_files, files_script_setup,
        MODE_BITS, 'z',
        "\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B"
        "\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[Bq",
        "FILE19.DAT", "Enter=open",
        20, 1, 1, 1
    },
    {   /* 1: calc */
        "calc", "Calculator", "[APP] CALC started\n",
        "=== Calculator ===\n",
        "Memory: ",
        suite_calc_main, send_calc, reset_calc, rend_calc, 0,
        MODE_RC, 'Z',
        "12+3\033[P4;3;1~=",
        "> 49", "12+37",
        7, 0, 1, 1
    },
    {   /* 2: clock */
        "clock", "Clock", "[APP] CLOCK started\n",
        "=== Clock ===\n",
        "< > month  ^ v year  t=today",
        suite_clock_main, send_clock, reset_clock, rend_clock, 0,
        MODE_RC, 'z',
        "r",
        "12:00:00", "September 2026",
        2, 0, 1, 1
    },
    {   /* 3: sysmon */
        "sysmon", "SysMon", "[APP] SYSMON started\n",
        "=== System Monitor ===\n",
        "k=kill  r=refresh  arrows=scroll",
        suite_sysmon_main, send_sysmon, reset_sysmon, rend_sysmon, 0,
        MODE_RC, 'z',
        "r",
        "DESKTOP.BIN", "Uptime: 00:00:12",
        2, 0, 1, 1
    },
    {   /* 4: hex */
        "hex", "Hex Viewer", "[APP] HEX started\n",
        "=== Hex Viewer ===\n",
        "No file open - File > Open (or press o)",
        suite_hex_main, send_hex, reset_hex, rend_hex, hex_script_setup,
        MODE_RC, 'z',
        "\033[B\033[A",
        "48 65 6C 6C 6F 20 57 6F 72 6C 64 21 0A 42 43 44", "|Hello World!.BCD|",
        3, 0, 1, 1
    },
    {   /* 5: tasks */
        "tasks", "Tasks", "[APP] TASKS started\n",
        "=== Tasks ===\n",
        "a=add  Enter/space=toggle  d=delete  r=reload",
        suite_tasks_main, send_tasks, reset_tasks, rend_tasks, 0,
        MODE_ALWAYS, 'z',
        "\033[Bq",
        " > [x] beta", "2 tasks, 1 done",
        2, 1, 2, 4
    },
    {   /* 6: find */
        "find", "Find", "[APP] FIND started\n",
        "=== Find ===\n",
        "type=edit  Enter=search  Bksp=edit  c=clear  arrows=scroll",
        suite_find_main, send_find, reset_find, rend_find, 0,
        MODE_RC, 1,
        "TE\n",
        "TEST.TXT", "Results: 2  (scanned 7 entries)",
        4, 0, 1, 1
    },
    {   /* 7: diff */
        "diff", "Diff", "[APP] DIFF started\n",
        "=== Diff ===\n",
        "o=open A  O=open B  c=compare  n=next change  arrows=scroll",
        suite_diff_main, send_diff, reset_diff, rend_diff, diff_script_setup,
        MODE_RC, 'z',
        "c\033[B",
        " - one", " + uno",
        3, 0, 1, 1
    },
    {   /* 8: notes */
        "notes", "Notes", "[APP] NOTES started\n",
        "=== Notes ===\n",
        "n=new  e=edit body  d=delete  Enter=save",
        suite_notes_main, send_notes, reset_notes, rend_notes, 0,
        MODE_RC, 'z',
        "\033[Br",
        "Second", "2 notes",
        3, 0, 1, 1
    },
    {   /* 9: unit */
        "unit", "Unit Converter", "[APP] UNIT started\n",
        "=== Unit Converter ===\n",
        "e=edit value  s=swap  <>=category  ^v=from unit",
        suite_unit_main, send_unit, reset_unit, rend_unit, 0,
        MODE_RC, 'z',
        "s\033[D",
        "Result: 3.28 ft", "Category: Length",
        3, 0, 1, 1
    },
};
#define NAPPS     ((int)(sizeof APPS / sizeof APPS[0]))
#define APP_NOTES 8

/* ====================================================================== */
/* layer B: forked end-to-end runs                                        */
/* ====================================================================== */

#define OUT_MAX 32768
#define RUN_FIRST_MS 2000   /* nothing at all within this -> give up     */
#define RUN_QUIET_MS 250    /* silence for this long -> the run is done  */

struct run_result {
    char out[OUT_MAX];
    int  len;
    int  status;        /* raw waitpid status                         */
    int  killed;        /* 1 when the harness had to SIGKILL the app  */
};

/* Read whatever the app printed (waiting up to wait_ms for the first byte)
 * and append it to the result.  Returns 1 when at least one byte arrived. */
static int run_drain(int fd, struct run_result *res, int wait_ms) {
    fd_set set;
    struct timeval tv;
    int ready, n;

    if (res->len >= OUT_MAX - 1) return 0;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = 1000 * (wait_ms % 1000);
    ready = select(fd + 1, &set, 0, 0, &tv);
    if (ready <= 0) return 0;
    n = (int)read(fd, res->out + res->len, OUT_MAX - 1 - res->len);
    if (n <= 0) return 0;
    res->len += n;
    res->out[res->len] = '\0';
    return 1;
}

static void run_app(const struct app_ops *o, const char *script,
                    struct run_result *res, int step_ms) {
    int in_pipe[2] = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    pid_t pid;

    memset(res, 0, sizeof *res);
    res->status = -1;

    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        res->out[0] = '\0';
        return;
    }

    fflush(NULL);
    pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        res->out[0] = '\0';
        return;
    }
    if (pid == 0) {
        /* Child: the app's real entry point, on the pipes. */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        o->entry();
        _exit(0);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    /*
     * step_ms == 0: hand the app the whole script at once.  That is what the
     * event-loop tests want (gui_read_event() buffered-reads ahead, exactly
     * like the desktop).  step_ms > 0: feed ONE byte at a time, which is the
     * only way the modal dialogs (they read stdin directly) can be scripted:
     * gui_read_event() must not swallow the dialog's input.
     */
    if (script != 0 && script[0] != '\0') {
        if (step_ms <= 0) {
            ssize_t w = write(in_pipe[1], script, strlen(script));
            (void)w;
        } else {
            for (const char *p = script; *p; p++) {
                ssize_t w = write(in_pipe[1], p, 1);
                (void)w;
                if (run_drain(out_pipe[0], res, 0)) {
                    while (run_drain(out_pipe[0], res, 0)) { }
                }
                usleep((useconds_t)(step_ms * 1000));
            }
        }
    }
    close(in_pipe[1]);   /* the app sees EOF once the script is drained */

    while (run_drain(out_pipe[0], res, res->len == 0 ? RUN_FIRST_MS : RUN_QUIET_MS)) { }
    close(out_pipe[0]);

    {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            res->killed = 1;
        }
        res->status = st;
    }
}

/* ====================================================================== */
/* layer A: cross-cutting invariants, one block per app                   */
/* ====================================================================== */

static void suite_invariants(const struct app_ops *o) {
    static char r1[8192], r2[8192];
    int n1, n2, rc;
    const char *nm = o->name;

    o->reset();
    n1 = o->render(r1, (int)sizeof r1);
    CHECKF(n1 > 0, "[%s] renders a non-empty screen before any input", nm);
    CHECKF(starts_with(r1, o->banner), "[%s] first line is the app banner", nm);
    CHECKF(has(r1, o->legend), "[%s] every screen carries the key legend", nm);

    n2 = o->render(r2, (int)sizeof r2);
    CHECKF(n2 == n1 && memcmp(r1, r2, (size_t)n1) == 0,
           "[%s] two renders of the same state are byte-identical", nm);

    CHECKF(n1 <= 2048, "[%s] render fits the 2048-byte window text buffer", nm);
    CHECKF(max_line_width(r1, n1) <= 110,
           "[%s] no rendered line is wider than 110 columns", nm);

    /* ESC alone: no frame, no change (it is not a command for any app). */
    rc = o->send(&EV_TYPE(GUI_EV_ESC));
    n2 = o->render(r2, (int)sizeof r2);
    CHECKF(no_redraw_requested(o, rc), "[%s] ESC asks for no redraw", nm);
    CHECKF(n2 == n1 && memcmp(r1, r2, (size_t)n1) == 0,
           "[%s] ESC leaves the screen unchanged", nm);

    /* An unknown key. */
    rc = o->send(&EV_CHAR(o->unknown_ch));
    n2 = o->render(r2, (int)sizeof r2);
    CHECKF(no_redraw_requested(o, rc), "[%s] an unknown key asks for no redraw", nm);
    CHECKF(n2 == n1 && memcmp(r1, r2, (size_t)n1) == 0,
           "[%s] an unknown key leaves the screen unchanged", nm);

    /* An unknown menu index. */
    rc = o->send(&EV_MENU(7, 9));
    n2 = o->render(r2, (int)sizeof r2);
    CHECKF(no_redraw_requested(o, rc),
           "[%s] an unknown menu index asks for no redraw", nm);
    CHECKF(n2 == n1 && memcmp(r1, r2, (size_t)n1) == 0,
           "[%s] an unknown menu index leaves the screen unchanged", nm);

    /* A right click. */
    rc = o->send(&EV_MOUSE(3, 3, 2));
    n2 = o->render(r2, (int)sizeof r2);
    CHECKF(no_redraw_requested(o, rc), "[%s] a right click asks for no redraw", nm);
    CHECKF(n2 == n1 && memcmp(r1, r2, (size_t)n1) == 0,
           "[%s] a right click leaves the screen unchanged", nm);
}

/* ====================================================================== */
/* layer B checks, one block per app                                      */
/* ====================================================================== */

static void suite_wire(const struct app_ops *o) {
    static struct run_result a, b, ig, sc;
    char want_title[128];
    const char *nm = o->name;
    int off, flen, i;

    o->reset();
    snprintf(want_title, sizeof want_title, "\033]T%s~", o->title);

    /* The mock world is per-app deterministic: a previous app's scripted
     * setup (the 24-entry listing) must not leak into this one. */
    mock_read_dir_reset();
    mock_sysinfo_override_reset();

    run_app(o, "\033", &a, 0);
    run_app(o, "\033", &b, 0);

    CHECKF(has(a.out, want_title),
           "[%s] startup sets the window title on the wire", nm);
    CHECKF(has(a.out, o->marker), "[%s] startup prints its console marker", nm);
    CHECKF(has(a.out, o->banner), "[%s] startup draws the app banner", nm);
    CHECKF(count_frames(a.out, a.len) == o->esc_frames,
           "[%s] ESC-only input produces %d frame(s)", nm, o->esc_frames);
    CHECKF(a.killed == 1, "[%s] stays alive with no quit key", nm);
    CHECKF(a.len == b.len && memcmp(a.out, b.out, (size_t)a.len) == 0,
           "[%s] two identical runs print byte-identical output", nm);

    /* Frame geometry on the wire; the escapes before the first '\f' are
     * startup protocol, not a screen. */
    {
        int bad_size = 0, bad_width = 0;
        for (i = 0; i < o->esc_frames; i++) {
            if (!frame_at(a.out, a.len, i, &off, &flen)) {
                bad_size = bad_width = 1;
                break;
            }
            if (flen > 2048) bad_size = 1;
            if (max_line_width(a.out + off, flen) > 110) bad_width = 1;
        }
        CHECKF(!bad_size, "[%s] every frame fits the 2048-byte text buffer", nm);
        CHECKF(!bad_width, "[%s] no frame line exceeds 110 columns", nm);
    }

    /* Ignored input: unknown key, unknown menu, right click. */
    {
        char script[64];
        if (o->unknown_ch >= 32 && o->unknown_ch <= 126)
            snprintf(script, sizeof script, "%c\033[M7;9~\033[P3;3;2~", o->unknown_ch);
        else
            snprintf(script, sizeof script, "\001\033[M7;9~\033[P3;3;2~");
        run_app(o, script, &ig, 0);
        CHECKF(count_frames(ig.out, ig.len) == o->ignored_frames,
               "[%s] unknown key/menu/right-click produce %d frame(s)",
               nm, o->ignored_frames);
        if (o->ignored_frames == 1) {
            CHECKF(has(ig.out, o->banner) && has(ig.out, o->legend),
                   "[%s] the ignored-input run drew exactly the startup screen", nm);
        } else {
            /* tasks re-renders after every event by design; the re-renders
             * must be byte-identical to the startup screen. */
            int off0 = 0, len0 = 0, offN = 0, lenN = 0;
            int same = frame_at(ig.out, ig.len, 0, &off0, &len0) &&
                       frame_at(ig.out, ig.len, o->ignored_frames - 1, &offN, &lenN) &&
                       len0 == lenN && len0 > 0 &&
                       memcmp(ig.out + off0, ig.out + offN, (size_t)len0) == 0;
            CHECKF(same, "[%s] its re-render after ignored input is byte-identical", nm);
        }
    }

    /* The scripted interaction. */
    if (o->pre_script) o->pre_script();
    run_app(o, o->script, &sc, 0);
    CHECKF(has(sc.out, o->expect0),
           "[%s] scripted interaction shows \"%s\"", nm, o->expect0);
    if (o->expect1 != 0) {
        CHECKF(has(sc.out, o->expect1),
               "[%s] scripted interaction shows \"%s\"", nm, o->expect1);
    }
    CHECKF(count_frames(sc.out, sc.len) == o->script_frames,
           "[%s] scripted interaction drew %d frame(s)", nm, o->script_frames);
    if (o->script_quits) {
        CHECKF(WIFEXITED(sc.status) && WEXITSTATUS(sc.status) == 0,
               "[%s] its quit key exits cleanly with status 0", nm);
    } else {
        CHECKF(sc.killed == 1, "[%s] keeps running after the script", nm);
    }
}

/* ====================================================================== */
/* app-specific interaction checks                                        */
/* ====================================================================== */

/* ---- fake dialogs ----------------------------------------------------- */

static int mock_dlg_confirm_ret = 0;
static int mock_dlg_confirm_calls = 0;
static char mock_dlg_prompt_text[64] = "";
static int mock_dlg_prompt_ret = 0;
static int mock_dlg_prompt_calls = 0;
static char mock_dlg_msg_title[64] = "";
static char mock_dlg_msg_body[128] = "";
static int mock_dlg_msg_calls = 0;

static int fake_confirm(const char *title, const char *msg) {
    (void)title; (void)msg;
    mock_dlg_confirm_calls++;
    return mock_dlg_confirm_ret;
}
static int fake_prompt(const char *title, const char *msg, char *buf, int max) {
    (void)title; (void)msg;
    mock_dlg_prompt_calls++;
    if (!mock_dlg_prompt_ret) return 0;
    snprintf(buf, (size_t)(max > 0 ? max : 1), "%s", mock_dlg_prompt_text);
    return 1;
}
static void fake_message(const char *title, const char *msg) {
    mock_dlg_msg_calls++;
    snprintf(mock_dlg_msg_title, sizeof mock_dlg_msg_title, "%s", title ? title : "");
    snprintf(mock_dlg_msg_body, sizeof mock_dlg_msg_body, "%s", msg ? msg : "");
}

/* ---- 0. files -------------------------------------------------------- */

static void suite_files_interaction(void) {
    static char out[8192], before[8192];
    char cwd[64];

    section("files: listing, scrolling, dialogs and navigation");
    mock_read_dir_reset();
    chdir("/home");
    fs_init(&FS);
    fs_render(out, (int)sizeof out, &FS);
    CHECK(FS.count == 7, "files: loads the 7-entry mock directory");
    CHECK(has(out, "Path: /home"), "files: the path line shows the mocked cwd");
    CHECK(has(out, "Free: 40.0M"), "files: the free cell comes from sysinfo(7)");
    CHECK(has(out, "EDITOR.BIN") && has(out, " 0B"),
          "files: a row shows the name and the 0-byte size cell");
    CHECK(has(out, "> EDITOR.BIN"), "files: the first row is marked as selected");

    /* A long directory: scrolling must change the visible subset. */
    install_long_listing();
    fs_refresh(&FS);
    fs_render(out, (int)sizeof out, &FS);
    CHECK(FS.count == 24 && has(out, "FILE00.DAT"),
          "files: a 24-entry directory loads and shows the first rows");
    for (int i = 0; i < 19; i++) fs_handle_event(&FS, &EV_TYPE(GUI_EV_DOWN));
    fs_render(before, (int)sizeof before, &FS);
    CHECK(FS.list.selected == 19 && FS.list.top == 2,
          "files: 19 downs move the 18-row window to top=2");
    CHECK(has(before, "FILE19.DAT") && !has(before, "FILE00.DAT"),
          "files: scrolling changes the visible subset (FILE00 scrolled off)");

    /* Delete: confirm first, then unlink. */
    fs_dlg_confirm = fake_confirm;
    fs_dlg_prompt = fake_prompt;
    fs_dlg_message = fake_message;
    mock_dlg_confirm_ret = 0;
    mock_dlg_confirm_calls = 0;
    mock_unlink_calls = 0;
    mock_unlink_last[0] = '\0';
    FS.list.selected = 0;
    fs_handle_event(&FS, &EV_CHAR('d'));
    CHECK(mock_dlg_confirm_calls == 1 && mock_unlink_calls == 0,
          "files: 'd' asks for confirmation first");
    mock_dlg_confirm_ret = 1;
    fs_handle_event(&FS, &EV_CHAR('d'));
    CHECK(mock_unlink_calls == 1 && strcmp(mock_unlink_last, "FILE00.DAT") == 0,
          "files: a confirmed delete unlinks the selected entry");

    /* Enter on a file: the info dialog carries name and size. */
    mock_dlg_msg_calls = 0;
    FS.list.selected = 1;
    fs_handle_event(&FS, &EV_CHAR('\n'));
    CHECK(mock_dlg_msg_calls == 1 && strcmp(mock_dlg_msg_title, "File") == 0 &&
          has(mock_dlg_msg_body, "FILE01.DAT") &&
          has(mock_dlg_msg_body, "(1024 bytes)"),
          "files: Enter on a file opens the File info dialog with name and size");

    /* Enter on a directory: chdir into it. */
    mock_read_dir_attr = 0x10;      /* every entry is a directory */
    fs_refresh(&FS);
    FS.list.selected = 0;
    fs_handle_event(&FS, &EV_CHAR('\n'));
    getcwd(cwd, sizeof cwd);
    CHECK(strcmp(cwd, "FILE00.DAT") == 0,
          "files: Enter on a [DIR] row chdirs into it");
    mock_read_dir_attr = 0;

    /* 'n' new folder goes through the prompt seam into mkdir. */
    mock_dlg_prompt_ret = 1;
    snprintf(mock_dlg_prompt_text, sizeof mock_dlg_prompt_text, "NEWDIR");
    mock_dlg_prompt_calls = 0;
    mock_mkdir_calls = 0;
    fs_handle_event(&FS, &EV_CHAR('n'));
    CHECK(mock_dlg_prompt_calls == 1 && mock_mkdir_calls == 1 &&
          strcmp(mock_mkdir_last, "NEWDIR") == 0,
          "files: 'n' prompts once and creates the folder named in the prompt");

    /* Backspace = up, Go>Root = "/". */
    fs_refresh(&FS);
    fs_handle_event(&FS, &EV_CHAR('\b'));
    getcwd(cwd, sizeof cwd);
    CHECK(strcmp(cwd, "..") == 0, "files: Backspace goes up one level");
    fs_handle_event(&FS, &EV_MENU(1, 1));
    getcwd(cwd, sizeof cwd);
    CHECK(strcmp(cwd, "/") == 0, "files: the Go>Root menu item jumps to '/'");

    /* Rename through the prompt seam. */
    mock_dlg_prompt_ret = 1;
    snprintf(mock_dlg_prompt_text, sizeof mock_dlg_prompt_text, "RENAMED.DAT");
    mock_rename_calls = 0;
    FS.list.selected = 0;
    fs_handle_event(&FS, &EV_CHAR('r'));
    CHECK(mock_rename_calls == 1 && strcmp(mock_rename_last_old, "FILE00.DAT") == 0 &&
          strcmp(mock_rename_last_new, "RENAMED.DAT") == 0,
          "files: 'r' renames the selection to the prompted name");

    /* Mouse click selects a row; an empty directory renders its marker. */
    fs_refresh(&FS);
    fs_handle_event(&FS, &EV_MOUSE(0, FS_ROW_ENTRY0 + 2, 1));
    CHECK(FS.list.selected == 2, "files: a left click selects the clicked row");
    mock_read_dir_count = -1;       /* empty directory */
    fs_refresh(&FS);
    fs_render(out, (int)sizeof out, &FS);
    CHECK(FS.count == 0 && has(out, "  (empty)"),
          "files: an empty directory renders the (empty) marker");
    CHECK(has(out, "Free: 40.0M"),
          "files: the free cell survives an empty directory");
    CHECK(fs_handle_event(&FS, &EV_CHAR('q')) == FS_ACT_QUIT,
          "files: 'q' asks the app to quit");
    mock_read_dir_reset();

    fs_dlg_confirm = dialog_confirm;
    fs_dlg_prompt = dialog_prompt;
    fs_dlg_message = dialog_message;
}

/* ---- 1. calc --------------------------------------------------------- */

static void suite_calc_interaction(void) {
    static char out[4096];

    section("calc: expression, evaluation, memory and keypad");
    reset_calc();
    calc_handle_event(&EV_CHAR('7'));
    calc_handle_event(&EV_CHAR('*'));
    calc_handle_event(&EV_CHAR('8'));
    calc_render(out, (int)sizeof out);
    CHECK(calc_expr_len == 3 && strcmp(calc_expr, "7*8") == 0,
          "calc: typing '7*8' builds the expression");
    CHECK(has(out, "7*8"), "calc: the display shows the expression");
    CHECK(calc_handle_event(&EV_CHAR('=')) == 1 && calc_result == 56,
          "calc: '=' evaluates the expression (7*8 = 56)");
    calc_render(out, (int)sizeof out);
    CHECK(has(out, "> 56"), "calc: the result line shows '> 56'");

    reset_calc();
    for (const char *k = "2+3*4="; *k; k++) calc_handle_event(&EV_CHAR(*k));
    calc_render(out, (int)sizeof out);
    CHECK(has(out, "> 20"), "calc: 2+3*4 = 20 (strictly left to right)");

    reset_calc();
    for (const char *k = "5/0="; *k; k++) calc_handle_event(&EV_CHAR(*k));
    calc_render(out, (int)sizeof out);
    CHECK(has(out, "Error: division by zero"),
          "calc: divide by zero enters the error state");
    CHECK(calc_handle_event(&EV_CHAR('7')) == 1 && calc_error == CALC_OK &&
          calc_expr_len == 1 && calc_expr[0] == '7',
          "calc: a digit recovers from the error state");

    reset_calc();
    for (const char *k = "9="; *k; k++) calc_handle_event(&EV_CHAR(*k));
    CHECK(calc_handle_event(&EV_MENU(0, 1)) == 1 && calc_memory == 9,
          "calc: the Mem+ menu item adds the current value to memory");
    calc_render(out, (int)sizeof out);
    CHECK(has(out, "Memory: 9"), "calc: the memory line shows the stored value");
    CHECK(calc_handle_event(&EV_MENU(0, 3)) == 1 &&
          calc_expr_len == 1 && calc_expr[0] == '9',
          "calc: MemRecall appends the memory value to the expression");
    CHECK(calc_handle_event(&EV_MENU(0, 0)) == 1 && calc_expr_len == 0,
          "calc: the Clear menu item empties the expression");
    CHECK(calc_memory == 9, "calc: Clear keeps the memory register");

    reset_calc();
    CHECK(calc_handle_event(&EV_MOUSE(4, CALC_ROW_KEYPAD, 1)) == 1 &&
          calc_expr_len == 1 && calc_expr[0] == '7',
          "calc: a click on keypad row 0 col 1 enters '7'");
    calc_handle_event(&EV_CHAR('8'));
    calc_handle_event(&EV_CHAR('\b'));
    CHECK(calc_expr_len == 1 && calc_expr[0] == '7',
          "calc: backspace deletes the last input character");
    CHECK(calc_handle_event(&EV_MOUSE(4, 1, 1)) == 0,
          "calc: a click outside the keypad is ignored");
    CHECK(calc_handle_event(&EV_CHAR(' ')) == 0, "calc: whitespace is ignored");
}

/* ---- 2. clock -------------------------------------------------------- */

static void suite_clock_interaction(void) {
    static char out[4096];
    struct sys_time t;

    section("clock: injected time, calendar navigation and uptime fallback");
    memset(&t, 0, sizeof t);
    t.epoch = 1789646400ULL;
    t.year = 2026; t.month = 9; t.day = 17;
    t.hour = 12; t.minute = 34; t.second = 56;
    t.weekday = 4;
    clk_init();
    clk_apply_time(&t);
    clk.disp_year = 2026;
    clk.disp_month = 9;
    clk_render(out, (int)sizeof out);
    CHECK(has(out, "12:34:56"), "clock: renders the injected time as HH:MM:SS");
    CHECK(has(out, "September 2026"),
          "clock: the calendar header shows the injected month");
    CHECK(has(out, "[17]"), "clock: today's day is bracketed in the grid");
    CHECK(has(out, "Thursday, September 17, 2026"),
          "clock: the date line uses the injected weekday");
    {
        char hms[16];
        CHECK(clk_fmt_hms(7, 5, 9, hms) == 8 && strcmp(hms, "07:05:09") == 0,
              "clock: HH:MM:SS zero-pads single digits");
    }
    {
        char row[32];
        CHECK(clk_art_time_row(12, 34, 0, row) == 17 && strlen(row) == 17 &&
              has(row, "#"),
              "clock: the big ASCII time row is 17 columns of glyphs");
    }

    CHECK(clk_handle_event(&EV_TYPE(GUI_EV_RIGHT)) == 1 && clk.disp_month == 10,
          "clock: the right arrow advances the displayed month");
    clk_render(out, (int)sizeof out);
    CHECK(has(out, "October 2026"), "clock: the new month is rendered");
    CHECK(clk_handle_event(&EV_TYPE(GUI_EV_UP)) == 1 && clk.disp_year == 2025,
          "clock: the up arrow moves the displayed year back");
    CHECK(clk_handle_event(&EV_CHAR('t')) == 1 && clk.disp_month == 9 &&
          clk.disp_year == 2026,
          "clock: 't' jumps back to the RTC month");

    /* Injected clock through the compat sysinfo override (live path). */
    mock_sysinfo_time_enabled = 1;
    mock_sysinfo_time = t;
    mock_sysinfo_time.hour = 6;
    mock_sysinfo_time.minute = 7;
    mock_sysinfo_time.second = 8;
    clk_refresh();
    clk_render(out, (int)sizeof out);
    CHECK(has(out, "06:07:08"),
          "clock: a sysinfo(6) injection drives the live refresh path");
    mock_sysinfo_override_reset();
    clk_refresh();

    /* No RTC: the uptime fallback screen. */
    clk.have_rtc = 0;
    clk.uptime_ms = 3661000ULL;      /* 1h 1m 1s */
    clk_render(out, (int)sizeof out);
    CHECK(has(out, "RTC unavailable"),
          "clock: without an RTC it shows the uptime fallback");
    CHECK(has(out, "01:01:01"), "clock: the fallback formats the uptime");
    CHECK(clk_handle_event(&EV_MENU(1, 0)) == 0,
          "clock: an unknown menu index is ignored");
    clk_init();
    CHECK(clk.have_rtc == 1, "clock: clk_init recovers the RTC from sysinfo(6)");
}

/* ---- 3. sysmon ------------------------------------------------------- */

static int fake_kill_calls = 0;
static int fake_kill_pid = -1;
static int fake_kill_sig = -1;
static int fake_kill_confirm = 1;

static int sysmon_fake_confirm(const char *title, const char *msg) {
    (void)title; (void)msg;
    return fake_kill_confirm;
}
static int sysmon_fake_kill(int pid, int sig) {
    fake_kill_calls++;
    fake_kill_pid = pid;
    fake_kill_sig = sig;
    return 0;
}

static void suite_sysmon_interaction(void) {
    static char out[8192], row[256];

    section("sysmon: injected system values, selection, kill and sort");
    reset_sysmon();
    sm_render(out, (int)sizeof out);
    CHECK(has(out, "PID  NAME             STATE"),
          "sysmon: the process table header is rendered");
    CHECK(has(out, "DESKTOP.BIN") && has(out, "EDITOR.BIN") && has(out, "SHELL"),
          "sysmon: lists the injected process table");
    CHECK(has(out, "Uptime: 00:00:12") && has(out, "CPUs: 4"),
          "sysmon: shows the injected uptime and CPU count");
    CHECK(has(out, "Mem: 24.0M/64.0M"),
          "sysmon: shows the injected memory totals");
    CHECK(has(out, "CPU ["), "sysmon: draws the CPU bar");

    /* Injection through the compat sysinfo override. */
    mock_sysinfo_mem_enabled = 1;
    mock_sysinfo_mem_total = 100ULL * 1024 * 1024;
    mock_sysinfo_mem_free = 50ULL * 1024 * 1024;
    sm_refresh();
    sm_render(out, (int)sizeof out);
    CHECK(has(out, "Mem: 50.0M/100.0M"),
          "sysmon: a different injected memory value changes the screen");
    {
        char bar[64];
        gui_bar(bar, SYSMON_BAR_WIDTH, 50);
        CHECK(has(out, bar), "sysmon: the memory bar reflects the injected 50%");
    }
    mock_sysinfo_mem_enabled = 0;

    /* CPU% from two injected samples: 10s elapsed, 0s idle -> 100%. */
    mock_sysinfo_cpu_enabled = 1;
    g_have_prev = 0;
    mock_sysinfo_cpu_uptime = 20000;
    mock_sysinfo_cpu_idle = 4000;
    sm_refresh();
    mock_sysinfo_cpu_uptime = 30000;
    mock_sysinfo_cpu_idle = 4000;
    sm_refresh();
    sm_render(out, (int)sizeof out);
    CHECK(has(out, "100%"), "sysmon: CPU% comes from two injected samples");
    mock_sysinfo_override_reset();

    /* Selection via arrows and clicks. */
    reset_sysmon();
    sm_handle_event(&EV_TYPE(GUI_EV_DOWN));
    sm_render(out, (int)sizeof out);
    CHECK(g_list.selected == 1, "sysmon: the down arrow moves the selection");
    CHECK(line_containing(out, "EDITOR.BIN", row, sizeof row) && has(row, " <"),
          "sysmon: the selected row carries the ' <' marker");
    sm_handle_event(&EV_MOUSE(0, SYSMON_LIST_TOP_ROW + 2, 1));
    CHECK(g_list.selected == 2, "sysmon: a left click selects the clicked process row");

    /* Kill path through the injected hooks. */
    {
        sysmon_confirm_fn saved_confirm = g_confirm;
        sysmon_kill_fn saved_kill = g_kill;
        g_confirm = sysmon_fake_confirm;
        g_kill = sysmon_fake_kill;
        fake_kill_calls = 0;
        fake_kill_confirm = 0;
        sm_handle_event(&EV_CHAR('k'));
        CHECK(fake_kill_calls == 0, "sysmon: a declined kill never signals");
        fake_kill_confirm = 1;
        sm_handle_event(&EV_CHAR('k'));
        CHECK(fake_kill_calls == 1 && fake_kill_pid == 3 && fake_kill_sig == 9,
              "sysmon: a confirmed kill SIGKILLs the selected pid");
        g_confirm = saved_confirm;
        g_kill = saved_kill;
    }

    /* Sort toggle keeps the selection and reorders the listing. */
    reset_sysmon();
    g_list.selected = 2;                       /* SHELL, last by pid */
    sm_handle_event(&EV_MENU(0, 2));
    CHECK(g_sort_mode == SYSMON_SORT_NAME,
          "sysmon: the Sort menu item flips to name order");
    sm_render(out, (int)sizeof out);
    CHECK(has(out, " <"), "sysmon: the selection survives the re-sort");
    sm_handle_event(&EV_MENU(0, 2));
    CHECK(g_sort_mode == SYSMON_SORT_PID,
          "sysmon: a second Sort flips back to pid order");
    CHECK(sm_handle_event(&EV_MENU(9, 9)) == 0,
          "sysmon: an unknown menu index is ignored");
}

/* ---- 4. hex ---------------------------------------------------------- */

static void suite_hex_interaction(void) {
    static char out[8192];

    section("hex: loading a byte stream, hex+ASCII rendering, scrolling");
    file_write_n(HEX_FIXTURE, HEX_T20, 20);
    file_write(HEX_EMPTY, "");

    reset_hex();
    hex_render(out, (int)sizeof out);
    CHECK(has(out, "No file open - File > Open (or press o)"),
          "hex: starts with the no-file hint");
    CHECK(hex_load(HEX_FIXTURE) == 1 && hex_len == 20 && hex_loaded == 1,
          "hex: loads a 20-byte stream");
    hex_render(out, (int)sizeof out);
    CHECK(has(out, "File: " HEX_FIXTURE "  (20 bytes)"),
          "hex: the header shows the name and byte count");
    CHECK(has(out, "00000000  48 65 6C 6C 6F 20 57 6F 72 6C 64 21 0A 42 43 44"),
          "hex: row 0 renders the bytes as uppercase hex");
    CHECK(has(out, "|Hello World!.BCD|"),
          "hex: row 0 renders the same bytes as ASCII");
    CHECK(has(out, "00000010  45 46 47 48") && has(out, "|EFGH"),
          "hex: the short second row keeps the columns aligned");
    CHECK(has(out, "Offset: 0x00000000 (row 1/2)"),
          "hex: the offset line reports the first row of two");

    CHECK(hex_handle_event(&EV_TYPE(GUI_EV_DOWN)) == HEX_ACT_REDRAW &&
          hex_top_row == 1,
          "hex: the down arrow scrolls one row");
    hex_render(out, (int)sizeof out);
    CHECK(has(out, "Offset: 0x00000010 (row 2/2)"),
          "hex: the offset line follows the scroll");
    CHECK(hex_handle_event(&EV_TYPE(GUI_EV_RIGHT)) == HEX_ACT_REDRAW &&
          hex_top_row == 1,
          "hex: paging past the end clamps to the last row");
    CHECK(hex_parse_offset("0x10") == 16 && hex_parse_offset(" 20 ") == 32 &&
          hex_parse_offset("zz") == -1 && hex_parse_offset("") == -1,
          "hex: offset parsing accepts 0x/plain and rejects junk");
    hex_goto_offset(20);
    CHECK(hex_top_row == 1, "hex: goto clamps the offset to the last row");
    hex_goto_offset(0);
    CHECK(hex_top_row == 0, "hex: goto 0 returns to the first row");
    CHECK(hex_total_rows(20) == 2 && hex_total_rows(0) == 0,
          "hex: row count is derived from the byte count");

    CHECK(hex_handle_event(&EV_CHAR('c')) == HEX_ACT_REDRAW && hex_loaded == 0,
          "hex: 'c' closes the file");
    hex_render(out, (int)sizeof out);
    CHECK(has(out, "No file open"), "hex: the closed screen shows the hint again");
    CHECK(hex_handle_event(&EV_CHAR('q')) == HEX_ACT_QUIT,
          "hex: 'q' asks the loop to quit");
    CHECK(hex_handle_event(&EV_CHAR('g')) == HEX_ACT_REDRAW && hex_loaded == 0,
          "hex: goto on a closed file is inert (no dialog, no load)");

    CHECK(hex_load(HEX_EMPTY) == 1 && hex_len == 0, "hex: loads an empty file");
    hex_render(out, (int)sizeof out);
    CHECK(has(out, "(0 bytes)") && has(out, "(row 0/0)"),
          "hex: an empty file renders a zero-byte header and no rows");
    hex_close_file();
}

/* ---- 5. tasks -------------------------------------------------------- */

static int tasks_render_capture(char *out, int cap) {
    return capture_into(out, cap, render_tasks_now);
}

static void suite_tasks_interaction(void) {
    static char out[8192];
    static char saved_first[64];
    int saved_count;

    section("tasks: add / toggle / delete round trip and persistence");
    write_tasks_fixture();
    reset_tasks();
    CHECK(tasks_state.count == 2 && tasks_done_count(&tasks_state) == 1,
          "tasks: loads the TODO.TXT fixture (2 items, 1 done)");
    tasks_render_capture(out, (int)sizeof out);
    CHECK(has(out, "2 tasks, 1 done"),
          "tasks: the count line reports items and done count");
    CHECK(has(out, " > [ ] alpha") && has(out, "   [x] beta"),
          "tasks: the first row is selected and shows the done state");

    CHECK(tasks_add(&tasks_state, "gamma") == 1 && tasks_state.count == 3,
          "tasks: add appends an item");
    tasks_list_sync(&tasks_state, &tasks_list);
    tasks_list.selected = tasks_state.count - 1;
    tasks_render_capture(out, (int)sizeof out);
    CHECK(has(out, " > [ ] gamma"), "tasks: the new item is rendered as selected");
    CHECK(tasks_toggle(&tasks_state, tasks_list.selected) == 1,
          "tasks: toggle flips the done flag");
    tasks_render_capture(out, (int)sizeof out);
    CHECK(has(out, "[x] gamma") && has(out, "3 tasks, 2 done"),
          "tasks: the toggled item renders as done");
    CHECK(tasks_delete(&tasks_state, tasks_list.selected) == 1 &&
          tasks_state.count == 2,
          "tasks: delete removes it - the round trip closes");
    tasks_list_sync(&tasks_state, &tasks_list);
    tasks_render_capture(out, (int)sizeof out);
    CHECK(!has(out, "gamma"), "tasks: the deleted item is gone from the screen");

    CHECK(tasks_add(&tasks_state, "") == 0 &&
          strcmp(tasks_state.status, "Nothing to add.") == 0,
          "tasks: empty text is rejected with a status line");
    CHECK(tasks_toggle(&tasks_state, 99) == -1 && tasks_delete(&tasks_state, -1) == 0,
          "tasks: out-of-range indices are refused");

    /* The prompt / confirm seams: the dialog text lands in the list. */
    tasks_prompt_fn = fake_prompt;
    tasks_confirm_fn = fake_confirm;
    mock_dlg_prompt_ret = 1;
    snprintf(mock_dlg_prompt_text, sizeof mock_dlg_prompt_text, "from the dialog");
    tasks_list.selected = 0;
    tasks_handle_event(&tasks_state, &tasks_list, &EV_CHAR('a'));
    CHECK(tasks_state.count == 3 &&
          strcmp(tasks_state.items[2].text, "from the dialog") == 0 &&
          tasks_list.selected == 2,
          "tasks: 'a' appends the text typed in the dialog and selects it");
    mock_dlg_confirm_ret = 0;
    tasks_handle_event(&tasks_state, &tasks_list, &EV_CHAR('d'));
    CHECK(tasks_state.count == 3, "tasks: a declined delete keeps the item");
    mock_dlg_confirm_ret = 1;
    tasks_handle_event(&tasks_state, &tasks_list, &EV_CHAR('d'));
    CHECK(tasks_state.count == 2 && strcmp(tasks_state.status, "Deleted.") == 0,
          "tasks: a confirmed delete removes the selected item");
    tasks_prompt_fn = dialog_prompt;
    tasks_confirm_fn = dialog_confirm;

    /* Persistence: save, wipe, reload.  The file starts empty so this is a
     * real round trip (compat's open() only grows the mock file, so a
     * shorter rewrite would leave a stale tail - the same caveat the app's
     * own FAT-16 notes document). */
    file_write(TASKS_FIXTURE, "");
    reset_tasks();
    tasks_add(&tasks_state, "first");
    tasks_add(&tasks_state, "second");
    tasks_toggle(&tasks_state, 1);
    tasks_list_sync(&tasks_state, &tasks_list);
    CHECK(tasks_save(&tasks_state) == 0, "tasks: saves the list to TODO.TXT");
    saved_count = tasks_state.count;
    snprintf(saved_first, sizeof saved_first, "%s", tasks_state.items[0].text);
    reset_tasks();
    CHECK(tasks_state.count == saved_count &&
          strcmp(tasks_state.items[0].text, saved_first) == 0 &&
          tasks_state.items[1].done == 1,
          "tasks: reloading TODO.TXT round-trips the saved list");

    /* Arrows and mouse drive the selection. */
    tasks_list.selected = 0;
    tasks_list.top = 0;
    tasks_handle_event(&tasks_state, &tasks_list, &EV_TYPE(GUI_EV_DOWN));
    CHECK(tasks_list.selected == 1, "tasks: the down arrow moves the selection");
    tasks_handle_event(&tasks_state, &tasks_list,
                       &EV_MOUSE(0, tasks_first_item_row(&tasks_state), 1));
    CHECK(tasks_list.selected == 0, "tasks: a click on the first row selects it");
    CHECK(tasks_handle_event(&tasks_state, &tasks_list, &EV_CHAR('q')) == 1,
          "tasks: 'q' asks the app to quit");
    write_tasks_fixture();
}

/* ---- 6. find --------------------------------------------------------- */

static void suite_find_interaction(void) {
    static char out[8192];

    section("find: typing a query, searching the mock tree, results");
    reset_find();
    CHECK(find_handle_event(&FIND, &EV_CHAR('T')) == 1 &&
          FIND.query_len == 1 && strcmp(FIND.query, "T") == 0,
          "find: typing appends to the query");
    find_handle_event(&FIND, &EV_CHAR('E'));
    find_handle_event(&FIND, &EV_CHAR('\b'));
    CHECK(FIND.query_len == 1, "find: backspace edits the query");
    find_handle_event(&FIND, &EV_CHAR('E'));
    find_render(out, (int)sizeof out, &FIND);
    CHECK(has(out, "Search: TE_"), "find: the query box shows the query and the cursor");

    CHECK(find_handle_event(&FIND, &EV_CHAR('\n')) == 1 && FIND.searched == 1,
          "find: Enter runs the search");
    CHECK(FIND.nresults == 2 && FIND.scanned == 7 && !FIND.trunc_scan,
          "find: the mock listing yields 2 matches out of 7 entries");
    find_render(out, (int)sizeof out, &FIND);
    CHECK(has(out, "Results: 2  (scanned 7 entries)"),
          "find: the status line reports the count");
    CHECK(has(out, "TEST.TXT") && has(out, "NOTES.TXT"),
          "find: both matching names are listed");
    CHECK(has(out, "> "), "find: the first result is marked as selected");

    /* Case-insensitive matching, then a zero-hit query. */
    reset_find();
    for (const char *q = "te"; *q; q++) find_handle_event(&FIND, &EV_CHAR(*q));
    find_handle_event(&FIND, &EV_CHAR('\n'));
    CHECK(FIND.nresults == 2, "find: matching is case-insensitive");
    reset_find();
    for (const char *q = "ZZZ"; *q; q++) find_handle_event(&FIND, &EV_CHAR(*q));
    find_handle_event(&FIND, &EV_CHAR('\n'));
    find_render(out, (int)sizeof out, &FIND);
    CHECK(FIND.nresults == 0 && has(out, "Results: 0"),
          "find: a query with no match reports zero results");

    /* An empty query never walks the tree. */
    reset_find();
    find_handle_event(&FIND, &EV_CHAR('\n'));
    find_render(out, (int)sizeof out, &FIND);
    CHECK(FIND.searched == 0 && FIND.scanned == 0 && has(out, "Results: none yet"),
          "find: an empty query does not walk the tree");

    /* Selection with arrows and the mouse, then clear. */
    reset_find();
    for (const char *q = "TE"; *q; q++) find_handle_event(&FIND, &EV_CHAR(*q));
    find_handle_event(&FIND, &EV_CHAR('\n'));
    find_handle_event(&FIND, &EV_TYPE(GUI_EV_DOWN));
    CHECK(FIND.list.selected == 1, "find: the down arrow moves the result selection");
    find_handle_event(&FIND, &EV_TYPE(GUI_EV_UP));
    CHECK(FIND.list.selected == 0, "find: the up arrow moves it back");
    find_handle_event(&FIND, &EV_MOUSE(0, FIND_RESULTS_ROW + 1, 1));
    CHECK(FIND.list.selected == 1, "find: a click selects the clicked result row");
    CHECK(find_handle_event(&FIND, &EV_CHAR('c')) == 1 && FIND.query_len == 0 &&
          FIND.searched == 0,
          "find: 'c' clears the query and the results");
    CHECK(find_handle_event(&FIND, &EV_MENU(0, 1)) == 1,
          "find: the Find>Clear menu item clears as well");
    CHECK(find_handle_event(&FIND, &EV_MENU(9, 0)) == 0,
          "find: an unknown menu index is ignored");
}

/* ---- 7. diff --------------------------------------------------------- */

static int write_long_fixture(const char *path, int lines, int change_line) {
    static char buf[4096];
    int len = 0;
    for (int i = 0; i < lines && len < (int)sizeof buf - 16; i++) {
        if (i == change_line)
            len += snprintf(buf + len, sizeof buf - (size_t)len, "changed %02d\n", i);
        else
            len += snprintf(buf + len, sizeof buf - (size_t)len, "line %02d\n", i);
    }
    file_write_n(path, buf, len);
    return len;
}

static int diff_render_count(char *out, int cap) {
    diff_render_to(out, cap);
    return (int)strlen(out);
}

static void suite_diff_interaction(void) {
    static char out[8192];

    section("diff: comparing two one-line files, markers, navigation");
    file_write(DIFF_A, "one\n");
    file_write(DIFF_B, "uno\n");
    reset_diff();
    CHECK(diff_load_slot(0, DIFF_A) == 1 && diff_load_slot(1, DIFF_B) == 1,
          "diff: loads both slots");
    diff_compare();
    CHECK(diff_valid == 1 && diff_row_count == 2 && diff_aonly == 1 &&
          diff_bonly == 1,
          "diff: two one-line files yield one - and one + row");
    diff_render_count(out, (int)sizeof out);
    CHECK(has(out, " - one"), "diff: the A-only line renders with the ' - ' marker");
    CHECK(has(out, " + uno"), "diff: the B-only line renders with the ' + ' marker");
    CHECK(has(out, "A: " DIFF_A " (1 lines)"),
          "diff: header slot A carries the name and line count");
    CHECK(has(out, "+1 -1"), "diff: the status line reports +added -removed");

    /* Identical files: no change rows at all. */
    file_write(DIFF_B2, "one\n");
    diff_load_slot(1, DIFF_B2);
    diff_compare();
    diff_render_count(out, (int)sizeof out);
    CHECK(has(out, "+0 -0") && has(out, "   one") && !has(out, " - one"),
          "diff: identical files produce one SAME row and a +0 -0 status");

    /* A long pair: 'n' jumps to the first change, then scrolling clamps. */
    write_long_fixture(DIFF_LONG_A, 30, -1);
    write_long_fixture(DIFF_LONG_B, 30, 25);
    CHECK(diff_load_slot(0, DIFF_LONG_A) == 1 &&
          diff_load_slot(1, DIFF_LONG_B) == 1,
          "diff: loads a 30-line pair");
    diff_compare();
    CHECK(diff_row_count == 31, "diff: 30 lines with one change produce 31 rows");
    CHECK(diff_handle_event(&EV_CHAR('n')) == 1 && view_top == 25,
          "diff: 'n' scrolls to the first changed row");
    CHECK(diff_handle_event(&EV_TYPE(GUI_EV_UP)) == 1 && view_top == 24,
          "diff: the up arrow scrolls one row back");
    for (int i = 0; i < 40; i++) diff_handle_event(&EV_TYPE(GUI_EV_UP));
    CHECK(view_top == 0, "diff: scrolling up stops at the first row");
    diff_handle_event(&EV_TYPE(GUI_EV_DOWN));
    CHECK(view_top == 1, "diff: the down arrow scrolls one row forward");
    for (int i = 0; i < 40; i++) diff_handle_event(&EV_TYPE(GUI_EV_DOWN));
    CHECK(view_top == 13, "diff: scrolling down clamps to the last full page");

    /* The Diff>Compare menu item re-runs the comparison. */
    diff_invalidate();
    CHECK(diff_valid == 0, "diff: invalidating drops the comparison");
    CHECK(diff_handle_event(&EV_MENU(1, 0)) == 1 && diff_valid == 1,
          "diff: the Diff>Compare menu item re-runs the comparison");

    /* The built-in no-memory fallback. */
    diff_force_fallback = 1;
    diff_compare();
    diff_render_count(out, (int)sizeof out);
    CHECK(has(out, "(fallback: no memory)"),
          "diff: the malloc-failure fallback is announced on the status line");
    diff_force_fallback = 0;
    diff_compare();
    diff_render_count(out, (int)sizeof out);
    CHECK(!has(out, "(fallback: no memory)"), "diff: the fallback marker clears");
    CHECK(diff_handle_event(&EV_MENU(9, 9)) == 0,
          "diff: an unknown menu index is ignored");
}

/* ---- 8. notes -------------------------------------------------------- */

static int notes_render_capture(char *out, int cap) {
    return capture_into(out, cap, notes_render);
}

static void suite_notes_interaction(void) {
    static char out[8192];
    static char ser[4096];

    section("notes: typing text into a note, editing, deleting, round trip");
    write_notes_fixture();
    notes_reset();
    CHECK(notes_load() == 1 && notes_count == 2 && notes_warn == 0,
          "notes: loads the two-note NOTES.TXT fixture");
    notes_render_capture(out, (int)sizeof out);
    CHECK(has(out, "2 notes") && has(out, "First") && has(out, "Second"),
          "notes: the list shows the loaded titles");
    CHECK(has(out, "body one"),
          "notes: the preview area shows the selected body");

    /* Typing a new note: the text lands in the list and the preview. */
    CHECK(notes_add("Typed", "hello world") == 1 && notes_count == 3,
          "notes: adding a note stores it and selects it");
    notes_render_capture(out, (int)sizeof out);
    CHECK(has(out, "Typed") && has(out, "hello world"),
          "notes: the typed title and body are rendered");
    CHECK(notes_edit_body("edited body") == 1 &&
          strcmp(notes_selected_body(), "edited body") == 0,
          "notes: editing replaces the selected body");
    notes_render_capture(out, (int)sizeof out);
    CHECK(has(out, "edited body") && !has(out, "hello world"),
          "notes: the edited body replaces the old preview text");
    CHECK(notes_delete() == 1 && notes_count == 2,
          "notes: deleting removes the selected note");
    notes_render_capture(out, (int)sizeof out);
    CHECK(!has(out, "Typed"), "notes: the deleted note disappears from the list");

    /* Serialise / parse round trip. */
    {
        int len = notes_serialize(ser, (int)sizeof ser);
        CHECK(len > 0 && has(ser, "### First\n") && has(ser, "\nbody one\n"),
              "notes: serialisation writes the '### title' two-line format");
        notes_reset();
        notes_parse(ser, len);
        CHECK(notes_count == 2 && strcmp(notes[0].title, "First") == 0 &&
              strcmp(notes[1].body, "body two") == 0,
              "notes: parsing the serialised text restores the notes");
    }

    /* The 20-note cap. */
    notes_reset();
    {
        int added = 0, rejected = 0;
        for (int i = 0; i < 25; i++) {
            char title[32];
            snprintf(title, sizeof title, "N%02d", i);
            if (notes_add(title, "b")) added++;
            else rejected++;
        }
        CHECK(added == 20 && rejected == 5 && notes_count == 20,
              "notes: the 20-note cap rejects the 21st note");
    }

    /* Save / reload against the real file. */
    notes_reset();
    CHECK(notes_add("Saved", "saved body") == 1, "notes: adds a note to save");
    CHECK(notes_save() == 1, "notes: saves to NOTES.TXT");
    CHECK(file_size_of(NOTES_FIXTURE) > 0,
          "notes: NOTES.TXT exists after the save");
    notes_reset();
    CHECK(notes_load() == 1 && notes_count == 1 &&
          strcmp(notes[0].title, "Saved") == 0,
          "notes: reloading NOTES.TXT round-trips the saved note");

    /* Selection, arrows, mouse, reload key. */
    write_notes_fixture();
    notes_reset();
    notes_load();
    notes_handle_event(&EV_TYPE(GUI_EV_DOWN));
    CHECK(notes_list.selected == 1, "notes: the down arrow moves the selection");
    notes_handle_event(&EV_MOUSE(0, notes_list_first_row(), 1));
    CHECK(notes_list.selected == 0, "notes: a click selects the clicked row");
    CHECK(notes_click(notes_list_first_row()) == 0 && notes_click(999) == -1,
          "notes: a click outside the list is ignored");
    CHECK(notes_handle_event(&EV_CHAR('r')) == 1, "notes: 'r' triggers a reload");
    CHECK(notes_handle_event(&EV_CHAR('z')) == 0, "notes: an unknown key is ignored");
    CHECK(notes_handle_event(&EV_MENU(9, 0)) == 0,
          "notes: an unknown menu index is ignored");
    CHECK(notes_list_first_row() == 2,
          "notes: the first list row is stable without the warning line");
    notes_warn = 1;
    CHECK(notes_list_first_row() == 3,
          "notes: the warning line shifts the list down one row");
    notes_warn = 0;
}

/* ---- 9. unit --------------------------------------------------------- */

static void suite_unit_interaction(void) {
    static char out[4096];

    section("unit: cycling units, conversion results, value editing");
    reset_unit();
    unit_render_to(out, (int)sizeof out);
    CHECK(has(out, "Category: Length"), "unit: starts in the Length category");
    CHECK(has(out, "From: 1 [m]      To: [ft]"), "unit: the default pair is m -> ft");
    CHECK(has(out, "Result: 3.28 ft"), "unit: 1 m converts to 3.28 ft");
    CHECK(has(out, "e=edit value"), "unit: the legend is rendered");

    /* Cycling the FROM unit changes the result. */
    CHECK(unit_handle_event(&EV_TYPE(GUI_EV_UP)) == 1,
          "unit: the up arrow cycles the FROM unit");
    unit_render_to(out, (int)sizeof out);
    CHECK(!has(out, "Result: 3.28 ft"), "unit: cycling the FROM unit changes the result");
    CHECK(has(out, "From: 1 [km]"), "unit: the FROM field shows the new unit");

    /* Cycling the category resets the pair. */
    CHECK(unit_handle_event(&EV_TYPE(GUI_EV_RIGHT)) == 1 &&
          strcmp(g_cats[g_unit.cat].name, "Mass") == 0,
          "unit: the right arrow cycles the category");
    unit_render_to(out, (int)sizeof out);
    CHECK(has(out, "Category: Mass") && has(out, "From: 1 [mg]"),
          "unit: the Mass category renders its own units");

    /* Swap exchanges FROM and TO. */
    CHECK(unit_handle_event(&EV_CHAR('s')) == 1 && g_unit.from == 1 && g_unit.to == 0,
          "unit: 's' swaps the two units");
    unit_render_to(out, (int)sizeof out);
    CHECK(has(out, "To: [mg]"), "unit: the swap shows up on screen");

    /* Editing the value through the dialog seam. */
    CHECK(unit_apply_value_text("42") == 1 && g_unit.value == 42 &&
          g_unit.msg[0] == '\0',
          "unit: a valid dialog value becomes the conversion input");
    unit_render_to(out, (int)sizeof out);
    CHECK(has(out, "From: 42 ["), "unit: the edited value is rendered");
    CHECK(unit_apply_value_text("abc") == 0 && has(g_unit.msg, "Invalid value"),
          "unit: junk from the dialog leaves an error message");
    unit_render_to(out, (int)sizeof out);
    CHECK(has(out, "! Invalid value"),
          "unit: the error message is rendered on the status line");
    CHECK(unit_apply_value_text("99999999999999999999") == 1 &&
          has(g_unit.msg, "clamped"),
          "unit: an overflowing value is clamped with a message");

    /* Temperature: 100 C -> 212 F. */
    unit_reset();
    g_unit.cat = 4;          /* Temperature */
    g_unit.from = 0;         /* C */
    g_unit.to = 1;           /* F */
    unit_apply_value_text("100");
    unit_render_to(out, (int)sizeof out);
    CHECK(has(out, "Category: Temperature") && has(out, "Result: 212 F"),
          "unit: 100 C converts to 212 F");

    /* Menus and mouse. */
    CHECK(unit_handle_event(&EV_MENU(0, 2)) == 1, "unit: the Swap menu item works");
    CHECK(unit_handle_event(&EV_MOUSE(0, 1, 1)) == 1,
          "unit: a click on the category row cycles it");
    CHECK(unit_handle_event(&EV_MOUSE(0, 2, 1)) == 1,
          "unit: a click on the From row cycles the unit");
    CHECK(unit_handle_event(&EV_MOUSE(0, 9, 1)) == 0,
          "unit: a click on a blank row is ignored");
    CHECK(unit_handle_event(&EV_MENU(9, 9)) == 0,
          "unit: an unknown menu index is ignored");
}

/* ====================================================================== */
/* layer B extras: the real dialogs, driven with scripted stdin           */
/* ====================================================================== */

static void suite_notes_dialogs(void) {
    static struct run_result r;
    static char buf[256];
    int n;
    FILE *f;

    section("notes: end-to-end dialogs (real dialog.c, stepped stdin)");
    file_write(NOTES_FIXTURE, "");
    notes_reset();
    notes_load();

    /*
     * 'n' -> title prompt -> body prompt, then 'e' -> edit body prompt.
     * The bytes are fed one at a time (step_ms = 20): the app's event loop
     * buffered-reads stdin through gui_read_event(), so a bulk script would
     * be swallowed by the toolkit before the modal dialog could see it.
     */
    run_app(&APPS[APP_NOTES], "nSuite note\ntyped body\needited body\n", &r, 20);
    CHECK(has(r.out, "New Note"),
          "notes(e2e): 'n' opens the real New Note dialog on the wire");
    CHECK(has(r.out, "Suite note"), "notes(e2e): the typed title appears on the wire");
    CHECK(has(r.out, "edited body"), "notes(e2e): the edited body is rendered");
    CHECK(file_size_of(NOTES_FIXTURE) > 0,
          "notes(e2e): the typed note was written to NOTES.TXT");
    f = fopen(NOTES_FIXTURE, "rb");
    n = f ? (int)fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    if (n < 0) n = 0;
    buf[n] = '\0';
    CHECK(has(buf, "### Suite note") && has(buf, "edited body"),
          "notes(e2e): NOTES.TXT holds the '### title' and the edited body");

    /* 'd' -> the real confirm dialog -> the note is gone. */
    notes_reset();
    notes_load();
    CHECK(notes_count == 1, "notes(e2e): the parent reloads the saved note");
    run_app(&APPS[APP_NOTES], "dy", &r, 20);
    CHECK(has(r.out, "Delete this note?"),
          "notes(e2e): delete asks for confirmation in the real dialog");
    f = fopen(NOTES_FIXTURE, "rb");
    n = f ? (int)fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    if (n < 0) n = 0;
    buf[n] = '\0';
    CHECK(n > 0 && strchr(buf, '#') == 0,
          "notes(e2e): the confirmed delete leaves no note data behind");
    notes_reset();
    notes_load();
    CHECK(notes_count == 0, "notes(e2e): NOTES.TXT parses back to zero notes");
}

/* ====================================================================== */
/* main                                                                   */
/* ====================================================================== */

int main(void) {
    printf("=== Apps Suite Host Tests (10 desktop apps) ===\n");
    printf("cross-app invariants + wire protocol + per-app interactions\n");

    /* Deterministic fixtures first: the forked runs inherit them. */
    write_tasks_fixture();
    write_notes_fixture();

    section("layer A: cross-cutting invariants");
    for (int i = 0; i < NAPPS; i++) suite_invariants(&APPS[i]);

    section("layer B: end-to-end wire protocol");
    for (int i = 0; i < NAPPS; i++) suite_wire(&APPS[i]);

    suite_files_interaction();
    suite_calc_interaction();
    suite_clock_interaction();
    suite_sysmon_interaction();
    suite_hex_interaction();
    suite_tasks_interaction();
    suite_find_interaction();
    suite_diff_interaction();
    suite_notes_interaction();
    suite_unit_interaction();
    suite_notes_dialogs();

    section("suite self-checks");
    CHECK(NAPPS == 10, "the suite covers all ten desktop applications");
    {
        int distinct = 1;
        for (int i = 0; i < NAPPS && distinct; i++)
            for (int j = i + 1; j < NAPPS; j++)
                if (strcmp(APPS[i].banner, APPS[j].banner) == 0) distinct = 0;
        CHECK(distinct, "every app has its own identity banner in the table");
    }

    printf("\n=== Test Results ===\n");
    printf("Checks run:    %d\n", checks_run);
    printf("Checks failed: %d\n", checks_failed);
    if (checks_failed == 0) {
        printf("ALL APPS SUITE TESTS PASSED\n");
        printf("TEST EXIT: 0\n");
        return 0;
    }
    printf("APPS SUITE TESTS FAILED\n");
    printf("TEST EXIT: 1\n");
    return 1;
}
