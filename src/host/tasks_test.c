/*
 * tasks_test.c - Host unit tests for tasks.c (HobbyOS Tasks to-do list).
 *
 * Style follows src/host/pong_test.c: the app source is included directly
 * (with `main` renamed to tasks_app_main) so tests can call its internal
 * helpers and inspect its state.
 *
 * Coverage:
 *   - line parsing (markers, [x]/[X], trailing spaces, CRLF, truncation,
 *     malformed lines)
 *   - buffer parsing / serialization / round-trip / size predictability
 *   - add (empty, full, truncation), toggle, delete, done-count math
 *   - selection + scrolling (gui_list) incl. empty-list and boundary cases
 *   - render output (line-by-line, via fd-1 capture), warning/status lines
 *   - startup marker / title / menu registration (fd-1 capture)
 *   - event handling (keys, menu items, mouse) incl. prompt/confirm fakes
 *   - one real end-to-end save/load cycle in /tmp
 *
 * compat.c mocks unlink()/chdir() inertly, so the storage indirection in
 * tasks.c (tasks_store) is used to exercise failure paths, and the real
 * process cwd is changed with the raw chdir syscall (the mocked chdir() only
 * updates an in-memory string).
 *
 * Requires the host working directory to be writable for the /tmp tests;
 * they clean up after themselves.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#define main tasks_app_main
#include "../user/tasks.c"
#undef main

/* ================================================================== */
/* Check framework                                                     */
/* ================================================================== */

static int checks_run = 0;
static int checks_failed = 0;

#define CHECK(cond, name) do { \
    checks_run++; \
    if (cond) { \
        printf("  PASS: %s\n", (name)); \
    } else { \
        printf("  FAIL: %s\n", (name)); \
        checks_failed++; \
    } \
} while (0)

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static void state_init(struct task_state *st) {
    st->count = 0;
    st->load_error = 0;
    st->status[0] = '\0';
}

static void list_init(struct gui_list *l) {
    l->selected = 0;
    l->top = 0;
    l->count = 0;
    l->visible = TASKS_VISIBLE;
}

static void fill_text(char *out, int len, char fill) {
    for (int i = 0; i < len; i++) out[i] = fill;
    out[len] = '\0';
}

/* Add `n` items named "task 0".."task n-1". Returns the number added. */
static int add_n(struct task_state *st, int n) {
    char buf[32];
    int added = 0;
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "task %d", i);
        added += tasks_add(st, buf);
    }
    return added;
}

/* ---- stdout capture (print()/print_console() write to fd 1) ---- */

#define CAP_MAX 8192
static char  cap_buf[CAP_MAX];
static FILE *cap_file = NULL;
static int   cap_saved_fd = -1;

static int capture_begin(void) {
    fflush(stdout);
    cap_file = tmpfile();
    if (!cap_file) return -1;
    cap_saved_fd = dup(1);
    if (cap_saved_fd < 0) { fclose(cap_file); cap_file = NULL; return -1; }
    dup2(fileno(cap_file), 1);
    return 0;
}

static int capture_end(void) {
    fflush(stdout);
    dup2(cap_saved_fd, 1);
    close(cap_saved_fd);
    cap_saved_fd = -1;
    fseek(cap_file, 0, SEEK_SET);
    int n = (int)fread(cap_buf, 1, CAP_MAX - 1, cap_file);
    cap_buf[n] = '\0';
    fclose(cap_file);
    cap_file = NULL;
    return n;
}

/* Split the captured output into lines (strips '\f' and '\r'). */
static char cap_lines[32][96];
static int  cap_line_count = 0;

static void cap_split(void) {
    int li = 0, ci = 0;
    cap_line_count = 0;
    for (int i = 0; cap_buf[i] && li < 32; i++) {
        char c = cap_buf[i];
        if (c == '\f' || c == '\r') continue;
        if (c == '\n') {
            cap_lines[li][ci] = '\0';
            li++;
            ci = 0;
            cap_line_count = li;
            continue;
        }
        if (ci < 95) cap_lines[li][ci++] = c;
    }
    if (ci > 0 && li < 32) {
        cap_lines[li][ci] = '\0';
        cap_line_count = li + 1;
    }
}

static const char *cap_line(int idx) {
    if (idx < 0 || idx >= cap_line_count) return "";
    return cap_lines[idx];
}

/* Render the state through tasks_render and capture its output. */
static int render_capture(struct task_state *st, struct gui_list *l) {
    if (capture_begin() != 0) return -1;
    tasks_render(st, l);
    int n = capture_end();
    cap_split();
    return n;
}

/* ---- storage ops fakes ---- */

static struct tasks_store_ops saved_ops;
static void ops_save(void)    { saved_ops = tasks_store; }
static void ops_restore(void) { tasks_store = saved_ops; }

static int fake_open_calls = 0;
static int fake_open_fail(const char *name, int flags, ...) {
    (void)name; (void)flags;
    fake_open_calls++;
    return -1;
}

static ssize_t fake_write_fail(int fd, const void *buf, size_t size) {
    (void)fd; (void)buf; (void)size;
    return -1;
}
static ssize_t fake_write_short(int fd, const void *buf, size_t size) {
    (void)fd; (void)buf;
    return size > 0 ? (ssize_t)size - 1 : 0;
}

static int unlink_calls = 0;
static int fake_unlink_count(const char *name) {
    (void)name;
    unlink_calls++;
    return 0;
}

/* Real deletion, used to emulate HobbyOS' unlink-then-recreate save. */
static int fake_unlink_real(const char *name) {
    return remove(name);
}

/* ---- prompt/confirm fakes ---- */

static const char *fake_prompt_text = NULL;
static int fake_prompt_result = 1;
static int fake_prompt_calls = 0;

static int fake_prompt(const char *title, const char *msg, char *buf, int max) {
    (void)title; (void)msg;
    fake_prompt_calls++;
    if (fake_prompt_result && fake_prompt_text)
        snprintf(buf, (size_t)max, "%s", fake_prompt_text);
    return fake_prompt_result;
}

static int fake_confirm_result = 1;
static int fake_confirm_calls = 0;

static int fake_confirm(const char *title, const char *msg) {
    (void)title; (void)msg;
    fake_confirm_calls++;
    return fake_confirm_result;
}

/* ---- real file helpers (absolute paths: /tmp) ---- */

static int read_file(const char *path, char *out, int cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = (int)fread(out, 1, (size_t)(cap - 1), f);
    out[n] = '\0';
    fclose(f);
    return n;
}

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int n = (int)fwrite(data, 1, strlen(data), f);
    fclose(f);
    return n;
}

/* compat.c's chdir() only updates an in-memory string, so change the real
 * process cwd with the raw syscall. */
static int real_chdir(const char *path) {
#if defined(SYS_chdir)
    return (int)syscall(SYS_chdir, path);
#else
    (void)path;
    return -1;
#endif
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    struct task_state st;
    struct gui_list list;
    struct task_item it;
    struct gui_event ev;
    char buf[TASKS_FILE_BYTES + 64];
    char longtext[128];
    int have_tmp = 0;

    printf("=== Tasks Host Test ===\n");
    printf("Unit tests for src/user/tasks.c\n");

    /* The app saves with the relative name TODO.TXT, so move the real process
     * cwd to /tmp before anything can write (compat's chdir() is an inert
     * in-memory mock). Everything after this point keeps files in /tmp. */
    have_tmp = (real_chdir("/tmp") == 0);
    remove("/tmp/TODO.TXT");

    /* ---------------- parse_line ---------------- */
    printf("\n-- tasks_parse_line --\n");
    memset(&it, 0, sizeof(it));
    CHECK(tasks_parse_line("[ ] buy milk", (int)strlen("[ ] buy milk"), &it) == 1 &&
          it.done == 0 && strcmp(it.text, "buy milk") == 0,
          "parse_line: '[ ] buy milk' -> open item with text 'buy milk'");

    CHECK(tasks_parse_line("[x] pay rent", (int)strlen("[x] pay rent"), &it) == 1 &&
          it.done == 1 && strcmp(it.text, "pay rent") == 0,
          "parse_line: '[x] pay rent' -> done item");

    CHECK(tasks_parse_line("[X] upper", (int)strlen("[X] upper"), &it) == 1 &&
          it.done == 1 && strcmp(it.text, "upper") == 0,
          "parse_line: '[X]' accepted as done (case-insensitive marker)");

    CHECK(tasks_parse_line("plain text", (int)strlen("plain text"), &it) == 0,
          "parse_line: line without a marker is malformed");

    CHECK(tasks_parse_line("[y] nope", (int)strlen("[y] nope"), &it) == 0,
          "parse_line: '[y]' marker is malformed");

    CHECK(tasks_parse_line("[  ] two spaces", (int)strlen("[  ] two spaces"), &it) == 0,
          "parse_line: '[  ]' malformed (bracket not at index 2)");

    CHECK(tasks_parse_line("", 0, &it) == 0,
          "parse_line: empty line is malformed");

    CHECK(tasks_parse_line("[x", (int)strlen("[x"), &it) == 0,
          "parse_line: truncated marker is malformed");

    CHECK(tasks_parse_line("   [ ] indented", (int)strlen("   [ ] indented"), &it) == 0,
          "parse_line: leading spaces make a line malformed");

    memset(&it, 0, sizeof(it));
    CHECK(tasks_parse_line("[ ]", 3, &it) == 1 && it.text[0] == '\0' && it.done == 0,
          "parse_line: marker-only line -> task with empty text");

    CHECK(tasks_parse_line("[ ] a b  ", (int)strlen("[ ] a b  "), &it) == 1 &&
          strcmp(it.text, "a b  ") == 0,
          "parse_line: trailing spaces preserved");

    CHECK(tasks_parse_line("[ ]  two", (int)strlen("[ ]  two"), &it) == 1 &&
          strcmp(it.text, " two") == 0,
          "parse_line: only one separator space consumed");

    CHECK(tasks_parse_line("[x]tight", (int)strlen("[x]tight"), &it) == 1 &&
          strcmp(it.text, "tight") == 0,
          "parse_line: missing space after marker tolerated");

    CHECK(tasks_parse_line("[x] win\r", (int)strlen("[x] win\r"), &it) == 1 &&
          strcmp(it.text, "win") == 0,
          "parse_line: trailing CR stripped (CRLF files)");

    fill_text(longtext, 0, 'x');
    memcpy(longtext, "[ ] ", 4);
    fill_text(longtext + 4, 70, 'y');                 /* 4 + 70 + 1 bytes */
    memset(&it, 0, sizeof(it));
    CHECK(tasks_parse_line(longtext, (int)strlen(longtext), &it) == 1 &&
          (int)strlen(it.text) == TASKS_TEXT_MAX,
          "parse_line: 70-char text truncated to 60 chars");

    fill_text(longtext, 0, '\0');
    memcpy(longtext, "[ ] ", 4);
    fill_text(longtext + 4, TASKS_TEXT_MAX, 'q');      /* exactly 60 chars */
    memset(&it, 0, sizeof(it));
    CHECK(tasks_parse_line(longtext, (int)strlen(longtext), &it) == 1 &&
          (int)strlen(it.text) == TASKS_TEXT_MAX &&
          it.text[TASKS_TEXT_MAX - 1] == 'q',
          "parse_line: exactly-60-char text kept in full");

    /* ---------------- parse_buffer ---------------- */
    printf("\n-- tasks_parse_buffer --\n");
    state_init(&st);
    CHECK(tasks_parse_buffer(&st, "", 0) == 0 && st.count == 0,
          "parse_buffer: empty buffer -> no items");

    state_init(&st);
    {
        const char *only_bad = "junk\nmore junk\n\n[  ] nope\n";
        CHECK(tasks_parse_buffer(&st, only_bad, (int)strlen(only_bad)) == 0 &&
              st.count == 0,
              "parse_buffer: only malformed lines -> empty list");
    }

    state_init(&st);
    {
        const char *mixed = "[ ] one\njunk\n[x] two\n\n[ ] three\n";
        int added = tasks_parse_buffer(&st, mixed, (int)strlen(mixed));
        CHECK(added == 3 && st.count == 3 &&
              strcmp(st.items[0].text, "one") == 0 && st.items[0].done == 0 &&
              strcmp(st.items[1].text, "two") == 0 && st.items[1].done == 1 &&
              strcmp(st.items[2].text, "three") == 0,
              "parse_buffer: malformed + blank lines skipped, order kept");
    }

    state_init(&st);
    {
        const char *crlf = "[ ] a\r\n[x] b\r\n";
        CHECK(tasks_parse_buffer(&st, crlf, (int)strlen(crlf)) == 2 &&
              strcmp(st.items[0].text, "a") == 0 &&
              strcmp(st.items[1].text, "b") == 0 && st.items[1].done == 1,
              "parse_buffer: CRLF file parsed");
    }

    state_init(&st);
    {
        const char *no_nl = "[ ] a\n[ ] b";
        CHECK(tasks_parse_buffer(&st, no_nl, (int)strlen(no_nl)) == 2 && st.count == 2,
              "parse_buffer: last line without newline parsed");
    }

    state_init(&st);
    {
        /* 105 valid lines -> capped at 100 items */
        char big[105 * 8];
        int n = 0;
        for (int i = 0; i < 105; i++) {
            memcpy(big + n, "[ ] x\n", 6);
            n += 6;
        }
        int added = tasks_parse_buffer(&st, big, n);
        CHECK(added == TASKS_MAX && st.count == TASKS_MAX,
              "parse_buffer: 105 lines capped at 100 items");
    }

    state_init(&st);
    {
        const char *part1 = "[ ] a\n[ ] b\n";
        const char *part2 = "[ ] c\n[ ] d\n[ ] e\n";
        tasks_parse_buffer(&st, part1, (int)strlen(part1));
        int added = tasks_parse_buffer(&st, part2, (int)strlen(part2));
        CHECK(added == 3 && st.count == 5 &&
              strcmp(st.items[0].text, "a") == 0 &&
              strcmp(st.items[4].text, "e") == 0,
              "parse_buffer: appends to items already in the list");
    }

    /* ---------------- serialize ---------------- */
    printf("\n-- tasks_serialize --\n");
    state_init(&st);
    memset(buf, 'Z', sizeof(buf));
    CHECK(tasks_serialize(&st, buf, (int)sizeof(buf)) == 0 && buf[0] == '\0',
          "serialize: empty list -> zero bytes + NUL");

    state_init(&st);
    tasks_add(&st, "a");
    memset(buf, 'Z', sizeof(buf));
    {
        int n = tasks_serialize(&st, buf, (int)sizeof(buf));
        CHECK(n == 6 && strcmp(buf, "[ ] a\n") == 0,
              "serialize: one open item -> \"[ ] a\\n\" (6 bytes)");
    }

    state_init(&st);
    tasks_add(&st, "a");
    tasks_toggle(&st, 0);
    {
        int n = tasks_serialize(&st, buf, (int)sizeof(buf));
        CHECK(n == 6 && strcmp(buf, "[x] a\n") == 0,
              "serialize: one done item -> \"[x] a\\n\"");
    }

    state_init(&st);
    tasks_add(&st, "a");          /* 6 bytes  */
    tasks_add(&st, "bb");         /* 7 bytes  */
    tasks_add(&st, "ccc");        /* 8 bytes  */
    {
        int n = tasks_serialize(&st, buf, (int)sizeof(buf));
        CHECK(n == 6 + 7 + 8 && n == 5 * 3 + 1 + 2 + 3,
              "serialize: size is predictable (4 + len(text) + 1 per item)");
        CHECK(buf[n] == '\0' && buf[n - 1] == '\n',
              "serialize: NUL-terminated when there is room");
    }

    state_init(&st);
    tasks_add(&st, "a");
    tasks_add(&st, "b");
    memset(buf, 'Z', sizeof(buf));
    {
        int n = tasks_serialize(&st, buf, 7);       /* room for exactly one item */
        CHECK(n == 6 && strcmp(buf, "[ ] a\n") == 0 && buf[7] == 'Z' && buf[8] == 'Z',
              "serialize: never writes past the cap");
    }

    /* round-trip: list -> buffer -> parse -> identical list */
    state_init(&st);
    tasks_add(&st, "milk");
    tasks_add(&st, "call mom");
    tasks_toggle(&st, 1);
    tasks_add(&st, "trail  ");
    {
        char text60[128];
        fill_text(text60, 0, 'z');
        fill_text(text60, 60, 'q');
        tasks_add(&st, text60);
    }
    {
        struct task_state rt;
        int n = tasks_serialize(&st, buf, (int)sizeof(buf));
        state_init(&rt);
        int parsed = tasks_parse_buffer(&rt, buf, n);
        int same = (parsed == st.count && rt.count == st.count);
        for (int i = 0; same && i < st.count; i++) {
            if (rt.items[i].done != st.items[i].done) same = 0;
            if (strcmp(rt.items[i].text, st.items[i].text) != 0) same = 0;
        }
        CHECK(n == 9 + 13 + 12 + 65, "serialize: multi-item byte count adds up");
        CHECK(same, "round-trip: list -> buffer -> parse is identical");
        CHECK((int)strlen(rt.items[3].text) == 60,
              "round-trip: 60-char text survives untouched");
    }

    /* a full list's serialized size is exactly predictable */
    state_init(&st);
    {
        int expected = 0;
        for (int i = 0; i < TASKS_MAX; i++) {
            char t[32];
            snprintf(t, sizeof(t), "item %03d", i);
            expected += 4 + (int)strlen(t) + 1;
            tasks_add(&st, t);
        }
        int n = tasks_serialize(&st, buf, (int)sizeof(buf));
        CHECK(n == expected && n < TASKS_FILE_BYTES && buf[n] == '\0',
              "serialize: 100-item list size matches the predictability rule");
    }

    /* ---------------- add / toggle / delete / done count ---------------- */
    printf("\n-- tasks_add / tasks_toggle / tasks_delete / tasks_done_count --\n");
    state_init(&st);
    CHECK(tasks_add(&st, "first") == 1 && st.count == 1 &&
          strcmp(st.items[0].text, "first") == 0 && st.items[0].done == 0,
          "add: appends a new open item");
    CHECK(strcmp(st.status, "Added.") == 0, "add: status reports success");

    fill_text(longtext, 0, 'x');
    fill_text(longtext, 70, 'w');
    CHECK(tasks_add(&st, longtext) == 1 && (int)strlen(st.items[1].text) == 60,
          "add: text longer than 60 chars truncated");

    state_init(&st);
    CHECK(tasks_add(&st, "") == 0 && st.count == 0 &&
          strcmp(st.status, "Nothing to add.") == 0,
          "add: empty text rejected with a message");

    state_init(&st);
    CHECK(add_n(&st, TASKS_MAX) == TASKS_MAX && st.count == TASKS_MAX,
          "add: 100 items accepted");
    CHECK(tasks_add(&st, "one too many") == 0 && st.count == TASKS_MAX &&
          strcmp(st.status, "List is full (max 100).") == 0,
          "add: 101st item rejected with 'List is full (max 100).'");

    state_init(&st);
    add_n(&st, 3);
    CHECK(tasks_toggle(&st, -1) == -1 && tasks_toggle(&st, 3) == -1 &&
          st.items[0].done == 0 && st.items[2].done == 0,
          "toggle: out-of-range indexes rejected, state unchanged");
    CHECK(tasks_toggle(&st, 0) == 1 && st.items[0].done == 1 &&
          tasks_toggle(&st, 0) == 0 && st.items[0].done == 0,
          "toggle: flips done flag both ways");

    state_init(&st);
    CHECK(tasks_done_count(&st) == 0, "done_count: empty list -> 0");
    add_n(&st, 5);
    tasks_toggle(&st, 0);
    tasks_toggle(&st, 3);
    CHECK(tasks_done_count(&st) == 2, "done_count: counts only done items");
    tasks_toggle(&st, 0);
    CHECK(tasks_done_count(&st) == 1, "done_count: tracks un-toggling");

    state_init(&st);
    add_n(&st, 3);
    CHECK(tasks_delete(&st, 1) == 1 && st.count == 2 &&
          strcmp(st.items[0].text, "task 0") == 0 &&
          strcmp(st.items[1].text, "task 2") == 0,
          "delete: middle item removed, order preserved");
    CHECK(tasks_delete(&st, 0) == 1 && st.count == 1 &&
          strcmp(st.items[0].text, "task 2") == 0,
          "delete: first item removed");
    CHECK(tasks_delete(&st, 0) == 1 && st.count == 0,
          "delete: last item removed -> empty list");
    CHECK(tasks_delete(&st, 0) == 0 && tasks_toggle(&st, 0) == -1,
          "delete/toggle: on empty list are no-ops");

    state_init(&st);
    add_n(&st, 2);
    CHECK(tasks_delete(&st, 2) == 0 && tasks_delete(&st, -1) == 0 && st.count == 2,
          "delete: out-of-range index rejected");

    /* ---------------- selection / scrolling ---------------- */
    printf("\n-- selection + scrolling (gui_list) --\n");
    state_init(&st);
    list_init(&list);
    tasks_list_sync(&st, &list);
    CHECK(list.count == 0 && list.selected == 0 && list.top == 0 &&
          list.visible == TASKS_VISIBLE,
          "list_sync: empty state -> zeroed selection, 18 visible rows");

    gui_list_move(&list, -1);
    CHECK(list.selected == 0 && list.top == 0,
          "move: up at top stays put (no crash on empty list)");
    gui_list_move(&list, 1);
    CHECK(list.selected == 0 && list.top == 0,
          "move: down on empty list stays put (no crash)");

    state_init(&st);
    add_n(&st, 25);
    list_init(&list);
    tasks_list_sync(&st, &list);
    gui_list_move(&list, -1);
    CHECK(list.selected == 0, "move: selection clamped at first item");
    for (int i = 0; i < 30; i++) gui_list_move(&list, 1);
    CHECK(list.selected == 24 && list.top == 24 - (TASKS_VISIBLE - 1),
          "move: selection scrolls into view at the last item");
    CHECK(list.top + list.visible == list.count,
          "scroll: bottom window ends exactly at the last item");

    /* delete keeps selection in range */
    state_init(&st);
    add_n(&st, 3);
    list_init(&list);
    tasks_list_sync(&st, &list);
    list.selected = 2;
    tasks_delete(&st, 2);
    tasks_list_sync(&st, &list);
    CHECK(list.selected == 1 && list.count == 2 && list.top == 0,
          "delete: selection clamped to the last remaining item");

    list.selected = 1;
    tasks_delete(&st, 0);
    tasks_delete(&st, 0);
    tasks_list_sync(&st, &list);
    CHECK(st.count == 0 && list.selected == 0 && list.top == 0,
          "delete: removing every item resets selection and scroll");

    /* click mapping */
    state_init(&st);
    add_n(&st, 20);
    list_init(&list);
    tasks_list_sync(&st, &list);
    CHECK(gui_list_click_row(&list, tasks_first_item_row(&st), tasks_first_item_row(&st)) == 0,
          "click: row of the first item maps to index 0");
    CHECK(gui_list_click_row(&list, tasks_first_item_row(&st) + 1,
                             tasks_first_item_row(&st)) == 1,
          "click: second row maps to index 1");
    CHECK(gui_list_click_row(&list, tasks_first_item_row(&st) - 1,
                             tasks_first_item_row(&st)) == -1,
          "click: row above the list maps to nothing");
    CHECK(gui_list_click_row(&list, tasks_first_item_row(&st) + TASKS_VISIBLE + 2,
                             tasks_first_item_row(&st)) == -1,
          "click: row below the visible window maps to nothing");

    /* ---------------- render ---------------- */
    printf("\n-- tasks_render (captured output) --\n");
    state_init(&st);
    list_init(&list);
    tasks_list_sync(&st, &list);
    CHECK(render_capture(&st, &list) > 0, "render: produces output");
    CHECK(strcmp(cap_line(0), "=== Tasks ===") == 0,
          "render: line 1 is '=== Tasks ==='");
    CHECK(strcmp(cap_line(1), "(no tasks yet - press a to add one)") == 0,
          "render: empty list shows the add hint on line 2");
    CHECK(strcmp(cap_line(2), "a=add  Enter/space=toggle  d=delete  r=reload") == 0 &&
          cap_line_count == 3,
          "render: empty screen is title + hint + key help");

    state_init(&st);
    tasks_add(&st, "a");
    tasks_add(&st, "b");
    tasks_toggle(&st, 1);
    tasks_add(&st, "c");
    list_init(&list);
    tasks_list_sync(&st, &list);
    list.selected = 1;
    tasks_set_status(&st, "");
    render_capture(&st, &list);
    CHECK(strcmp(cap_line(1), "3 tasks, 1 done") == 0,
          "render: line 2 shows 'N tasks, M done'");
    CHECK(strcmp(cap_line(2), "   [ ] a") == 0 &&
          strcmp(cap_line(3), " > [x] b") == 0 &&
          strcmp(cap_line(4), "   [ ] c") == 0,
          "render: item rows use ' > ' / '   ' and '[ ] '/'[x] '");
    CHECK(cap_line_count == 6 &&
          strcmp(cap_line(5), "a=add  Enter/space=toggle  d=delete  r=reload") == 0,
          "render: key help is the last line");

    /* determinism */
    {
        char first[CAP_MAX];
        capture_begin();
        tasks_render(&st, &list);
        capture_end();
        memcpy(first, cap_buf, sizeof(first));
        render_capture(&st, &list);
        CHECK(strcmp(first, cap_buf) == 0,
              "render: two renders of the same state are byte-identical");
    }

    /* warning + status lines shift the item window */
    st.load_error = 1;
    render_capture(&st, &list);
    CHECK(strcmp(cap_line(2), "warning: cannot read TODO.TXT (starting empty)") == 0 &&
          strcmp(cap_line(3), "   [ ] a") == 0 &&
          tasks_first_item_row(&st) == 3,
          "render: load failure shows a warning line above the items");
    st.load_error = 0;
    tasks_set_status(&st, "Deleted.");
    render_capture(&st, &list);
    CHECK(strcmp(cap_line(2), "Deleted.") == 0 &&
          strcmp(cap_line(3), "   [ ] a") == 0 &&
          tasks_first_item_row(&st) == 3,
          "render: status line shown above the items");
    tasks_set_status(&st, "");
    st.load_error = 1;
    tasks_set_status(&st, "Save failed!");
    CHECK(tasks_first_item_row(&st) == 4,
          "first_item_row: warning + status add two header rows");
    st.load_error = 0;
    tasks_set_status(&st, "");

    /* full window + scrolling selection marker */
    state_init(&st);
    add_n(&st, 100);
    tasks_set_status(&st, "");              /* add() leaves an "Added." status */
    list_init(&list);
    tasks_list_sync(&st, &list);
    render_capture(&st, &list);
    CHECK(cap_line_count == 2 + TASKS_VISIBLE + 1,
          "render: 100 items draw exactly 18 item rows");
    CHECK(strcmp(cap_line(2), " > [ ] task 0") == 0,
          "render: first item is selected initially");
    CHECK(strcmp(cap_line(cap_line_count - 1),
                 "a=add  Enter/space=toggle  d=delete  r=reload") == 0,
          "render: key help still last with a full list");

    list.selected = 20;
    gui_list_ensure_visible(&list);
    render_capture(&st, &list);
    CHECK(list.top == 3 && strcmp(cap_line(2), "   [ ] task 3") == 0 &&
          strcmp(cap_line(19), " > [ ] task 20") == 0,
          "render: scrolled window keeps the selection visible");

    /* header line length cap (window is ~70 cols) */
    state_init(&st);
    add_n(&st, 100);
    tasks_set_status(&st, "");
    list_init(&list);
    tasks_list_sync(&st, &list);
    render_capture(&st, &list);
    {
        int longest = 0;
        for (int i = 0; i < cap_line_count; i++) {
            int n = (int)strlen(cap_line(i));
            if (n > longest) longest = n;
        }
        CHECK(longest <= 70, "render: no line exceeds 70 columns");
    }

    /* ---------------- startup sequences ---------------- */
    printf("\n-- startup (title, menu, marker) --\n");
    capture_begin();
    tasks_startup();
    capture_end();
    CHECK(strstr(cap_buf, "[APP] TASKS started\n") != NULL,
          "startup: prints the '[APP] TASKS started' marker");
    CHECK(strstr(cap_buf, "\033]T") != NULL && strstr(cap_buf, "Tasks") != NULL,
          "startup: sets the window title to 'Tasks'");
    CHECK(strstr(cap_buf, "\033]M0;Tasks;Add,Toggle,Delete,Reload\a") != NULL,
          "startup: registers menu 0 'Tasks' -> Add,Toggle,Delete,Reload");

    /* ---------------- event handling ---------------- */
    printf("\n-- tasks_handle_event --\n");
    state_init(&st);
    add_n(&st, 3);
    list_init(&list);
    tasks_list_sync(&st, &list);
    memset(&ev, 0, sizeof(ev));

    ev.type = GUI_EV_DOWN;
    tasks_handle_event(&st, &list, &ev);
    ev.type = GUI_EV_DOWN;
    tasks_handle_event(&st, &list, &ev);
    CHECK(list.selected == 2, "event: DOWN moves the selection down");
    ev.type = GUI_EV_UP;
    tasks_handle_event(&st, &list, &ev);
    CHECK(list.selected == 1, "event: UP moves the selection up");

    ev.type = GUI_EV_CHAR;
    ev.ch = ' ';
    tasks_handle_event(&st, &list, &ev);
    CHECK(st.items[1].done == 1, "event: space toggles the selected item");
    ev.ch = '\n';
    tasks_handle_event(&st, &list, &ev);
    CHECK(st.items[1].done == 0, "event: Enter toggles the selected item");

    ev.ch = 'x';
    CHECK(tasks_handle_event(&st, &list, &ev) == 0 && st.count == 3,
          "event: unknown keys are ignored (no quit)");
    ev.type = GUI_EV_ESC;
    CHECK(tasks_handle_event(&st, &list, &ev) == 0 && st.count == 3,
          "event: ESC is ignored");
    ev.type = 12345;
    CHECK(tasks_handle_event(&st, &list, &ev) == 0 && st.count == 3,
          "event: unknown event types are ignored");
    ev.type = GUI_EV_CHAR;
    ev.ch = 'q';
    CHECK(tasks_handle_event(&st, &list, &ev) == 1 &&
          st.count == 3 && list.selected == 1,
          "event: 'q' asks the main loop to exit");
    ev.ch = 'Q';
    CHECK(tasks_handle_event(&st, &list, &ev) == 1,
          "event: 'Q' also asks to exit");

    /* menu items */
    state_init(&st);
    add_n(&st, 3);
    list_init(&list);
    tasks_list_sync(&st, &list);
    ev.type = GUI_EV_MENU;
    ev.menu = 0;
    ev.item = 1;                                   /* Toggle */
    tasks_handle_event(&st, &list, &ev);
    CHECK(st.items[0].done == 1, "menu: item 1 (Toggle) toggles the selection");
    ev.item = 9;
    CHECK(tasks_handle_event(&st, &list, &ev) == 0 && st.count == 3,
          "menu: unknown item index ignored");

    /* mouse click selects a row */
    list.selected = 0;
    ev.type = GUI_EV_MOUSE;
    ev.button = 1;
    ev.y = tasks_first_item_row(&st) + 2;
    ev.x = 5;
    tasks_handle_event(&st, &list, &ev);
    CHECK(list.selected == 2, "mouse: left click selects the clicked row");
    list.selected = 1;
    ev.button = 2;
    ev.y = tasks_first_item_row(&st);
    tasks_handle_event(&st, &list, &ev);
    CHECK(list.selected == 1, "mouse: right click does not change the selection");

    /* ---------------- prompt / confirm paths (fakes) ---------------- */
    printf("\n-- add/delete dialogs (faked prompt + confirm) --\n");

    tasks_prompt_fn = fake_prompt;
    tasks_confirm_fn = fake_confirm;

    state_init(&st);
    list_init(&list);
    tasks_list_sync(&st, &list);
    fake_prompt_text = "from dialog";
    fake_prompt_result = 1;
    fake_prompt_calls = 0;
    ev.type = GUI_EV_CHAR;
    ev.ch = 'a';
    tasks_handle_event(&st, &list, &ev);
    CHECK(fake_prompt_calls == 1 && st.count == 1 &&
          strcmp(st.items[0].text, "from dialog") == 0 && list.selected == 0,
          "dialog: 'a' prompts, appends the typed text and selects it");

    fake_prompt_result = 0;                         /* user pressed ESC */
    tasks_handle_event(&st, &list, &ev);
    CHECK(st.count == 1, "dialog: cancelled prompt adds nothing");

    fake_confirm_result = 0;
    fake_confirm_calls = 0;
    ev.ch = 'd';
    tasks_handle_event(&st, &list, &ev);
    CHECK(fake_confirm_calls == 1 && st.count == 1,
          "dialog: 'd' asks for confirmation and keeps the item on No");
    fake_confirm_result = 1;
    tasks_handle_event(&st, &list, &ev);
    CHECK(st.count == 0 && list.selected == 0 && strcmp(st.status, "Deleted.") == 0,
          "dialog: confirmed delete removes the item");

    state_init(&st);
    list_init(&list);
    tasks_list_sync(&st, &list);
    tasks_handle_event(&st, &list, &ev);            /* 'd' on empty list */
    CHECK(st.count == 0 && strcmp(st.status, "Nothing to delete.") == 0,
          "dialog: delete on empty list is a no-op with a message");

    /* menu Add / Delete mirror the keys */
    fake_prompt_text = "via menu";
    fake_prompt_result = 1;
    ev.type = GUI_EV_MENU;
    ev.item = 0;
    tasks_handle_event(&st, &list, &ev);
    CHECK(st.count == 1 && strcmp(st.items[0].text, "via menu") == 0,
          "menu: item 0 (Added) routes through the prompt");

    /* ---------------- real file I/O in /tmp ---------------- */
    printf("\n-- persistence (real files in /tmp) --\n");
    CHECK(have_tmp, "chdir(/tmp) for the real-file tests");

    if (have_tmp) {
        /* load an existing file with malformed lines mixed in */
        write_file("/tmp/TODO.TXT",
                   "[ ] buy milk\njunk line\n[x] ship it\n[  ] broken\n[ ] trail  \n");
        state_init(&st);
        CHECK(tasks_load(&st) == 3 && st.count == 3 && st.load_error == 0 &&
              strcmp(st.items[0].text, "buy milk") == 0 &&
              st.items[1].done == 1 && strcmp(st.items[1].text, "ship it") == 0 &&
              strcmp(st.items[2].text, "trail  ") == 0,
              "load: reads TODO.TXT, ignores malformed lines, keeps trailing spaces");

        /* load replaces the in-memory list */
        add_n(&st, 5);
        int n = tasks_load(&st);
        CHECK(n == 3 && st.count == 3 && st.status[0] == '\0',
              "load: reload replaces the in-memory list and clears status");

        /* empty file is not an error */
        write_file("/tmp/TODO.TXT", "");
        state_init(&st);
        CHECK(tasks_load(&st) == 0 && st.count == 0 && st.load_error == 0,
              "load: empty file -> empty list, no warning");

        /* only malformed content is not an error either */
        write_file("/tmp/TODO.TXT", "nope\nnope\n");
        state_init(&st);
        CHECK(tasks_load(&st) == 0 && st.count == 0 && st.load_error == 0,
              "load: all-malformed file -> empty list, no warning");

        /* save writes exactly the serialized bytes */
        state_init(&st);
        tasks_add(&st, "alpha");
        tasks_add(&st, "beta");
        tasks_toggle(&st, 1);
        remove("/tmp/TODO.TXT");
        CHECK(tasks_save(&st) == 0, "save: returns 0 on success");
        {
            char file[256];
            file[0] = '\0';
            int fn = read_file("/tmp/TODO.TXT", file, (int)sizeof(file));
            CHECK(fn == 19 && strcmp(file, "[ ] alpha\n[x] beta\n") == 0,
                  "save: TODO.TXT contains exactly the serialized list");
            CHECK(fn == 10 + 9,
                  "save: file size matches the predictability rule");
        }

        /* load after save is a faithful round-trip */
        state_init(&st);
        CHECK(tasks_load(&st) == 2 && strcmp(st.items[0].text, "alpha") == 0 &&
              st.items[1].done == 1 && strcmp(st.items[1].text, "beta") == 0,
              "save+load: round-trip through the real file is identical");

        /* unlink-before-write: the HobbyOS save drops a stale tail */
        ops_save();
        unlink_calls = 0;
        tasks_store.unlink = fake_unlink_count;
        tasks_save(&st);
        CHECK(unlink_calls == 1,
              "save: unlinks TODO.TXT before recreating it (stale-tail fix)");
        ops_restore();

        /* with a real unlink the file is always exactly the new list */
        ops_save();
        tasks_store.unlink = fake_unlink_real;
        tasks_add(&st, "gamma");
        tasks_save(&st);                            /* 3 items on disk */
        tasks_delete(&st, 0);                       /* drop "alpha" */
        tasks_save(&st);                            /* now: beta(done) + gamma */
        ops_restore();
        {
            char file[256];
            file[0] = '\0';
            read_file("/tmp/TODO.TXT", file, (int)sizeof(file));
            CHECK(strcmp(file, "[x] beta\n[ ] gamma\n") == 0,
                  "save: shorter list leaves no stale bytes behind");
        }

        /* open failure path -> warning state */
        remove("/tmp/TODO.TXT");
        write_file("/tmp/TODO.TXT", "[ ] keep me\n");
        state_init(&st);
        add_n(&st, 4);
        ops_save();
        fake_open_calls = 0;
        tasks_store.open = fake_open_fail;
        CHECK(tasks_load(&st) == -1 && st.count == 0 && st.load_error == 1 &&
              fake_open_calls == 1,
              "load: unreadable file -> -1, empty list, load_error set");
        ops_restore();
        list_init(&list);
        tasks_list_sync(&st, &list);
        render_capture(&st, &list);
        CHECK(strcmp(cap_line(2), "warning: cannot read TODO.TXT (starting empty)") == 0 &&
              strcmp(cap_line(1), "(no tasks yet - press a to add one)") == 0,
              "load: warning state renders the warning line");
        CHECK(tasks_load(&st) == 1 && st.load_error == 0 && st.count == 1,
              "load: succeeds again once storage is restored");

        /* write failure path -> save error + status message */
        ops_save();
        tasks_store.write = fake_write_fail;
        CHECK(tasks_save(&st) == -1, "save: write failure reported as -1");
        ops_restore();

        ops_save();
        tasks_store.write = fake_write_short;
        CHECK(tasks_save(&st) == -1, "save: short write reported as -1");
        ops_restore();

        /* a failed save during an action surfaces in the status line */
        state_init(&st);
        tasks_add(&st, "keep");
        list_init(&list);
        tasks_list_sync(&st, &list);
        ops_save();
        tasks_store.write = fake_write_fail;
        ev.type = GUI_EV_CHAR;
        ev.ch = ' ';
        tasks_handle_event(&st, &list, &ev);
        ops_restore();
        CHECK(st.items[0].done == 1 && strcmp(st.status, "Save failed!") == 0,
              "save: failed action save sets 'Save failed!' status");

        /* menu Reload replaces the in-memory list from disk */
        write_file("/tmp/TODO.TXT", "[x] from disk\n[ ] also disk\n");
        state_init(&st);
        add_n(&st, 7);
        list_init(&list);
        tasks_list_sync(&st, &list);
        ev.type = GUI_EV_MENU;
        ev.item = 3;
        tasks_handle_event(&st, &list, &ev);
        CHECK(st.count == 2 && st.items[0].done == 1 &&
              strcmp(st.items[1].text, "also disk") == 0 &&
              strcmp(st.status, "Reloaded 2 tasks.") == 0,
              "menu: Reload replaces the list from TODO.TXT");

        /* 'r' key reload path */
        write_file("/tmp/TODO.TXT", "[ ] three\n[ ] four\n[ ] five\n");
        ev.type = GUI_EV_CHAR;
        ev.ch = 'r';
        tasks_handle_event(&st, &list, &ev);
        CHECK(st.count == 3 && strcmp(st.status, "Reloaded 3 tasks.") == 0,
              "event: 'r' reloads from disk");

        /* reload failure -> warning + message */
        ops_save();
        tasks_store.open = fake_open_fail;
        ev.ch = 'r';
        tasks_handle_event(&st, &list, &ev);
        ops_restore();
        CHECK(st.count == 0 && st.load_error == 1 &&
              strcmp(st.status, "Reload failed - starting empty.") == 0,
              "event: failed reload empties the list with a message");

        /* a full add/save cycle through the key handler */
        remove("/tmp/TODO.TXT");
        state_init(&st);
        list_init(&list);
        tasks_list_sync(&st, &list);
        fake_prompt_text = "written by 'a'";
        ev.type = GUI_EV_CHAR;
        ev.ch = 'a';
        tasks_handle_event(&st, &list, &ev);
        {
            char file[256];
            file[0] = '\0';
            read_file("/tmp/TODO.TXT", file, (int)sizeof(file));
            CHECK(strcmp(file, "[ ] written by 'a'\n") == 0,
                  "integration: 'a' writes the new item straight to TODO.TXT");
        }
        state_init(&st);
        CHECK(tasks_load(&st) == 1 && strcmp(st.items[0].text, "written by 'a'") == 0,
              "integration: reload sees the item added through the UI");

        remove("/tmp/TODO.TXT");
    } else {
        printf("  FAIL: cannot chdir to /tmp - file tests skipped\n");
        checks_failed++;
        checks_run++;
    }

    /* ---------------- summary ---------------- */
    printf("\n=== Test Results ===\n");
    printf("Checks run:    %d\n", checks_run);
    printf("Checks passed: %d\n", checks_run - checks_failed);
    printf("Checks failed: %d\n", checks_failed);

    if (checks_failed == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d CHECK(S) FAILED\n", checks_failed);
    return 1;
}
