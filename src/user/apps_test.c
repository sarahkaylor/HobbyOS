/*
 * apps_test.c - In-OS desktop application harness (APPS_T.BIN).
 *
 * Runs in place of the desktop: it links src/user/desktop.c as
 * desktop_main() (compiled with -DDESKTOP_TEST_WRAPPER, exactly like
 * src/user/editor_test.c) and provides the three symbols the desktop would
 * otherwise take from the kernel/system:
 *
 *   read_dir()   - serves a fixed, deterministic 10-entry start-menu
 *                  listing (indices 0..9 = the 10 GUI app binaries). The
 *                  start-menu navigation below is therefore exact and
 *                  independent of the on-disk directory order.
 *   get_events() - the injected-event queue (mouse + keyboard), the same
 *                  injection path editor_test.c uses.
 *   flush_fb()   - calls the real SYS_FLUSH_FB and then runs the test
 *                  state machine. The desktop calls it once per finished
 *                  frame, so it is both where the next synthetic event is
 *                  injected and where the previous one's effect is checked:
 *                  by the time flush_fb() runs, the desktop has already
 *                  processed the pending events and re-rendered.
 *
 * For each of the 10 GUI applications (FILES, CALC, CLOCK, SYSMON, HEX,
 * TASKS, FIND, DIFF, NOTES, UNIT) the harness drives the REAL window
 * manager through a full lifecycle:
 *
 *   1. Click "Apps" in the taskbar, press Down <menu index> times, press
 *      Enter: the start menu launches the app binary as a real process
 *      (spawn2) into a real tiled window.
 *   2. Assert POSITIVELY that the window was created AND drew:
 *        - num_windows == 1                          (WM created the window)
 *        - the window title equals the app's own title: the app's startup
 *          escape sequence (ESC ] T <title> ~, sent by gui_set_title())
 *          was parsed by the WM and overrode the WM's default title.
 *        - app-specific substrings are present in the captured window text:
 *          the bytes the app PRINTED through its stdout pipe, which the WM
 *          parsed (clear-screen '\f', escapes) into windows[0].text - the
 *          same buffer wm_draw_windows() renders from.
 *        - the REAL framebuffer (graphics_get_pixel read-back) contains the
 *          focused title-bar fill color, the focused menu-bar fill color
 *          and white 8x8-font glyph pixels inside the content area. This
 *          proves the focused window was actually composited: a counter or
 *          a text buffer alone would not catch a broken renderer.
 *   3. Optionally inject a short scripted key sequence and assert the app's
 *      own functional response (e.g. CALC "2+3=" -> "> 5"). Each check is
 *      documented next to the app table entry.
 *   4. Close the window - using the app's OWN quit key where the source
 *      defines one ('q' in files/hex/tasks), else the desktop's F4
 *      close-the-focused-window key - and assert num_windows returns to 0
 *      before the next app starts.
 *
 * Verdict: 10 PASS lines + "APPS TEST PASSED" + exit(0); any timeout or
 * failed check prints "APPS_T] FAIL ..." and "APPS TEST FAILED (<reason>)"
 * and exits 1. run_apps_test.py watches the serial console for the verdict;
 * when the harness exits, the kernel scheduler halts QEMU (no processes
 * left), so the driver usually sees QEMU terminate by itself.
 *
 * Responsiveness measurement: before FILES' drag stages the harness settles
 * the listing, sends exactly one Down arrow and counts the desktop frames
 * that carried a visible change until the response has settled (the
 * selected line moved and two frames looked unchanged), printing
 *   [APPS_T] FILES down-arrow: N painting frames (first at frame F),
 *            M desktop frames, T ms to settle, P fb pixels painted
 * The pixel count comes from the APPS_T build of the graphics library
 * (-DPAINT_STATS, see graphics.c) and is the regression number for the
 * desktop's damage-driven repaint: one line move must stay in the low tens
 * of thousands of pixels, not millions (it was ~70M before the compositor
 * repaired windows at line granularity).
 *
 * Only this binary is mocked: the apps are real processes loaded from the
 * FAT volume and use the real read_dir()/syscalls.
 */

#include "libc.h"
#include "graphics/graphics.h"
#include "graphics/window.h"

extern int desktop_main(void);
extern struct window windows[];   /* window.c */
extern int num_windows;           /* window.c */

/* ====================================================================== */
/* Real syscalls (same shape as editor_test.c)                            */
/* ====================================================================== */

static long syscall(long num, long a0, long a1, long a2, long a3) {
#ifdef __x86_64__
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                   : "memory");
  return x0;
#endif
}

#define SYS_FLUSH_FB 10

/* ====================================================================== */
/* Mock input: the injected-event queue the desktop reads via get_events  */
/* ====================================================================== */

#define MAX_MOCK_EVENTS 256
static struct virtio_input_event mock_events[MAX_MOCK_EVENTS];
static int mock_events_head = 0;
static int mock_events_tail = 0;

void inject_mock_event(uint16_t type, uint16_t code, uint32_t value) {
  int next = (mock_events_head + 1) % MAX_MOCK_EVENTS;
  if (next != mock_events_tail) {
    mock_events[mock_events_head].type = type;
    mock_events[mock_events_head].code = code;
    mock_events[mock_events_head].value = value;
    mock_events_head = next;
  }
}

int get_events(void *buf, int max_events) {
  struct virtio_input_event *events = (struct virtio_input_event *)buf;
  int count = 0;
  while (mock_events_tail != mock_events_head && count < max_events) {
    events[count++] = mock_events[mock_events_tail];
    mock_events_tail = (mock_events_tail + 1) % MAX_MOCK_EVENTS;
  }
  return count;
}

/* ====================================================================== */
/* Mock read_dir: the fixed start-menu listing (exact indices 0..9)       */
/* ====================================================================== */

#define APP_COUNT 10
/* Two extra entries exist only so the FILES app has a real drag & drop
 * target and a real text file to open: a directory and a .TXT.  They are
 * not launchable, and non-pinned menu items sort after the apps, so start
 * menu navigation for the ten apps is unchanged. */
#define MENU_EXTRA 2
static const char *const MENU_LIST[APP_COUNT + MENU_EXTRA] = {
  "FILES.BIN",  /* 0 */
  "CALC.BIN",   /* 1 */
  "CLOCK.BIN",  /* 2 */
  "SYSMON.BIN", /* 3 */
  "HEX.BIN",    /* 4 */
  "TASKS.BIN",  /* 5 */
  "FIND.BIN",   /* 6 */
  "DIFF.BIN",   /* 7 */
  "NOTES.BIN",  /* 8 */
  "UNIT.BIN",   /* 9 */
  "TESTDIR",    /* 10: directory on the real disk (drag & drop target) */
  "E2E.TXT",    /* 11: text file on the real disk (open-in-editor target) */
};

int read_dir(const char *path, int index, struct sys_dirent *ent) {
  (void)path;
  if (index < 0 || index >= APP_COUNT + MENU_EXTRA)
    return -1;
  const char *name = MENU_LIST[index];
  int i = 0;
  while (name[i] && i < (int)sizeof(ent->name) - 1) {
    ent->name[i] = name[i];
    i++;
  }
  ent->name[i] = '\0';
  ent->attr = (index == 10) ? 0x10 : 0;             /* TESTDIR is a dir */
  ent->size = (index == 11) ? 44 : 0;               /* E2E.TXT is 44 bytes */
  return 0;
}

/* ====================================================================== */
/* Key injection helpers                                                  */
/* ====================================================================== */

/* US keymap reverse lookup (same table editor_test.c uses). Only the keys
 * this harness scripts are supplied; everything else maps to 0. */
static int char_to_keycode(char c) {
  char keymap[128] = {0,    27,  '1', '2',  '3',  '4',  '5', '6', '7',  '8',
                      '9',  '0', '-', '=',  '\b', '\t', 'q', 'w', 'e',  'r',
                      't',  'y', 'u', 'i',  'o',  'p',  '[', ']', '\n', 0,
                      'a',  's', 'd', 'f',  'g',  'h',  'j', 'k', 'l',  ';',
                      '\'', '`', 0,   '\\', 'z',  'x',  'c', 'v', 'b',  'n',
                      'm',  ',', '.', '/',  0,    '*',  0,   ' ', 0};
  for (int i = 0; i < 128; i++) {
    if (keymap[i] == c) return i;
  }
  return 0;
}

#define KEY_ENTER  28
#define KEY_F4     62
#define KEY_UP     103
#define KEY_DOWN   108
#define KEY_LSHIFT 42
#define KEY_EQUAL  13

static void inject_key(int code) {
  inject_mock_event(EV_KEY, (uint16_t)code, 1); /* press */
}

static void inject_char(char c) {
  int code = char_to_keycode(c);
  if (code > 0) inject_key(code);
}

/* '+' = LeftShift + '=' (desktop maps '=' through shift_keymap). */
static void inject_plus(void) {
  inject_mock_event(EV_KEY, KEY_LSHIFT, 1);
  inject_key(KEY_EQUAL);
  inject_mock_event(EV_KEY, KEY_LSHIFT, 0);
}

/* Script DSL for the app table's `keys` field:
 *   printable char -> that key
 *   '\n'           -> Enter
 *   '\x01'         -> Down arrow      '\x02' -> Up arrow
 *   '\x03'         -> F4 (WM close)   '\x04' -> '+'  */
static void inject_keys(const char *keys) {
  for (int i = 0; keys[i]; i++) {
    switch (keys[i]) {
    case '\x01': inject_key(KEY_DOWN);  break;
    case '\x02': inject_key(KEY_UP);    break;
    case '\x03': inject_key(KEY_F4);    break;
    case '\x04': inject_plus();         break;
    case '\n':   inject_key(KEY_ENTER); break;
    default:     inject_char(keys[i]);  break;
    }
  }
}

/* Taskbar "Apps" button geometry (see desktop.c): x in [6,70), y >= 742. */
#define APPS_BTN_MX 20
#define APPS_BTN_MY 750
static void inject_mouse(int x, int y) {
  inject_mock_event(EV_ABS, ABS_X, (uint32_t)((x * 0x7FFF) / SCREEN_WIDTH));
  inject_mock_event(EV_ABS, ABS_Y, (uint32_t)((y * 0x7FFF) / SCREEN_HEIGHT));
}
static void inject_left_click(void) { inject_key(0x110); }

/* Full press/motion/release, what the WM forwards as ESC [ P / G / R.
 * (inject_key sends only the press; drag & drop needs the release too.) */
static void inject_left_press(int x, int y) {
  inject_mouse(x, y);
  inject_mock_event(EV_KEY, 0x110, 1);
}
static void inject_left_release(void) {
  inject_mock_event(EV_KEY, 0x110, 0);
}

/* Absolute pixel at the centre of content cell (col,row) of window `w`.
 * Cells start at (w->x + 10, w->y + 44), 8px wide, 10px tall (see the WM's
 * wm_mouse_cell). */
static int cell_x(const struct window *w, int col) { return w->x + 10 + col * 8 + 4; }
static int cell_y(const struct window *w, int row) { return w->y + 44 + row * 10 + 5; }
/* FILES lays its first listing row on content row 3 (FS_ROW_ENTRY0). */
#define FILES_ROW0 3

/* Content row of the first window-text line containing `needle` (-1 when
 * absent). The WM stores one screen line per content row and FILES renders
 * from the top, so line index == content row. The listing a *child* process
 * sees is the real FAT root (the desktop-side read_dir mock only affects
 * the WM's own menu load), so rows must be located by name, not assumed. */
static int find_row_of(const char *text, const char *needle) {
  int row = 0;
  for (int i = 0; text[i]; i++) {
    if (text[i] == '\n') {
      row++;
      continue;
    }
    if (i == 0 || text[i - 1] == '\n') {
      int line_end = i;
      while (text[line_end] && text[line_end] != '\n') line_end++;
      for (int s = i; s < line_end; s++) {
        int j = 0;
        while (needle[j] && (s + j) < line_end && text[s + j] == needle[j]) j++;
        if (!needle[j]) return row;
      }
    }
  }
  return -1;
}

/* Keep the desktop's frame loop ticking while a condition is awaited: an
 * absolute mouse move to the (inert) Apps button sets needs_redraw, so the
 * next flush_fb() comes almost immediately instead of waiting for the 1 Hz
 * taskbar-clock redraw. It cannot disturb the WM state (mouse moves alone
 * never open/close menus or launch anything). */
static void keepalive_tick(void) {
  inject_mouse(APPS_BTN_MX, APPS_BTN_MY);
}

/* ====================================================================== */
/* Tiny string helpers (this libc has almost none)                        */
/* ====================================================================== */

static int contains(const char *hay, const char *needle) {
  if (!hay || !needle) return 0;
  if (!needle[0]) return 1;
  for (int i = 0; hay[i]; i++) {
    int j = 0;
    while (needle[j] && hay[i + j] == needle[j]) j++;
    if (!needle[j]) return 1;
  }
  return 0;
}

static int same_str(const char *a, const char *b) {
  int i = 0;
  while (a[i] && a[i] == b[i]) i++;
  return a[i] == b[i];
}

/* Copy at most cap-1 bytes (NUL-terminated). */
static void copy_str(char *dst, int cap, const char *src) {
  int i = 0;
  if (cap <= 0) return;
  while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
  dst[i] = '\0';
}

/* Does `t` contain a "HH:MM:SS"-shaped run (two digits, colon, two digits,
 * colon, two digits)? */
static int has_hms(const char *t) {
  for (int i = 0; t[i]; i++) {
    if (t[i] >= '0' && t[i] <= '9' &&
        t[i + 1] >= '0' && t[i + 1] <= '9' && t[i + 2] == ':' &&
        t[i + 3] >= '0' && t[i + 3] <= '9' &&
        t[i + 4] >= '0' && t[i + 4] <= '9' && t[i + 5] == ':' &&
        t[i + 6] >= '0' && t[i + 6] <= '9' &&
        t[i + 7] >= '0' && t[i + 7] <= '9') {
      return 1;
    }
  }
  return 0;
}

/* Copy the first "> "-prefixed line of the window text (the selected row in
 * FILES' listing) into out. Returns 1 when one was found. */
static int grab_selected_line(const char *t, char *out, int cap) {
  int line_start = 1;
  for (int i = 0; t[i]; i++) {
    if (line_start && t[i] == '>' && t[i + 1] == ' ') {
      int j = 0;
      while (t[i + j] && t[i + j] != '\n' && j < cap - 1) {
        out[j] = t[i + j];
        j++;
      }
      out[j] = '\0';
      return 1;
    }
    line_start = (t[i] == '\n');
  }
  return 0;
}

/* ====================================================================== */
/* Per-app functional checks                                              */
/* ====================================================================== */

/* All checks get (text captured just before the scripted keys, text now). */

static int files_check(const char *before, const char *after) {
  /* FILES: one Down arrow must move the listing selection. Proves the app
   * parsed the ESC [ B sequence the WM forwarded and re-rendered the
   * listing: the "> " selected row differs from the pre-key capture while
   * the screen is still a FILES screen. */
  char a[64], b[64];
  if (!grab_selected_line(before, a, (int)sizeof a)) return 0;
  if (!grab_selected_line(after, b, (int)sizeof b)) return 0;
  if (same_str(a, b)) return 0;
  return contains(after, "Enter=open");
}

static int calc_check(const char *before, const char *after) {
  /* CALC: the scripted keys were "2", shift+"=" ("+"), "3", "=" which is
   * calc.c's actual input scheme; the evaluator must show "> 5". Proves
   * key input, expression evaluation and the result line rendering. */
  (void)before;
  return contains(after, "> 5");
}

static int clock_check(const char *before, const char *after) {
  /* CLOCK: a live wall clock (RTC or uptime fallback) must render an
   * HH:MM:SS time string. Proves the app's periodic refresh produced time
   * text, not just a static header. */
  (void)before;
  return has_hms(after);
}

static int tasks_check(const char *before, const char *after) {
  /* TASKS: 'd' (delete) on the empty list must report the documented
   * status. Proves the key was handled and the status line re-rendered.
   * (Note: open(, 0) creates files on demand in this kernel, so TODO.TXT is
   * created empty by the app's startup load - there is no warning line.) */
  (void)before;
  return contains(after, "Nothing to delete.");
}

static int find_check(const char *before, const char *after) {
  /* FIND: the scripted keys typed "unit" and pressed Enter. The query box
   * must show the typed text and the search must have found the UNIT.BIN
   * file in the real FAT root ("Results: 1"). Proves character input, the
   * backspace/edit path's rendering and a real directory-tree search. */
  (void)before;
  if (!contains(after, "Search: unit_")) return 0;
  if (!contains(after, "Results: 1")) return 0;
  return contains(after, "UNIT.BIN");
}

static int diff_check(const char *before, const char *after) {
  /* DIFF: 'c' (Compare) with no files loaded must replace the "Press
   * Compare to diff" hint with the +0/-0 result summary. Proves the
   * compare command ran and re-rendered the status line. */
  (void)before;
  if (!contains(after, "+0 -0")) return 0;
  return !contains(after, "Press Compare to diff");
}

static int unit_check(const char *before, const char *after) {
  /* UNIT: 's' swaps FROM/TO. The default screen is Length: 1 [m] -> [ft];
   * after the swap the units must be reversed. Proves the key was handled
   * and the conversion line re-rendered with swapped units. */
  (void)before;
  return contains(after, "From: 1 [ft]      To: [m]");
}

/* ====================================================================== */
/* App table                                                              */
/* ====================================================================== */

struct app_case {
  const char *menu;         /* start-menu entry (menu index = table index)   */
  const char *title;        /* expected window title (app's gui_set_title)   */
  const char *content[3];   /* required substrings of the captured text      */
  const char *keys;         /* scripted keys ("" = none)                     */
  int (*funcheck)(const char *, const char *); /* post-key check (or 0)    */
  const char *what;         /* human-readable functional check description   */
  char quit;                /* in-app quit key; 0 => WM F4 close             */
};

static const struct app_case APPS[APP_COUNT] = {
  /* 0 */ { "FILES.BIN",   "Files",
            {"Path: ", "m=mounts", "Free: "},
            "\x01", files_check,
            "down-arrow selection, drag & drop move, Enter opens a .TXT", 0 },
  /* 1 */ { "CALC.BIN",    "Calculator",
            {"=== Calculator ===", "Memory: "},
            "2\x04" "3=", calc_check, "2+3= shows \"> 5\"", 0 },
  /* 2 */ { "CLOCK.BIN",   "Clock",
            {"=== Clock ==="},
            "", clock_check, "time string HH:MM:SS is rendered", 0 },
  /* 3 */ { "SYSMON.BIN",  "SysMon",
            {"=== System Monitor ===", "Uptime:", "APPS_T.BIN"},
            "", 0, 0, 0 },
  /* 4 */ { "HEX.BIN",     "Hex Viewer",
            {"=== Hex Viewer ===", "No file open"},
            "", 0, 0, 'q' },
  /* 5 */ { "TASKS.BIN",   "Tasks",
            {"=== Tasks ===", "(no tasks yet - press a to add one)"},
            "d", tasks_check, "'d' on the empty list shows the status line", 'q' },
  /* 6 */ { "FIND.BIN",    "Find",
            {"=== Find ===", "Search: _", "Results: none yet"},
            "unit\n", find_check, "typing \"unit\" + Enter finds UNIT.BIN", 0 },
  /* 7 */ { "DIFF.BIN",    "Diff",
            {"=== Diff ===", "Press Compare to diff"},
            "c", diff_check, "'c' compare shows the +0/-0 summary", 0 },
  /* 8 */ { "NOTES.BIN",   "Notes",
            {"=== Notes ===", "(no notes - press n to create one)",
             "n=new  e=edit body"},
            "", 0, 0, 0 },
  /* 9 */ { "UNIT.BIN",    "Unit Converter",
            {"=== Unit Converter ===", "From: 1 [m]      To: [ft]"},
            "s", unit_check, "'s' swaps FROM/TO units", 0 },
};

/* ====================================================================== */
/* Framebuffer verification                                               */
/* ====================================================================== */

/* Positive render check: read pixels back from the real framebuffer.
 *  - the focused title-bar fill color at (x+4, y+4) - before the title text
 *    starts at x+8 - proves wm_draw_windows() drew this window with focus.
 *  - the focused menu-bar fill at (x+5, y+26) proves the window chrome
 *    (menu strip) was composited with focus, not left as background.
 *  - white pixels inside the content area are 8x8-font glyph pixels: the
 *    WM rasterised the text it captured, so the app's output is visibly on
 *    screen (a mere byte-count/text-buffer assertion would not catch a
 *    renderer that never writes the framebuffer). */
static int fb_window_rendered(const struct window *w) {
  if (graphics_get_pixel(w->x + 4, w->y + 4) != (uint32_t)COLOR(96, 166, 255))
    return 0;
  if (graphics_get_pixel(w->x + 5, w->y + 26) != (uint32_t)COLOR(206, 208, 214))
    return 0;
  int x1 = w->x + w->w - 8;
  if (x1 > w->x + 620) x1 = w->x + 620; /* bounded scan */
  int y1 = w->y + 320;
  int white = 0;
  for (int y = w->y + 44; y < y1; y++) {
    for (int x = w->x + 10; x < x1; x++) {
      if (graphics_get_pixel(x, y) == (uint32_t)COLOR(255, 255, 255)) white++;
    }
  }
  return white >= 20; /* a couple of 8x8 glyphs worth of lit pixels */
}

/* ====================================================================== */
/* State machine                                                          */
/* ====================================================================== */

#define ST_OPEN   0
#define ST_NAV    1
#define ST_LAUNCH 2
#define ST_FUNC   3
#define ST_DRAG_ARM  7   /* FILES: press on a file row, drag to the folder */
#define ST_DRAG_HOLD 8   /* FILES: wait for the [drop] highlight, release   */
#define ST_DRAG_DROP 9   /* FILES: verify the move happened on the disk     */
#define ST_TXT_CLICK 10  /* FILES: click the .TXT row and press Enter       */
#define ST_TXT_WAIT  11  /* FILES: the editor window must open with it      */
#define ST_NAV_SETTLE  12 /* FILES: wait for the screen to settle, then     */
#define ST_NAV_MEASURE 13 /* FILES: measure one Down arrow's repaint cost   */
#define ST_QUIT   4
#define ST_CLOSE  5
#define ST_DONE   6

#define LAUNCH_TIMEOUT_MS 6000
#define FUNC_TIMEOUT_MS   8000
#define MEASURE_TIMEOUT_MS 30000
#define CLOSE_TIMEOUT_MS  5000

/* FILES key-responsiveness measurement: one Down arrow on a settled screen,
 * then count how many frames the WM took to finish painting the response,
 * how long that took in ms, and - with -DPAINT_STATS (the APPS_T graphics
 * library, see graphics.c) - how many framebuffer pixels it painted.  The
 * report line is "[APPS_T] FILES down-arrow: ..." on the serial console. */
static char nav_prev[MAX_TEXT]; /* last observed window text            */
static int  nav_stable = 0;     /* consecutive frames with no text delta */
static int  nav_frames_changed = 0;
static int  nav_frames_total = 0;
static int  nav_first_frame = 0; /* frame index of the first visible change */
static char nav_sel_before[64];  /* selected listing line before the key   */
static long nav_t0 = 0;
static long nav_ms = 0;
#ifdef PAINT_STATS
extern unsigned long graphics_paint_pixels;
static unsigned long nav_px0 = 0;
static unsigned long nav_px = 0;
#endif

/* Has the selected ("> ...") listing line moved since the key was sent? */
static int nav_sel_changed(void) {
  char cur[64];
  if (!nav_sel_before[0]) return 0;
  if (!grab_selected_line(windows[0].text, cur, (int)sizeof cur)) return 0;
  return !same_str(cur, nav_sel_before);
}

static int st_app = 0;
static int st_stage = ST_OPEN;
static long st_deadline = 0;
static char snap[512]; /* window text captured before the scripted keys */
static int st_drag_tx = 0;  /* pixel the drag hovers before release */
static int st_drag_ty = 0;

static long now_ms(void) { return sysinfo(1, 0, 0); }
static void arm_timeout(int ms) { st_deadline = now_ms() + ms; }
static int timed_out(void) { return now_ms() > st_deadline; }

static void cprint_digits(int v) {
  char buf[16];
  int n = 0;
  if (v == 0) {
    print_console("0");
    return;
  }
  if (v < 0) { print_console("-"); v = -v; }
  while (v > 0 && n < 15) { buf[n++] = (char)('0' + v % 10); v /= 10; }
  for (int i = n - 1; i >= 0; i--) {
    char c[2];
    c[0] = buf[i];
    c[1] = '\0';
    print_console(c);
  }
}

/* Print "[APPS_T] FAIL <app>: <stage> - <detail>" + the verdict, then exit
 * with failure status so the kernel process exits and the driver (or the
 * kernel halt path, when nothing else is alive) terminates QEMU. */
static void fail(const char *stage, const char *detail) {
  print_console("[APPS_T] FAIL ");
  print_console(APPS[st_app].menu);
  print_console(": ");
  print_console(stage);
  if (detail && detail[0]) {
    print_console(" - ");
    print_console(detail);
  }
  print_console("\n");
  if (num_windows == 1) {
    /* Diagnostic: what the WM actually captured into the window. */
    char dbg[512];
    copy_str(dbg, (int)sizeof dbg, windows[0].text);
    print_console("[APPS_T] window text: [");
    print_console(dbg);
    print_console("]\n");
  }
  print_console("APPS TEST FAILED (");
  print_console(APPS[st_app].menu);
  print_console(": ");
  print_console(stage);
  print_console(")\n");
  exit(1);
  while (1) {
  }
}

static void start_app(void) {
  st_stage = ST_OPEN;
  st_deadline = 0;
}

static void app_passed(const char *extra) {
  print_console("[APPS_T] PASS ");
  cprint_digits(st_app + 1);
  print_console("/10 ");
  print_console(APPS[st_app].menu);
  print_console(": window+title+content+fb");
  if (extra && extra[0]) {
    print_console("; ");
    print_console(extra);
  }
  print_console("; closed\n");
  st_app++;
  if (st_app >= APP_COUNT) {
    print_console("[APPS_T] all 10 applications launched, drew and closed\n");
    print_console("APPS TEST PASSED\n");
    exit(0);
    while (1) {
    }
  }
  start_app();
}

void flush_fb(void) {
  /* Actually push the composed frame to the GPU (real syscall), exactly
   * like editor_test.c does. */
  syscall(SYS_FLUSH_FB, 0, 0, 0, 0);

  if (st_stage == ST_DONE) return;

  switch (st_stage) {
  case ST_OPEN:
    /* Open the start menu: click the taskbar "Apps" button (keyboard-less
     * open). Next tick the menu is open; then navigate. */
    inject_mouse(APPS_BTN_MX, APPS_BTN_MY);
    inject_left_click();
    st_stage = ST_NAV;
    break;

  case ST_NAV:
    /* The mocked listing is fixed, so "Down x index, Enter" selects exactly
     * APPS[st_app].menu (start_sel is reset to 0 when the menu opens). */
    for (int i = 0; i < st_app; i++) inject_key(KEY_DOWN);
    inject_key(KEY_ENTER);
    arm_timeout(LAUNCH_TIMEOUT_MS);
    st_stage = ST_LAUNCH;
    break;

  case ST_LAUNCH: {
    if (num_windows > 1) {
      fail("launch", "more than one window exists");
    } else if (num_windows == 1) {
      const struct window *w = &windows[0];
      const char *text = w->text;
      int missing = -1;
      if (!same_str(w->title, APPS[st_app].title)) {
        if (timed_out()) fail("launch", "window title never matched the app's title");
        keepalive_tick();
        break;
      }
      for (int i = 0; i < 3; i++) {
        if (APPS[st_app].content[i] && !contains(text, APPS[st_app].content[i])) {
          missing = i;
          break;
        }
      }
      if (missing >= 0) {
        if (timed_out()) fail("launch", "expected app content never appeared");
        keepalive_tick();
        break;
      }
      if (!fb_window_rendered(w)) {
        if (timed_out()) fail("render", "window pixels not found in the framebuffer");
        keepalive_tick();
        break;
      }
      /* Window exists, has the app's title, shows the expected text and is
       * composited. Snapshot the text, then script the optional keys. */
      copy_str(snap, (int)sizeof snap, text);
      if (APPS[st_app].keys[0]) inject_keys(APPS[st_app].keys);
      if (APPS[st_app].funcheck) {
        arm_timeout(FUNC_TIMEOUT_MS);
        st_stage = ST_FUNC;
      } else {
        st_stage = ST_QUIT;
      }
    } else { /* no window yet */
      if (timed_out()) fail("launch", "app window never appeared");
      keepalive_tick();
    }
    break;
  }

  case ST_FUNC: {
    if (num_windows != 1) {
      fail("interact", "window vanished during the functional check");
    }
    if (APPS[st_app].funcheck(snap, windows[0].text)) {
      if (st_app == 0) {
        /* Settle, then measure one Down arrow's repaint cost before the
         * drag stages (see the responsiveness report line). */
        nav_stable = 0;
        nav_frames_changed = 0;
        nav_frames_total = 0;
        copy_str(nav_prev, (int)sizeof nav_prev, windows[0].text);
        arm_timeout(MEASURE_TIMEOUT_MS);
        st_stage = ST_NAV_SETTLE;
      } else {
        st_stage = ST_QUIT;
      }
    } else if (timed_out()) {
      fail("interact", APPS[st_app].what ? APPS[st_app].what
                                          : "functional check did not pass");
    } else {
      keepalive_tick();
    }
    break;
  }

  case ST_DRAG_ARM: {
    /* FILES drag & drop: press on the DRAGME.TXT row, then move the pointer
     * to the TESTDIR row with the button held. The WM forwards the press as
     * ESC [ P and the motion as ESC [ G; the app must highlight the drop
     * target ([drop]) and mark the dragged row ([moving]). Rows are looked
     * up by name because the app lists the real FAT root, and the listing
     * may still be arriving from the pipe when this stage starts. */
    const struct window *w = &windows[0];
    int src_row = find_row_of(w->text, "DRAGME.TXT");
    int dst_row = find_row_of(w->text, "TESTDIR");
    if (src_row < 0 || dst_row < 0) {
      if (timed_out()) fail("drag", "listing never showed DRAGME.TXT and TESTDIR");
      keepalive_tick();
      break;
    }
    if (!w->mouse_events) fail("drag", "FILES did not opt into mouse events");
    st_drag_tx = cell_x(w, 10);
    st_drag_ty = cell_y(w, dst_row);
    inject_left_press(cell_x(w, 10), cell_y(w, src_row));
    inject_mouse(st_drag_tx, st_drag_ty);
    arm_timeout(FUNC_TIMEOUT_MS);
    st_stage = ST_DRAG_HOLD;
    break;
  }

  case ST_DRAG_HOLD:
    if (timed_out()) fail("drag", "drop target was never highlighted");
    if (contains(windows[0].text, "[drop]") &&
        contains(windows[0].text, "[moving]")) {
      inject_mouse(st_drag_tx, st_drag_ty);   /* same cell: keeps the loop ticking */
      inject_left_release();
      arm_timeout(FUNC_TIMEOUT_MS);
      st_stage = ST_DRAG_DROP;
    } else {
      inject_mouse(st_drag_tx, st_drag_ty);
    }
    break;

  case ST_DRAG_DROP:
    if (timed_out()) fail("drag", "release was never processed");
    if (!contains(windows[0].text, "[drop]")) {
      /* The app re-rendered without the drag markers: it must have issued a
       * real move. Verify on the real FAT volume that DRAGME.TXT left the
       * root and now sits inside TESTDIR (both are real syscalls). */
      if (contains(windows[0].text, "Cannot move"))
        fail("drag", "the app reported a failed move");
      int gone = unlink("/DRAGME.TXT");
      int there = unlink("/TESTDIR/DRAGME.TXT");
      if (gone != -1) fail("drag", "/DRAGME.TXT still exists after the drop");
      if (there != 0) fail("drag", "/TESTDIR/DRAGME.TXT is missing after the drop");
      st_stage = ST_TXT_CLICK;
    } else {
      inject_mouse(st_drag_tx, st_drag_ty);
    }
    break;

  case ST_TXT_CLICK: {
    /* Click the E2E.TXT row and press Enter: the type handler must ask the
     * WM (ESC ] R) to open the file in EDITOR.BIN. Waits for the listing to
     * finish arriving after the drop's re-render. */
    const struct window *w = &windows[0];
    int row = find_row_of(w->text, "E2E.TXT");
    if (row < 0) {
      if (timed_out()) fail("open", "listing never showed E2E.TXT");
      keepalive_tick();
      break;
    }
    inject_left_press(cell_x(w, 10), cell_y(w, row));
    inject_left_release();
    inject_key(KEY_ENTER);
    arm_timeout(FUNC_TIMEOUT_MS);
    st_stage = ST_TXT_WAIT;
    break;
  }

  case ST_TXT_WAIT:
    if (num_windows == 2) {
      const struct window *ed = &windows[1];
      if (same_str(ed->title, "EDITOR") &&
          contains(ed->text, "E2E.TXT") &&
          contains(ed->text, "Line one of the e2e note.")) {
        st_stage = ST_QUIT;      /* F4 closes the editor, then FILES */
      } else if (timed_out()) {
        fail("open", "editor window opened but did not load the file");
      } else {
        keepalive_tick();
      }
    } else if (timed_out()) {
      fail("open", "no editor window after Enter on a .TXT row");
    } else {
      keepalive_tick();
    }
    break;

  case ST_NAV_SETTLE: {
    /* Wait until two consecutive frames show the same window text and the
     * listing has a selected row, then inject exactly one Down arrow and
     * start measuring. */
    if (num_windows != 1) fail("measure", "window vanished before the measurement");
    if (same_str(windows[0].text, nav_prev)) {
      nav_stable++;
    } else {
      nav_stable = 0;
      copy_str(nav_prev, (int)sizeof nav_prev, windows[0].text);
    }
    if (nav_stable >= 2) {
      nav_sel_before[0] = '\0';
      grab_selected_line(windows[0].text, nav_sel_before,
                         (int)sizeof nav_sel_before);
    }
    if (nav_stable >= 2 && nav_sel_before[0]) {
      nav_frames_changed = 0;
      nav_frames_total = 0;
      nav_stable = 0;
#ifdef PAINT_STATS
      nav_px0 = graphics_paint_pixels;
      nav_px = 0;
#endif
      nav_t0 = now_ms();
      inject_key(KEY_DOWN);
      st_stage = ST_NAV_MEASURE;
    } else if (timed_out()) {
      fail("measure", "FILES screen never settled");
    } else {
      keepalive_tick();
    }
    break;
  }

  case ST_NAV_MEASURE:
    /* Count the frames that carried a visible change.  The measurement only
     * ends once the app's response is actually on screen (the selected
     * listing line differs from the pre-key one) AND two consecutive frames
     * looked the same; before that we keep pumping frames so the app gets
     * scheduled and its output drained. */
    nav_frames_total++;
    if (!same_str(windows[0].text, nav_prev)) {
      copy_str(nav_prev, (int)sizeof nav_prev, windows[0].text);
      nav_frames_changed++;
      if (nav_frames_changed == 1) nav_first_frame = nav_frames_total;
      nav_ms = now_ms() - nav_t0;
      nav_stable = 0;
    } else if (nav_frames_changed > 0) {
      nav_stable++;
    }
    if (nav_stable >= 2 && nav_sel_changed()) {
#ifdef PAINT_STATS
      nav_px = graphics_paint_pixels - nav_px0;
#endif
      print_console("[APPS_T] FILES down-arrow: ");
      cprint_digits(nav_frames_changed);
      print_console(" painting frames (first at frame ");
      cprint_digits(nav_first_frame);
      print_console("), ");
      cprint_digits(nav_frames_total);
      print_console(" desktop frames, ");
      cprint_digits((int)nav_ms);
      print_console(" ms to settle");
#ifdef PAINT_STATS
      print_console(", ");
      cprint_digits((int)nav_px);
      print_console(" fb pixels painted");
#endif
      print_console("\n");
      arm_timeout(FUNC_TIMEOUT_MS); /* the drag stage may need a re-render */
      st_stage = ST_DRAG_ARM;
    } else if (timed_out()) {
      fail("measure", "down-arrow response never settled");
    } else {
      keepalive_tick();
    }
    break;

  case ST_QUIT:
    /* Quit with the app's own key when it has one; otherwise F4 (the WM's
     * "close the focused window" hook). */
    if (APPS[st_app].quit)
      inject_char(APPS[st_app].quit);
    else
      inject_key(KEY_F4);
    arm_timeout(CLOSE_TIMEOUT_MS);
    st_stage = ST_CLOSE;
    break;

  case ST_CLOSE:
    if (num_windows == 0) {
      app_passed(APPS[st_app].what);
      break;
    }
    if (timed_out()) fail("close", "window did not close");
    if (!APPS[st_app].quit) {
      /* F4 closes the *focused* window. FILES ends with a second window open
       * (the editor it launched), so once that one is gone, focus must be
       * brought back before F4 can close FILES: a press+release inside the
       * first window is a plain click that focuses it (row 1 is the path
       * line - not a listing row, so the selection does not change). */
      const struct window *w = &windows[0];
      inject_left_press(cell_x(w, 10), cell_y(w, 1));
      inject_left_release();
      inject_key(KEY_F4);
    } else {
      keepalive_tick();
    }
    break;

  default:
    break;
  }
}

/* ====================================================================== */
/* Entry point                                                            */
/* ====================================================================== */

__attribute__((section(".text._start")))
void _start(void) {
  print_console("[APPS_T] in-OS desktop app harness: driving the real WM through 10 apps\n");
  start_app();
  desktop_main(); /* never returns: flush_fb() runs the harness */
  print_console("[APPS_T] FAIL: desktop_main returned\nAPPS TEST FAILED (desktop loop exited)\n");
  exit(1);
}
