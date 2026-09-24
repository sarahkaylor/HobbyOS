/*
 * find.c - Find (File finder) for HobbyOS.
 *
 * Windowed text app: search the CURRENT directory tree (recursively, at
 * most FIND_MAX_DEPTH levels below the cwd) for directory entries whose
 * NAME contains a typed fragment, case-insensitively.
 *
 * Interaction
 *   - Line 2 is an inline query box (single cursor at the end, cap 40).
 *   - Enter (or menu Find>Search) runs the search from getcwd().
 *   - 'c' (or menu Find>Clear) clears the query and the results.  'c' is
 *     the documented Clear shortcut, so it is consumed by the shortcut and
 *     is never typed into the query; uppercase 'C' still types normally so
 *     C-names remain searchable.
 *   - Up/Down (or a left click on a result row) scroll/select the list.
 *   - There is no Exit item (the desktop closes the window); 'q' is an
 *     ordinary query character, per the app conventions.
 *
 * Search semantics
 *   - Case-insensitive substring match on the entry NAME only (not path).
 *   - An EMPTY query never walks the tree (documented decision): the status
 *     line keeps saying "Results: none yet" and nothing is scanned.
 *   - At most FIND_MAX_SCAN (300) entries are examined and at most
 *     FIND_MAX_RESULTS (60) matches are kept; the status line notes the
 *     truncation ("stopped at 300" / "first 60 kept").
 *   - Directories are matched too and tagged [D]; a directory is only
 *     descended into while depth < FIND_MAX_DEPTH.
 *
 * Walk discipline (cwd-restore): find_walk() saves the cwd, chdir()s into
 * the directory it is about to read, reads the entries, recurses into
 * subdirectories (each recursive call saves/restores its own cwd), then
 * chdir()s back to the saved directory.  read_dir() returning -1 (end of
 * directory or error) simply ends the enumeration loop, so the walk is
 * always safe and the caller's cwd is always restored.
 *
 * IN-OS VERIFICATION PATH (host tests cannot cover the descent):
 *   compat.c's read_dir() returns a fixed 7-file list with attr == 0 and no
 *   subdirectories, so the host tests exercise matching, result storage,
 *   path building, counters, truncation flags and rendering directly, but
 *   never the recursive descent or the chdir/add-back cycle.  To verify
 *   those on a real machine: boot HobbyOS, create nested directories
 *   (mkdir /DOCS, mkdir /DOCS/SUB) with a file in each, launch `find` from
 *   the desktop, type a fragment that only exists two levels down and press
 *   Enter: the hit must be listed with its full /DOCS/SUB/NAME path, then
 *   `ls` in the shell must still show the original cwd (proving the walk
 *   restored it).
 *
 * Host unit tests: src/host/find_test.c includes this file directly.
 */

#include "libc.h"
#include "gui.h"

/* ---- Limits ---------------------------------------------------------- */

#define FIND_QUERY_MAX     40    /* query box cap (chars, excl. NUL)     */
#define FIND_PATH_MAX     120    /* path buffer cap (chars, excl. NUL)   */
#define FIND_MAX_SCAN     300    /* entries examined per search          */
#define FIND_MAX_RESULTS   60    /* matches kept per search              */
#define FIND_MAX_DEPTH      3    /* recursion depth below the cwd        */
#define FIND_VISIBLE       16    /* result rows on screen                */
#define FIND_ROW_W         66    /* rendered width of one result row     */
#define FIND_ROW_PATH_W    50    /* path field width inside a row        */
#define FIND_ROW_SIZE_W    10    /* right-aligned size field width       */
#define FIND_RESULTS_ROW    3    /* first content row holding a result   */
#define FIND_SCREEN_MAX  1900    /* render buffer (window text buf 2048) */

#define FIND_ATTR_DIR      0x10  /* FAT attribute bit: directory         */

/* ---- State ----------------------------------------------------------- */

struct find_result {
  char path[FIND_PATH_MAX];
  uint32_t size;
  int is_dir;
};

struct find_state {
  char query[FIND_QUERY_MAX + 1];
  int  query_len;
  int  searched;       /* 1 once a search ran (even a zero-hit one)    */
  int  nresults;
  int  scanned;        /* entries examined by the last search          */
  int  trunc_scan;     /* stopped because FIND_MAX_SCAN was hit        */
  int  trunc_results;  /* stopped because FIND_MAX_RESULTS was hit     */
  struct find_result results[FIND_MAX_RESULTS];
  struct gui_list list;
};

static struct find_state FIND;

/* ====================================================================== */
/* Name matching                                                          */
/* ====================================================================== */

/* True for the "." and ".." pseudo-entries (never descend into those). */
static int find_is_dot(const char *name) {
  if (name[0] != '.') return 0;
  if (name[1] == '\0') return 1;
  return name[1] == '.' && name[2] == '\0';
}

/* Case-insensitive substring match on the entry NAME only.
 * Documented decision: an empty needle matches NOTHING - an empty query
 * must not "match the whole tree" (find_search_from() short-circuits
 * before walking, so this is belt-and-braces). */
static int find_name_matches(const char *name, const char *needle) {
  if (needle == 0 || needle[0] == '\0') return 0;
  return gui_ci_find(name, needle) >= 0;
}

/* ====================================================================== */
/* Path building and results                                              */
/* ====================================================================== */

/* Build "<dir>/<name>" into dst (cap chars, always NUL-terminated).
 * A dir of "" or "/" yields "/<name>" (root), "/home" yields
 * "/home/<name>" and "/home/" yields "/home/<name>" (no double slash).
 * Paths longer than cap-1 chars are truncated.  Returns the length. */
static int find_join_path(char *dst, int cap, const char *dir, const char *name) {
  int j = 0;
  if (cap <= 0) return 0;
  int dlen = gui_strlen(dir);
  if (dlen == 0 || (dlen == 1 && dir[0] == '/')) {
    if (j < cap - 1) dst[j++] = '/';
  } else {
    for (int i = 0; i < dlen && j < cap - 1; i++) dst[j++] = dir[i];
    if (j > 0 && dst[j - 1] != '/' && j < cap - 1) dst[j++] = '/';
  }
  for (int i = 0; name[i] != '\0' && j < cap - 1; i++) dst[j++] = name[i];
  dst[j] = '\0';
  return j;
}

/* Append one match.  Returns 0, or -1 when the result table is full. */
static int find_add_result(struct find_state *st, const char *path,
                           uint32_t size, int is_dir) {
  if (st->nresults >= FIND_MAX_RESULTS) return -1;
  struct find_result *r = &st->results[st->nresults];
  gui_strncpy(r->path, path, FIND_PATH_MAX);
  r->size = size;
  r->is_dir = is_dir;
  st->nresults++;
  return 0;
}

/* ====================================================================== */
/* Query editing                                                          */
/* ====================================================================== */

static int find_query_append(struct find_state *st, int ch) {
  if (st->query_len >= FIND_QUERY_MAX) return 0;
  st->query[st->query_len++] = (char)ch;
  st->query[st->query_len] = '\0';
  return 1;
}

static int find_query_backspace(struct find_state *st) {
  if (st->query_len <= 0) return 0;
  st->query_len--;
  st->query[st->query_len] = '\0';
  return 1;
}

/* Clear the query AND the result list (selection/top/count clamp to 0). */
static void find_clear(struct find_state *st) {
  st->query[0] = '\0';
  st->query_len = 0;
  st->searched = 0;
  st->nresults = 0;
  st->scanned = 0;
  st->trunc_scan = 0;
  st->trunc_results = 0;
  st->list.selected = 0;
  st->list.top = 0;
  st->list.count = 0;
  st->list.visible = FIND_VISIBLE;
}

static void find_init(struct find_state *st) {
  find_clear(st);
}

/* ====================================================================== */
/* The walk                                                               */
/* ====================================================================== */

/* Recursively scan `dir` (an absolute path) for name matches.  `depth` is
 * the number of levels below the search root: the root is depth 0, and a
 * subdirectory is only descended into while depth < FIND_MAX_DEPTH, so
 * entries are examined at depths 0..FIND_MAX_DEPTH inclusive.
 *
 * cwd discipline: remember the cwd, chdir() into `dir`, enumerate, recurse,
 * then chdir() back.  Aborts cleanly (flags set) when either cap is hit,
 * and any read_dir() failure just ends the enumeration. */
static void find_walk(struct find_state *st, const char *dir, int depth) {
  char saved[FIND_PATH_MAX];
  if (getcwd(saved, sizeof(saved)) == 0) saved[0] = '\0';

  if (dir[0] != '\0' && chdir(dir) != 0) {
    /* Not enterable: skip this subtree, cwd unchanged. */
    return;
  }

  int index = 0;
  struct sys_dirent ent;
  while (read_dir(dir, index, &ent) == 0) {
    index++;
    if (ent.name[0] == '\0') break;            /* end of directory    */
    if (find_is_dot(ent.name)) continue;       /* "." / ".."          */

    st->scanned++;
    int is_dir = (ent.attr & FIND_ATTR_DIR) != 0;

    if (find_name_matches(ent.name, st->query)) {
      char path[FIND_PATH_MAX];
      find_join_path(path, FIND_PATH_MAX, dir, ent.name);
      if (find_add_result(st, path, ent.size, is_dir) != 0) {
        st->trunc_results = 1;             /* result table full   */
        goto out;
      }
    }

    if (st->scanned >= FIND_MAX_SCAN) {
      /* Peek: only report truncation if more entries really exist. */
      struct sys_dirent peek;
      if (read_dir(dir, index, &peek) == 0) st->trunc_scan = 1;
      goto out;
    }

    if (is_dir && depth < FIND_MAX_DEPTH) {
      char child[FIND_PATH_MAX];
      find_join_path(child, FIND_PATH_MAX, dir, ent.name);
      find_walk(st, child, depth + 1);
      if (st->trunc_scan || st->trunc_results) goto out;
    }
  }

out:
  if (saved[0] != '\0') chdir(saved);            /* restore the cwd     */
}

/* Run a search rooted at `base` (absolute path).  Resets the previous
 * results first.  Used by find_run_search() and by the host tests, which
 * pass an explicit base so they do not depend on the mocked cwd. */
static void find_search_from(struct find_state *st, const char *base) {
  st->nresults = 0;
  st->scanned = 0;
  st->trunc_scan = 0;
  st->trunc_results = 0;
  st->list.selected = 0;
  st->list.top = 0;
  st->list.count = 0;
  st->list.visible = FIND_VISIBLE;

  if (st->query_len <= 0) {
    /* Documented decision: an empty query does not walk the tree. */
    st->searched = 0;
    return;
  }
  st->searched = 1;
  if (base == 0 || base[0] == '\0') base = "/";
  find_walk(st, base, 0);

  st->list.count = st->nresults;
  gui_list_ensure_visible(&st->list);
}

/* Enter key / menu Find>Search: search the current directory tree. */
static void find_run_search(struct find_state *st) {
  char cwd[FIND_PATH_MAX];
  char *got = getcwd(cwd, sizeof(cwd));
  find_search_from(st, got != 0 ? cwd : "/");
}

/* ====================================================================== */
/* Rendering                                                              */
/* ====================================================================== */

/* Bounded appends; both keep `out` NUL-terminated and return the new len. */
static int find_put(char *out, int cap, int len, const char *s) {
  if (cap <= 0) return 0;
  for (int i = 0; s[i] != '\0' && len < cap - 1; i++) out[len++] = s[i];
  out[len] = '\0';
  return len;
}

static int find_putc(char *out, int cap, int len, int ch) {
  if (cap <= 0) return 0;
  if (len < cap - 1) out[len++] = (char)ch;
  out[len] = '\0';
  return len;
}

static int find_put_num(char *out, int cap, int len, long v) {
  char num[24];
  gui_itoa(v, num);
  return find_put(out, cap, len, num);
}

/* Render one result row: exactly FIND_ROW_W chars + NUL.
 * Layout: ["> "|"  "]["[D] "|"    "][path field 50][size field 10].
 * Long paths are clipped to "..." + tail; the size is gui_size_str() for
 * files and "DIR" for directories, right-aligned in the last 10 columns. */
static int find_format_row(char *out, int cap, const struct find_result *r,
                           int selected) {
  if (cap < FIND_ROW_W + 1) {
    if (cap > 0) out[0] = '\0';
    return 0;
  }
  int j = 0;
  out[j++] = selected ? '>' : ' ';
  out[j++] = ' ';
  if (r->is_dir) {
    out[j++] = '['; out[j++] = 'D'; out[j++] = ']'; out[j++] = ' ';
  } else {
    out[j++] = ' '; out[j++] = ' '; out[j++] = ' '; out[j++] = ' ';
  }

  int plen = gui_strlen(r->path);
  if (plen <= FIND_ROW_PATH_W) {
    for (int i = 0; i < plen; i++) out[j++] = r->path[i];
    for (int i = plen; i < FIND_ROW_PATH_W; i++) out[j++] = ' ';
  } else {
    int keep = FIND_ROW_PATH_W - 3;
    out[j++] = '.'; out[j++] = '.'; out[j++] = '.';
    for (int i = 0; i < keep; i++) out[j++] = r->path[plen - keep + i];
  }

  char sz[16];
  if (r->is_dir) gui_strcpy(sz, "DIR");
  else gui_size_str(r->size, sz);
  int slen = gui_strlen(sz);
  if (slen > FIND_ROW_SIZE_W) slen = FIND_ROW_SIZE_W;
  for (int i = slen; i < FIND_ROW_SIZE_W; i++) out[j++] = ' ';
  for (int i = 0; i < slen; i++) out[j++] = sz[i];

  out[j] = '\0';
  return j;                             /* == FIND_ROW_W               */
}

/* Line 2: the query box with the trailing cursor; empty query shows hint. */
static int find_format_search(char *out, int cap, const struct find_state *st) {
  int len = 0;
  len = find_put(out, cap, len, "Search: ");
  if (st->query_len == 0) {
    len = find_put(out, cap, len,
                   "_  (type a name fragment, Enter=search)");
  } else {
    for (int i = 0; i < st->query_len; i++)
      len = find_putc(out, cap, len, st->query[i]);
    len = find_putc(out, cap, len, '_');
  }
  return len;
}

/* Line 3: result count + scan counter, or "none yet" before a search. */
static int find_format_status(char *out, int cap, const struct find_state *st) {
  if (!st->searched) return find_put(out, cap, 0, "Results: none yet");
  int len = 0;
  len = find_put(out, cap, len, "Results: ");
  len = find_put_num(out, cap, len, st->nresults);
  len = find_put(out, cap, len, "  (scanned ");
  len = find_put_num(out, cap, len, st->scanned);
  len = find_put(out, cap, len, " entries");
  if (st->trunc_scan) {
    len = find_put(out, cap, len, ", stopped at ");
    len = find_put_num(out, cap, len, FIND_MAX_SCAN);
  }
  if (st->trunc_results) {
    len = find_put(out, cap, len, ", first ");
    len = find_put_num(out, cap, len, FIND_MAX_RESULTS);
    len = find_put(out, cap, len, " kept");
  }
  len = find_put(out, cap, len, ")");
  return len;
}

/* Render the full screen into out (NUL-terminated).  Returns the length.
 * Fixed layout (content rows, 0-based): row 0 title, row 1 query box,
 * row 2 status, rows 3..18 the 16 result rows, row 19 blank, row 20 the
 * key legend - so the legend always sits on the same row (mouse mapping). */
static int find_render(char *out, int cap, const struct find_state *st) {
  int len = 0;
  char line[FIND_ROW_W + 1];
  char small[160];

  len = find_put(out, cap, len, "=== Find ===\n");

  find_format_search(small, sizeof(small), st);
  len = find_put(out, cap, len, small);
  len = find_putc(out, cap, len, '\n');

  find_format_status(small, sizeof(small), st);
  len = find_put(out, cap, len, small);
  len = find_putc(out, cap, len, '\n');

  for (int i = 0; i < FIND_VISIBLE; i++) {
    int idx = st->list.top + i;
    if (idx >= 0 && idx < st->list.count && idx < FIND_MAX_RESULTS) {
      find_format_row(line, sizeof(line), &st->results[idx],
                      idx == st->list.selected);
    } else {
      for (int k = 0; k < FIND_ROW_W; k++) line[k] = ' ';
      line[FIND_ROW_W] = '\0';
    }
    len = find_put(out, cap, len, line);
    len = find_putc(out, cap, len, '\n');
  }

  len = find_putc(out, cap, len, '\n');
  len = find_put(out, cap, len,
                 "type=edit  Enter=search  Bksp=edit  c=clear  arrows=scroll\n");
  return len;
}

static void find_redraw(void) {
  static char screen[FIND_SCREEN_MAX];
  gui_clear();
  find_render(screen, sizeof(screen), &FIND);
  print(screen);
}

/* ====================================================================== */
/* Events                                                                 */
/* ====================================================================== */

/* Apply one event.  Returns 1 when the screen needs a redraw, else 0. */
static int find_handle_event(struct find_state *st, const struct gui_event *ev) {
  switch (ev->type) {
  case GUI_EV_CHAR:
    if (ev->ch == '\n' || ev->ch == '\r') {       /* Enter = search   */
      find_run_search(st);
      return 1;
    }
    if (ev->ch == 8 || ev->ch == 127) {           /* Bksp / DEL       */
      find_query_backspace(st);
      return 1;
    }
    if (ev->ch == 'c') {                          /* Clear shortcut   */
      find_clear(st);
      return 1;
    }
    if (ev->ch >= 32 && ev->ch <= 126) {          /* printable: type   */
      find_query_append(st, ev->ch);
      return 1;
    }
    return 0;

  case GUI_EV_UP:
    gui_list_move(&st->list, -1);
    return 1;
  case GUI_EV_DOWN:
    gui_list_move(&st->list, 1);
    return 1;

  case GUI_EV_MENU:
    if (ev->menu != 0) return 0;                  /* menu 0 only      */
    if (ev->item == 0) find_run_search(st);       /* Find > Search    */
    else if (ev->item == 1) find_clear(st);       /* Find > Clear     */
    else return 0;
    return 1;

  case GUI_EV_MOUSE:
    if (ev->button == 1 && ev->state == GUI_MOUSE_PRESS) {
      int idx = gui_list_click_row(&st->list, ev->y, FIND_RESULTS_ROW);
      if (idx >= 0 && idx != st->list.selected) {
        st->list.selected = idx;
        return 1;
      }
    }
    return 0;

  default:
    return 0;
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
  gui_set_title("Find");
  gui_add_menu(0, "Find", "Search,Clear");
  gui_enable_mouse();
  print_console("[APP] FIND started\n");

  find_init(&FIND);
  find_redraw();

  for (;;) {
    struct gui_event ev;
    if (gui_read_event(&ev)) {
      if (find_handle_event(&FIND, &ev)) find_redraw();
    }
  }
}
