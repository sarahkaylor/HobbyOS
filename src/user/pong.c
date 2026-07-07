#include "libc.h"
#include "graphics/graphics.h"

/*
 * Pong — a simple video game for HobbyOS.
 *
 * Controls:
 *   Up/Down arrow keys (or W/S) move the left paddle.
 *   The right paddle is AI-controlled.
 *   Press Q to quit.
 *   Press R to reset after someone scores.
 *
 * The game runs as a user-space program using the VirtIO GPU
 * framebuffer (mapped via map_fb) and the VirtIO input event
 * system (get_events) for keyboard input.
 */

/* --- Constants --- */
#define PADDLE_WIDTH   12
#define PADDLE_HEIGHT  80
#define BALL_SIZE       8
#define PADDLE_SPEED    8
#define AI_SPEED        5
#define BALL_SPEED      4
#define WINNING_SCORE   5

/* Colors */
#define COLOR_BG       COLOR(0, 0, 0)
#define COLOR_PADDLE   COLOR(255, 255, 255)
#define COLOR_BALL     COLOR(255, 255, 255)
#define COLOR_TEXT     COLOR(255, 255, 255)
#define COLOR_DASH     COLOR(80, 80, 80)

/* --- Game state --- */
static int player_y;      /* left paddle top-left Y */
static int ai_y;          /* right paddle top-left Y */
static int ball_x, ball_y;
static int ball_dx, ball_dy;
static int player_score;
static int ai_score;
static int game_over;     /* 0 = playing, 1 = someone won */
static int winner;        /* 0 = player, 1 = AI */

/* Simple PRNG (same approach as graphics_test.c) */
static unsigned int rng_seed = 42;
static unsigned int rng(void) {
    rng_seed = (rng_seed * 1103515245 + 12345) & 0x7fffffff;
    return rng_seed;
}

/* --- Drawing helpers --- */

static void draw_paddle(int x, int y, uint32_t color) {
    graphics_draw_rect(x, y, PADDLE_WIDTH, PADDLE_HEIGHT, color);
}

static void draw_ball(int x, int y, uint32_t color) {
    graphics_draw_rect(x, y, BALL_SIZE, BALL_SIZE, color);
}

/* Draw a dashed center line */
static void draw_center_line(void) {
    int x = SCREEN_WIDTH / 2;
    for (int y = 0; y < SCREEN_HEIGHT; y += 16) {
        graphics_draw_rect(x - 1, y, 2, 8, COLOR_DASH);
    }
}

/* Draw a single digit (0-9) at (x, y) using simple block font */
static void draw_digit(int x, int y, int digit, uint32_t color) {
    /* 5x7 block font for digits 0-9 */
    /* Each digit is 5 columns x 7 rows */
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

static void draw_score(void) {
    /* Player score (left side) */
    if (player_score >= 10) {
        draw_digit(SCREEN_WIDTH / 2 - 60, 20, player_score / 10, COLOR_TEXT);
        draw_digit(SCREEN_WIDTH / 2 - 48, 20, player_score % 10, COLOR_TEXT);
    } else {
        draw_digit(SCREEN_WIDTH / 2 - 48, 20, player_score, COLOR_TEXT);
    }
    /* AI score (right side) */
    if (ai_score >= 10) {
        draw_digit(SCREEN_WIDTH / 2 + 30, 20, ai_score / 10, COLOR_TEXT);
        draw_digit(SCREEN_WIDTH / 2 + 42, 20, ai_score % 10, COLOR_TEXT);
    } else {
        draw_digit(SCREEN_WIDTH / 2 + 30, 20, ai_score, COLOR_TEXT);
    }
}

/* --- Game logic --- */

static void reset_ball(int toward_left) {
    ball_x = SCREEN_WIDTH / 2 - BALL_SIZE / 2;
    ball_y = SCREEN_HEIGHT / 2 - BALL_SIZE / 2;
    /* Randomize vertical direction */
    int dy = (rng() % 3) - 1; /* -1, 0, or 1 */
    if (dy == 0) dy = 1;
    ball_dx = toward_left ? -BALL_SPEED : BALL_SPEED;
    ball_dy = dy * (BALL_SPEED / 2);
    if (ball_dy == 0) ball_dy = 2;
}

static void reset_game(void) {
    player_y = SCREEN_HEIGHT / 2 - PADDLE_HEIGHT / 2;
    ai_y = SCREEN_HEIGHT / 2 - PADDLE_HEIGHT / 2;
    player_score = 0;
    ai_score = 0;
    game_over = 0;
    winner = 0;
    reset_ball(rng() & 1);
}

static void update_ai(void) {
    int ai_center = ai_y + PADDLE_HEIGHT / 2;
    int ball_center = ball_y + BALL_SIZE / 2;
    /* Only track the ball when it's moving toward the AI paddle */
    if (ball_dx > 0) {
        if (ai_center < ball_center - 4) {
            ai_y += AI_SPEED;
        } else if (ai_center > ball_center + 4) {
            ai_y -= AI_SPEED;
        }
    }
    /* Clamp AI paddle to screen */
    if (ai_y < 0) ai_y = 0;
    if (ai_y + PADDLE_HEIGHT > SCREEN_HEIGHT) ai_y = SCREEN_HEIGHT - PADDLE_HEIGHT;
}

static int check_collision(int bx, int by, int bw, int bh,
                          int px, int py, int pw, int ph) {
    /* AABB collision */
    if (bx < px + pw && bx + bw > px &&
        by < py + ph && by + bh > py) {
        return 1;
    }
    return 0;
}

static void update_ball(void) {
    ball_x += ball_dx;
    ball_y += ball_dy;

    /* Top/bottom wall bounce */
    if (ball_y < 0) {
        ball_y = 0;
        ball_dy = -ball_dy;
    }
    if (ball_y + BALL_SIZE > SCREEN_HEIGHT) {
        ball_y = SCREEN_HEIGHT - BALL_SIZE;
        ball_dy = -ball_dy;
    }

    /* Left paddle (player) collision */
    int player_x = 20;
    if (check_collision(ball_x, ball_y, BALL_SIZE, BALL_SIZE,
                        player_x, player_y, PADDLE_WIDTH, PADDLE_HEIGHT)) {
        ball_x = player_x + PADDLE_WIDTH;
        ball_dx = -ball_dx;
        /* Add some English based on where it hit the paddle */
        int hit_pos = (ball_y + BALL_SIZE / 2) - (player_y + PADDLE_HEIGHT / 2);
        ball_dy = hit_pos / 3;
        if (ball_dy == 0) ball_dy = (rng() & 1) ? 1 : -1;
    }

    /* Right paddle (AI) collision */
    int ai_x = SCREEN_WIDTH - 20 - PADDLE_WIDTH;
    if (check_collision(ball_x, ball_y, BALL_SIZE, BALL_SIZE,
                        ai_x, ai_y, PADDLE_WIDTH, PADDLE_HEIGHT)) {
        ball_x = ai_x - BALL_SIZE;
        ball_dx = -ball_dx;
        int hit_pos = (ball_y + BALL_SIZE / 2) - (ai_y + PADDLE_HEIGHT / 2);
        ball_dy = hit_pos / 3;
        if (ball_dy == 0) ball_dy = (rng() & 1) ? 1 : -1;
    }

    /* Scoring: ball goes off left or right edge */
    if (ball_x < 0) {
        /* AI scores */
        ai_score++;
        if (ai_score >= WINNING_SCORE) {
            game_over = 1;
            winner = 1;
        } else {
            reset_ball(0); /* serve toward AI (right) */
        }
    }
    if (ball_x + BALL_SIZE > SCREEN_WIDTH) {
        /* Player scores */
        player_score++;
        if (player_score >= WINNING_SCORE) {
            game_over = 1;
            winner = 0;
        } else {
            reset_ball(1); /* serve toward player (left) */
        }
    }
}

/* --- Rendering --- */

static void render(void) {
    graphics_clear(COLOR_BG);
    draw_center_line();
    draw_score();
    draw_paddle(20, player_y, COLOR_PADDLE);
    draw_paddle(SCREEN_WIDTH - 20 - PADDLE_WIDTH, ai_y, COLOR_PADDLE);
    draw_ball(ball_x, ball_y, COLOR_BALL);

    if (game_over) {
        /* Draw a colored banner for the winner */
        int msg_x = SCREEN_WIDTH / 2 - 40;
        int msg_y = SCREEN_HEIGHT / 2 - 20;
        graphics_draw_rect(msg_x - 10, msg_y - 5, 100, 20, COLOR(0, 0, 0));
        graphics_draw_rect(msg_x, msg_y, 80, 10, winner == 0 ? COLOR(0, 255, 0) : COLOR(255, 50, 50));
    }

    graphics_flush();
}

/* --- Input handling --- */

static void handle_input(void) {
    struct virtio_input_event events[16];
    int num = get_events(events, 16);
    for (int i = 0; i < num; i++) {
        struct virtio_input_event *ev = &events[i];
        if (ev->type != EV_KEY || ev->value != 1)
            continue; /* Only key presses */

        /* Arrow keys: 103=Up, 108=Down */
        if (ev->code == 103) {
            player_y -= PADDLE_SPEED;
        } else if (ev->code == 108) {
            player_y += PADDLE_SPEED;
        } else if (ev->code == 17) { /* W key */
            player_y -= PADDLE_SPEED;
        } else if (ev->code == 31) { /* S key */
            player_y += PADDLE_SPEED;
        } else if (ev->code == 16) { /* Q key = quit */
            print("Pong: Quitting.\n");
            exit(0);
        } else if (ev->code == 19) { /* R key = reset */
            reset_game();
        }
    }
    /* Clamp player paddle */
    if (player_y < 0) player_y = 0;
    if (player_y + PADDLE_HEIGHT > SCREEN_HEIGHT)
        player_y = SCREEN_HEIGHT - PADDLE_HEIGHT;
}

/* --- Main entry point --- */

#ifndef HOST_TEST
__attribute__((section(".text._start")))
#endif
void _start(void) {
    print("Pong: Starting...\n");

    if (graphics_init() != 0) {
        print("Pong: Failed to initialize graphics!\n");
        exit(1);
    }

    print("Pong: Graphics initialized. Use Up/Down arrows or W/S to move.\n");
    print("Pong: Press Q to quit, R to reset.\n");

    reset_game();

    while (1) {
        handle_input();
        if (!game_over) {
            update_ai();
            update_ball();
        }
        render();
        /* Small delay to control game speed — yield to scheduler */
        yield();
    }

    exit(0);
}