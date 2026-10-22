/* games_scan_test.c — T1 host golden test for the S2 game-launcher /GAMES
 * scan + menu ordering (GB-039).  Exercises the pure classify/order logic
 * (no guest FS): .BIN/ELF/OTHER classification, pinned-first-then-scanned
 * ordering, the MAX_MENU_ITEMS cap, and the kind labels.  Run: this binary. */
#include "../user/games_scan.h"
#include <stdio.h>
#include <string.h>

static int g_fails = 0;
static int g_checks = 0;

static void check(int cond, const char *msg) {
  g_checks++;
  if (cond) { printf("  PASS: %s\n", msg); }
  else { g_fails++; printf("  FAIL: %s\n", msg); }
}

static int str_eq(const char *a, const char *b) {
  int i = 0;
  while (a[i] && b[i] && a[i] == b[i]) i++;
  return a[i] == b[i];
}

int main(void) {
  printf("=== games_classify ===\n");
  check(games_classify("DOOM.BIN") == GAMES_BIN, "DOOM.BIN -> BIN");
  check(games_classify("star.bin") == GAMES_BIN, "star.bin (lower) -> BIN");
  check(games_classify("GAME.EXE") == GAMES_BIN, "GAME.EXE -> BIN");
  check(games_classify("foo.ELF") == GAMES_ELF, "foo.ELF -> ELF");
  check(games_classify("linuxgame") == GAMES_ELF, "extension-less -> ELF candidate");
  check(games_classify("map.wad") == GAMES_OTHER, "map.wad -> OTHER (data)");
  check(games_classify("NOTES.TXT") == GAMES_OTHER, "NOTES.TXT -> OTHER");
  check(games_classify("") == GAMES_NONE, "empty -> NONE");

  printf("=== games_order (pinned-first, then scanned, cap) ===\n");
  struct games_entry pinned[2] = {
    {"PONG.BIN", GAMES_BIN},
    {"MILLIPED.BIN", GAMES_BIN},
  };
  struct games_entry scanned[4] = {
    {"NEWGAME.BIN", GAMES_BIN},
    {"compat_game", GAMES_ELF},
    {"skip.txt", GAMES_OTHER},
    {"another_elf", GAMES_ELF},
  };
  struct games_entry out[8];
  int n = games_order(out, pinned, 2, scanned, 4, 8);
  check(n == 5, "order surfaces 2 pinned + 3 launchable scanned (not OTHER)");
  check(str_eq(out[0].name, "PONG.BIN"), "pinned PONG first");
  check(str_eq(out[1].name, "MILLIPED.BIN"), "pinned MILLIPED second");
  check(str_eq(out[2].name, "NEWGAME.BIN"), "scanned BIN third");
  check(str_eq(out[3].name, "compat_game"), "scanned ELF fourth");
  check(str_eq(out[4].name, "another_elf"), "second scanned ELF fifth");
  check(out[3].kind == GAMES_ELF, "compat_game keeps ELF kind");
  check(out[4].kind == GAMES_ELF, "another_elf keeps ELF kind");

  /* cap at max=2 (only the two pinned fit) */
  struct games_entry out2[2];
  int n2 = games_order(out2, pinned, 2, scanned, 4, 2);
  check(n2 == 2, "cap: only 2 entries when max=2");
  check(str_eq(out2[0].name, "PONG.BIN") && str_eq(out2[1].name, "MILLIPED.BIN"),
        "cap keeps pinned first");

  /* no pinned -> scanned only */
  struct games_entry out3[4];
  int n3 = games_order(out3, NULL, 0, scanned, 4, 3);
  check(n3 == 3, "no pinned -> 3 launchable scanned (cap 3)");

  printf("=== games_kind_str ===\n");
  char buf[8];
  games_kind_str(GAMES_ELF, buf, sizeof(buf));
  check(str_eq(buf, "ELF"), "kind label ELF");
  games_kind_str(GAMES_BIN, buf, sizeof(buf));
  check(str_eq(buf, "BIN"), "kind label BIN");
  games_kind_str(99, buf, sizeof(buf));
  check(str_eq(buf, "---"), "out-of-range kind -> '---'");

  printf("\nChecks: %d, failed: %d\n", g_checks, g_fails);
  if (g_fails) { printf("GAMES SCAN TEST FAILED\n"); return 1; }
  printf("GAMES SCAN TEST PASSED\n");
  return 0;
}
