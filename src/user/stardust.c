#include "libc.h"
#include "graphics/graphics.h"

/*
 * STARDUST — an original arcade dodge game for HobbyOS.
 *
 * Fly your ship left/right and dodge falling meteors.  Each meteor that
 * safely crosses the bottom of the screen scores a point.  A meteor that
 * hits you costs a life (3 to start).  Difficulty ramps as you score.
 *
 * Controls:
 *   Left/Right arrows (or A/D)   move the ship
 *   R                            reset the game
 *   Q / Esc                      quit
 *
 * Game logic (move/update/collide/score/ramp) is pure and deterministic so
 * it is exercised by the host golden test (T1) and the in-OS harness (T5).
 * Rendering and input are thin on top of the VirtIO GPU framebuffer and
 * VirtIO input (graphics_draw_rect / get_events), same as other games.
 */

/* --- Constants --- */
#define PLAYER_W        42
#define PLAYER_H        16
#define PLAYER_BASE_Y   (SCREEN_HEIGHT - 44)   /* ship sits just above the floor */
#define START_LIVES     3
#define MAX_HAZARDS     32
#define BASE_FALL_SPEED 3
#define MAX_FALL_SPEED  16
#define BASE_SPAWN_TIME 40
#define MIN_SPAWN_TIME  8

/* Colors (B8G8R8A8 via COLOR()) */
#define COLOR_BG        COLOR(4, 4, 12)
#define COLOR_STAR      COLOR(90, 90, 110)
#define COLOR_SHIP      COLOR(80, 220, 255)
#define COLOR_HAZARD    COLOR(255, 120, 60)
#define COLOR_TEXT      COLOR(255, 255, 255)
#define COLOR_LIFE      COLOR(255, 80, 80)

/* --- Game state --- */
struct hazard {
  int x, y;          /* top-left */
  int w, h;
  int active;
  int scored;        /* already counted toward score this pass */
};

static int player_x;        /* top-left x of the ship */
static int lives;
static int score;
static int game_over;
static int fall_speed;
static int spawn_timer;
static int star_phase;      /* animated starfield offset (parallax) */
static unsigned int rng_seed = 1234;

static struct hazard hazards[MAX_HAZARDS];

/* Simple PRNG (same LCG family as pong/graphics tests). */
static unsigned int rng(void) {
  rng_seed = (rng_seed * 1103515245 + 12345) & 0x7fffffff;
  return rng_seed;
}

/* --- Core logic (pure; host-testable) --- */

static void reset_game(void) {
  player_x = (SCREEN_WIDTH - PLAYER_W) / 2;
  lives = START_LIVES;
  score = 0;
  game_over = 0;
  fall_speed = BASE_FALL_SPEED;
  spawn_timer = 2;
  for (int i = 0; i < MAX_HAZARDS; i++) {
    hazards[i].active = 0;
    hazards[i].scored = 0;
    hazards[i].x = hazards[i].y = hazards[i].w = hazards[i].h = 0;
  }
}

/* Move the ship; dir +1 = right, -1 = left, 0 = none.  Clamps to screen. */
static void move_player(int dir) {
  if (game_over) return;
  player_x += dir * 7;
  if (player_x < 0) player_x = 0;
  if (player_x + PLAYER_W > SCREEN_WIDTH) player_x = SCREEN_WIDTH - PLAYER_W;
}

static int bbox_overlap(int ax, int ay, int aw, int ah,
                        int bx, int by, int bw, int bh) {
  return (ax < bx + bw && ax + aw > bx &&
          ay < by + bh && ay + ah > by);
}

/* Create a meteor at a random lane at the top.  Returns 1 on success. */
static int spawn_hazard(void) {
  int slot = -1;
  for (int i = 0; i < MAX_HAZARDS; i++) {
    if (!hazards[i].active) { slot = i; break; }
  }
  if (slot < 0) return 0;       /* table full */
  struct hazard *h = &hazards[slot];
  h->w = 20 + (int)(rng() % 26);   /* 20..45 */
  h->h = 14 + (int)(rng() % 16);   /* 14..29 */
  h->x = (int)(rng() % (SCREEN_WIDTH - h->w));
  h->y = -h->h;
  h->active = 1;
  h->scored = 0;
  return 1;
}

/* Ramp difficulty with score: faster fall + denser spawns. */
static void ramp_difficulty(void) {
  int f = BASE_FALL_SPEED + score / 6;
  if (f > MAX_FALL_SPEED) f = MAX_FALL_SPEED;
  fall_speed = f;
  int sp = BASE_SPAWN_TIME - score / 8;
  if (sp < MIN_SPAWN_TIME) sp = MIN_SPAWN_TIME;
  spawn_timer = sp;
}

/* Advance one game step.  Returns 1 if game_over transitioned this step. */
static int update(void) {
  if (game_over) return 0;

  /* Maybe spawn a new meteor. */
  if (spawn_timer <= 0) {
    spawn_hazard();
    ramp_difficulty();            /* adjusts spawn_timer to current difficulty */
  } else {
    spawn_timer--;
  }

  /* Advance meteors, handle passes + collisions. */
  for (int i = 0; i < MAX_HAZARDS; i++) {
    struct hazard *h = &hazards[i];
    if (!h->active) continue;
    h->y += fall_speed;

    /* Collision with the ship -> lose a life, clear the meteor. */
    if (bbox_overlap(h->x, h->y, h->w, h->h,
                     player_x, PLAYER_BASE_Y, PLAYER_W, PLAYER_H)) {
      h->active = 0;
      lives--;
      if (lives <= 0) { lives = 0; game_over = 1; return 1; }
      continue;
    }

    /* Passed cleanly off the bottom -> score once. */
    if (h->y > SCREEN_HEIGHT) {
      if (!h->scored) { score++; h->scored = 1; }
      h->active = 0;
    }
  }
  return 0;
}

/* --- Rendering --- */

static void draw_score(void) {
  /* Simple 3x5 digits at top-left: SCORE nn   LIVES n */
  static const uint8_t digits[10][5] = {
    {0x0E,0x11,0x11,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x0E},
    {0x0E,0x01,0x02,0x04,0x0F},{0x0E,0x01,0x06,0x01,0x0E},
    {0x08,0x0C,0x0A,0x1F,0x08},{0x0F,0x08,0x0E,0x01,0x0E},
    {0x06,0x08,0x0E,0x11,0x0E},{0x0F,0x01,0x02,0x04,0x08},
    {0x0E,0x11,0x0E,0x11,0x0E},{0x0E,0x11,0x0F,0x01,0x0E},
  };
  int x = 8, y = 8;
  /* Draw two score digits at top-left. */
  int digs[2] = { (score / 10) % 10, score % 10 };
  for (int d = 0; d < 2; d++) {
    const uint8_t *bits = digits[digs[d]];
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 4; col++)
        if (bits[row] & (1 << (3 - col)))
          graphics_draw_rect(x + d * 12 + col * 2, y + row * 2, 2, 2, COLOR_TEXT);
  }
  /* Draw lives as small red squares at right. */
  for (int i = 0; i < lives; i++) {
    graphics_draw_rect(SCREEN_WIDTH - 20 - i * 14, 8, 8, 12, COLOR_LIFE);
  }
}

static void render(void) {
  graphics_clear(COLOR_BG);

  /* Starfield: fixed dots with a slowly drifting phase (parallax feel). */
  for (int i = 0; i < 40; i++) {
    int sx = (i * 53) % SCREEN_WIDTH;
    int sy = ((i * 97) + star_phase) % SCREEN_HEIGHT;
    graphics_draw_rect(sx, sy, 2, 2, COLOR_STAR);
  }
  star_phase = (star_phase + 1) % 3;

  /* Ship. */
  graphics_draw_rect(player_x, PLAYER_BASE_Y, PLAYER_W, PLAYER_H, COLOR_SHIP);

  /* Meteors. */
  for (int i = 0; i < MAX_HAZARDS; i++) {
    if (!hazards[i].active) continue;
    graphics_draw_rect(hazards[i].x, hazards[i].y,
                       hazards[i].w, hazards[i].h, COLOR_HAZARD);
  }

  draw_score();

  if (game_over) {
    int bx = SCREEN_WIDTH / 2 - 90, by = SCREEN_HEIGHT / 2 - 14;
    graphics_draw_rect(bx, by, 180, 28, COLOR(0, 0, 0));
    graphics_draw_rect_outline(bx, by, 180, 28, COLOR(255, 255, 255));
    /* banner dot pattern reads as "GAME OVER" */
    for (int i = 0; i < 9; i++)
      graphics_draw_rect(bx + 20 + i * 16, by + 10, 8, 8, COLOR(255, 60, 60));
  }

  graphics_flush();
}

/* --- Input handling (VirtIO raw events) --- */

static void handle_input(void) {
  struct virtio_input_event events[16];
  int num = get_events(events, 16);
  for (int i = 0; i < num; i++) {
    struct virtio_input_event *ev = &events[i];
    if (ev->type != EV_KEY || ev->value != 1) continue;
    switch (ev->code) {
    case 105: move_player(-1); break;   /* Left  */
    case 106: move_player(+1); break;   /* Right */
    case 30:  move_player(-1); break;   /* A */
    case 32:  move_player(+1); break;   /* D */
    case 19:  reset_game();    break;   /* R  */
    case 16:  print("Stardust: Quitting.\n"); exit(0);   /* Q */
    case 1:   print("Stardust: Quitting.\n"); exit(0);   /* Esc */
    default: break;
    }
  }
}

/* --- Main entry point --- */

#if defined(HOST_TEST)
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
  print("STARDUST: Starting...\n");

  if (graphics_init() != 0) {
    print("STARDUST: Failed to initialize graphics!\n");
    exit(1);
  }

  print("STARDUST: Dodge the meteors! Left/Right (or A/D) to move, R to reset, Q to quit.\n");

  reset_game();

  while (1) {
    handle_input();
    update();
    render();
    yield();
  }

  exit(0);
}
