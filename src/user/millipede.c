#include "libc.h"
#include "graphics/graphics.h"

/*
 * Millipede — a simplified Millipede arcade game for HobbyOS.
 *
 * Based on the 1982 Atari classic. A segmented millipede zigzags down
 * the screen. The player moves a ship at the bottom and shoots to
 * destroy segments. Hit segments become mushrooms that the millipede
 * must bounce around.
 *
 * Controls:
 *   Arrow keys — move player ship (Up/Down/Left/Right)
 *   Space       — shoot
 *   Q           — quit
 *   R           — restart after game over
 */

/* === Constants === */
#define MAX_SEGMENTS   30
#define SEGMENT_SIZE   10
#define PLAYER_W       22
#define PLAYER_H       14
#define BULLET_W        3
#define BULLET_H        8
#define BULLET_SPEED    5
#define PLAYER_SPEED    5
#define PLAYER_HSPEED   (PLAYER_SPEED * 4)  /* horizontal: 4x faster for snappy gameplay */
#define MUSHROOM_SIZE  10
#define MAX_BULLETS     5
#define MAX_MUSHROOMS  60
#define MAX_PARTICLES  20

/* Millipede speed */
#define MILLI_XSPEED    2
#define MILLI_VSTEP     14   /* pixels to drop on direction change */

/* Timing */
#define FIRE_RATE        8   /* frames between auto-fire shots */

/* Colors */
#define COLOR_BG         COLOR(0, 0, 0)
#define COLOR_PLAYER     COLOR(0, 255, 80)
#define COLOR_HEAD       COLOR(255, 180, 0)
#define COLOR_SEGMENT    COLOR(0, 200, 60)
#define COLOR_BULLET     COLOR(255, 255, 0)
#define COLOR_MUSHROOM   COLOR(140, 110, 80)
#define COLOR_TEXT       COLOR(255, 255, 255)
#define COLOR_LIFE       COLOR(0, 255, 80)
#define COLOR_GAMEOVER   COLOR(255, 50, 50)
#define COLOR_PARTICLE   COLOR(255, 200, 50)
#define COLOR_SCORE_BG   COLOR(20, 20, 20)

/* === Data structures === */
typedef struct {
    int x, y;
} Point;

typedef struct {
    int x, y, active;
    int life; /* particle fade counter */
    int dx, dy;
} Particle;

typedef struct {
    int x, y;
    int active;
} Mushroom;

typedef struct {
    int x, y;
    int active;
    int w, h;
} Bullet;

typedef struct {
    Point segs[MAX_SEGMENTS];
    int count;
    int dir_x;          /* 1 = right, -1 = left */
    int moved_down;     /* true after a vertical step */
    int active;
} Millipede;

/* === Game state === */
static int player_x, player_y;
static int score;
static int lives;
static int game_over;
static int level;
static int game_paused;   /* brief pause after death */
static Millipede millipede;
static Bullet bullets[MAX_BULLETS];
static Mushroom mushrooms[MAX_MUSHROOMS];
static int num_mushrooms;
static Particle particles[MAX_PARTICLES];
static int num_particles;
static int fire_cooldown;

static unsigned int rng_seed = 42;
static unsigned int rng(void) {
    rng_seed = (rng_seed * 1103515245 + 12345) & 0x7fffffff;
    return rng_seed;
}

/* === Drawing helpers === */

/* Draw the player ship as a simple triangle/arrow shape */
static void draw_player(int x, int y, uint32_t color) {
    /* Body — central rectangle */
    graphics_draw_rect(x + 4, y + 2, PLAYER_W - 8, PLAYER_H - 4, color);
    /* Nose — triangle pointing up */
    for (int i = 0; i < 4; i++) {
        graphics_draw_rect(x + 6 + i, y - i - 2 + 2, PLAYER_W - 12 - i * 2, 1, color);
    }
    /* Wings */
    graphics_draw_rect(x, y + 2, 4, 3, color);
    graphics_draw_rect(x + PLAYER_W - 4, y + 2, 4, 3, color);
    /* Tail */
    graphics_draw_rect(x + 6, y + PLAYER_H - 3, 2, 3, color);
    graphics_draw_rect(x + PLAYER_W - 8, y + PLAYER_H - 3, 2, 3, color);
}

/* Draw a millipede segment (rounded-ish rectangle) */
static void draw_segment(int x, int y, int is_head, uint32_t color) {
    graphics_draw_rect(x + 1, y, SEGMENT_SIZE - 2, SEGMENT_SIZE, color);
    graphics_draw_rect(x, y + 1, SEGMENT_SIZE, SEGMENT_SIZE - 2, color);
    if (is_head) {
        /* Eyes — two white dots */
        graphics_draw_rect(x + 2, y + 2, 2, 2, COLOR(255, 255, 255));
        graphics_draw_rect(x + SEGMENT_SIZE - 4, y + 2, 2, 2, COLOR(255, 255, 255));
    }
}

/* Draw a bullet */
static void draw_bullet(int x, int y, uint32_t color) {
    graphics_draw_rect(x, y, BULLET_W, BULLET_H, color);
}

/* Draw a mushroom */
static void draw_mushroom(int x, int y, uint32_t color) {
    /* Stem */
    graphics_draw_rect(x + 3, y + 5, 4, 5, COLOR(200, 180, 150));
    /* Cap */
    graphics_draw_rect(x, y, MUSHROOM_SIZE, 6, color);
    graphics_draw_rect(x + 1, y + 1, MUSHROOM_SIZE - 2, 1, COLOR(160, 130, 100));
    /* Spots */
    graphics_draw_rect(x + 2, y + 2, 2, 2, COLOR(255, 200, 150));
    graphics_draw_rect(x + 6, y + 2, 2, 2, COLOR(255, 200, 150));
}

/* Draw a particle */
static void draw_particle(Particle *p) {
    int a = (p->life * 255) / 10;
    uint32_t faded = COLOR((a * ((COLOR_PARTICLE >> 16) & 0xFF)) / 255,
                           (a * ((COLOR_PARTICLE >> 8) & 0xFF)) / 255,
                           (a * (COLOR_PARTICLE & 0xFF)) / 255);
    graphics_draw_rect(p->x, p->y, 3, 3, faded);
}

/* Score display using 5x7 block font */
static void draw_digit(int x, int y, int digit, uint32_t color) {
    static const uint8_t digits[10][7] = {
        {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F}, /* 0 */
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
        {0x1F, 0x01, 0x01, 0x1F, 0x10, 0x10, 0x1F}, /* 2 */
        {0x1F, 0x01, 0x01, 0x1F, 0x01, 0x01, 0x1F}, /* 3 */
        {0x11, 0x11, 0x11, 0x1F, 0x01, 0x01, 0x01}, /* 4 */
        {0x1F, 0x10, 0x10, 0x1F, 0x01, 0x01, 0x1F}, /* 5 */
        {0x1F, 0x10, 0x10, 0x1F, 0x11, 0x11, 0x1F}, /* 6 */
        {0x1F, 0x01, 0x01, 0x02, 0x04, 0x04, 0x04}, /* 7 */
        {0x1F, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x1F}, /* 8 */
        {0x1F, 0x11, 0x11, 0x1F, 0x01, 0x01, 0x1F}, /* 9 */
    };
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = digits[digit][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col))) {
                graphics_draw_rect(x + col * 2, y + row * 2, 2, 2, color);
            }
        }
    }
}

static void draw_number(int x, int y, int num, uint32_t color) {
    if (num >= 100) {
        draw_digit(x, y, num / 100, color);
        draw_digit(x + 12, y, (num / 10) % 10, color);
        draw_digit(x + 24, y, num % 10, color);
    } else if (num >= 10) {
        draw_digit(x, y, num / 10, color);
        draw_digit(x + 12, y, num % 10, color);
    } else {
        draw_digit(x, y, num, color);
    }
}

/* === Game logic === */

static void add_particles(int x, int y) {
    for (int i = 0; i < 3 && num_particles < MAX_PARTICLES; i++) {
        particles[num_particles].x = x - 3 + (rng() % 7);
        particles[num_particles].y = y - 3 + (rng() % 7);
        particles[num_particles].dx = (rng() % 3) - 1;
        particles[num_particles].dy = (rng() % 3) - 1;
        particles[num_particles].life = 10;
        particles[num_particles].active = 1;
        num_particles++;
    }
}

static void update_particles(void) {
    int i = 0;
    while (i < num_particles) {
        particles[i].x += particles[i].dx;
        particles[i].y += particles[i].dy;
        particles[i].life--;
        if (particles[i].life <= 0) {
            particles[i] = particles[num_particles - 1];
            num_particles--;
        } else {
            i++;
        }
    }
}

/* Create a new millipede at the top of the screen */
static void spawn_millipede(void) {
    millipede.count = 10 + level * 2;
    if (millipede.count > MAX_SEGMENTS) millipede.count = MAX_SEGMENTS;

    int start_x = 80;
    int start_y = 40;

    for (int i = 0; i < millipede.count; i++) {
        millipede.segs[i].x = start_x - i * SEGMENT_SIZE;
        millipede.segs[i].y = start_y;
    }

    millipede.dir_x = 1;
    millipede.moved_down = 0;
    millipede.active = 1;
}

static void reset_game(void) {
    player_x = SCREEN_WIDTH / 2 - PLAYER_W / 2;
    player_y = SCREEN_HEIGHT - 80;
    score = 0;
    lives = 3;
    level = 1;
    game_over = 0;
    game_paused = 0;
    num_mushrooms = 0;
    num_particles = 0;
    fire_cooldown = 0;

    for (int i = 0; i < MAX_BULLETS; i++) {
        bullets[i].active = 0;
    }

    spawn_millipede();
}

/* Drop a mushroom when a segment is destroyed */
static void drop_mushroom(int x, int y) {
    if (num_mushrooms >= MAX_MUSHROOMS) return;
    /* Snap to a rough grid so mushrooms line up */
    mushrooms[num_mushrooms].x = (x / 16) * 16;
    if (mushrooms[num_mushrooms].x < 0) mushrooms[num_mushrooms].x = 0;
    if (mushrooms[num_mushrooms].x + MUSHROOM_SIZE > SCREEN_WIDTH)
        mushrooms[num_mushrooms].x = SCREEN_WIDTH - MUSHROOM_SIZE;
    mushrooms[num_mushrooms].y = (y / 16) * 16;
    if (mushrooms[num_mushrooms].y < 10) mushrooms[num_mushrooms].y = 10;
    if (mushrooms[num_mushrooms].y + MUSHROOM_SIZE > SCREEN_HEIGHT - 30)
        mushrooms[num_mushrooms].y = SCREEN_HEIGHT - 30 - MUSHROOM_SIZE;
    mushrooms[num_mushrooms].active = 1;
    num_mushrooms++;
}

/* Check if a rect overlaps any active mushroom */
static int check_mushroom_collision(int x, int y, int w, int h) {
    for (int i = 0; i < num_mushrooms; i++) {
        if (!mushrooms[i].active) continue;
        int mx = mushrooms[i].x;
        int my = mushrooms[i].y;
        if (x < mx + MUSHROOM_SIZE && x + w > mx &&
            y < my + MUSHROOM_SIZE && y + h > my) {
            return 1;
        }
    }
    return 0;
}

/* Fire a bullet from the player */
static void fire_bullet(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) {
            bullets[i].x = player_x + PLAYER_W / 2 - BULLET_W / 2;
            bullets[i].y = player_y - BULLET_H;
            bullets[i].w = BULLET_W;
            bullets[i].h = BULLET_H;
            bullets[i].active = 1;
            break;
        }
    }
}

static void update_bullets(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) continue;
        bullets[i].y -= BULLET_SPEED;
        if (bullets[i].y + bullets[i].h < 0) {
            bullets[i].active = 0;
        }
    }
}

/* Update millipede movement */
static void update_millipede(void) {
    if (!millipede.active) return;

    /* Move the head */
    int hx = millipede.segs[0].x + millipede.dir_x * MILLI_XSPEED;
    int hy = millipede.segs[0].y;

    /* Check for wall collisions */
    if (hx < 30) {
        hx = 30;
        millipede.dir_x = 1;
        millipede.segs[0].y += MILLI_VSTEP;
        millipede.moved_down = 1;
    } else if (hx + SEGMENT_SIZE > SCREEN_WIDTH - 30) {
        hx = SCREEN_WIDTH - 30 - SEGMENT_SIZE;
        millipede.dir_x = -1;
        millipede.segs[0].y += MILLI_VSTEP;
        millipede.moved_down = 1;
    }

    /* Check for mushroom collisions at the new head position */
    if (check_mushroom_collision(hx, millipede.segs[0].y, SEGMENT_SIZE, SEGMENT_SIZE)) {
        hx = millipede.segs[0].x;
        millipede.dir_x = -millipede.dir_x;
        millipede.segs[0].y += MILLI_VSTEP;
        millipede.moved_down = 1;
    }

    millipede.segs[0].x = hx;
    millipede.segs[0].y = hy;

    /* Update rest of segments: each follows the one in front */
    int prev_x = millipede.segs[0].x;
    int prev_y = millipede.segs[0].y;

    for (int i = 1; i < millipede.count; i++) {
        int dx = prev_x - millipede.segs[i].x;
        int dy = prev_y - millipede.segs[i].y;
        int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

        if (dist >= SEGMENT_SIZE) {
            if (dx > 0) millipede.segs[i].x += MILLI_XSPEED;
            else if (dx < 0) millipede.segs[i].x -= MILLI_XSPEED;
            if (dy > 0) millipede.segs[i].y += 1;
            else if (dy < 0) millipede.segs[i].y -= 1;
        }

        prev_x = millipede.segs[i].x;
        prev_y = millipede.segs[i].y;
    }

    /* Check if millipede reached the player's area */
    if (millipede.segs[0].y + SEGMENT_SIZE >= player_y) {
        lives--;
        if (lives <= 0) {
            game_over = 1;
        } else {
            spawn_millipede();
            game_paused = 30;
        }
    }
}

/* Check bullet-millipede collisions */
static void check_bullet_hits(void) {
    for (int b = 0; b < MAX_BULLETS; b++) {
        if (!bullets[b].active) continue;

        for (int s = 0; s < millipede.count; s++) {
            int sx = millipede.segs[s].x;
            int sy = millipede.segs[s].y;

            if (bullets[b].x < sx + SEGMENT_SIZE &&
                bullets[b].x + bullets[b].w > sx &&
                bullets[b].y < sy + SEGMENT_SIZE &&
                bullets[b].y + bullets[b].h > sy) {

                /* Hit! */
                bullets[b].active = 0;
                int is_head = (s == 0);

                /* Destroy the segment — turn it into a mushroom */
                drop_mushroom(sx, sy);
                add_particles(sx + SEGMENT_SIZE / 2, sy + SEGMENT_SIZE / 2);

                if (is_head) {
                    /* Head destroyed — whole millipede dies */
                    millipede.active = 0;
                    score += 500;
                } else {
                    /* Split: kill segments from the hit point to the tail */
                    int killed = 0;
                    for (int k = s; k < millipede.count; k++) {
                        drop_mushroom(millipede.segs[k].x, millipede.segs[k].y);
                        add_particles(millipede.segs[k].x + SEGMENT_SIZE / 2,
                                      millipede.segs[k].y + SEGMENT_SIZE / 2);
                        killed++;
                    }
                    millipede.count = s;
                    score += killed * 100;

                    if (millipede.count <= 0) {
                        millipede.active = 0;
                    }
                }
                goto next_bullet;
            }
        }
next_bullet:;
    }
}

/* Check for bullet-mushroom collision */
static void check_mushroom_bullet_hits(void) {
    for (int b = 0; b < MAX_BULLETS; b++) {
        if (!bullets[b].active) continue;

        for (int m = 0; m < num_mushrooms; m++) {
            if (!mushrooms[m].active) continue;
            int mx = mushrooms[m].x;
            int my = mushrooms[m].y;

            if (bullets[b].x < mx + MUSHROOM_SIZE &&
                bullets[b].x + BULLET_W > mx &&
                bullets[b].y < my + MUSHROOM_SIZE &&
                bullets[b].y + BULLET_H > my) {

                bullets[b].active = 0;
                mushrooms[m].active = 0;
                add_particles(mx + MUSHROOM_SIZE / 2, my + MUSHROOM_SIZE / 2);
                score += 25;
                break;
            }
        }
    }
}

/* Spawn a new millipede when current one is dead */
static void check_spawn_new(void) {
    if (!millipede.active) {
        static int spawn_timer = 0;
        spawn_timer++;
        if (spawn_timer > 20) {
            spawn_timer = 0;
            level++;
            spawn_millipede();
        }
    }
}

/* === Input handling === */

static void handle_input(void) {
    struct virtio_input_event events[16];
    int num = get_events(events, 16);
    for (int i = 0; i < num; i++) {
        struct virtio_input_event *ev = &events[i];
        if (ev->type != EV_KEY) continue;

        if (ev->value == 1) { /* Key press */
            /* Arrow keys: 103=Up, 108=Down, 105=Left, 106=Right */
            if (ev->code == 103) { /* Up */
                player_y -= PLAYER_SPEED;
            } else if (ev->code == 108) { /* Down */
                player_y += PLAYER_SPEED;
            } else if (ev->code == 105) { /* Left */
                player_x -= PLAYER_HSPEED;
            } else if (ev->code == 106) { /* Right */
                player_x += PLAYER_HSPEED;
            } else if (ev->code == 57) { /* Space = fire */
                fire_bullet();
            } else if (ev->code == 16) { /* Q = quit */
                print("Millipede: Quitting.\n");
                exit(0);
            } else if (ev->code == 19) { /* R = restart */
                if (game_over) {
                    reset_game();
                }
            }
        }
    }

    /* Clamp player to bottom area of screen */
    if (player_x < 5) player_x = 5;
    if (player_x + PLAYER_W > SCREEN_WIDTH - 5) player_x = SCREEN_WIDTH - 5 - PLAYER_W;
    if (player_y < SCREEN_HEIGHT - 150) player_y = SCREEN_HEIGHT - 150;
    if (player_y + PLAYER_H > SCREEN_HEIGHT - 5) player_y = SCREEN_HEIGHT - 5 - PLAYER_H;
}

/* Continuous auto-fire for classic arcade feel */
static void auto_fire(void) {
    if (game_over || game_paused > 0) return;
    fire_cooldown--;
    if (fire_cooldown <= 0) {
        fire_bullet();
        fire_cooldown = FIRE_RATE;
    }
}

/* === Rendering === */

static void render(void) {
    graphics_clear(COLOR_BG);

    /* Draw mushrooms */
    for (int i = 0; i < num_mushrooms; i++) {
        if (mushrooms[i].active) {
            draw_mushroom(mushrooms[i].x, mushrooms[i].y, COLOR_MUSHROOM);
        }
    }

    /* Draw millipede segments (back to front) */
    if (millipede.active) {
        for (int i = millipede.count - 1; i >= 0; i--) {
            uint32_t color = (i == 0) ? COLOR_HEAD : COLOR_SEGMENT;
            draw_segment(millipede.segs[i].x, millipede.segs[i].y, i == 0, color);
        }
    }

    /* Draw bullets */
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            draw_bullet(bullets[i].x, bullets[i].y, COLOR_BULLET);
        }
    }

    /* Draw particles */
    for (int i = 0; i < num_particles; i++) {
        draw_particle(&particles[i]);
    }

    /* Draw player */
    if (!game_over) {
        draw_player(player_x, player_y, COLOR_PLAYER);
    }

    /* Score bar at top */
    graphics_draw_rect(0, 0, SCREEN_WIDTH, 30, COLOR_SCORE_BG);
    draw_number(10, 10, score, COLOR_TEXT);

    /* Lives indicator */
    graphics_draw_rect(SCREEN_WIDTH - 80, 10, 10, 10, COLOR_LIFE);
    draw_number(SCREEN_WIDTH - 65, 10, lives, COLOR_LIFE);

    /* Level indicator */
    draw_number(SCREEN_WIDTH / 2 + 25, 10, level, COLOR_TEXT);

    if (game_over) {
        int cx = SCREEN_WIDTH / 2;
        int cy = SCREEN_HEIGHT / 2;
        graphics_draw_rect(cx - 70, cy - 20, 140, 40, COLOR(30, 0, 0));
        graphics_draw_rect(cx - 60, cy - 10, 120, 20, COLOR_GAMEOVER);
        graphics_draw_rect(cx - 80, cy + 30, 160, 20, COLOR(0, 0, 0));
        graphics_draw_rect(cx - 75, cy + 32, 150, 16, COLOR(80, 80, 80));
    }

    graphics_flush();
}

/* === Entry point === */

#ifndef HOST_TEST
__attribute__((section(".text._start")))
#endif
void _start(void) {
    print("Millipede: Starting...\n");

    if (graphics_init() != 0) {
        print("Millipede: Failed to initialize graphics!\n");
        exit(1);
    }

    print("Millipede: Graphics initialized. Use arrow keys to move, Space to shoot.\n");
    print("Millipede: Press Q to quit, R to restart.\n");

    reset_game();

    while (1) {
        if (game_paused > 0) {
            game_paused--;
        }

        handle_input();
        auto_fire();

        if (!game_over && game_paused == 0) {
            update_bullets();
            check_bullet_hits();
            check_mushroom_bullet_hits();
            update_millipede();
            update_particles();
            check_spawn_new();
        }

        render();
        yield();
    }

    exit(0);
}
