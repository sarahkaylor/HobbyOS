/*
 * notes.c - Notes manager for HobbyOS.
 *
 * Keeps up to NOTES_MAX short notes (title + single-line body) and persists
 * them to NOTES.TXT in the current directory (FAT-16 8.3 name).
 *
 * File format (two lines per note):
 *     ### <title>
 *     <body>
 * A line starting with "### " begins a note. The next line that does NOT
 * start with "### " becomes that note's body; if the following line is
 * another "### " line (or the file ends) the note's body is "". Any further
 * consecutive non-"###" lines are ignored. Titles are kept to 24 chars and
 * bodies to 60 chars; overlong values are clipped on parse, on add/edit and
 * again on save. A maximum of 20 notes is kept; notes beyond that are
 * dropped on load and further adds are rejected.
 *
 * Screen layout (notes_render()):
 *     === Notes ===
 *     N notes                        (or the "no notes" hint)
 *     [WARNING line while NOTES.TXT cannot be accessed]
 *     12 title rows: selected "> title", others "  title"  (clipped to 30)
 *     72-column separator rule
 *     3-row body preview (hard-clipped at 70 cols/row, never word-wrapped;
 *                         "..." appended when the body does not fully fit)
 *     n=new  e=edit body  d=delete  Enter=save
 *
 * Rendering is deterministic for a given state: one gui_clear() followed by
 * the complete screen.
 *
 * Entry point pattern is kept from the stub: the host unit test includes
 * this file with `#define main notes_app_main`.
 */

#include <fcntl.h>
#include "libc.h"
#include "gui.h"
#include "dialog.h"

/* ---- Constants ---------------------------------------------------- */

#define NOTES_MAX          20    /* capacity, kept in memory and on disk */
#define NOTE_TITLE_MAX     24    /* max title characters */
#define NOTE_BODY_MAX      60    /* max body characters */
#define NOTES_LIST_ROWS    12    /* title rows drawn on screen */
#define NOTES_TITLE_COLS   30    /* title display width on screen */
#define NOTES_PREVIEW_ROWS 3     /* preview area height */
#define NOTES_PREVIEW_COLS 70    /* preview row width */
#define NOTES_RULE_COLS    72    /* separator rule width */
#define NOTES_SER_MAX      2048  /* 20 * (4+24+1+60+1) = 1800 + slack */
#define NOTES_IO_MAX       2048  /* largest file we read back */

#define NOTES_FILE    "NOTES.TXT"
#define NOTES_FOOTER  "n=new  e=edit body  d=delete  Enter=save"
#define NOTES_WARN_MSG "WARNING: cannot access NOTES.TXT - data kept in memory"

/* ---- State -------------------------------------------------------- */

struct note {
  char title[NOTE_TITLE_MAX + 1];   /* NUL-terminated, <= 24 chars */
  char body[NOTE_BODY_MAX + 1];     /* NUL-terminated, <= 60 chars */
};

/* Deliberately non-static: the host unit test includes this file and drives
 * this state directly. */
struct note notes[NOTES_MAX];
int notes_count;                 /* number of valid entries in notes[] */
struct gui_list notes_list;      /* selected / top / count / visible */
int notes_warn;                  /* 1 = show the WARNING line */

static char notes_ser_buf[NOTES_SER_MAX];  /* serialisation scratch */
static char notes_io_buf[NOTES_IO_MAX];    /* read/write scratch */

/* ---- Small string helpers ----------------------------------------- */

/* Copy at most `max` chars from src[0..n) into dst (always NUL-terminated).
 * Stops at the first NUL or '\n' so a title/body can never contain a
 * newline and break the two-lines-per-note file format. */
void notes_clip_n(char *dst, const char *src, int n, int max) {
  int i = 0;
  while (i < n && i < max && src[i] != '\0' && src[i] != '\n') {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

/* notes_clip_n for NUL-terminated strings. */
void notes_clip(char *dst, const char *src, int max) {
  notes_clip_n(dst, src, 0x40000000, max);
}

/* ---- State helpers ------------------------------------------------ */

/* Clear all notes and the list state (does not touch notes_warn). */
static void notes_clear(void) {
  for (int i = 0; i < NOTES_MAX; i++) {
    notes[i].title[0] = '\0';
    notes[i].body[0] = '\0';
  }
  notes_count = 0;
  notes_list.selected = 0;
  notes_list.top = 0;
  notes_list.count = 0;
  notes_list.visible = NOTES_LIST_ROWS;
}

/* Full reset, including the warning flag (used at startup / by tests). */
void notes_reset(void) {
  notes_clear();
  notes_warn = 0;
}

/* Re-sync the gui_list scroll state with notes_count after a mutation:
 * count mirror, selection clamped into [0, count-1], kept visible. */
void notes_refresh_list(void) {
  notes_list.count = notes_count;
  notes_list.visible = NOTES_LIST_ROWS;
  if (notes_list.selected > notes_count - 1) notes_list.selected = notes_count - 1;
  if (notes_list.selected < 0) notes_list.selected = 0;
  gui_list_ensure_visible(&notes_list);
}

/* Body of the selected note ("" when there is none). */
const char *notes_selected_body(void) {
  int s = notes_list.selected;
  if (notes_count <= 0 || s < 0 || s >= notes_count) return "";
  return notes[s].body;
}

/* Screen row (0-based) of the first title row: the warning line, when
 * shown, takes the row between the header and the list. */
int notes_list_first_row(void) {
  return notes_warn ? 3 : 2;
}

/* ---- Parsing ------------------------------------------------------ */

/* Parse `len` bytes of file text into notes[] (replaces current state).
 * Lines are split on '\n'; a trailing '\r' is ignored so CRLF files parse
 * cleanly. See the file-format comment at the top of this file. */
void notes_parse(const char *data, int len) {
  int i = 0;
  int expect_body = 0;   /* 1 = the next non-"###" line is a body */

  if (!data || len < 0) len = 0;
  notes_clear();

  while (i < len) {
    int j = i;
    int llen;

    while (j < len && data[j] != '\n') j++;
    llen = j - i;
    if (llen > 0 && data[i + llen - 1] == '\r') llen--;

    if (llen >= 4 && data[i] == '#' && data[i + 1] == '#' &&
        data[i + 2] == '#' && data[i + 3] == ' ') {
      if (notes_count < NOTES_MAX) {
        notes_clip_n(notes[notes_count].title, data + i + 4, llen - 4,
                     NOTE_TITLE_MAX);
        notes[notes_count].body[0] = '\0';
        notes_count++;
        expect_body = 1;
      } else {
        expect_body = 0;   /* over the cap: drop this note too */
      }
    } else if (expect_body) {
      notes_clip_n(notes[notes_count - 1].body, data + i, llen, NOTE_BODY_MAX);
      expect_body = 0;
    }
    /* else: an extra consecutive line - ignored */

    i = j + 1;
  }

  notes_refresh_list();
}

/* ---- Serialisation ------------------------------------------------ */

/* Render all notes as file text into `out` (at most `cap` bytes including
 * the NUL). Returns the number of bytes written, or -1 if it would not
 * fit. Titles/bodies are clipped again here so a caller cannot inject an
 * overlong value that violates the on-disk format. */
int notes_serialize(char *out, int cap) {
  char t[NOTE_TITLE_MAX + 1];
  char b[NOTE_BODY_MAX + 1];
  int len = 0;

  if (!out || cap < 1) return -1;

  for (int k = 0; k < notes_count; k++) {
    int tlen, blen, need;

    notes_clip(t, notes[k].title, NOTE_TITLE_MAX);
    notes_clip(b, notes[k].body, NOTE_BODY_MAX);
    tlen = gui_strlen(t);
    blen = gui_strlen(b);
    need = 4 + tlen + 1 + blen + 1;

    if (len + need + 1 > cap) return -1;   /* +1 for the NUL */

    out[len++] = '#'; out[len++] = '#'; out[len++] = '#'; out[len++] = ' ';
    for (int i = 0; i < tlen; i++) out[len++] = t[i];
    out[len++] = '\n';
    for (int i = 0; i < blen; i++) out[len++] = b[i];
    out[len++] = '\n';
  }

  out[len] = '\0';
  return len;
}

/* ---- File I/O ----------------------------------------------------- */

/* Load NOTES.TXT into memory. Returns 1 on success, 0 when the file could
 * not be read (warning line shown, previous in-memory notes kept). */
int notes_load(void) {
  int fd = open(NOTES_FILE, 0);
  int n;

  if (fd < 0) {
    notes_warn = 1;
    return 0;
  }
  n = read(fd, notes_io_buf, NOTES_IO_MAX - 1);
  close(fd);
  if (n < 0) {
    notes_warn = 1;
    return 0;
  }
  notes_io_buf[n] = '\0';
  notes_parse(notes_io_buf, n);
  notes_warn = 0;
  return 1;
}

/* Save notes to NOTES.TXT (open/write/close; open failure -> warning line,
 * data stays in memory). Returns 1 on success, 0 on failure.
 *
 * The FAT-16 driver (and the host mock) only ever GROW a file's size, so
 * rewriting a shrunk note list over an older, longer file would leave the
 * old tail bytes on disk and those stale notes would come back on the next
 * load. To avoid that, the previous file length is probed first and the
 * new content is padded with '\n' up to it: extra blank lines are ignored
 * by notes_parse(), so the parsed result is exactly the in-memory notes. */
int notes_save(void) {
  int new_len = notes_serialize(notes_ser_buf, NOTES_SER_MAX);
  int old_len = 0;
  int fd, got, wrote = 0, pad;

  if (new_len < 0) {
    notes_warn = 1;             /* cannot happen with the fixed caps */
    return 0;
  }

  /* Probe the length of the file we are about to replace. */
  fd = open(NOTES_FILE, 0);
  if (fd < 0) {
    notes_warn = 1;
    return 0;
  }
  got = read(fd, notes_io_buf, NOTES_IO_MAX);
  close(fd);
  if (got > 0) old_len = got;

  fd = open(NOTES_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    notes_warn = 1;
    return 0;
  }
  if (new_len > 0) wrote = write(fd, notes_ser_buf, new_len);
  pad = old_len - new_len;
  if (pad > 0) {
    char nl[64];
    for (int i = 0; i < 64; i++) nl[i] = '\n';
    while (pad > 0) {
      int chunk = (pad > 64) ? 64 : pad;
      write(fd, nl, chunk);
      pad -= chunk;
    }
  }
  close(fd);

  if (wrote < new_len) {
    notes_warn = 1;
    return 0;
  }
  notes_warn = 0;
  return 1;
}

/* ---- Note operations ---------------------------------------------- */

/* Append a note (title must be non-empty; title/body are clipped to the
 * caps). Returns 0 when rejected (no title, or the 20-note cap is full). */
int notes_add(const char *title, const char *body) {
  if (notes_count >= NOTES_MAX) return 0;
  if (!title || !title[0]) return 0;
  if (!body) body = "";

  notes_clip(notes[notes_count].title, title, NOTE_TITLE_MAX);
  notes_clip(notes[notes_count].body, body, NOTE_BODY_MAX);
  notes_count++;
  notes_refresh_list();
  notes_list.selected = notes_count - 1;   /* select the new note */
  gui_list_ensure_visible(&notes_list);
  return 1;
}

/* Replace the selected note's body. Returns 0 when there is no selection. */
int notes_edit_body(const char *body) {
  if (notes_count <= 0) return 0;
  if (!body) body = "";
  notes_clip(notes[notes_list.selected].body, body, NOTE_BODY_MAX);
  return 1;
}

/* Delete the selected note. Returns 0 when there is nothing to delete.
 * The selection stays in range (and visible) afterwards. */
int notes_delete(void) {
  int sel = notes_list.selected;

  if (notes_count <= 0) return 0;

  for (int i = sel; i < notes_count - 1; i++) notes[i] = notes[i + 1];
  notes_count--;
  notes[notes_count].title[0] = '\0';
  notes[notes_count].body[0] = '\0';

  notes_list.selected = sel;      /* refresh clamps to the new last note */
  notes_refresh_list();
  return 1;
}

/* Mouse click: row `y` (window content row) selects a note; returns the
 * index selected, or -1 when the row is not a list row. */
int notes_click(int y) {
  int idx = gui_list_click_row(&notes_list, y, notes_list_first_row());
  if (idx < 0) return -1;
  notes_list.selected = idx;
  return idx;
}

/* ---- Rendering ---------------------------------------------------- */

/* Format preview row `row` (0..NOTES_PREVIEW_ROWS-1) of a note body into
 * `out` (>= NOTES_PREVIEW_COLS + 5 bytes). The body is hard-split into
 * 70-column chunks - it is never word-wrapped - and if it does not fully
 * fit in the preview area the last shown row gets "..." appended. Bodies
 * are normally <= 60 chars, so they occupy row 0 only. */
void notes_preview_row(const char *body, int row, char *out) {
  int len = gui_strlen(body);
  int start = row * NOTES_PREVIEW_COLS;
  int n = 0;

  if (start < len) {
    n = len - start;
    if (n > NOTES_PREVIEW_COLS) n = NOTES_PREVIEW_COLS;
    for (int i = 0; i < n; i++) out[i] = body[start + i];
  }
  out[n] = '\0';

  if (row == NOTES_PREVIEW_ROWS - 1 && start + n < len) {
    out[n++] = '.';
    out[n++] = '.';
    out[n++] = '.';
    out[n] = '\0';
  }
}

/* Draw the complete screen (one gui_clear() + full redraw). */
void notes_render(void) {
  char buf[160];

  gui_clear();

  print("=== Notes ===\n");
  if (notes_count == 0) {
    print("(no notes - press n to create one)\n");
  } else {
    gui_itoa(notes_count, buf);
    print(buf);
    print(" notes\n");
  }
  if (notes_warn) print(NOTES_WARN_MSG "\n");

  for (int row = 0; row < NOTES_LIST_ROWS; row++) {
    int idx = notes_list.top + row;
    if (idx < notes_count) {
      print(idx == notes_list.selected ? "> " : "  ");
      notes_clip(buf, notes[idx].title, NOTES_TITLE_COLS);
      print(buf);
    }
    print("\n");
  }

  gui_rule(buf, NOTES_RULE_COLS);
  print(buf);
  print("\n");

  for (int r = 0; r < NOTES_PREVIEW_ROWS; r++) {
    notes_preview_row(notes_selected_body(), r, buf);
    print(buf);
    print("\n");
  }

  print(NOTES_FOOTER "\n");
}

/* ---- Actions ------------------------------------------------------ */

/* 'n' / menu New: prompt for title, then body, append, save.
 * Cancelling the title prompt or the body prompt aborts the add; an empty
 * title is rejected. The body may be left empty (title-only note). */
void notes_cmd_new(void) {
  char title[NOTE_TITLE_MAX + 1];
  char body[NOTE_BODY_MAX + 1];

  if (notes_count >= NOTES_MAX) {
    dialog_message("Notes", "Note limit reached (20).");
    return;
  }
  title[0] = '\0';
  if (!dialog_prompt("New Note", "Title:", title, sizeof(title))) return;
  if (!title[0]) return;
  body[0] = '\0';
  if (!dialog_prompt("New Note", "Body:", body, sizeof(body))) return;

  notes_add(title, body);
  notes_save();
}

/* 'e' / menu Edit: prompt for the new body, update, save.
 *
 * Prefill note: dialog_prompt() (src/user/dialog.c) clears buf[0] and keeps
 * its own length counter starting at 0, so it does NOT support prefilling.
 * We pass the current body in the buffer only so it is initialised; the
 * dialog always starts empty and the user retypes the body. (dialog.c is a
 * shared, read-only dependency for this app.) */
void notes_cmd_edit(void) {
  char body[NOTE_BODY_MAX + 1];

  if (notes_count <= 0) return;
  notes_clip(body, notes_selected_body(), NOTE_BODY_MAX);
  if (!dialog_prompt("Edit Note", "Body:", body, sizeof(body))) return;

  notes_edit_body(body);
  notes_save();
}

/* 'd' / menu Delete: confirm, remove the selected note, save. */
void notes_cmd_delete(void) {
  if (notes_count <= 0) return;
  if (!dialog_confirm("Delete Note", "Delete this note?")) return;
  notes_delete();
  notes_save();
}

/* ---- Event handling ----------------------------------------------- */

/* Handle one input event. Returns 1 when the event changed state (caller
 * should redraw), 0 when it was ignored. */
int notes_handle_event(const struct gui_event *ev) {
  if (!ev) return 0;

  switch (ev->type) {
  case GUI_EV_UP:
    gui_list_move(&notes_list, -1);
    return 1;
  case GUI_EV_DOWN:
    gui_list_move(&notes_list, 1);
    return 1;
  case GUI_EV_CHAR:
    switch (ev->ch) {
    case 'n': case 'N': notes_cmd_new();    return 1;
    case 'e': case 'E': notes_cmd_edit();   return 1;
    case 'd': case 'D': notes_cmd_delete(); return 1;
    case 'r': case 'R': notes_load();       return 1;
    case '\n': case '\r': notes_save();     return 1;
    default: return 0;
    }
  case GUI_EV_MENU:
    if (ev->menu == 0) {
      switch (ev->item) {
      case 0: notes_cmd_new();    return 1;   /* New */
      case 1: notes_cmd_edit();   return 1;   /* Edit */
      case 2: notes_cmd_delete(); return 1;   /* Delete */
      case 3: notes_save();       return 1;   /* Save */
      }
    }
    return 0;
  case GUI_EV_MOUSE:
    if (ev->button == 1 && ev->state == GUI_MOUSE_PRESS) return notes_click(ev->y) >= 0;
    return 0;
  }
  return 0;
}

/* ---- Startup ------------------------------------------------------ */

void notes_init(void) {
  gui_set_title("Notes");
  gui_add_menu(0, "Note", "New,Edit,Delete,Save");
  gui_enable_mouse();

  notes_reset();
  print_console("[APP] NOTES started\n");
  notes_load();
  notes_render();
}

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
  notes_init();

  for (;;) {
    struct gui_event ev;
    if (gui_read_event(&ev)) {
      if (notes_handle_event(&ev)) notes_render();
    }
  }
}
