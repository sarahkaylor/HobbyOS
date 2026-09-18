/*
 * gui_test.c - Host unit tests for the HobbyOS GUI toolkit (src/user/gui.c).
 *
 * Style: includes the implementation directly so static details are visible
 * and no extra link rules are needed beyond compat.o.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#include "../user/gui.c"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } \
} while (0)

#define RUN(fn) do { tests_run++; printf("  Test: %s ...\n", #fn); fn(); } while (0)

/* ---- decoding ---- */

static void test_decode_char(void) {
    struct gui_event ev;
    int n = gui_decode("a", 1, &ev);
    CHECK(n == 1, "char consumed 1 byte");
    CHECK(ev.type == GUI_EV_CHAR, "char type");
    CHECK(ev.ch == 'a', "char value");

    n = gui_decode("\n", 1, &ev);
    CHECK(n == 1 && ev.type == GUI_EV_CHAR && ev.ch == '\n', "newline as char");
}

static void test_decode_arrows(void) {
    struct gui_event ev;
    CHECK(gui_decode("\033[A", 3, &ev) == 3 && ev.type == GUI_EV_UP, "up");
    CHECK(gui_decode("\033[B", 3, &ev) == 3 && ev.type == GUI_EV_DOWN, "down");
    CHECK(gui_decode("\033[C", 3, &ev) == 3 && ev.type == GUI_EV_RIGHT, "right");
    CHECK(gui_decode("\033[D", 3, &ev) == 3 && ev.type == GUI_EV_LEFT, "left");
    // incomplete
    CHECK(gui_decode("\033[A", 2, &ev) == 0, "arrow incomplete");
    CHECK(gui_decode("\033", 1, &ev) == 0, "bare ESC incomplete");
}

static void test_decode_menu(void) {
    struct gui_event ev;
    int n = gui_decode("\033[M1;3~", 7, &ev);
    CHECK(n == 7, "menu sequence length");
    CHECK(ev.type == GUI_EV_MENU, "menu type");
    CHECK(ev.menu == 1 && ev.item == 3, "menu indices");
    // incomplete at various points
    CHECK(gui_decode("\033[M1;3", 6, &ev) == 0, "missing terminator -> incomplete");
    CHECK(gui_decode("\033[M1;", 5, &ev) == 0, "missing item -> incomplete");
    // invalid: no digits
    CHECK(gui_decode("\033[Mx;3~", 7, &ev) == -1, "bad menu -> invalid");
}

static void test_decode_mouse(void) {
    struct gui_event ev;
    int n = gui_decode("\033[P12;5;1~", 10, &ev);
    CHECK(n == 10, "mouse sequence length");
    CHECK(ev.type == GUI_EV_MOUSE, "mouse type");
    CHECK(ev.x == 12 && ev.y == 5 && ev.button == 1, "mouse coords");
    CHECK(gui_decode("\033[P0;0;2~", 9, &ev) == 9 && ev.x == 0 && ev.y == 0 && ev.button == 2,
          "mouse corner");
    CHECK(gui_decode("\033[P12;5;1", 9, &ev) == 0, "mouse incomplete");
}

/* ---- numbers ---- */

static void test_itoa(void) {
    char buf[32];
    CHECK(gui_itoa(0, buf) == 1 && !strcmp(buf, "0"), "itoa 0");
    CHECK(gui_itoa(123, buf) == 3 && !strcmp(buf, "123"), "itoa 123");
    CHECK(gui_itoa(-45, buf) == 3 && !strcmp(buf, "-45"), "itoa -45");
    CHECK(gui_uitoa(4000000000UL, buf) == 10 && !strcmp(buf, "4000000000"), "uitoa big");
    CHECK(gui_uitoa_z(7, buf, 2) == 2 && !strcmp(buf, "07"), "zpad 2");
    CHECK(gui_uitoa_z(7, buf, 4) == 4 && !strcmp(buf, "0007"), "zpad 4");
    CHECK(gui_uitoa_z(12345, buf, 2) == 5 && !strcmp(buf, "12345"), "zpad no shrink");
    CHECK(gui_itoa_pad(42, buf, 6) == 6 && !strcmp(buf, "    42"), "pad");
}

static void test_size_str(void) {
    char buf[32];
    gui_size_str(512, buf);
    CHECK(!strcmp(buf, "512B"), "512B");
    gui_size_str(1024, buf);
    CHECK(!strcmp(buf, "1.0K"), "1.0K");
    gui_size_str(1536, buf);
    CHECK(!strcmp(buf, "1.5K"), "1.5K");
    gui_size_str(64ULL * 1024 * 1024, buf);
    CHECK(!strcmp(buf, "64.0M"), "64.0M");
    gui_size_str(150ULL * 1024 * 1024, buf);
    CHECK(!strcmp(buf, "150.0M"), "150.0M");
    gui_size_str(2ULL * 1024 * 1024 * 1024, buf);
    CHECK(!strcmp(buf, "2.0G"), "2.0G");
    gui_size_str(1023, buf);
    CHECK(!strcmp(buf, "1023B"), "1023B");
}

/* ---- canvas ---- */

static void test_rule_and_box(void) {
    char buf[128];
    gui_rule(buf, 8);
    CHECK(!strcmp(buf, "+------+"), "rule 8");
    gui_box_row(buf, 20, "Files");
    CHECK(strlen(buf) == 20, "box row length");
    CHECK(buf[0] == '+' && buf[19] == '+', "box row corners");
    CHECK(strstr(buf, "Files") != NULL, "box row title");
}

static void test_bar(void) {
    char buf[128];
    int n = gui_bar(buf, 20, 50);
    CHECK(n == 20 && (int)strlen(buf) == 20, "bar width");
    CHECK(buf[0] == '[' && buf[15] == ']', "bar brackets");
    CHECK(!strcmp(buf + 16, " 50%"), "bar pct 50");
    gui_bar(buf, 20, 0);
    CHECK(!strcmp(buf + 16, "  0%"), "bar pct 0");
    gui_bar(buf, 20, 100);
    CHECK(!strcmp(buf + 16, "100%"), "bar pct 100");
    gui_bar(buf, 20, 5);
    CHECK(!strcmp(buf + 16, "  5%"), "bar pct 5");
    gui_bar(buf, 20, 250);
    CHECK(!strcmp(buf + 16, "100%"), "bar clamps high");
    gui_bar(buf, 20, -3);
    CHECK(!strcmp(buf + 16, "  0%"), "bar clamps low");
    // fill count sanity: 50% of 14 inner = 7 '#'
    int hashes = 0;
    for (int i = 0; buf[i]; i++) if (buf[i] == '#') hashes++;
    CHECK(hashes == 0, "0% has no hashes");
    gui_bar(buf, 20, 50);
    hashes = 0;
    for (int i = 0; buf[i]; i++) if (buf[i] == '#') hashes++;
    CHECK(hashes == 7, "50% has 7 hashes");
}

static void test_fit_center(void) {
    char buf[64];
    gui_fit(buf, 10, "abc");
    CHECK(!strcmp(buf, "abc       ") && strlen(buf) == 10, "fit pads");
    gui_fit(buf, 3, "abcdef");
    CHECK(!strcmp(buf, "abc") && strlen(buf) == 3, "fit truncates");
    gui_center(buf, 11, "hi");
    CHECK(!strcmp(buf, "    hi     "), "center");
    gui_center(buf, 4, "hello");
    CHECK(strlen(buf) == 4, "center clips");
}

/* ---- lists ---- */

static void test_list(void) {
    struct gui_list l = {0, 0, 100, 10};
    gui_list_move(&l, 3);
    CHECK(l.selected == 3 && l.top == 0, "move within");
    gui_list_move(&l, 20);
    CHECK(l.selected == 23 && l.top == 14, "scroll follows");
    gui_list_move(&l, -100);
    CHECK(l.selected == 0 && l.top == 0, "clamp top");
    gui_list_move(&l, 1000);
    CHECK(l.selected == 99 && l.top == 90, "clamp bottom");
    // click mapping: rows 5..14 visible, top=90 -> click row 7 => item 92
    CHECK(gui_list_click_row(&l, 7, 5) == 92, "click maps");
    CHECK(gui_list_click_row(&l, 3, 5) == -1, "click above");
    CHECK(gui_list_click_row(&l, 20, 5) == -1, "click below");
}

/* ---- strings ---- */

static void test_strings(void) {
    char buf[64];
    CHECK(gui_strlen("hello") == 5, "strlen");
    gui_strcpy(buf, "abc");
    CHECK(!strcmp(buf, "abc"), "strcpy");
    gui_strncpy(buf, "abcdef", 4);
    CHECK(!strcmp(buf, "abc"), "strncpy truncates + terminates");
    CHECK(gui_strcmp("abc", "abc") == 0, "strcmp eq");
    CHECK(gui_strcmp("abc", "abd") < 0, "strcmp lt");
    CHECK(gui_strncmp("abcx", "abcy", 3) == 0, "strncmp prefix");
    CHECK(gui_starts_with("Files.BIN", "Files"), "starts_with");
    CHECK(!gui_starts_with("Files", "Files.BIN"), "starts_with false");
    gui_strcpy(buf, "ab");
    CHECK(gui_append(buf, "cd") == 4 && !strcmp(buf, "abcd"), "append");
    CHECK(gui_ci_find("Hello World", "world") == 6, "ci_find");
    CHECK(gui_ci_find("Hello", "xyz") == -1, "ci_find miss");
    gui_upper(buf, "abc.bin", sizeof(buf));
    CHECK(!strcmp(buf, "ABC.BIN"), "upper");
}

int main(void) {
    printf("=== gui toolkit unit tests ===\n");
    RUN(test_decode_char);
    RUN(test_decode_arrows);
    RUN(test_decode_menu);
    RUN(test_decode_mouse);
    RUN(test_itoa);
    RUN(test_size_str);
    RUN(test_rule_and_box);
    RUN(test_bar);
    RUN(test_fit_center);
    RUN(test_list);
    RUN(test_strings);

    printf("=== Results: %d run, %d failed ===\n", tests_run, tests_failed);
    if (tests_failed) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL GUI TESTS PASSED\n");
    return 0;
}
