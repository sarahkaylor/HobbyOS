/* Host unit tests for the Apps menu ordering + display labels (desktop.c).
 *
 * Exercises menu_apps_first() / menu_display_name() directly, and load_menu()
 * end-to-end through the compat mock read_dir override, mirroring the three
 * menus the real suites see: the full desktop disk, desktop_test's two-file
 * menu, and apps_test's all-ten-apps mock.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../user_include/libc.h"

extern char menu_items[80][16];
extern int num_menu_items;
extern void menu_apps_first(void);
extern void menu_display_name(const char *raw, char *out, int max);
extern void load_menu(void);

/* compat.c mock read_dir override (same pattern as apps_suite_test.c). */
extern int  mock_read_dir_count;
extern char mock_read_dir_names[40][32];
void mock_read_dir_reset(void);

static int fails = 0, checks = 0;

static void check(int cond, const char *msg) {
  checks++;
  if (cond) printf("  PASS: %s\n", msg);
  else { printf("  FAIL: %s\n", msg); fails++; }
}

static void set_menu(const char *const *names, int n) {
  num_menu_items = n;
  for (int i = 0; i < n; i++) {
    int k = 0;
    while (names[i][k] && k < 15) { menu_items[i][k] = names[i][k]; k++; }
    menu_items[i][k] = '\0';
  }
}

static int menu_is(const char *const *names, int n) {
  if (num_menu_items != n) return 0;
  for (int i = 0; i < n; i++)
    if (strcmp(menu_items[i], names[i]) != 0) return 0;
  return 1;
}

int main(void) {
  printf("[TEST] desktop menu ordering (Apps pinned first)\n");

  /* 1) Mixed list: apps scattered among other files -> apps first; both
   *    groups keep their relative read_dir order. */
  {
    const char *in[] = {
      "SHTEST.BIN", "CALC.BIN", "EDITOR.BIN", "FILES.BIN", "CLOCK.BIN",
      "TEST.TXT", "SYSMON.BIN", "HEX.BIN", "TASKS.BIN", "FIND.BIN",
      "NOTES.TXT", "DIFF.BIN", "NOTES.BIN", "UNIT.BIN", "PONG.BIN",
    };
    const char *want[] = {
      "CALC.BIN", "FILES.BIN", "CLOCK.BIN", "SYSMON.BIN", "HEX.BIN",
      "TASKS.BIN", "FIND.BIN", "DIFF.BIN", "NOTES.BIN", "UNIT.BIN",
      "SHTEST.BIN", "EDITOR.BIN", "TEST.TXT", "NOTES.TXT", "PONG.BIN",
    };
    set_menu(in, 15);
    menu_apps_first();
    check(menu_is(want, 15),
          "scattered apps pinned first; relative order preserved");
  }

  /* 2) No pinned apps present (desktop_test's two-file menu): unchanged. */
  {
    const char *in[] = { "NETTEST.BIN", "EDITOR.BIN" };
    set_menu(in, 2);
    menu_apps_first();
    check(menu_is(in, 2), "no pinned apps present -> order untouched");
  }

  /* 3) All entries pinned (apps_test harness case): unchanged. */
  {
    const char *in[] = {
      "FILES.BIN", "CALC.BIN", "CLOCK.BIN", "SYSMON.BIN", "HEX.BIN",
      "TASKS.BIN", "FIND.BIN", "DIFF.BIN", "NOTES.BIN", "UNIT.BIN",
    };
    set_menu(in, 10);
    menu_apps_first();
    check(menu_is(in, 10), "all pinned -> order untouched");
  }

  /* 4) Display labels strip a trailing ".BIN" only. */
  {
    char out[16];
    menu_display_name("FILES.BIN", out, sizeof(out));
    check(strcmp(out, "FILES") == 0, "\"FILES.BIN\" displays as \"FILES\"");
    menu_display_name("SYSMON.BIN", out, sizeof(out));
    check(strcmp(out, "SYSMON") == 0, "\"SYSMON.BIN\" displays as \"SYSMON\"");
    menu_display_name("TEST.TXT", out, sizeof(out));
    check(strcmp(out, "TEST.TXT") == 0, "non-\".BIN\" name unchanged");
    menu_display_name("SH.BIN", out, sizeof(out));
    check(strcmp(out, "SH") == 0, "\"SH.BIN\" displays as \"SH\"");
  }

  /* 5) load_menu() end-to-end through the mock read_dir override. */
  {
    mock_read_dir_reset();
    mock_read_dir_count = 6;
    snprintf(mock_read_dir_names[0], 32, "STRESS.BIN");
    snprintf(mock_read_dir_names[1], 32, "UNIT.BIN");
    snprintf(mock_read_dir_names[2], 32, "SHTEST.BIN");
    snprintf(mock_read_dir_names[3], 32, "FILES.BIN");
    snprintf(mock_read_dir_names[4], 32, "NOTES.TXT");
    snprintf(mock_read_dir_names[5], 32, "TASKS.BIN");
    load_menu();
    const char *want[] = {
      "UNIT.BIN", "FILES.BIN", "TASKS.BIN",
      "STRESS.BIN", "SHTEST.BIN", "NOTES.TXT",
    };
    check(menu_is(want, 6), "load_menu(): mock dir reordered, apps first");
  }

  /* 6) load_menu() on the default canned dir (editor_test's world): the
   *    editor stays at index 0 so the right-click-first-item flow holds. */
  {
    mock_read_dir_reset();
    load_menu();
    check(num_menu_items == 7 && strcmp(menu_items[0], "EDITOR.BIN") == 0,
          "default mock dir: EDITOR.BIN stays at menu index 0");
  }

  printf("[TEST] desktop menu: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
