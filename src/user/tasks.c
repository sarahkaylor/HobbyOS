/*
 * tasks.c - Tasks (to-do list) for HobbyOS.
 *
 * A windowed to-do list: add / toggle / delete items, persisted to TODO.TXT
 * in the current directory (one item per line: "[ ] text" or "[x] text").
 *
 * Usage contract (see gui.h):
 *   - Draw with print() after gui_clear() ("\f" clears the window)
 *   - Read input with gui_read_event() / gui_read_event_timeout()
 *   - Register menus with gui_add_menu() (libc.h)
 *
 * Persistence notes:
 *   - TODO.TXT is reloaded at startup; if it cannot be opened the list starts
 *     empty and a warning line is shown.
 *   - The list is saved after every change. HobbyOS has no ftruncate and
 *     fat16 keeps the largest size a file ever had, so rewriting a shorter
 *     list would leave a stale tail that would come back as ghost items on
 *     the next load. tasks_save() therefore unlinks TODO.TXT before writing
 *     the whole serialized buffer, which guarantees exact file contents.
 *
 * File format (documented choices):
 *   - A line is a task iff it starts with "[ ]", "[x]" or "[X]"; both cases
 *     of 'x' are accepted (a hand-edited "[X] milk" is not thrown away).
 *   - Exactly one space between the marker and the text is a separator and is
 *     consumed; any further spaces belong to the text (trailing spaces are
 *     preserved).
 *   - A trailing '\r' (CRLF files) is stripped; everything else is kept.
 *   - Malformed lines are ignored on load.
 *   - Text is truncated to 60 characters; the list holds at most 100 items.
 */

#include "libc.h"
#include "gui.h"
#include "dialog.h"

/* ---- Constants ---- */

#define TASKS_MAX        100
#define TASKS_TEXT_MAX    60
#define TASKS_VISIBLE     18     /* item rows on screen */
#define TASKS_STATUS_MAX  48     /* transient status line buffer */
#define TASKS_FILE       "TODO.TXT"

/* One serialized item: "[x] " (4) + text (<=60) + '\n' (1) */
#define TASKS_ITEM_BYTES (4 + TASKS_TEXT_MAX + 1)
/* Worst-case serialized file: 100 items * 65 bytes = 6500 bytes */
#define TASKS_FILE_BYTES (TASKS_MAX * TASKS_ITEM_BYTES)

/* ---- State ---- */

struct task_item {
  char text[TASKS_TEXT_MAX + 1];
  int  done;
};

struct task_state {
  struct task_item items[TASKS_MAX];
  int  count;
  int  load_error;                    /* 1 = TODO.TXT could not be opened */
  char status[TASKS_STATUS_MAX];      /* transient message ("" = none) */
};

/* Storage indirection: host tests replace these with failing fakes to
 * exercise the error paths (a missing file cannot be simulated otherwise,
 * because open(, 0) creates files on demand). */
struct tasks_store_ops {
  int (*open)(const char *name, int flags, ...);
  ssize_t (*read)(int fd, void *buf, size_t size);
  ssize_t (*write)(int fd, const void *buf, size_t size);
  int (*close)(int fd);
  int (*unlink)(const char *name);
};

static struct tasks_store_ops tasks_store = { open, read, write, close, unlink };

/* Prompt/confirm indirection: default to the real dialog library; host tests
 * swap in fakes so the non-interactive parts stay testable. */
static int (*tasks_prompt_fn)(const char *title, const char *msg, char *buf, int max) = dialog_prompt;
static int (*tasks_confirm_fn)(const char *title, const char *msg) = dialog_confirm;

/* App state (BSS: ~13 KB, kept out of the small user stack). */
static struct task_state tasks_state;
static struct gui_list tasks_list;

/* ---- Status line ---- */

void tasks_set_status(struct task_state *st, const char *msg) {
  int i = 0;
  while (msg[i] && i < TASKS_STATUS_MAX - 1) { st->status[i] = msg[i]; i++; }
  st->status[i] = '\0';
}

/* Build "prefix<number>suffix" (e.g. "Reloaded 3 tasks.") as the status. */
void tasks_set_status_num(struct task_state *st, const char *prefix, int value,
                          const char *suffix) {
  char num[24];
  int nl = gui_uitoa((unsigned long)value, num);
  int j = 0;
  for (int i = 0; prefix[i] && j < TASKS_STATUS_MAX - 1; i++) st->status[j++] = prefix[i];
  for (int i = 0; i < nl && j < TASKS_STATUS_MAX - 1; i++) st->status[j++] = num[i];
  for (int i = 0; suffix[i] && j < TASKS_STATUS_MAX - 1; i++) st->status[j++] = suffix[i];
  st->status[j] = '\0';
}

/* ---- Parsing / serialization ---- */

/* Parse one line (possibly without its '\n') into *out.
 * Returns 1 when the line is a valid task, 0 when it is malformed. */
int tasks_parse_line(const char *line, int len, struct task_item *out) {
  /* Tolerate CRLF files (and a stray NUL) at the end of the line. */
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\0')) len--;
  if (len < 3) return 0;
  if (line[0] != '[' || line[2] != ']') return 0;

  int done;
  if (line[1] == ' ') done = 0;
  else if (line[1] == 'x' || line[1] == 'X') done = 1;
  else return 0;

  int t = 3;
  if (t < len && line[t] == ' ') t++;      /* consume the separator space */
  int tl = len - t;
  if (tl > TASKS_TEXT_MAX) tl = TASKS_TEXT_MAX;
  for (int i = 0; i < tl; i++) out->text[i] = line[t + i];
  out->text[tl] = '\0';
  out->done = done;
  return 1;
}

/* Parse a whole file image. Malformed lines are ignored; items beyond the
 * 100-item cap are dropped silently. Returns the number of items added. */
int tasks_parse_buffer(struct task_state *st, const char *buf, int len) {
  int added = 0;
  int start = 0;
  for (int i = 0; i <= len; i++) {
    if (i == len || buf[i] == '\n') {
      struct task_item it;
      if (tasks_parse_line(buf + start, i - start, &it)) {
        if (st->count < TASKS_MAX) {
          st->items[st->count] = it;
          st->count++;
          added++;
        }
      }
      start = i + 1;
    }
  }
  return added;
}

/* Serialize every item as "[ ] text\n" / "[x] text\n".
 * Never writes more than `cap` bytes; NUL-terminates when there is room.
 * Returns the number of bytes written. */
int tasks_serialize(const struct task_state *st, char *out, int cap) {
  int n = 0;
  for (int i = 0; i < st->count; i++) {
    int tl = gui_strlen(st->items[i].text);
    if (tl > TASKS_TEXT_MAX) tl = TASKS_TEXT_MAX;
    if (n + 4 + tl + 1 > cap) break;         /* keep the spare byte for NUL */
    out[n++] = '[';
    out[n++] = st->items[i].done ? 'x' : ' ';
    out[n++] = ']';
    out[n++] = ' ';
    for (int k = 0; k < tl; k++) out[n++] = st->items[i].text[k];
    out[n++] = '\n';
  }
  if (n < cap) out[n] = '\0';
  return n;
}

/* ---- File I/O ---- */

/* Load TODO.TXT into st (replacing the current contents).
 * Returns the item count, or -1 when the file cannot be opened (the list is
 * then empty and st->load_error is set so the UI shows a warning). */
int tasks_load(struct task_state *st) {
  static char buf[TASKS_FILE_BYTES + 1];

  st->count = 0;
  st->status[0] = '\0';

  int fd = tasks_store.open(TASKS_FILE, 0);
  if (fd < 0) {
    st->load_error = 1;
    return -1;
  }
  st->load_error = 0;

  int n = tasks_store.read(fd, buf, TASKS_FILE_BYTES);
  tasks_store.close(fd);
  if (n < 0) n = 0;
  buf[n] = '\0';

  return tasks_parse_buffer(st, buf, n);
}

/* Save the whole list to TODO.TXT.
 * Returns 0 on success, -1 on failure. */
int tasks_save(const struct task_state *st) {
  static char buf[TASKS_FILE_BYTES + 1];

  int len = tasks_serialize(st, buf, TASKS_FILE_BYTES + 1);

  /* Recreate the file so a shorter list cannot leave a stale tail behind
   * (see the persistence note at the top of this file). */
  tasks_store.unlink(TASKS_FILE);
  int fd = tasks_store.open(TASKS_FILE, 0);
  if (fd < 0) return -1;

  int w = 0;
  if (len > 0) w = tasks_store.write(fd, buf, len);
  tasks_store.close(fd);
  if (len > 0 && w != len) return -1;
  return 0;
}

/* ---- List operations ---- */

/* Append a task (text truncated to 60 chars). Returns 1 when added, 0 when
 * rejected (empty text or list full); the status line explains a rejection. */
int tasks_add(struct task_state *st, const char *text) {
  int tl = gui_strlen(text);
  if (tl <= 0) {
    tasks_set_status(st, "Nothing to add.");
    return 0;
  }
  if (st->count >= TASKS_MAX) {
    tasks_set_status(st, "List is full (max 100).");
    return 0;
  }
  if (tl > TASKS_TEXT_MAX) tl = TASKS_TEXT_MAX;
  for (int i = 0; i < tl; i++) st->items[st->count].text[i] = text[i];
  st->items[st->count].text[tl] = '\0';
  st->items[st->count].done = 0;
  st->count++;
  tasks_set_status(st, "Added.");
  return 1;
}

/* Flip the done flag of item idx. Returns the new flag, or -1 if out of range. */
int tasks_toggle(struct task_state *st, int idx) {
  if (idx < 0 || idx >= st->count) return -1;
  st->items[idx].done = !st->items[idx].done;
  return st->items[idx].done;
}

/* Remove item idx, keeping the order of the remaining items.
 * Returns 1 when removed, 0 for an out-of-range index. */
int tasks_delete(struct task_state *st, int idx) {
  if (idx < 0 || idx >= st->count) return 0;
  for (int i = idx; i < st->count - 1; i++) st->items[i] = st->items[i + 1];
  st->count--;
  return 1;
}

int tasks_done_count(const struct task_state *st) {
  int n = 0;
  for (int i = 0; i < st->count; i++) {
    if (st->items[i].done) n++;
  }
  return n;
}

/* Keep the scroll helper in sync with the state and the selection in range. */
void tasks_list_sync(struct task_state *st, struct gui_list *l) {
  l->count = st->count;
  l->visible = TASKS_VISIBLE;
  if (l->count <= 0) {
    l->selected = 0;
    l->top = 0;
    return;
  }
  if (l->selected > l->count - 1) l->selected = l->count - 1;
  if (l->selected < 0) l->selected = 0;
  gui_list_ensure_visible(l);
}

/* Row of the first item row on screen (0-based): title + count, plus optional
 * warning and status lines. Used for mouse click mapping. */
int tasks_first_item_row(const struct task_state *st) {
  int row = 2;
  if (st->load_error) row++;
  if (st->status[0]) row++;
  return row;
}

/* ---- Actions ---- */

static void tasks_action_save(struct task_state *st) {
  if (tasks_save(st) != 0) tasks_set_status(st, "Save failed!");
}

static void tasks_action_add(struct task_state *st, struct gui_list *l) {
  char buf[TASKS_TEXT_MAX + 1];
  buf[0] = '\0';
  if (!tasks_prompt_fn("Add Task", "Task text:", buf, TASKS_TEXT_MAX + 1)) return;
  if (tasks_add(st, buf)) {
    l->selected = st->count - 1;         /* select the new item */
    tasks_list_sync(st, l);
    tasks_action_save(st);
  } else {
    tasks_list_sync(st, l);
  }
}

static void tasks_action_toggle(struct task_state *st, struct gui_list *l) {
  if (st->count <= 0) return;
  if (tasks_toggle(st, l->selected) < 0) return;
  tasks_action_save(st);
}

static void tasks_action_delete(struct task_state *st, struct gui_list *l) {
  if (st->count <= 0) {
    tasks_set_status(st, "Nothing to delete.");
    return;
  }
  if (!tasks_confirm_fn("Delete Task", "Delete the selected task?")) return;
  if (tasks_delete(st, l->selected)) {
    tasks_set_status(st, "Deleted.");
    tasks_list_sync(st, l);              /* keeps selection in range */
    tasks_action_save(st);               /* may overwrite the status */
  }
}

static void tasks_action_reload(struct task_state *st, struct gui_list *l) {
  int n = tasks_load(st);
  if (n < 0) {
    tasks_set_status(st, "Reload failed - starting empty.");
  } else {
    tasks_set_status_num(st, "Reloaded ", n, " tasks.");
  }
  l->selected = 0;
  l->top = 0;
  tasks_list_sync(st, l);
}

/* ---- Rendering ---- */

/* Print the complete screen (deterministic given st/l). */
void tasks_render(const struct task_state *st, const struct gui_list *l) {
  char num[24];

  gui_clear();
  print("=== Tasks ===\n");

  if (st->count == 0) {
    print("(no tasks yet - press a to add one)\n");
  } else {
    gui_uitoa((unsigned long)st->count, num);
    print(num);
    print(" tasks, ");
    gui_uitoa((unsigned long)tasks_done_count(st), num);
    print(num);
    print(" done\n");
  }

  if (st->load_error) print("warning: cannot read TODO.TXT (starting empty)\n");
  if (st->status[0]) { print(st->status); print("\n"); }

  for (int row = 0; row < TASKS_VISIBLE; row++) {
    int idx = l->top + row;
    if (idx < 0 || idx >= st->count) break;
    print(idx == l->selected ? " > " : "   ");
    print(st->items[idx].done ? "[x] " : "[ ] ");
    print(st->items[idx].text);
    print("\n");
  }

  print("a=add  Enter/space=toggle  d=delete  r=reload\n");
}

/* ---- Event handling ---- */

/* Handle one event. Returns 1 when the app should exit(0), else 0. */
int tasks_handle_event(struct task_state *st, struct gui_list *l,
                       const struct gui_event *ev) {
  switch (ev->type) {
  case GUI_EV_UP:
    gui_list_move(l, -1);
    return 0;
  case GUI_EV_DOWN:
    gui_list_move(l, 1);
    return 0;
  case GUI_EV_CHAR:
    switch (ev->ch) {
    case 'q': case 'Q': return 1;
    case 'a': case 'A': tasks_action_add(st, l); return 0;
    case 'd': case 'D': tasks_action_delete(st, l); return 0;
    case 'r': case 'R': tasks_action_reload(st, l); return 0;
    case ' ': case '\n': tasks_action_toggle(st, l); return 0;
    default: return 0;
    }
  case GUI_EV_MENU:
    switch (ev->item) {
    case 0: tasks_action_add(st, l); return 0;      /* Tasks > Add */
    case 1: tasks_action_toggle(st, l); return 0;   /* Tasks > Toggle */
    case 2: tasks_action_delete(st, l); return 0;   /* Tasks > Delete */
    case 3: tasks_action_reload(st, l); return 0;   /* Tasks > Reload */
    default: return 0;
    }
  case GUI_EV_MOUSE:
    /* Left click on an item row selects it (rows are 0-based content
     * cells; a click on the already selected row just re-selects it).
     * Only presses select - the release half of the click is ignored. */
    if (ev->button == 1 && ev->state == GUI_MOUSE_PRESS) {
      int idx = gui_list_click_row(l, ev->y, tasks_first_item_row(st));
      if (idx >= 0) l->selected = idx;
    }
    return 0;
  default:
    return 0;
  }
}

/* ---- Startup ---- */

void tasks_startup(void) {
  gui_set_title("Tasks");
  gui_add_menu(0, "Tasks", "Add,Toggle,Delete,Reload");
  gui_enable_mouse();
  print_console("[APP] TASKS started\n");
}

/* ---- Entry point ---- */

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
  tasks_state.count = 0;
  tasks_state.load_error = 0;
  tasks_state.status[0] = '\0';
  tasks_list.selected = 0;
  tasks_list.top = 0;
  tasks_list.count = 0;
  tasks_list.visible = TASKS_VISIBLE;

  tasks_startup();

  /* Load TODO.TXT from the current directory (warning line on failure). */
  tasks_load(&tasks_state);
  tasks_list_sync(&tasks_state, &tasks_list);
  tasks_render(&tasks_state, &tasks_list);

  for (;;) {
    struct gui_event ev;
    if (!gui_read_event(&ev)) continue;
    if (tasks_handle_event(&tasks_state, &tasks_list, &ev)) break;
    tasks_render(&tasks_state, &tasks_list);
  }

  exit(0);
#ifdef HOST_TEST
  return 0;
#endif
}
