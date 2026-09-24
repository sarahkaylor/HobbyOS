/*
 * notes_test.c - Host unit tests for notes.c (HobbyOS Notes manager).
 *
 * The app source is included directly (with `#define main notes_app_main`)
 * so the tests can drive its state and helpers. Dialogs are NOT exercised
 * here: dialog_prompt()/dialog_confirm() block on stdin and would hang a
 * host test; the dialog-driven wrappers (notes_cmd_*) are kept thin and the
 * logic behind them (notes_add / notes_edit_body / notes_delete / notes_save)
 * is tested directly.
 *
 * File tests use compat.c's real host open/read/write and therefore switch
 * the *real* process cwd to a scratch directory under /tmp first (compat's
 * chdir is an inert in-memory mock, so the syscall is used directly).
 *
 * Every check prints PASS/FAIL, a summary is printed at the end, and the
 * exit status is non-zero if anything failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#define main notes_app_main
#include "../user/notes.c"
#undef main

/* ---- Host-only helpers (not provided by compat.c) ----------------- */

extern int dup(int oldfd);
extern int dup2(int oldfd, int newfd);
extern long syscall(long number, ...);   /* used for a real chdir() */

#define SCRATCH_DIR  "/tmp/notes_host_e2e"
#define SCRATCH_FILE SCRATCH_DIR "/NOTES.TXT"
#define CAP_PATH     "/tmp/notes_host_render.txt"
#define CAP_MAX      4096

/* ---- Tiny check framework ----------------------------------------- */

static int checks_run = 0;
static int checks_failed = 0;

static void check(int cond, const char *msg) {
  checks_run++;
  if (cond) {
    printf("PASS: %s\n", msg);
  } else {
    checks_failed++;
    printf("FAIL: %s\n", msg);
  }
}

static void check_int(const char *what, int got, int want) {
  checks_run++;
  if (got == want) {
    printf("PASS: %s (%d)\n", what, got);
  } else {
    checks_failed++;
    printf("FAIL: %s (got %d, want %d)\n", what, got, want);
  }
}

static void check_str(const char *what, const char *got, const char *want) {
  checks_run++;
  if (got && want && strcmp(got, want) == 0) {
    printf("PASS: %s\n", what);
  } else {
    checks_failed++;
    printf("FAIL: %s (got \"%s\", want \"%s\")\n",
           what, got ? got : "(null)", want ? want : "(null)");
  }
}

/* ---- stdout capture (for render/init output) ---------------------- */

static int cap_saved_fd = -1;

static void capture_start(const char *path) {
  FILE *f;
  fflush(stdout);
  cap_saved_fd = dup(1);
  f = fopen(path, "w");
  if (f) {
    dup2(fileno(f), 1);
    fclose(f);
  }
}

static void capture_stop(void) {
  fflush(stdout);
  if (cap_saved_fd >= 0) {
    dup2(cap_saved_fd, 1);
    close(cap_saved_fd);
    cap_saved_fd = -1;
  }
}

/* Run fn() with stdout redirected, then return its captured output. */
static int capture_output(void (*fn)(void), const char *path, char *out, int cap) {
  FILE *f;
  int n = 0;
  capture_start(path);
  fn();
  capture_stop();
  f = fopen(path, "rb");
  if (!f) { out[0] = '\0'; return -1; }
  n = (int)fread(out, 1, (size_t)(cap - 1), f);
  fclose(f);
  if (n < 0) n = 0;
  out[n] = '\0';
  return n;
}

/* Run notes_render() with stdout redirected, then return its captured output
 * with the leading clear-screen byte ('\f' from gui_clear()) stripped so
 * line 0 is the first real screen row. */
static char render_cap_buf[CAP_MAX];

static int raw_render_to(char *out, int cap) {
  return capture_output(notes_render, CAP_PATH, out, cap);
}

static void render_to(char *out, int cap) {
  const char *src = render_cap_buf;
  int i = 0;
  raw_render_to(render_cap_buf, sizeof render_cap_buf);
  if (src[0] == '\f') src++;
  while (src[i] && i < cap - 1) {
    out[i] = src[i];
    i++;
  }
  out[i] = '\0';
}

/* ---- Line helpers -------------------------------------------------- */

static const char *line_at(const char *text, int index) {
  const char *p = text;
  int cur = 0;
  for (;;) {
    const char *e = strchr(p, '\n');
    if (cur == index) return p;
    if (!e) return NULL;
    p = e + 1;
    cur++;
  }
}

static int line_eq(const char *text, int index, const char *want) {
  const char *p = line_at(text, index);
  size_t wl = strlen(want);
  if (!p) return 0;
  if (strncmp(p, want, wl) != 0) return 0;
  return p[wl] == '\n' || p[wl] == '\0';
}

static int line_len(const char *text, int index) {
  const char *p = line_at(text, index);
  const char *e;
  if (!p) return -1;
  e = strchr(p, '\n');
  return e ? (int)(e - p) : (int)strlen(p);
}

static int max_line_len(const char *text) {
  int mx = 0, i = 0;
  const char *p;
  while ((p = line_at(text, i)) != NULL) {
    const char *e = strchr(p, '\n');
    int n = e ? (int)(e - p) : (int)strlen(p);
    if (n > mx) mx = n;
    if (!e) break;
    i++;
  }
  return mx;
}

static int text_has(const char *text, const char *needle) {
  return strstr(text, needle) != NULL;
}

/* ---- Misc helpers -------------------------------------------------- */

static void make_string(char *buf, int n, char c) {
  for (int i = 0; i < n; i++) buf[i] = c;
  buf[n] = '\0';
}

static void setup_text(const char *data) {
  notes_reset();
  notes_parse(data, (int)strlen(data));
}

/* 3 notes: "Note 0".."Note 2" with bodies "body 0".."body 2". */
static void make_notes(int n) {
  notes_reset();
  for (int i = 0; i < n; i++) {
    char t[32], b[64];
    snprintf(t, sizeof t, "Note %d", i);
    snprintf(b, sizeof b, "body %d", i);
    notes_add(t, b);
  }
}

static int read_file(const char *path, char *out, int cap) {
  FILE *f = fopen(path, "rb");
  int n;
  if (!f) return -1;
  n = (int)fread(out, 1, (size_t)(cap - 1), f);
  fclose(f);
  if (n < 0) n = 0;
  out[n] = '\0';
  return n;
}

static long file_size(const char *path) {
  FILE *f = fopen(path, "rb");
  long n;
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fclose(f);
  return n;
}

static int write_file(const char *path, const char *data) {
  FILE *f = fopen(path, "wb");
  size_t n;
  if (!f) return -1;
  n = fwrite(data, 1, strlen(data), f);
  fclose(f);
  return (int)n;
}

static int file_env_ok = 0;

static void file_env_setup(void) {
  char cmd[256];
  snprintf(cmd, sizeof cmd, "mkdir -p %s", SCRATCH_DIR);
  if (system(cmd) != 0) { printf("NOTE: '%s' failed\n", cmd); return; }
  if (syscall(SYS_chdir, SCRATCH_DIR) != 0) {
    printf("NOTE: real chdir(%s) failed\n", SCRATCH_DIR);
    return;
  }
  remove(SCRATCH_FILE);
  file_env_ok = 1;
}

/* ================================================================== */
/* Tests: parsing                                                     */
/* ================================================================== */

static void test_parse(void) {
  char file[4096];
  char t[128], b[128];
  int p;

  printf("\n-- parse --\n");

  setup_text("");
  check_int("parse: empty buffer -> 0 notes", notes_count, 0);
  check_int("parse: empty buffer -> list.count 0", notes_list.count, 0);

  setup_text("### Solo\n");
  check_int("parse: title-only note -> 1 note", notes_count, 1);
  check_str("parse: title-only note title", notes[0].title, "Solo");
  check_str("parse: title-only note body is empty", notes[0].body, "");

  setup_text("### First\nhello world\n");
  check_int("parse: title+body -> 1 note", notes_count, 1);
  check_str("parse: title", notes[0].title, "First");
  check_str("parse: body", notes[0].body, "hello world");

  setup_text("### A\none\n\n### B\ntwo\n");
  check_int("parse: blank line between notes -> 2 notes", notes_count, 2);
  check_str("parse: note A body", notes[0].body, "one");
  check_str("parse: note B body", notes[1].body, "two");

  setup_text("### A\none\nextra1\nextra2\n");
  check_int("parse: extra lines -> still 1 note", notes_count, 1);
  check_str("parse: extra lines ignored, body = first line", notes[0].body, "one");

  setup_text("### A\n### B\n");
  check_int("parse: adjacent titles -> 2 notes", notes_count, 2);
  check_str("parse: adjacent titles, A body empty", notes[0].body, "");
  check_str("parse: adjacent titles, B body empty", notes[1].body, "");
  check_str("parse: adjacent titles, B title", notes[1].title, "B");

  setup_text("###   spaced  title  \n");
  check_int("parse: spaced title -> 1 note", notes_count, 1);
  check_str("parse: spaces in title preserved", notes[0].title, "  spaced  title  ");

  make_string(t, 40, 'T');
  make_string(b, 100, 'B');
  snprintf(file, sizeof file, "### %s\n%s\n", t, b);
  setup_text(file);
  check_int("parse: overlong values -> 1 note", notes_count, 1);
  check_int("parse: overlong title clipped to 24", gui_strlen(notes[0].title), NOTE_TITLE_MAX);
  check_int("parse: overlong body clipped to 60", gui_strlen(notes[0].body), NOTE_BODY_MAX);

  p = 0;
  for (int i = 0; i < 25; i++) {
    p += snprintf(file + p, sizeof file - (size_t)p, "### N%d\nb%d\n", i, i);
  }
  setup_text(file);
  check_int("parse: 25 notes truncated to 20", notes_count, NOTES_MAX);
  check_str("parse: note 19 kept", notes[19].title, "N19");
  check_str("parse: note 19 body kept (note 20's body not attached)", notes[19].body, "b19");
  {
    int found = 0;
    for (int i = 0; i < notes_count; i++) {
      if (strcmp(notes[i].title, "N20") == 0) found = 1;
    }
    check(!found, "parse: note 20 dropped (title absent)");
  }

  setup_text("junk line\n### A\nbody\n");
  check_int("parse: junk before first title ignored", notes_count, 1);
  check_str("parse: junk-before title", notes[0].title, "A");
  check_str("parse: junk-before body", notes[0].body, "body");

  setup_text("### A\r\nbody\r\n");
  check_str("parse: CRLF title", notes[0].title, "A");
  check_str("parse: CRLF body", notes[0].body, "body");

  setup_text("### A\nbody");
  check_int("parse: unterminated last line -> 1 note", notes_count, 1);
  check_str("parse: unterminated body", notes[0].body, "body");

  setup_text("### A\nx\n#### four hashes\n");
  check_int("parse: '####' line is not a title", notes_count, 1);
  check_str("parse: '####' line ignored after body", notes[0].body, "x");

  setup_text("#### only\n");
  check_int("parse: only '####' line -> 0 notes", notes_count, 0);

  setup_text("### A\n1\n### B\n2\n");
  setup_text("### C\n3\n");
  check_int("parse: re-parse replaces state", notes_count, 1);
  check_str("parse: re-parse new title", notes[0].title, "C");
  check_str("parse: re-parse clears old tail", notes[1].title, "");
}

/* ================================================================== */
/* Tests: serialisation                                               */
/* ================================================================== */

static void test_serialize(void) {
  char buf[CAP_MAX];
  char small[5];
  char t[128], b[128];
  char expect[256];
  struct note before[NOTES_MAX];
  int len, saved_count;

  printf("\n-- serialize --\n");

  notes_reset();
  len = notes_serialize(buf, sizeof buf);
  check_int("serialize: 0 notes -> length 0", len, 0);
  check_str("serialize: 0 notes -> empty string", buf, "");

  notes_reset();
  notes_add("T", "B");
  len = notes_serialize(buf, sizeof buf);
  check_int("serialize: 1 note length", len, 8);
  check_str("serialize: 1 note exact text", buf, "### T\nB\n");

  notes_reset();
  notes_add("T", "B");
  len = notes_serialize(small, sizeof small);
  check_int("serialize: tiny buffer -> -1", len, -1);

  notes_reset();
  make_string(t, 40, 'T');
  make_string(b, 100, 'B');
  notes_add(t, b);
  snprintf(expect, sizeof expect, "### TTTTTTTTTTTTTTTTTTTTTTTT\n%s\n",
           "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
  len = notes_serialize(buf, sizeof buf);
  check_int("serialize: clipped note length (28 + 60 + 1)", len, 28 + 1 + 60 + 1);
  check_str("serialize: clipped note exact text (title 24 / body 60)", buf, expect);

  notes_reset();
  for (int i = 0; i < NOTES_MAX; i++) {
    char tt[32], bb[32];
    snprintf(tt, sizeof tt, "note%d", i);
    snprintf(bb, sizeof bb, "value%d", i);
    notes_add(tt, bb);
  }
  saved_count = notes_count;
  for (int i = 0; i < NOTES_MAX; i++) before[i] = notes[i];
  len = notes_serialize(buf, sizeof buf);
  check(len > 0 && len < (int)sizeof buf, "serialize: 20 notes fit in the buffer");
  notes_parse(buf, len);
  check_int("round-trip: 20 notes -> 20 notes", notes_count, saved_count);
  int ok = 1;
  for (int i = 0; i < NOTES_MAX; i++) {
    if (strcmp(notes[i].title, before[i].title) != 0) ok = 0;
    if (strcmp(notes[i].body, before[i].body) != 0) ok = 0;
  }
  check(ok, "round-trip: all 20 titles/bodies survive serialize+parse");

  notes_reset();
  len = notes_serialize(buf, sizeof buf);
  notes_parse(buf, len > 0 ? len : 0);
  check_int("round-trip: 0 notes -> 0 notes", notes_count, 0);

  len = notes_serialize(NULL, 100);
  check_int("serialize: NULL buffer -> -1", len, -1);
}

/* ================================================================== */
/* Tests: state + mutations                                           */
/* ================================================================== */

static void test_mutations(void) {
  char t[128], b[128];

  printf("\n-- state and mutations --\n");

  make_notes(3);
  notes_warn = 1;
  notes_list.top = 1;
  notes_reset();
  check_int("reset: count 0", notes_count, 0);
  check_int("reset: selected 0", notes_list.selected, 0);
  check_int("reset: top 0", notes_list.top, 0);
  check_int("reset: warn cleared", notes_warn, 0);
  check_int("reset: list.count 0", notes_list.count, 0);
  check_int("reset: list.visible 12", notes_list.visible, NOTES_LIST_ROWS);

  notes_reset();
  check_int("add: first add returns 1", notes_add("A", "a"), 1);
  check_int("add: count 1", notes_count, 1);
  check_int("add: new note selected", notes_list.selected, 0);
  check_int("add: second add returns 1", notes_add("B", "b"), 1);
  check_int("add: count 2", notes_count, 2);
  check_int("add: newest selected", notes_list.selected, 1);
  check_str("add: stored title", notes[1].title, "B");
  check_str("add: stored body", notes[1].body, "b");
  check_int("add: list.count mirrors notes_count", notes_list.count, 2);

  notes_reset();
  make_string(t, 40, 'T');
  make_string(b, 100, 'B');
  notes_add(t, b);
  check_int("add: overlong title clipped to 24", gui_strlen(notes[0].title), NOTE_TITLE_MAX);
  check_int("add: overlong body clipped to 60", gui_strlen(notes[0].body), NOTE_BODY_MAX);

  notes_reset();
  check_int("add: empty title rejected", notes_add("", "body"), 0);
  check_int("add: empty title does not add", notes_count, 0);
  check_int("add: NULL title rejected", notes_add(NULL, "body"), 0);
  check_int("add: NULL body treated as empty", notes_add("ok", NULL), 1);
  check_str("add: NULL body stored empty", notes[0].body, "");

  make_notes(NOTES_MAX);
  check_int("add: cap reached at 20", notes_count, NOTES_MAX);
  check_int("add: 21st note rejected", notes_add("overflow", "x"), 0);
  check_int("add: cap holds at 20", notes_count, NOTES_MAX);
  check_str("add: last note unchanged", notes[NOTES_MAX - 1].title, "Note 19");

  make_notes(3);
  notes_list.selected = 1;
  check_int("edit: returns 1 with a selection", notes_edit_body("updated"), 1);
  check_str("edit: body updated", notes[1].body, "updated");
  check_str("edit: other notes untouched", notes[0].body, "body 0");
  make_string(b, 80, 'E');
  notes_edit_body(b);
  check_int("edit: overlong body clipped to 60", gui_strlen(notes[1].body), NOTE_BODY_MAX);
  notes_reset();
  check_int("edit: no notes -> 0", notes_edit_body("x"), 0);

  make_notes(3);
  notes_list.selected = 1;
  check_int("delete: returns 1", notes_delete(), 1);
  check_int("delete: count 2", notes_count, 2);
  check_str("delete: following note shifted up", notes[1].title, "Note 2");
  check_int("delete: selection stays in range", notes_list.selected, 1);
  check_int("delete: last slot cleared", gui_strlen(notes[2].title), 0);

  make_notes(3);
  notes_list.selected = 2;
  notes_delete();
  check_int("delete last selected: count 2", notes_count, 2);
  check_int("delete last selected: selection moves to new last", notes_list.selected, 1);

  make_notes(1);
  notes_delete();
  check_int("delete only note: count 0", notes_count, 0);
  check_int("delete only note: selected 0", notes_list.selected, 0);
  check_int("delete only note: list.count 0", notes_list.count, 0);
  check_int("delete: nothing left to delete -> 0", notes_delete(), 0);

  make_notes(NOTES_MAX);
  notes_list.selected = NOTES_MAX - 1;
  gui_list_ensure_visible(&notes_list);
  check_int("scroll: top follows selection (20 notes)", notes_list.top, NOTES_MAX - NOTES_LIST_ROWS);
  notes_delete();
  check_int("delete while scrolled: count 19", notes_count, 19);
  check_int("delete while scrolled: selection in range", notes_list.selected, 18);
  check_int("delete while scrolled: top adjusted", notes_list.top, 18 - NOTES_LIST_ROWS + 1);
  check(notes_list.selected >= notes_list.top &&
        notes_list.selected < notes_list.top + NOTES_LIST_ROWS,
        "delete while scrolled: selection stays visible");

  make_notes(3);
  notes_list.selected = 0;
  gui_list_move(&notes_list, -1);
  check_int("move: UP at first item clamps", notes_list.selected, 0);
  gui_list_move(&notes_list, 1);
  check_int("move: DOWN moves selection", notes_list.selected, 1);
  gui_list_move(&notes_list, 10);
  check_int("move: DOWN clamps at last item", notes_list.selected, 2);
  gui_list_move(&notes_list, -10);
  check_int("move: UP clamps at first item", notes_list.selected, 0);

  make_notes(1);
  notes_list.selected = 0;
  check_int("selected body: mirrors selection", strcmp(notes_selected_body(), "body 0"), 0);
  notes_reset();
  check_str("selected body: empty when no notes", notes_selected_body(), "");

  /* direct refresh_list checks: out-of-range selection/top are repaired */
  make_notes(2);
  notes_list.selected = 99;
  notes_list.top = 50;
  notes_refresh_list();
  check_int("refresh_list: count mirrored", notes_list.count, 2);
  check_int("refresh_list: selection clamped", notes_list.selected, 1);
  check_int("refresh_list: top pulled back to the selection", notes_list.top, 1);
  notes_reset();
  notes_refresh_list();
  check_int("refresh_list: empty list -> selected 0", notes_list.selected, 0);
  check_int("refresh_list: empty list -> top 0", notes_list.top, 0);

  /* direct click mapping checks */
  make_notes(3);
  check_int("click: returns the row index", notes_click(notes_list_first_row() + 1), 1);
  check_int("click: selects that note", notes_list.selected, 1);
  check_int("click: row below the list -> -1", notes_click(notes_list_first_row() + 9), -1);
  check_int("click: row above the list -> -1", notes_click(notes_list_first_row() - 1), -1);
}

/* ================================================================== */
/* Tests: events (no dialogs / no files)                              */
/* ================================================================== */

static void test_events(void) {
  struct gui_event ev;

  printf("\n-- events --\n");

  make_notes(3);
  notes_list.selected = 0;   /* make_notes() selects the last note */
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_DOWN;
  check_int("event DOWN handled", notes_handle_event(&ev), 1);
  check_int("event DOWN moves selection", notes_list.selected, 1);
  ev.type = GUI_EV_UP;
  check_int("event UP handled", notes_handle_event(&ev), 1);
  check_int("event UP moves selection", notes_list.selected, 0);
  notes_handle_event(&ev);
  check_int("event UP at top stays 0", notes_list.selected, 0);

  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_CHAR;
  ev.ch = 'x';
  check_int("event unknown char ignored", notes_handle_event(&ev), 0);
  ev.ch = 27;
  check_int("event ESC ignored", notes_handle_event(&ev), 0);
  ev.type = GUI_EV_NONE;
  check_int("event NONE ignored", notes_handle_event(&ev), 0);
  check_int("event NULL ignored", (int)notes_handle_event(NULL), 0);

  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MENU;
  ev.menu = 1;
  ev.item = 0;
  check_int("event menu 1 ignored", notes_handle_event(&ev), 0);
  ev.menu = 0;
  ev.item = 9;
  check_int("event menu 0 item 9 ignored", notes_handle_event(&ev), 0);
  check_int("event menu 0 item 9 changes nothing", notes_count, 3);

  make_notes(3);
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MOUSE;
  ev.button = 1;
  ev.y = notes_list_first_row() + 2;
  check_int("mouse click on title row handled", notes_handle_event(&ev), 1);
  check_int("mouse click selects that note", notes_list.selected, 2);
  ev.y = notes_list_first_row() + 11;
  check_int("mouse click on empty row ignored", notes_handle_event(&ev), 0);
  check_int("mouse click on empty row keeps selection", notes_list.selected, 2);
  ev.y = notes_list_first_row() + 12;   /* separator rule */
  check_int("mouse click on rule ignored", notes_handle_event(&ev), 0);
  ev.button = 2;
  ev.y = notes_list_first_row();
  check_int("right-click ignored", notes_handle_event(&ev), 0);
  check_int("right-click keeps selection", notes_list.selected, 2);

  notes_warn = 1;
  check_int("mouse: warning shifts the list down", notes_list_first_row(), 3);
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MOUSE;
  ev.button = 1;
  ev.y = 2;    /* warning line row */
  check_int("mouse: click on warning row ignored", notes_handle_event(&ev), 0);
  ev.y = 3;
  check_int("mouse: click on first row with warning", notes_handle_event(&ev), 1);
  check_int("mouse: selects first note with warning", notes_list.selected, 0);
  notes_warn = 0;
  check_int("warning flag cleared -> first row 2 again", notes_list_first_row(), 2);
}

/* ================================================================== */
/* Tests: preview formatting                                          */
/* ================================================================== */

static void test_preview(void) {
  char big[300];
  char out[NOTES_PREVIEW_COLS + 8];
  char expect[NOTES_PREVIEW_COLS + 8];

  printf("\n-- preview formatting --\n");

  notes_preview_row("", 0, out);
  check_str("preview: empty body row 0", out, "");
  notes_preview_row("", 1, out);
  check_str("preview: empty body row 1", out, "");
  notes_preview_row("", 2, out);
  check_str("preview: empty body row 2", out, "");

  notes_preview_row("hello", 0, out);
  check_str("preview: short body on row 0", out, "hello");
  notes_preview_row("hello", 1, out);
  check_str("preview: short body row 1 empty", out, "");
  notes_preview_row("hello", 2, out);
  check_str("preview: short body row 2 empty", out, "");

  make_string(big, 70, 'a');
  notes_preview_row(big, 0, out);
  check_int("preview: exactly 70 chars -> row 0 length 70", (int)strlen(out), NOTES_PREVIEW_COLS);
  notes_preview_row(big, 1, out);
  check_str("preview: exactly 70 chars -> row 1 empty", out, "");

  make_string(big, 210, 'b');
  notes_preview_row(big, 0, out);
  check_int("preview: 210 chars -> row 0 full", (int)strlen(out), NOTES_PREVIEW_COLS);
  notes_preview_row(big, 1, out);
  check_int("preview: 210 chars -> row 1 full", (int)strlen(out), NOTES_PREVIEW_COLS);
  notes_preview_row(big, 2, out);
  check_int("preview: 210 chars -> row 2 full, no ellipsis", (int)strlen(out), NOTES_PREVIEW_COLS);
  check_str("preview: 210 chars -> row 2 has no ellipsis", out, big + 140);

  make_string(big, 211, 'c');
  notes_preview_row(big, 2, out);
  check_int("preview: 211 chars -> row 2 is 70 + '...'", (int)strlen(out), NOTES_PREVIEW_COLS + 3);
  check_str("preview: 211 chars -> row 2 ends with '...'", out + NOTES_PREVIEW_COLS, "...");
  check(memcmp(out, big + 140, NOTES_PREVIEW_COLS) == 0,
        "preview: 211 chars -> row 2 shows chars 140..209");

  make_string(big, 250, 'd');
  notes_preview_row(big, 0, out);
  check(memcmp(out, big, NOTES_PREVIEW_COLS) == 0 && out[NOTES_PREVIEW_COLS] == '\0',
        "preview: 250 chars -> row 0 is chars 0..69 (clip, no wrap)");
  notes_preview_row(big, 1, out);
  check_int("preview: 250 chars -> row 1 holds chars 70..139", (int)strlen(out), NOTES_PREVIEW_COLS);
  check(memcmp(out, big + 70, NOTES_PREVIEW_COLS) == 0,
        "preview: 250 chars -> row 1 starts at char 70");
  notes_preview_row(big, 2, out);
  make_string(expect, NOTES_PREVIEW_COLS, 'd');
  strcat(expect, "...");
  check_str("preview: 250 chars -> row 2 clipped with '...'", out, expect);

  make_string(big, 100, 'e');
  notes_preview_row(big, 0, out);
  check_int("preview: 100 chars -> row 0 full", (int)strlen(out), NOTES_PREVIEW_COLS);
  notes_preview_row(big, 1, out);
  check_int("preview: 100 chars -> row 1 holds remaining 30", (int)strlen(out), 30);
  notes_preview_row(big, 2, out);
  check_str("preview: 100 chars -> row 2 empty (all shown)", out, "");

  /* clip helper boundaries */
  char clipt[64];
  make_string(big, 200, 'f');
  notes_clip(clipt, big, 24);
  check_int("clip: 24-char cap", (int)strlen(clipt), 24);
  notes_clip(clipt, "short", 24);
  check_str("clip: shorter string copied as-is", clipt, "short");
  notes_clip(clipt, "has\nnewline", 24);
  check_str("clip: stops at newline", clipt, "has");
}

/* ================================================================== */
/* Tests: rendering                                                   */
/* ================================================================== */

static void test_render(void) {
  char cap[CAP_MAX];
  char cap2[CAP_MAX];
  char rule[NOTES_RULE_COLS + 1];
  char long_title[64];
  int fr, rule_row, footer_row;

  printf("\n-- rendering --\n");

  /* --- header with notes --- */
  make_notes(3);
  notes_list.selected = 0;
  render_to(cap, sizeof cap);
  check(line_eq(cap, 0, "=== Notes ==="), "render: line 0 is the title");
  check(line_eq(cap, 1, "3 notes"), "render: line 1 is 'N notes'");
  raw_render_to(cap2, sizeof cap2);
  check(cap2[0] == '\f', "render: screen starts with the clear-screen byte");

  fr = notes_list_first_row();
  check_int("render: first list row (no warning)", fr, 2);
  check(line_eq(cap, fr + 0, "> Note 0"), "render: selected row prefix '> '");
  check(line_eq(cap, fr + 1, "  Note 1"), "render: unselected row prefix '  '");
  check(line_eq(cap, fr + 2, "  Note 2"), "render: third note row");
  check(line_eq(cap, fr + 3, ""), "render: spare rows blank");

  rule_row = fr + NOTES_LIST_ROWS;
  rule[0] = '+';
  for (int i = 1; i < NOTES_RULE_COLS - 1; i++) rule[i] = '-';
  rule[NOTES_RULE_COLS - 1] = '+';
  rule[NOTES_RULE_COLS] = '\0';
  check(line_eq(cap, rule_row, rule), "render: 72-column separator rule");
  check_int("render: rule width", line_len(cap, rule_row), NOTES_RULE_COLS);

  check(line_eq(cap, rule_row + 1, "body 0"), "render: preview row 1 is the selected body");
  check(line_eq(cap, rule_row + 2, ""), "render: preview row 2 blank for short body");
  check(line_eq(cap, rule_row + 3, ""), "render: preview row 3 blank for short body");

  footer_row = rule_row + 1 + NOTES_PREVIEW_ROWS;
  check(line_eq(cap, footer_row, "n=new  e=edit body  d=delete  Enter=save"),
        "render: footer line exact");
  check(line_eq(cap, footer_row + 1, ""), "render: nothing after the footer");

  /* --- empty state --- */
  make_notes(0);
  render_to(cap, sizeof cap);
  check(line_eq(cap, 1, "(no notes - press n to create one)"),
        "render: empty state hint");
  check(line_eq(cap, 2, ""), "render: empty state list rows blank");
  check(line_eq(cap, rule_row + 1, ""), "render: empty state preview blank");

  /* --- selection moves with the preview --- */
  make_notes(2);
  notes_list.selected = 1;
  render_to(cap, sizeof cap);
  check(line_eq(cap, 2, "  Note 0"), "render: selection indicator moves (row 0)");
  check(line_eq(cap, 3, "> Note 1"), "render: selection indicator moves (row 1)");
  check(line_eq(cap, NOTES_LIST_ROWS + 2 + 1, "body 1"),
        "render: preview follows the selection");

  /* --- warning line --- */
  notes_warn = 1;
  render_to(cap, sizeof cap);
  check(line_eq(cap, 2, "WARNING: cannot access NOTES.TXT - data kept in memory"),
        "render: warning line under the header");
  check_int("render: list shifts down when warning shown", notes_list_first_row(), 3);
  check(line_eq(cap, 3, "  Note 0"), "render: first list row becomes 3 with warning");
  check(line_eq(cap, 4, "> Note 1"), "render: selection follows the shifted list");
  check(line_eq(cap, 3 + NOTES_LIST_ROWS, rule), "render: rule follows the list with warning");
  notes_warn = 0;

  /* --- long titles are stored/displayed clipped --- */
  notes_reset();
  make_string(long_title, 40, 'L');
  notes_add(long_title, "x");
  render_to(cap, sizeof cap);
  {
    char expect[64];
    char clipped[NOTE_TITLE_MAX + 1];
    notes_clip(clipped, long_title, NOTE_TITLE_MAX);
    snprintf(expect, sizeof expect, "> %s", clipped);
    check(line_eq(cap, 2, expect), "render: title row shows the 24-char stored title");
    check_int("render: title row length", line_len(cap, 2), 2 + NOTE_TITLE_MAX);
    /* The 30-column display clip is defensive (titles are stored at 24
     * chars max), so it is exercised through notes_clip() directly. */
    notes_clip(clipped, long_title, NOTES_TITLE_COLS);
    check_int("clip: display width 30", (int)strlen(clipped), NOTES_TITLE_COLS);
  }

  /* --- long rendered list scrolls (top) --- */
  make_notes(NOTES_MAX);
  notes_list.selected = NOTES_MAX - 1;
  gui_list_ensure_visible(&notes_list);
  render_to(cap, sizeof cap);
  check(line_eq(cap, 2, "  Note 8"), "render: scrolled list starts at top");
  check(line_eq(cap, 2 + 11, "> Note 19"), "render: selected note last visible row");

  /* --- determinism + size limits --- */
  render_to(cap, sizeof cap);
  render_to(cap2, sizeof cap2);
  check(strcmp(cap, cap2) == 0, "render: deterministic for the same state");
  check_int("render: longest line <= 110 columns", max_line_len(cap) <= 110, 1);
  check_int("render: full screen < 1800 chars", (int)strlen(cap) < 1800, 1);
}

/* ================================================================== */
/* Tests: startup marker / init                                       */
/* ================================================================== */

static void test_init(void) {
  char cap[CAP_MAX];

  printf("\n-- init / startup marker --\n");

  if (!file_env_ok) {
    check(0, "init: file environment (scratch dir + chdir) available");
    return;
  }
  remove(SCRATCH_FILE);

  capture_output(notes_init, CAP_PATH, cap, sizeof cap);
  check(text_has(cap, "[APP] NOTES started\n"), "init: prints '[APP] NOTES started'");
  check(text_has(cap, "\033]TNotes~"), "init: sets window title to 'Notes'");
  check(text_has(cap, "\033]M0;Note;New,Edit,Delete,Save"),
        "init: registers menu 0 'Note' with 4 items");
  check(text_has(cap, "\033]P1~"), "init: enables mouse");
  check(text_has(cap, "=== Notes ==="), "init: renders the screen");
  check_int("init: empty NOTES.TXT -> 0 notes", notes_count, 0);
  check_int("init: no warning after clean load", notes_warn, 0);
}

/* ================================================================== */
/* Tests: real file end-to-end                                        */
/* ================================================================== */

static void test_files(void) {
  char cap[CAP_MAX];
  char ser[CAP_MAX];
  char disk[CAP_MAX];
  long size_before;
  int len, n;
  struct gui_event ev;

  printf("\n-- file end-to-end (real NOTES.TXT) --\n");

  if (!file_env_ok) {
    check(0, "files: file environment (scratch dir + chdir) available");
    return;
  }

  /* --- fresh save --- */
  remove(SCRATCH_FILE);
  make_notes(2);
  check_int("save: returns 1 (fresh file)", notes_save(), 1);
  check_int("save: warning cleared on success", notes_warn, 0);

  len = notes_serialize(ser, sizeof ser);
  n = read_file(SCRATCH_FILE, disk, sizeof disk);
  check(n == len, "save: file length matches serialization");
  check_str("save: file content matches serialization exactly", disk, ser);

  /* --- load round trip --- */
  notes_reset();
  check_int("load: returns 1", notes_load(), 1);
  check_int("load: 2 notes read back", notes_count, 2);
  check_str("load: first title", notes[0].title, "Note 0");
  check_str("load: second body", notes[1].body, "body 1");
  check_int("load: no warning", notes_warn, 0);

  /* --- shrink: delete + save must not leave stale notes on disk --- */
  size_before = file_size(SCRATCH_FILE);
  notes_list.selected = 0;
  check_int("file: delete selected note", notes_delete(), 1);
  check_int("file: save after delete", notes_save(), 1);
  check(file_size(SCRATCH_FILE) <= size_before, "file: rewritten file does not grow");
  notes_reset();
  check_int("file: reload after delete", notes_load(), 1);
  check_int("file: only the remaining note comes back", notes_count, 1);
  check_str("file: remaining note title", notes[0].title, "Note 1");
  check_str("file: deleted note did not resurrect", notes[0].body, "body 1");

  /* --- edit body + save round trip --- */
  notes_edit_body("edited body");
  check_int("file: save after edit", notes_save(), 1);
  notes_reset();
  notes_load();
  check_str("file: edited body round-trips", notes[0].body, "edited body");

  /* --- Enter key saves --- */
  notes_add("Extra", "ex");
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_CHAR;
  ev.ch = '\n';
  check_int("file: Enter event handled", notes_handle_event(&ev), 1);
  notes_reset();
  notes_load();
  check_int("file: Enter saved the new note", notes_count, 2);
  check_str("file: Enter-saved note title", notes[1].title, "Extra");

  /* --- menu Save item --- */
  notes_add("Menu", "mm");
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MENU;
  ev.menu = 0;
  ev.item = 3;
  check_int("file: menu Save handled", notes_handle_event(&ev), 1);
  notes_reset();
  notes_load();
  check_int("file: menu Save persisted the note", notes_count, 3);
  check_str("file: menu-saved note title", notes[2].title, "Menu");

  /* --- 'r' reloads from disk --- */
  write_file(SCRATCH_FILE, "### Zulu\nzzz\n");
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_CHAR;
  ev.ch = 'r';
  check_int("file: 'r' event handled", notes_handle_event(&ev), 1);
  check_int("file: 'r' reloaded state", notes_count, 1);
  check_str("file: 'r' loaded new title", notes[0].title, "Zulu");
  check_str("file: 'r' loaded new body", notes[0].body, "zzz");

  /* --- save failure: NOTES.TXT is a directory --- */
  notes_add("Keep", "k");
  n = notes_count;
  check_int("warning: NOTES.TXT made inaccessible",
            system("rm -f " SCRATCH_FILE "; mkdir " SCRATCH_FILE), 0);
  check_int("warning: load fails -> 0", notes_load(), 0);
  check_int("warning: load failure raises the flag", notes_warn, 1);
  check_int("warning: save fails -> 0", notes_save(), 0);
  check_int("warning: save failure raises the flag", notes_warn, 1);
  check_int("warning: data kept in memory", notes_count, n);
  render_to(cap, sizeof cap);
  check(line_eq(cap, 2, "WARNING: cannot access NOTES.TXT - data kept in memory"),
        "warning: rendered warning line while save fails");
  check(line_eq(cap, 3, "  Zulu"), "warning: list still rendered while failing");

  check_int("warning: NOTES.TXT restored",
            system("rmdir " SCRATCH_FILE), 0);
  check_int("warning: save succeeds again", notes_save(), 1);
  check_int("warning: flag cleared after recovery", notes_warn, 0);
  notes_reset();
  check_int("warning: reload after recovery", notes_load(), 1);
  check_int("warning: recovered file holds all notes", notes_count, n);
}

/* ================================================================== */
/* Runner                                                             */
/* ================================================================== */

int main(void) {
  printf("=== Notes Host Test ===\n");
  printf("Testing note parsing, persistence, editing and rendering...\n");

  file_env_setup();
  if (!file_env_ok) {
    printf("NOTE: falling back to read-only logic tests (no scratch dir)\n");
  }

  test_init();
  test_parse();
  test_serialize();
  test_mutations();
  test_events();
  test_preview();
  test_render();
  test_files();

  printf("\n=== Test Results ===\n");
  printf("Checks run:    %d\n", checks_run);
  printf("Checks passed: %d\n", checks_run - checks_failed);
  printf("Checks failed: %d\n", checks_failed);

  /* Leave no residue behind. */
  if (file_env_ok) {
    syscall(SYS_chdir, "/");
    system("rm -rf " SCRATCH_DIR);
  }
  remove(CAP_PATH);

  if (checks_failed == 0 && checks_run > 0) {
    printf("\nALL TESTS PASSED\n");
    return 0;
  }
  printf("\n%d CHECK(S) FAILED\n", checks_failed);
  return 1;
}
