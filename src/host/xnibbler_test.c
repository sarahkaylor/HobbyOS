/*
 * xnibbler_test.c - host golden tests for XNIBBLER.BIN, the original
 * X11-era nibbler/Snake game on the HobbyOS X11 support library
 * (src/user/x11/apps/xnibbler/main.c).
 *
 * The game's rules live in static, input/render-free functions
 * (reset_game / step_snake / set_direction / cell_occupied / spawn_food),
 * so this test includes the game source and drives the board logic
 * directly — no X, no display.  It asserts the classic nibbler invariants:
 * reset geometry, forward motion, a buffered turn, food growth + score,
 * wall death, self-collision death, and the anti-reverse rule.
 */
#define main xnibbler_main
#include "../user/x11/apps/xnibbler/main.c"
#undef main

#include <stdio.h>
#include <string.h>

static int checks_pass = 0;
static int checks_fail = 0;

static void check(int cond, const char *label) {
  if (cond) {
    checks_pass++;
  } else {
    checks_fail++;
    printf("FAIL: %s\n", label);
  }
}

int main(void) {
  /* --- reset geometry: snake centred, heading right ------------------- */
  reset_game();
  check(len == START_LEN, "reset length == START_LEN");
  check(score == 0 && game_over == 0, "reset has zero score, not game over");
  /* The head is at body_x[head] (centre cell), to the right of the tail. */
  check(body_x[head] > body_x[tail], "head is to the right of tail on reset");
  check(body_y[head] == body_y[tail], "snake lies on one row on reset");
  check(food_x >= 0 && food_x < COLS && food_y >= 0 && food_y < ROWS,
        "food spawns inside the board");
  check(!cell_occupied(food_x, food_y), "food never spawns on the snake");

  /* --- forward motion: a steering key starts it, then it advances ------- */
  reset_game();
  check(!started, "snake idles until the first steering key");
  set_direction(1, 0);                       /* start heading right */
  check(started, "first steering key starts the snake");
  int hx0 = body_x[head], hy0 = body_y[head];
  check(step_snake() == 1, "step_snake returns 1 while alive");
  check(body_x[head] == hx0 + 1 && body_y[head] == hy0,
        "snake advances one cell right after the start key");
  check(game_over == 0, "not game over after a plain step");

  /* --- buffered turn: set_direction applies at the next step ---------- */
  set_direction(0, -1);            /* turn up */
  check(step_snake() == 1, "step with a turn still lives");
  check(body_y[head] == hy0 - 1 && body_x[head] == hx0 + 1,
        "buffered turn up applied at the next step");

  /* --- anti-reverse: cannot turn straight back into the body ---------- */
  set_direction(1, 0);             /* keep going right (matches current) */
  step_snake();
  set_direction(-1, 0);            /* try to reverse 180 degrees */
  int hx = body_x[head];
  step_snake();
  check(body_x[head] == hx + 1, "reverse turn is ignored (keeps moving right)");
  check(game_over == 0, "no crash from the ignored reverse turn");

  /* --- food: eating grows the snake and scores ------------------------ */
  reset_game();
  /* Place food two cells directly ahead of the head and step forward until
   * the head reaches it (one plain step, then the eating step). */
  int fx = body_x[head] + 2, fy = body_y[head];
  if (fx < COLS && !cell_occupied(fx, fy)) {
    food_x = fx; food_y = fy;
    int before = len;
    do { set_direction(1, 0); step_snake(); }
    while (game_over == 0 && (body_x[head] != fx || body_y[head] != fy));
    check(body_x[head] == fx && body_y[head] == fy,
          "snake head reaches the food");
    check(len == before + 1, "eating food grows the snake by one");
    check(score == 10, "eating food scores 10");
  }

  /* --- wall collision ------------------------------------------------- */
  reset_game();
  /* March right (filling food with each eat is fine) until the head hits
   * the right wall and the game ends. */
  int guard = 0;
  while (!game_over && guard++ < 10000) {
    set_direction(1, 0);
    step_snake();
  }
  check(game_over == 1, "hitting the right wall ends the game");

  /* --- self collision ------------------------------------------------- */
  reset_game();
  /* Grow to length ~8 by forcing food directly ahead (5 eats): cells
   * x10..x17 on the centre row.  Then a compact right-angle + same-row
   * return puts the head on a body cell the trailing tail has not yet
   * receded past. */
  for (int i = 0; i < 5; i++) {
    food_x = body_x[head] + 1;
    food_y = body_y[head];
    set_direction(1, 0);
    step_snake();
  }
  check(len == 8, "snake grew to length 8 for the self-collision test");
  check(game_over == 0, "still alive after growing");
  /* Head now at x=17 (cy).  Up one, left one, down one lands at (16,cy),
   * which the (still long) body occupies. */
  set_direction(0, -1); step_snake();                    /* (17,cy-1) */
  set_direction(-1, 0); step_snake();                    /* (16,cy-1) */
  set_direction(0, 1);  step_snake();                    /* (16,cy) -> body! */
  check(game_over == 1, "turning back into the body ends the game");

  printf("Tests run: %d, passed %d, failed %d\n",
         checks_pass + checks_fail, checks_pass, checks_fail);
  if (checks_fail) {
    printf("XNIBBLER host test FAILED.\n");
    return 1;
  }
  printf("XNIBBLER host test PASSED.\n");
  return 0;
}
