/*
 * find_test.c - Host unit tests for src/user/find.c (HobbyOS "Find" app).
 *
 * The app source is included directly (entry point renamed to find_app_main)
 * so the tests drive the real state struct and helpers, and render into a
 * buffer instead of stdout.
 *
 * Covered here: name matching (case-insensitivity, empty needle, needle
 * longer than name, match in the middle), query editing + cap, result
 * storage + the 60-result cap, path building for root and nested
 * directories, the 300-entry scan cap and truncation flags, status-line
 * formatting for 0/1/many results, the scanned counter, selection clamping,
 * event handling (typing, Enter, Bksp, 'c', menus, arrows, mouse) and the
 * full render (layout, row formatting, screen budget, determinism).
 *
 * NOT covered on the host: the recursive descent and the chdir/restore
 * cycle - compat.c's read_dir() returns a fixed 7-file list (EDITOR.BIN,
 * DESKTOP.BIN, SH.BIN, LS.BIN, CAT.BIN, TEST.TXT, NOTES.TXT) with no
 * subdirectories and attr == 0.  find.c's header documents the in-OS
 * verification path for the descent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#define main find_app_main
#include "../user/find.c"
#undef main

/* ---- check framework ---- */

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

/* Copy text line `idx` (0-based) of buf into out (cap bytes, NUL-terminated);
 * returns its length.  Used to check the fixed screen layout. */
static int get_line(const char *buf, int idx, char *out, int cap) {
  int line = 0;
  int i = 0;
  int j = 0;
  while (buf[i] != '\0' && line < idx) {
    if (buf[i] == '\n') line++;
    i++;
  }
  while (buf[i] != '\0' && buf[i] != '\n' && j < cap - 1) out[j++] = buf[i++];
  out[j] = '\0';
  return j;
}

/* ---- little builders ---- */

static void set_query(const char *q) {
  find_clear(&FIND);
  for (int i = 0; q[i] != '\0'; i++) find_query_append(&FIND, q[i]);
}

static struct gui_event ev_char(int ch) {
  struct gui_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = GUI_EV_CHAR;
  ev.ch = ch;
  return ev;
}

static struct gui_event ev_arrow(int type) {
  struct gui_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  return ev;
}

static struct gui_event ev_menu(int menu, int item) {
  struct gui_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = GUI_EV_MENU;
  ev.menu = menu;
  ev.item = item;
  return ev;
}

static struct gui_event ev_mouse(int x, int y, int button) {
  struct gui_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = GUI_EV_MOUSE;
  ev.x = x; ev.y = y; ev.button = button;
  return ev;
}

/* ====================================================================== */
int main(void) {
  printf("find_test: host unit tests for the HobbyOS Find app\n\n");

  /* ---- 1. find_is_dot ------------------------------------------- */
  check(find_is_dot(".") == 1, "is_dot: \".\" is a dot entry");
  check(find_is_dot("..") == 1, "is_dot: \"..\" is a dot entry");
  check(find_is_dot(".hidden") == 0, "is_dot: \".hidden\" is not");
  check(find_is_dot("A.B") == 0, "is_dot: \"A.B\" is not");
  check(find_is_dot("") == 0, "is_dot: empty name is not");

  /* ---- 2. find_name_matches (case-insensitive substring) -------- */
  check(find_name_matches("NOTES.TXT", "NOTES") == 1, "match: exact prefix");
  check(find_name_matches("notes.txt", "NOTES") == 1, "match: needle upper, name lower");
  check(find_name_matches("NOTES.TXT", "notes") == 1, "match: needle lower, name upper");
  check(find_name_matches("notes.txt", "NoTeS") == 1, "match: mixed case");
  check(find_name_matches("TEST.TXT", "TES") == 1, "match: prefix substring");
  check(find_name_matches("TEST.TXT", "T.T") == 1, "match: middle substring with dot");
  check(find_name_matches("NOTES.TXT", "S.T") == 1, "match: dot substring in second name");
  check(find_name_matches("NOTES.TXT", "TES") == 1, "match: substring straddling");
  check(find_name_matches("TEST.TXT", "TEST.TXT") == 1, "match: needle == name");
  check(find_name_matches("TEST.TXT", "ZZZ") == 0, "match: absent fragment");
  check(find_name_matches("TEST.TXT", "") == 0, "match: empty needle -> no match (documented)");
  check(find_name_matches("TEST.TXT", "TEST.TXTX") == 0, "match: needle longer than name");
  check(find_name_matches("", "A") == 0, "match: empty name");
  check(find_name_matches("TEST.TXT", "test.txt.z") == 0, "match: needle longer, trailing");
  check(find_name_matches("TEST.TXT", " T") == 0, "match: space is not in the name");

  /* ---- 3. find_join_path ---------------------------------------- */
  {
    char p[FIND_PATH_MAX + 8];
    check(find_join_path(p, FIND_PATH_MAX, "/", "A.TXT") == 6 &&
          s_eq(p, "/A.TXT"), "join: root -> \"/NAME\"");
    check(find_join_path(p, FIND_PATH_MAX, "", "A.TXT") == 6 &&
          s_eq(p, "/A.TXT"), "join: empty dir -> \"/NAME\"");
    check(find_join_path(p, FIND_PATH_MAX, "/home", "A.TXT") == 11 &&
          s_eq(p, "/home/A.TXT"), "join: nested -> \"dir/NAME\"");
    check(find_join_path(p, FIND_PATH_MAX, "/home/", "A.TXT") == 11 &&
          s_eq(p, "/home/A.TXT"), "join: trailing slash adds no double slash");
    check(find_join_path(p, FIND_PATH_MAX, "/DOCS/SUB", "N.TXT") == 15 &&
          s_eq(p, "/DOCS/SUB/N.TXT"), "join: two levels deep");

    /* Truncation: exactly cap-1 chars, NUL-terminated, no overflow. */
    char big[600];
    big[0] = '/';
    for (int i = 1; i < 500; i++) big[i] = 'x';
    big[500] = '\0';
    for (int i = 0; i < FIND_PATH_MAX + 8; i++) p[i] = '#';
    int n = find_join_path(p, FIND_PATH_MAX, big, "TAIL.TXT");
    check(n == FIND_PATH_MAX - 1, "join: long path truncated to cap-1");
    check(p[FIND_PATH_MAX - 1] == '\0', "join: truncated path is NUL-terminated");
    check((int)strlen(p) == FIND_PATH_MAX - 1, "join: truncated path length");
    check(p[FIND_PATH_MAX] == '#', "join: no write past the cap (canary)");
  }

  /* ---- 4. query editing ----------------------------------------- */
  {
    find_init(&FIND);
    check(FIND.query_len == 0 && FIND.query[0] == '\0', "query: init empty");
    check(find_query_append(&FIND, 'A') == 1 && FIND.query_len == 1 &&
          s_eq(FIND.query, "A"), "query: append one char");
    check(find_query_append(&FIND, 'b') == 1 && s_eq(FIND.query, "Ab"),
          "query: append keeps case");
    check(find_query_backspace(&FIND) == 1 && s_eq(FIND.query, "A") &&
          FIND.query_len == 1, "query: backspace removes last char");
    check(find_query_backspace(&FIND) == 1 && FIND.query_len == 0 &&
          s_eq(FIND.query, ""), "query: backspace to empty");
    check(find_query_backspace(&FIND) == 0 && FIND.query_len == 0,
          "query: backspace on empty is a no-op");

    /* Cap at 40. */
    find_clear(&FIND);
    {
      int all_ok = 1;
      for (int i = 0; i < FIND_QUERY_MAX; i++) {
        if (find_query_append(&FIND, 'q') != 1) all_ok = 0;
      }
      check(all_ok, "query: appends accepted up to the cap");
    }
    check(FIND.query_len == FIND_QUERY_MAX, "query: length == cap (40)");
    check(find_query_append(&FIND, 'X') == 0, "query: append beyond cap rejected");
    check(FIND.query_len == FIND_QUERY_MAX, "query: length still cap after reject");
    check(FIND.query[FIND_QUERY_MAX] == '\0', "query: NUL terminator at cap");
    check(FIND.query[0] == 'q' && FIND.query[FIND_QUERY_MAX - 1] == 'q',
          "query: content intact at the cap");
  }

  /* ---- 5. find_add_result + result cap --------------------------- */
  {
    find_clear(&FIND);
    check(find_add_result(&FIND, "/home/A.TXT", 123, 0) == 0 &&
          FIND.nresults == 1, "result: add one");
    check(s_eq(FIND.results[0].path, "/home/A.TXT") &&
          FIND.results[0].size == 123 && FIND.results[0].is_dir == 0,
          "result: fields stored");
    check(find_add_result(&FIND, "/home/D", 0, 1) == 0 &&
          FIND.results[1].is_dir == 1, "result: dir flag stored");

    find_clear(&FIND);
    for (int i = 0; i < FIND_MAX_RESULTS; i++)
      find_add_result(&FIND, "/home/PAD.DAT", 0, 0);
    check(FIND.nresults == FIND_MAX_RESULTS, "result: fills to cap (60)");
    check(find_add_result(&FIND, "/home/OVER.DAT", 0, 0) == -1,
          "result: add beyond cap rejected");
    check(FIND.nresults == FIND_MAX_RESULTS, "result: count stays at cap");
  }

  /* ---- 6. find_clear resets + selection clamp -------------------- */
  {
    find_init(&FIND);
    find_query_append(&FIND, 'X');
    find_add_result(&FIND, "/home/A.TXT", 0, 0);
    FIND.searched = 1;
    FIND.scanned = 42;
    FIND.trunc_scan = 1;
    FIND.trunc_results = 1;
    FIND.list.selected = 5;
    FIND.list.top = 3;
    FIND.list.count = 9;
    find_clear(&FIND);
    check(FIND.query_len == 0 && FIND.searched == 0 && FIND.nresults == 0,
          "clear: query/results cleared");
    check(FIND.scanned == 0 && FIND.trunc_scan == 0 && FIND.trunc_results == 0,
          "clear: counters and truncation flags cleared");
    check(FIND.list.selected == 0 && FIND.list.top == 0 && FIND.list.count == 0,
          "clear: selection/top/count clamped to 0");
    check(FIND.list.visible == FIND_VISIBLE, "clear: visible set to 16");
    find_init(&FIND);
    check(FIND.query_len == 0 && FIND.nresults == 0 && FIND.list.count == 0,
          "init: same as clear");
  }

  /* ---- 7. search over the mock tree ------------------------------ */
  {
    set_query("TXT");
    FIND.searched = 0;
    find_search_from(&FIND, "/home");
    check(FIND.searched == 1, "search: marked as searched");
    check(FIND.scanned == 7, "search: scanned all 7 mock entries");
    check(FIND.nresults == 2, "search: 2 hits for TXT");
    check(s_eq(FIND.results[0].path, "/home/TEST.TXT") &&
          s_eq(FIND.results[1].path, "/home/NOTES.TXT"),
          "search: hits keep read_dir order with full paths");
    check(FIND.results[0].is_dir == 0 && FIND.results[1].is_dir == 0,
          "search: mock entries are files");
    check(FIND.trunc_scan == 0 && FIND.trunc_results == 0,
          "search: no truncation on the mock tree");
    check(FIND.list.count == 2 && FIND.list.selected == 0 && FIND.list.top == 0,
          "search: list synced to results");
  }
  {
    set_query("editor");
    find_search_from(&FIND, "/home");
    check(FIND.nresults == 1 && s_eq(FIND.results[0].path, "/home/EDITOR.BIN"),
          "search: case-insensitive hit for \"editor\"");
  }
  {
    set_query("BIN");
    find_search_from(&FIND, "/home");
    check(FIND.nresults == 5 && FIND.scanned == 7,
          "search: 5 hits for BIN, same scanned count");
  }
  {
    set_query("E");
    find_search_from(&FIND, "/home");
    check(FIND.nresults == 4, "search: 4 hits for E");
  }
  {
    set_query("ZZZ");
    find_search_from(&FIND, "/home");
    check(FIND.searched == 1 && FIND.nresults == 0 && FIND.scanned == 7,
          "search: zero hits still searched the tree");
    check(FIND.list.count == 0 && FIND.list.selected == 0,
          "search: empty result list is safe");
  }
  {
    /* Root base: paths must be "/NAME". */
    set_query("TXT");
    find_search_from(&FIND, "/");
    check(FIND.nresults == 2 && s_eq(FIND.results[0].path, "/TEST.TXT") &&
          s_eq(FIND.results[1].path, "/NOTES.TXT"),
          "search: root base builds \"/NAME\" paths");
    /* NULL/empty base falls back to "/". */
    set_query("TXT");
    find_search_from(&FIND, 0);
    check(FIND.nresults == 2 && s_eq(FIND.results[0].path, "/TEST.TXT"),
          "search: NULL base falls back to root");
    set_query("TXT");
    find_search_from(&FIND, "");
    check(FIND.nresults == 2, "search: empty base falls back to root");
  }
  {
    /* Empty query: no walk at all (documented decision). */
    set_query("");
    FIND.searched = 1;
    find_search_from(&FIND, "/home");
    check(FIND.searched == 0 && FIND.nresults == 0 && FIND.scanned == 0,
          "search: empty query performs no walk");
  }
  {
    /* find_run_search() end-to-end: uses the (mocked) cwd. */
    chdir("/home");
    set_query("TXT");
    find_run_search(&FIND);
    check(FIND.nresults == 2 && s_eq(FIND.results[0].path, "/home/TEST.TXT"),
          "search: run_search uses getcwd as the base");
    check(s_eq(FIND.query, "TXT"), "search: query untouched by the walk");
  }

  /* ---- 8. truncation flags --------------------------------------- */
  {
    /* Result cap: 59 pre-filled + first mock hit -> 60, then stop. */
    find_clear(&FIND);
    for (int i = 0; i < FIND_MAX_RESULTS - 1; i++)
      find_add_result(&FIND, "/home/PAD.DAT", 0, 0);
    for (int i = 0; "TXT"[i] != '\0'; i++) find_query_append(&FIND, "TXT"[i]);
    find_walk(&FIND, "/home", 0);
    check(FIND.nresults == FIND_MAX_RESULTS && FIND.trunc_results == 1,
          "trunc: 59 pre-filled + hits -> capped at 60, flagged");
    check(FIND.trunc_scan == 0, "trunc: result cap does not set scan flag");
  }
  {
    /* Result table already full: first hit stops the walk. */
    find_clear(&FIND);
    for (int i = 0; i < FIND_MAX_RESULTS; i++)
      find_add_result(&FIND, "/home/PAD.DAT", 0, 0);
    for (int i = 0; "TXT"[i] != '\0'; i++) find_query_append(&FIND, "TXT"[i]);
    find_walk(&FIND, "/home", 0);
    check(FIND.nresults == FIND_MAX_RESULTS && FIND.trunc_results == 1,
          "trunc: full table + hit -> count stays 60, flagged");
    check(FIND.scanned == 6, "trunc: walk stopped at the first overflowing hit");
  }
  {
    /* Scan cap: 299 pre-scanned, first entry pushes it to 300. */
    find_clear(&FIND);
    for (int i = 0; "EDITOR"[i] != '\0'; i++) find_query_append(&FIND, "EDITOR"[i]);
    FIND.scanned = FIND_MAX_SCAN - 1;
    find_walk(&FIND, "/home", 0);
    check(FIND.scanned == FIND_MAX_SCAN, "trunc: scanned stops at 300");
    check(FIND.trunc_scan == 1, "trunc: scan cap flagged when more entries exist");
    check(FIND.nresults == 1 && s_eq(FIND.results[0].path, "/home/EDITOR.BIN"),
          "trunc: entry that hit the cap was still matched");
    check(FIND.trunc_results == 0, "trunc: scan cap does not set result flag");
  }
  {
    /* Scan cap with no match: still truncated, no results. */
    find_clear(&FIND);
    for (int i = 0; "ZZZ"[i] != '\0'; i++) find_query_append(&FIND, "ZZZ"[i]);
    FIND.scanned = FIND_MAX_SCAN - 1;
    find_walk(&FIND, "/home", 0);
    check(FIND.scanned == FIND_MAX_SCAN && FIND.trunc_scan == 1 &&
          FIND.nresults == 0, "trunc: scan cap without hits");
  }

  /* ---- 9. status line formatting --------------------------------- */
  {
    char st[160];
    find_clear(&FIND);
    find_format_status(st, sizeof(st), &FIND);
    check(s_eq(st, "Results: none yet"), "status: before any search");

    set_query("TEST");
    find_search_from(&FIND, "/home");
    find_format_status(st, sizeof(st), &FIND);
    check(s_eq(st, "Results: 1  (scanned 7 entries)"), "status: 1 result");

    set_query("TXT");
    find_search_from(&FIND, "/home");
    find_format_status(st, sizeof(st), &FIND);
    check(s_eq(st, "Results: 2  (scanned 7 entries)"), "status: 2 results");

    set_query("ZZZ");
    find_search_from(&FIND, "/home");
    find_format_status(st, sizeof(st), &FIND);
    check(s_eq(st, "Results: 0  (scanned 7 entries)"), "status: 0 results");

    /* Truncation notes (state forced: cheap and exact). */
    FIND.searched = 1;
    FIND.nresults = 1;
    FIND.scanned = FIND_MAX_SCAN;
    FIND.trunc_scan = 1;
    FIND.trunc_results = 0;
    find_format_status(st, sizeof(st), &FIND);
    check(s_eq(st, "Results: 1  (scanned 300 entries, stopped at 300)"),
          "status: scan truncation note");

    FIND.nresults = FIND_MAX_RESULTS;
    FIND.scanned = 187;
    FIND.trunc_scan = 0;
    FIND.trunc_results = 1;
    find_format_status(st, sizeof(st), &FIND);
    check(s_eq(st, "Results: 60  (scanned 187 entries, first 60 kept)"),
          "status: result truncation note");
  }

  /* ---- 10. row formatting ---------------------------------------- */
  {
    char row[FIND_ROW_W + 1];
    struct find_result r;
    memset(&r, 0, sizeof(r));

    gui_strncpy(r.path, "/home/DOCS", FIND_PATH_MAX);
    r.size = 4096; r.is_dir = 1;
    int n = find_format_row(row, sizeof(row), &r, 0);
    check(n == FIND_ROW_W, "row: dir row is exactly 66 chars");
    check(strncmp(row, "  [D] /home/DOCS ", 17) == 0, "row: dir prefix \"  [D] \" + path");
    check(s_eq(row + FIND_ROW_W - 10, "       DIR"), "row: dir size field is right-aligned DIR");

    gui_strncpy(r.path, "/home/TEST.TXT", FIND_PATH_MAX);
    r.size = 0; r.is_dir = 0;
    n = find_format_row(row, sizeof(row), &r, 0);
    check(n == FIND_ROW_W && strncmp(row, "      /home/TEST.TXT ", 20) == 0,
          "row: file prefix is 4 blanks + path");
    check(s_eq(row + FIND_ROW_W - 10, "        0B"), "row: 0-byte file shows \"0B\"");

    r.size = 1536;
    find_format_row(row, sizeof(row), &r, 0);
    check(s_eq(row + FIND_ROW_W - 10, "      1.5K"), "row: size 1536 -> \"1.5K\"");

    r.size = 1024 * 1024;
    find_format_row(row, sizeof(row), &r, 0);
    check(s_eq(row + FIND_ROW_W - 10, "      1.0M"), "row: size 1 MiB -> \"1.0M\"");

    r.size = 0; r.is_dir = 0;
    find_format_row(row, sizeof(row), &r, 1);
    check(row[0] == '>' && row[1] == ' ', "row: selected row prefixed \"> \"");
    check(row[2] == ' ' && row[3] == ' ', "row: selected file keeps 4-blank tag");

    /* Clipping: 80-char path -> "..." + the last 47 chars. */
    char longpath[128];
    int ln = 0;
    for (int i = 0; "/home/"[i] != '\0'; i++) longpath[ln++] = "/home/"[i];
    while (ln < 80) { longpath[ln] = (char)('0' + (ln % 10)); ln++; }
    longpath[ln] = '\0';
    memset(&r, 0, sizeof(r));
    gui_strncpy(r.path, longpath, FIND_PATH_MAX);
    r.size = 12; r.is_dir = 0;
    n = find_format_row(row, sizeof(row), &r, 0);
    check(n == FIND_ROW_W, "row: clipped row still 66 chars");
    check(row[6] == '.' && row[7] == '.' && row[8] == '.',
          "row: over-long path starts with \"...\"");
    {
      char tail[48];
      int plen = (int)strlen(longpath);
      memcpy(tail, longpath + (plen - 47), 47);
      tail[47] = '\0';
      check(memcmp(row + 9, tail, 47) == 0, "row: clipped path keeps its tail");
    }

    /* Exactly 50 chars of path: no clipping. */
    char exact[64];
    int en = 0;
    while (en < 50) { exact[en] = (char)('a' + (en % 26)); en++; }
    exact[50] = '\0';
    memset(&r, 0, sizeof(r));
    gui_strncpy(r.path, exact, FIND_PATH_MAX);
    find_format_row(row, sizeof(row), &r, 0);
    check(memcmp(row + 6, exact, 50) == 0, "row: 50-char path fits without clipping");
  }

  /* ---- 11. full render ------------------------------------------- */
  {
    static char buf[FIND_SCREEN_MAX];
    static char buf2[FIND_SCREEN_MAX];

    find_clear(&FIND);
    int len = find_render(buf, sizeof(buf), &FIND);
    check(len > 0 && len < FIND_SCREEN_MAX && (int)strlen(buf) == len,
          "render: NUL-terminated within the buffer");
    check(len < 1800, "render: one screen stays under the 1800-char budget");
    check(strncmp(buf, "=== Find ===\n", 13) == 0, "render: line 1 is \"=== Find ===\"");
    check(has(buf, "Search: _  (type a name fragment, Enter=search)"),
          "render: empty query shows the placeholder hint");
    check(has(buf, "Results: none yet\n"), "render: line 3 before a search");
    check(has(buf, "type=edit  Enter=search  Bksp=edit  c=clear  arrows=scroll\n"),
          "render: key legend line present");
    {
      int lines = 0;
      int maxlen = 0;
      int cur = 0;
      for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
          lines++;
          if (cur > maxlen) maxlen = cur;
          cur = 0;
        } else {
          cur++;
        }
      }
      check(lines == 21, "render: 21 lines (3 header + 16 rows + blank + legend)");
      check(maxlen <= 110, "render: no line longer than 110 chars");
    }

    set_query("TXT");
    find_search_from(&FIND, "/home");
    len = find_render(buf, sizeof(buf), &FIND);
    check(has(buf, "Search: TXT_\n"), "render: query echoed with cursor");
    check(has(buf, "Results: 2  (scanned 7 entries)\n"), "render: status after a search");

    /* Rows sit in fixed positions: content line 3 = result 0. */
    {
      char expect[FIND_ROW_W + 2];
      char line3[FIND_ROW_W + 2];
      find_format_row(expect, sizeof(expect), &FIND.results[0], 1);
      check(has(buf, expect), "render: selected first row");
      find_format_row(expect, sizeof(expect), &FIND.results[1], 0);
      check(has(buf, expect), "render: second row not selected");
      find_format_row(expect, sizeof(expect), &FIND.results[0], 1);
      get_line(buf, 3, line3, sizeof(line3));
      check(s_eq(line3, expect), "render: content line 3 is the selected hit");
      check(get_line(buf, 20, line3, sizeof(line3)) > 0 &&
            line3[0] == 't', "render: content line 20 is the legend");
    }

    /* Determinism: same state -> byte-identical render. */
    int len2 = find_render(buf2, sizeof(buf2), &FIND);
    check(len == len2 && memcmp(buf, buf2, len) == 0,
          "render: deterministic for identical state");
  }

  /* ---- 12. scrolling / selection --------------------------------- */
  {
    /* 2 results: clamps at both ends. */
    set_query("TXT");
    find_search_from(&FIND, "/home");
    {
      struct gui_event e;
      e = ev_arrow(GUI_EV_DOWN);
      check(find_handle_event(&FIND, &e) == 1 && FIND.list.selected == 1,
            "scroll: DOWN selects second result");
      check(find_handle_event(&FIND, &e) == 1 && FIND.list.selected == 1,
            "scroll: DOWN clamps at the last result");
      e = ev_arrow(GUI_EV_UP);
      check(find_handle_event(&FIND, &e) == 1 && FIND.list.selected == 0,
            "scroll: UP selects first result");
      check(find_handle_event(&FIND, &e) == 1 && FIND.list.selected == 0,
            "scroll: UP clamps at the first result");
    }

    /* No results: arrows must not move or crash. */
    find_clear(&FIND);
    {
      struct gui_event e = ev_arrow(GUI_EV_DOWN);
      check(find_handle_event(&FIND, &e) == 1 && FIND.list.selected == 0 &&
            FIND.list.top == 0, "scroll: DOWN with empty list stays at 0");
    }

    /* 20 synthetic results: scrolling keeps the selection visible. */
    find_clear(&FIND);
    for (int i = 0; i < 20; i++) {
      char name[24];
      snprintf(name, sizeof(name), "/home/P%02d.DAT", i);
      find_add_result(&FIND, name, (uint32_t)i, 0);
    }
    FIND.list.count = FIND.nresults;
    gui_list_ensure_visible(&FIND.list);
    {
      struct gui_event e = ev_arrow(GUI_EV_DOWN);
      for (int i = 0; i < 20; i++) find_handle_event(&FIND, &e);
      check(FIND.list.selected == 19, "scroll: 20 DOWNs reach the last item");
      check(FIND.list.top == 4, "scroll: top follows (19-16+1)");
    }
    {
      static char buf[FIND_SCREEN_MAX];
      char expect[FIND_ROW_W + 2];
      char line3[FIND_ROW_W + 2];
      char line18[FIND_ROW_W + 2];
      find_render(buf, sizeof(buf), &FIND);
      find_format_row(expect, sizeof(expect), &FIND.results[4], 0);
      get_line(buf, 3, line3, sizeof(line3));
      check(s_eq(line3, expect), "scroll: first visible row is item 4");
      find_format_row(expect, sizeof(expect), &FIND.results[19], 1);
      get_line(buf, 18, line18, sizeof(line18));
      check(s_eq(line18, expect), "scroll: last visible row is selected item 19");
    }
  }

  /* ---- 13. events: typing / Enter / backspace / clear ------------ */
  {
    find_clear(&FIND);
    struct gui_event e;
    e = ev_char('a');
    check(find_handle_event(&FIND, &e) == 1 && s_eq(FIND.query, "a"),
          "event: printable appends");
    e = ev_char(' ');
    check(find_handle_event(&FIND, &e) == 1 && s_eq(FIND.query, "a "),
          "event: space is a printable query char");
    e = ev_char('~');
    check(find_handle_event(&FIND, &e) == 1 && s_eq(FIND.query, "a ~"),
          "event: highest printable char appends");
    e = ev_char(9);
    check(find_handle_event(&FIND, &e) == 0 && s_eq(FIND.query, "a ~"),
          "event: tab is ignored");
    e = ev_char(31);
    check(find_handle_event(&FIND, &e) == 0 && s_eq(FIND.query, "a ~"),
          "event: control char is ignored");

    e = ev_char(8);
    check(find_handle_event(&FIND, &e) == 1 && s_eq(FIND.query, "a "),
          "event: backspace (8) edits");
    e = ev_char(127);
    check(find_handle_event(&FIND, &e) == 1 && s_eq(FIND.query, "a"),
          "event: DEL (127) edits");

    /* 'c' clears (documented shortcut); 'C' still types. */
    e = ev_char('c');
    check(find_handle_event(&FIND, &e) == 1 && FIND.query_len == 0 &&
          FIND.searched == 0, "event: 'c' clears query + results");
    e = ev_char('C');
    check(find_handle_event(&FIND, &e) == 1 && s_eq(FIND.query, "C"),
          "event: 'C' types (clear shortcut is lowercase only)");

    /* Enter searches. */
    find_clear(&FIND);
    for (int i = 0; "TXT"[i] != '\0'; i++) {
      e = ev_char("TXT"[i]);
      find_handle_event(&FIND, &e);
    }
    e = ev_char('\n');
    check(find_handle_event(&FIND, &e) == 1 && FIND.searched == 1 &&
          FIND.nresults == 2, "event: Enter runs the search");
    e = ev_char('\r');
    check(find_handle_event(&FIND, &e) == 1 && FIND.searched == 1,
          "event: CR also runs the search");

    /* Empty query + Enter = no walk, keeps "none yet". */
    find_clear(&FIND);
    e = ev_char('\n');
    check(find_handle_event(&FIND, &e) == 1 && FIND.searched == 0 &&
          FIND.nresults == 0, "event: Enter with empty query does not walk");

    /* Typing past the cap through events. */
    find_clear(&FIND);
    e = ev_char('z');
    for (int i = 0; i < 45; i++) find_handle_event(&FIND, &e);
    check(FIND.query_len == FIND_QUERY_MAX, "event: query caps at 40 via events");

    /* ESC and sideways arrows are ignored. */
    e = ev_arrow(GUI_EV_ESC);
    check(find_handle_event(&FIND, &e) == 0, "event: ESC ignored");
    e = ev_arrow(GUI_EV_LEFT);
    check(find_handle_event(&FIND, &e) == 0, "event: LEFT ignored");
    e = ev_arrow(GUI_EV_RIGHT);
    check(find_handle_event(&FIND, &e) == 0, "event: RIGHT ignored");
  }

  /* ---- 14. events: menus ----------------------------------------- */
  {
    find_clear(&FIND);
    struct gui_event e;
    for (int i = 0; "TXT"[i] != '\0'; i++) {
      e = ev_char("TXT"[i]);
      find_handle_event(&FIND, &e);
    }
    e = ev_menu(0, 0);            /* Find > Search */
    check(find_handle_event(&FIND, &e) == 1 && FIND.searched == 1 &&
          FIND.nresults == 2, "menu: Find>Search runs the search");

    e = ev_menu(0, 1);            /* Find > Clear */
    check(find_handle_event(&FIND, &e) == 1 && FIND.query_len == 0 &&
          FIND.nresults == 0 && FIND.searched == 0,
          "menu: Find>Clear clears everything");

    /* Re-populate, then make sure other menus/items change nothing. */
    for (int i = 0; "TXT"[i] != '\0'; i++) {
      e = ev_char("TXT"[i]);
      find_handle_event(&FIND, &e);
    }
    e = ev_menu(0, 0);
    find_handle_event(&FIND, &e);
    e = ev_menu(1, 0);
    check(find_handle_event(&FIND, &e) == 0 && FIND.nresults == 2,
          "menu: foreign menu index ignored");
    e = ev_menu(0, 5);
    check(find_handle_event(&FIND, &e) == 0 && FIND.nresults == 2,
          "menu: out-of-range item ignored");
  }

  /* ---- 15. events: mouse ----------------------------------------- */
  {
    set_query("TXT");
    find_search_from(&FIND, "/home");
    struct gui_event e;

    e = ev_mouse(10, 4, 1);       /* row 4 = second result */
    check(find_handle_event(&FIND, &e) == 1 && FIND.list.selected == 1,
          "mouse: left click on row 4 selects result 1");
    e = ev_mouse(10, 3, 1);       /* row 3 = first result */
    check(find_handle_event(&FIND, &e) == 1 && FIND.list.selected == 0,
          "mouse: left click on row 3 selects result 0");
    e = ev_mouse(10, 19, 1);      /* blank row past the results */
    check(find_handle_event(&FIND, &e) == 0 && FIND.list.selected == 0,
          "mouse: click on a blank row is ignored");
    e = ev_mouse(10, 3, 2);       /* right button */
    check(find_handle_event(&FIND, &e) == 0, "mouse: right click ignored");
  }

  /* ---- 16. one full interaction transcript ----------------------- */
  {
    static char buf[FIND_SCREEN_MAX];
    struct gui_event e;
    find_init(&FIND);
    for (int i = 0; "notes"[i] != '\0'; i++) {
      e = ev_char("notes"[i]);
      find_handle_event(&FIND, &e);
    }
    e = ev_char('\n');
    find_handle_event(&FIND, &e);
    check(FIND.nresults == 1 && s_eq(FIND.results[0].path, "/home/NOTES.TXT"),
          "e2e: type \"notes\" + Enter finds NOTES.TXT");
    {
      char expect[FIND_ROW_W + 2];
      find_format_row(expect, sizeof(expect), &FIND.results[0], 1);
      find_render(buf, sizeof(buf), &FIND);
      check(has(buf, expect), "e2e: rendered screen shows the selected hit");
    }
    check(has(buf, "Results: 1  (scanned 7 entries)"),
          "e2e: rendered status line matches");
    e = ev_char('c');
    find_handle_event(&FIND, &e);
    find_render(buf, sizeof(buf), &FIND);
    check(has(buf, "Results: none yet") && has(buf, "Search: _"),
          "e2e: after 'c' the screen is back to the empty state");
  }

  /* ---- summary --------------------------------------------------- */
  printf("\nfind_test summary: %d checks run, %d failed\n", checks_run, checks_failed);
  if (checks_failed == 0) {
    printf("ALL TESTS PASSED\n");
    return 0;
  }
  printf("TESTS FAILED\n");
  return 1;
}
