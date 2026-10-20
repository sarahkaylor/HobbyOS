/*
 * STARDUST Host Test (T1) — validates the original arcade game's logic on
 * the host machine.
 *
 * Compiles src/user/stardust.c with HOST_TEST (which gives it a plain
 * `int main` entry) and drives the pure game-logic functions directly:
 *   - reset_game / move_player / boundary clamping
 *   - meteor spawn (lane bounds, table fill)
 *   - meteor fall (monotonic descent)
 *   - AABB collision (bbox_overlap) and life loss on hit
 *   - scoring when a meteor safely crosses the bottom
 *   - lives / game-over transition
 *   - difficulty ramp (fall speed + spawn cadence increase with score)
 *   - render() completes without crashing (mock framebuffer from compat.c)
 *
 * This is the DoD T1 tier for GB-038: pure userland logic, seconds to run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"

/* Redefine entry point for host test. */
#define main stardust_main
#include "../user/stardust.c"
#undef main

/* Test framework */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  Test: %s ... ", #name); \
    test_##name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* --- Individual tests --- */

void test_graphics_init(void) {
  int ret = graphics_init();
  ASSERT(ret == 0, "graphics_init should return 0");
}

void test_reset_game(void) {
  reset_game();
  ASSERT(score == 0, "score should be 0 after reset");
  ASSERT(lives == START_LIVES, "lives should be START_LIVES after reset");
  ASSERT(game_over == 0, "game_over should be 0 after reset");
  ASSERT(fall_speed == BASE_FALL_SPEED, "fall_speed starts at BASE_FALL_SPEED");
  ASSERT(player_x == (SCREEN_WIDTH - PLAYER_W) / 2, "ship centered on reset");
  ASSERT(player_x >= 0 && player_x + PLAYER_W <= SCREEN_WIDTH, "ship in horizontal bounds");
  int active = 0;
  for (int i = 0; i < MAX_HAZARDS; i++) if (hazards[i].active) active++;
  ASSERT(active == 0, "no meteors active after reset");
}

void test_ship_movement_and_clamping(void) {
  reset_game();
  int start = player_x;
  move_player(+1);
  ASSERT(player_x == start + 7, "ship moves right by 7");
  /* Drive far off the right edge; must clamp. */
  for (int i = 0; i < SCREEN_WIDTH; i++) move_player(+1);
  ASSERT(player_x + PLAYER_W <= SCREEN_WIDTH, "ship clamps to right edge");
  ASSERT(player_x == SCREEN_WIDTH - PLAYER_W, "ship exactly at right edge");

  move_player(-1);
  ASSERT(player_x == SCREEN_WIDTH - PLAYER_W - 7, "ship moves left by 7");
  for (int i = 0; i < SCREEN_WIDTH; i++) move_player(-1);
  ASSERT(player_x == 0, "ship clamps to left edge");
}

void test_spawn_hazard_lane_bounds(void) {
  reset_game();
  for (int i = 0; i < 500; i++) {
    ASSERT(spawn_hazard() == 1, "spawn_hazard succeeds on empty table");
    /* Find the just-spawned active meteor and check bounds. */
    int found = 0;
    for (int j = 0; j < MAX_HAZARDS; j++) {
      if (hazards[j].active) {
        ASSERT(hazards[j].x >= 0, "meteor x >= 0");
        ASSERT(hazards[j].x + hazards[j].w <= SCREEN_WIDTH, "meteor within width");
        ASSERT(hazards[j].y < 0, "meteor spawns off top of screen");
        ASSERT(hazards[j].w >= 20 && hazards[j].w <= 45, "meteor width in range");
        ASSERT(hazards[j].h >= 14 && hazards[j].h <= 29, "meteor height in range");
        hazards[j].active = 0;  /* clear to avoid filling the table */
        found = 1;
        break;
      }
    }
    ASSERT(found == 1, "spawned meteor exists");
  }
}

void test_spawn_table_full(void) {
  reset_game();
  for (int i = 0; i < MAX_HAZARDS; i++) spawn_hazard();
  int active = 0;
  for (int i = 0; i < MAX_HAZARDS; i++) if (hazards[i].active) active++;
  ASSERT(active == MAX_HAZARDS, "table can fill to MAX_HAZARDS");
  ASSERT(spawn_hazard() == 0, "spawn_hazard fails when table is full");
}

void test_meteor_fall_and_score(void) {
  reset_game();
  /* Seed a single active meteor above the screen, far from the ship's row. */
  int slot = -1;
  for (int i = 0; i < MAX_HAZARDS; i++) if (!hazards[i].active) { slot = i; break; }
  hazards[slot].active = 1;
  hazards[slot].scored = 0;
  hazards[slot].x = 8;
  hazards[slot].w = 20;
  hazards[slot].h = 14;
  hazards[slot].y = -30;     /* start above frame */
  int last = hazards[slot].y;
  /* Run enough steps to carry it off the bottom, no ship collision. */
  int s0 = score;
  for (int k = 0; k < 400 && hazards[slot].active; k++) {
    /* Neutralize spawns during the run so our meteor is the only one. */
    spawn_timer = 999;
    update();
    if (hazards[slot].active) {
      ASSERT(hazards[slot].y > last, "meteor falls downward monotonically");
      last = hazards[slot].y;
    }
  }
  ASSERT(!hazards[slot].active, "meteor deactivates after passing bottom");
  ASSERT(score == s0 + 1, "score increments once when meteor passes cleanly");
}

void test_meteor_collision_loses_life(void) {
  reset_game();
  int slot = -1;
  for (int i = 0; i < MAX_HAZARDS; i++) if (!hazards[i].active) { slot = i; break; }
  /* Place a meteor overlapping the ship's row. */
  hazards[slot].active = 1;
  hazards[slot].scored = 0;
  hazards[slot].x = player_x + 5;
  hazards[slot].w = PLAYER_W;
  hazards[slot].h = PLAYER_H;
  hazards[slot].y = PLAYER_BASE_Y;   /* exactly on the ship */
  ASSERT(bbox_overlap(hazards[slot].x, hazards[slot].y, hazards[slot].w, hazards[slot].h,
                      player_x, PLAYER_BASE_Y, PLAYER_W, PLAYER_H) == 1,
         "overlapping meteor and ship collide");
  int lives0 = lives;
  spawn_timer = 999;
  update();
  ASSERT(lives == lives0 - 1, "collision costs one life");
  ASSERT(!hazards[slot].active, "collided meteor is cleared");
}

void test_bbox_overlap(void) {
  ASSERT(bbox_overlap(0, 0, 10, 10, 5, 5, 10, 10) == 1, "overlapping boxes collide");
  ASSERT(bbox_overlap(0, 0, 10, 10, 20, 20, 10, 10) == 0, "disjoint boxes do not collide");
  ASSERT(bbox_overlap(0, 0, 10, 10, 10, 0, 10, 10) == 0, "touching edge is not a collision");
  ASSERT(bbox_overlap(0, 0, 10, 10, 0, 10, 10, 10) == 0, "touching corner-edge is not a collision");
}

void test_game_over_on_zero_lives(void) {
  reset_game();
  /* Force 2 colliding meteors to drain lives to 0. */
  int slot = -1;
  for (int i = 0; i < MAX_HAZARDS; i++) if (!hazards[i].active) { slot = i; break; }
  hazards[slot].active = 1; hazards[slot].scored = 0;
  hazards[slot].x = player_x + 2; hazards[slot].w = PLAYER_W;
  hazards[slot].h = PLAYER_H; hazards[slot].y = PLAYER_BASE_Y;

  spawn_timer = 999;
  update();   /* lives 3 -> 2 (game continues) */
  ASSERT(!game_over, "game continues with lives remaining");
  ASSERT(lives == 2, "two lives remain after one hit");

  for (int i = 0; i < MAX_HAZARDS; i++) if (!hazards[i].active) { slot = i; break; }
  hazards[slot].active = 1; hazards[slot].scored = 0;
  hazards[slot].x = player_x + 2; hazards[slot].w = PLAYER_W;
  hazards[slot].h = PLAYER_H; hazards[slot].y = PLAYER_BASE_Y;
  update();   /* lives 2 -> 1 */
  for (int i = 0; i < MAX_HAZARDS; i++) if (!hazards[i].active) { slot = i; break; }
  hazards[slot].active = 1; hazards[slot].scored = 0;
  hazards[slot].x = player_x + 2; hazards[slot].w = PLAYER_W;
  hazards[slot].h = PLAYER_H; hazards[slot].y = PLAYER_BASE_Y;
  int g = update();   /* lives 1 -> 0 -> game over */
  ASSERT(lives == 0, "lives floor at 0");
  ASSERT(game_over == 1, "game_over set when lives reach 0");
  ASSERT(g == 1, "update returns 1 on game-over transition");
}

void test_difficulty_ramp(void) {
  reset_game();
  int base = fall_speed;
  ASSERT(base == BASE_FALL_SPEED, "begins at base fall speed");
  score = 60;      /* 60/6 = +10 over base -> capped at MAX_FALL_SPEED */
  ramp_difficulty();
  ASSERT(fall_speed > base, "fall speed increases with score");
  ASSERT(fall_speed <= MAX_FALL_SPEED, "fall speed is capped at MAX_FALL_SPEED");
  score = 10000;
  ramp_difficulty();
  ASSERT(fall_speed == MAX_FALL_SPEED, "fall speed saturates at MAX_FALL_SPEED");
  ASSERT(spawn_timer >= MIN_SPAWN_TIME, "spawn timer never drops below MIN_SPAWN_TIME");
}

void test_render_no_crash(void) {
  reset_game();
  render();
  /* With an active meteor present. */
  spawn_hazard();
  render();
  /* Game-over state renders. */
  game_over = 1; lives = 0;
  render();
  ASSERT(1, "render completes without crashing in all states");
}

void test_rng(void) {
  unsigned int v1 = rng();
  unsigned int v2 = rng();
  ASSERT(v1 != v2, "RNG produces different consecutive values");
  ASSERT(v1 < 0x80000000 && v2 < 0x80000000, "RNG values in signed-positive range");
}

/* --- Main test runner --- */

int main(void) {
  printf("=== STARDUST Host Test ===\n");
  printf("Testing the original arcade game logic on host machine...\n\n");

  graphics_init();

  TEST(graphics_init);
  TEST(reset_game);
  TEST(ship_movement_and_clamping);
  TEST(spawn_hazard_lane_bounds);
  TEST(spawn_table_full);
  TEST(meteor_fall_and_score);
  TEST(meteor_collision_loses_life);
  TEST(bbox_overlap);
  TEST(game_over_on_zero_lives);
  TEST(difficulty_ramp);
  TEST(render_no_crash);
  TEST(rng);

  printf("\n=== Test Results ===\n");
  printf("Tests run:    %d\n", tests_run);
  printf("Tests passed: %d\n", tests_passed);
  printf("Tests failed: %d\n", tests_failed);

  if (tests_failed == 0) {
    printf("\n✓ All tests PASSED!\n");
    return 0;
  } else {
    printf("\n✗ %d test(s) FAILED!\n", tests_failed);
    return 1;
  }
}
