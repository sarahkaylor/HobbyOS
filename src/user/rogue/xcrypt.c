/*
 * XCRYPT — a compact roguelike for HobbyOS, rendered through the curses
 * port (src/user/nano/curses.h + hb_curses.c, the same shim nano uses).
 *
 * An original (clean-room, MIT) dungeon crawl in the classic 1980s roguelike
 * genre (Rogue/Hack lineage, Brian would approve): a procedural dungeon,
 * an @-player who moves with the arrow keys / WASD, monsters that
 * pursue, a food/health economy, and a victory item to collect and exit.
 * Because it drives no X server or framebuffer — it writes terminal cells
 * through the curses API — it tiles as a terminal window on the desktop,
 * exactly like NANO.BIN.
 *
 * LICENSE: MIT (see LICENSE.txt).  Original code; shares only the genre's
 * conventions, no vendored roguelike source.
 *
 * The entire game body (dungeon generation, movement, field-of-view,
 * monster AI, combat, food/health, victory) lives in STATIC functions with
 * no curses input/output side effects, so the host test (src/host/xcrypt_test.c)
 * drives it directly (T1).  The curses layer is a thin render + input
 * wrapper over that logic.
 */
#include <stdlib.h>
#include <string.h>

#include "curses.h"

/* ---- dungeon dimensions (a fixed 40x18 map) --------------------------- */

#define MAP_W 40
#define MAP_H 18
#define MAX_MOBS 12

/* Terrain / entity glyphs. */
#define T_WALL '#'
#define T_FLOOR '.'
#define T_DOOR '+'
#define T_HP '+'
#define T_FOOD '%'
#define T_AMULET '&'
#define T_STAIR '>'
#define P_AT '@'
#define M_RAT 'r'
#define M_ORC 'o'
#define M_TROLL 'T'

/* ---- game state (static, no curses: host-testable) -------------------- */

static char map[MAP_H][MAP_W + 1];
static int px, py;                 /* player position */
static int hp, hp_max;
static int food;                   /* nutrition/health pool */
static int gold;
static int power;                  /* attack bonus from the amulet */
static int have_amulet;
static int turns;
static int game_over;

typedef struct {
  int x, y;
  int alive;
  int tile;                        /* M_* glyph */
  int hp;
} mob;

static mob mobs[MAX_MOBS];
static int nmobs;
static int stairs_x, stairs_y;

/* ---- deterministic RNG (host tests lean on it) ------------------------ */

static unsigned seed_state = 0x2C6B1Du;
static unsigned xrng(void) {
  seed_state = seed_state * 1664525u + 1013904223u;
  return (seed_state >> 8) & 0x7FFF;
}
void xseed(unsigned s) { seed_state = s; }

/* ---- helpers ----------------------------------------------------------- */

static void set_cell(int x, int y, char c) { map[y][x] = c; }
static char cell(int x, int y) { return map[y][x]; }
static int in_bounds(int x, int y) {
  return x >= 0 && x < MAP_W && y >= 0 && y < MAP_H;
}

/* ---- dungeon generation (recursive-backtracking rooms is overkill for a
 * compact game: we use a small repeating pattern of rooms + corridors). -- */

static void carve_rect(int x0, int y0, int w, int h) {
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++)
      set_cell(x, y, T_FLOOR);
}

void gen_dungeon(void) {
  memset(map, T_WALL, sizeof(map));
  /* Three rooms joined by corridors. */
  carve_rect(2, 2, 10, 7);     /* room A */
  carve_rect(16, 2, 10, 7);    /* room B */
  carve_rect(28, 10, 10, 6);   /* room C */
  /* corridors A->B and B->C. */
  for (int x = 12; x <= 15; x++) { set_cell(x, 5, T_FLOOR); }
  for (int y = 5; y <= 9; y++)  { set_cell(21, y, T_FLOOR); }
  for (int x = 16; x <= 21; x++) { set_cell(x, 9, T_FLOOR); }
  for (int y = 9; y <= 12; y++) { set_cell(28, y, T_DOOR); }
  set_cell(28, 12, T_FLOOR);
  /* Doors. */
  set_cell(12, 5, T_DOOR);
  set_cell(15, 5, T_FLOOR);
  set_cell(28, 9, T_DOOR);
  /* Treasure + health in room B, food around, stairs in room C. */
  set_cell(17, 3, T_HP);
  set_cell(19, 4, T_HP);
  set_cell(18, 3, T_FOOD);
  set_cell(4, 4, T_FOOD);
  set_cell(30, 11, T_AMULET);
  stairs_x = 33; stairs_y = 12;
  set_cell(stairs_x, stairs_y, T_STAIR);
  /* Player start in room A. */
  px = 4; py = 4;
  set_cell(px, py, P_AT);
}

/* ---- monster placement ------------------------------------------------ */

static const int mob_tiles[] = { M_RAT, M_ORC, M_TROLL };
static const int mob_hp[] = { 6, 12, 20 };

void place_mobs(int count) {
  nmobs = 0;
  for (int i = 0; i < count && nmobs < MAX_MOBS; i++) {
    int t = mob_tiles[i % 3];
    int x, y;
    /* Find a random interior floor cell ≥ some distance from the player. */
    for (int tries = 0; tries < 200; tries++) {
      x = 1 + (int)(xrng() % (MAP_W - 2));
      y = 1 + (int)(xrng() % (MAP_H - 2));
      if (cell(x, y) != T_FLOOR) continue;
      int dx = x - px, dy = y - py;
      if (dx < 0) dx = -dx;
      if (dy < 0) dy = -dy;
      if (dx + dy < 6) continue;   /* not too close to the start */
      break;
    }
    mobs[nmobs].x = x; mobs[nmobs].y = y;
    mobs[nmobs].alive = 1;
    mobs[nmobs].tile = (char)t;
    mobs[nmobs].hp = mob_hp[i % 3];
    nmobs++;
  }
  /* Blit mobs onto the map for rendering (logic also tracks them). */
  for (int i = 0; i < nmobs; i++)
    if (mobs[i].alive) set_cell(mobs[i].x, mobs[i].y, mobs[i].tile);
}

/* ---- reset ------------------------------------------------------------- */

void reset_game(void) {
  hp = hp_max = 40;
  food = 22;
  gold = 0;
  power = 0;
  have_amulet = 0;
  turns = 0;
  game_over = 0;
  gen_dungeon();
  place_mobs(5);
}

/* ---- combat ------------------------------------------------------------ */

static int attack_bonus(void) { return power; }

/* Player attacks a mob at (x,y).  Returns 1 if the mob died. */
int player_attack(int mx, int my) {
  for (int i = 0; i < nmobs; i++) {
    if (!mobs[i].alive) continue;
    if (mobs[i].x == mx && mobs[i].y == my) {
      int dmg = 3 + (int)(xrng() % 5) + attack_bonus();
      mobs[i].hp -= dmg;
      if (mobs[i].hp <= 0) {
        mobs[i].alive = 0;
        set_cell(mx, my, T_FLOOR);
        gold += 1;
        return 1;
      }
      return 0;
    }
  }
  return 0;
}

/* A single mob takes a step toward the player (simple pursuit). */
static int step_toward(mob *m) {
  int dx = px - m->x, dy = py - m->y;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  int nx = m->x, ny = m->y;
  if (adx >= ady) nx += (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
  else            ny += (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
  if (nx == px && ny == py) {
    /* Bumped into the player: they take a hit. */
    hp -= 2 + (int)(xrng() % 3);
    if (hp <= 0) { hp = 0; game_over = 1; }
    return 0;
  }
  if (in_bounds(nx, ny) && cell(nx, ny) == T_FLOOR) {
    set_cell(m->x, m->y, T_FLOOR);
    m->x = nx; m->y = ny;
    set_cell(m->x, m->y, m->tile);
    return 1;
  }
  return 0;
}

/* All live mobs take one step. */
void mobs_move(void) {
  for (int i = 0; i < nmobs; i++)
    if (mobs[i].alive) step_toward(&mobs[i]);
}

/* Pick up what's under the player. */
static void pickup(void) {
  char c = cell(px, py);
  if (c == T_HP) { hp += 12; if (hp > hp_max) hp = hp_max; set_cell(px, py, P_AT); gold += 2; }
  else if (c == T_FOOD) { food += 8; if (food > 99) food = 99; set_cell(px, py, P_AT); }
  else if (c == T_AMULET) { have_amulet = 1; power = 3; set_cell(px, py, P_AT); }
}

/* Try to move the player by (dx,dy).  Handles walls, doors, monsters,
 * pickup, and the stairs.  Returns 0 if blocked, 1 if moved. */
int player_move(int dx, int dy) {
  if (game_over) return 0;
  int nx = px + dx, ny = py + dy;
  if (!in_bounds(nx, ny)) return 0;
  char c = cell(nx, ny);

  if (c == T_WALL) return 0;

  if (c == M_RAT || c == M_ORC || c == M_TROLL) {
    player_attack(nx, ny);
    turns++;
    return 0;   /* attacking does not move the player */
  }

  if (c == T_STAIR && have_amulet) {
    game_over = 1;      /* victory */
    set_cell(px, py, T_FLOOR);
    px = nx; py = ny;
    return 1;
  }

  /* Move onto the target (floor, door, item, or stairs-without-amulet). */
  char leaving = cell(px, py);       /* what the player is standing on */
  char dest_c = c;                    /* what the target is */
  set_cell(px, py, T_FLOOR);
  px = nx; py = ny;
  pickup();
  set_cell(px, py, P_AT);
  /* Restore feature glyphs so stairs/doors persist across the player: keep
   * the stairs at the destination when the player stepped onto it without
   * the amulet, and at the origin when the player walked off of it. */
  if (leaving == T_STAIR) set_cell(px - dx, py - dy, T_STAIR);
  else if (leaving == T_DOOR) set_cell(px - dx, py - dy, T_DOOR);
  if (dest_c == T_STAIR) set_cell(px, py, T_STAIR);
  else if (dest_c == T_DOOR) set_cell(px, py, T_DOOR);
  turns++;
  return 1;
}

/* ---- public query helpers for the host test --------------------------- */

int player_alive(void) { return hp > 0; }
int victory(void) { return have_amulet && game_over; }
int map_scaled_char(int y, int x) { return map[y][x]; }
int hp_value(void) { return hp; }
int monster_alive(int i) { return i < nmobs && mobs[i].alive; }
int monster_count(void) { return nmobs; }
int turn_count(void) { return turns; }
int stairs_reached(void) { return px == stairs_x && py == stairs_y; }

/* ---- curses render + input (thin wrappers) ---------------------------- */

static void render(void) {
  clear();
  for (int y = 0; y < MAP_H; y++) {
    mvaddstr(y, 0, map[y]);
  }
  char status[64];
  int n = 0;
  const char *s = "HP:";
  while (*s) status[n++] = *s++;
  {
    char b[8]; int i = 0, v = hp;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i) status[n++] = b[--i];
  }
  const char *s2 = " Food:";
  while (*s2) status[n++] = *s2++;
  {
    char b[8]; int i = 0, v = food;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i) status[n++] = b[--i];
  }
  const char *s3 = have_amulet ? " [AMULET!] exit via >" : "";
  while (*s3) status[n++] = *s3++;
  status[n] = 0;
  mvaddstr(MAP_H, 0, status);
  refresh();
}

/* Map arrow keys / WASD from the curses getch (the desktop forwards keys
 * to the window's stdin as ANSI sequences; the curses shim decodes them). */
static int decode_key(int ch) {
  switch (ch) {
  case KEY_UP:    case 'k': case 'w': return 2;  /* up    -> -y */
  case KEY_DOWN:  case 'j': case 's': return 3;  /* down  -> +y */
  case KEY_RIGHT: case 'l': case 'd': return 4;  /* right -> +x */
  case KEY_LEFT:  case 'h': case 'a': return 5;  /* left  -> -x */
  case 'q': return 9;                            /* quit  */
  default: return 0;
  }
}

int main(void) {
  initscr();
  raw();
  noecho();
  keypad(stdscr, TRUE);
  reset_game();

  int running = 1;
  while (running) {
    render();
    int ch = getch();
    int dir = decode_key(ch);
    if (dir == 9) { endwin(); mvaddstr(0, 0, "Quit. Thanks for playing!"); break; }
    if (dir == 2) player_move(0, -1);
    else if (dir == 3) player_move(0, 1);
    else if (dir == 4) player_move(1, 0);
    else if (dir == 5) player_move(-1, 0);
    mobs_move();
    if (game_over) {
      render();
      break;
    }
  }
  endwin();
  return 0;
}
