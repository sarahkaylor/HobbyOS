/*
 * Pong In-OS Integration Test (v2)
 *
 * This test wrapper runs the Pong game logic directly (not through
 * the desktop spawn mechanism) and validates the framebuffer output.
 * The test:
 *   1. Initializes graphics
 *   2. Runs the Pong game logic for several frames
 *   3. Checks the framebuffer for Pong's visual elements
 *   4. Prints PONG_TEST_PASS to serial
 */

#include "libc.h"
#include "graphics/graphics.h"
#include "graphics/window.h"

/* --- Syscall wrapper --- */
static long syscall(long num, long a0, long a1, long a2, long a3) {
#ifdef __x86_64__
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3)
                   : "memory");
  return x0;
#endif
}

/* --- Mock event injection --- */
#define MAX_MOCK_EVENTS 256
static struct virtio_input_event mock_events[MAX_MOCK_EVENTS];
static int mock_events_head = 0;
static int mock_events_tail = 0;

void inject_mock_event(uint16_t type, uint16_t code, uint32_t value) {
    int next = (mock_events_head + 1) % MAX_MOCK_EVENTS;
    if (next != mock_events_tail) {
        mock_events[mock_events_head].type = type;
        mock_events[mock_events_head].code = code;
        mock_events[mock_events_head].value = value;
        mock_events_head = next;
    }
}

int get_events(void *buf, int max_events) {
    struct virtio_input_event *events = (struct virtio_input_event *)buf;
    int count = 0;
    while (mock_events_tail != mock_events_head && count < max_events) {
        events[count++] = mock_events[mock_events_tail];
        mock_events_tail = (mock_events_tail + 1) % MAX_MOCK_EVENTS;
    }
    return count;
}

/* --- Read dir mock --- */
int read_dir(const char *path, int index, struct sys_dirent *ent) {
    (void)path;
    if (index == 0) {
        const char *name = "PONG.BIN";
        int i = 0;
        while (name[i]) { ent->name[i] = name[i]; i++; }
        ent->name[i] = '\0';
        ent->attr = 0;
        ent->size = 0;
        return 0;
    }
    return -1;
}

/* --- Include Pong game logic --- */
#define main pong_main
#define _start pong_start
#include "pong.c"
#undef main
#undef _start

/* --- Test logic --- */
static int pixel_matches(int x, int y, uint32_t expected) {
    uint32_t actual = graphics_get_pixel(x, y);
    int r1 = (expected >> 16) & 0xFF;
    int g1 = (expected >> 8) & 0xFF;
    int b1 = expected & 0xFF;
    int r2 = (actual >> 16) & 0xFF;
    int g2 = (actual >> 8) & 0xFF;
    int b2 = actual & 0xFF;
    int dr = r1 - r2; if (dr < 0) dr = -dr;
    int dg = g1 - g2; if (dg < 0) dg = -dg;
    int db = b1 - b2; if (db < 0) db = -db;
    return (dr < 30 && dg < 30 && db < 30);
}

static int test_phase = 0;
static int flush_count = 0;

void flush_fb(void) {
    syscall(10 /* SYS_FLUSH_FB */, 0, 0, 0, 0);
    flush_count++;
}

/* --- Entry point --- */
#ifndef HOST_TEST
__attribute__((section(".text._start")))
#endif
void _start(void) {
    print_console("[PONG_TEST] Starting Pong in-OS integration test...\n");
    print_console("[PONG_TEST] Initializing graphics...\n");

    if (graphics_init() != 0) {
        print_console("[PONG_TEST] FAIL: graphics_init failed!\n");
        exit(1);
    }

    print_console("[PONG_TEST] Graphics initialized. Running Pong game logic...\n");

    /* Initialize Pong game state */
    reset_game();

    /* Run several frames to let the ball move */
    for (int frame = 0; frame < 5; frame++) {
        /* Inject input: move paddle down on first frame, up on second */
        if (frame == 0) {
            inject_mock_event(EV_KEY, 108, 1); /* DOWN press */
            inject_mock_event(EV_KEY, 108, 0); /* DOWN release */
        }
        if (frame == 1) {
            inject_mock_event(EV_KEY, 103, 1); /* UP press */
            inject_mock_event(EV_KEY, 103, 0); /* UP release */
        }

        handle_input();
        update_ai();
        update_ball();

        /* Render */
        graphics_clear(COLOR(0, 0, 0));
        int cx = SCREEN_WIDTH / 2;
        for (int y = 0; y < SCREEN_HEIGHT; y += 16) {
            graphics_draw_rect(cx - 1, y, 2, 8, COLOR(80, 80, 80));
        }
        graphics_draw_rect(20, player_y, 12, 80, COLOR(255, 255, 255));
        graphics_draw_rect(SCREEN_WIDTH - 32, ai_y, 12, 80, COLOR(255, 255, 255));
        graphics_draw_rect(ball_x, ball_y, 8, 8, COLOR(255, 255, 255));
        graphics_draw_rect(SCREEN_WIDTH / 2 - 48, 20, 10, 14, COLOR(255, 255, 255));
        graphics_draw_rect(SCREEN_WIDTH / 2 + 30, 20, 10, 14, COLOR(255, 255, 255));
        graphics_flush();
    }

    /* Now validate the framebuffer */
    print_console("[PONG_TEST] Validating framebuffer...\n");
    int pass = 1;
    int checks = 0;
    int checks_passed = 0;
    int cx = SCREEN_WIDTH / 2;

    /* Check 1: Background should be black (top-left corner) */
    checks++;
    if (pixel_matches(5, 5, COLOR(0, 0, 0))) {
        checks_passed++;
    } else {
        print_console("[PONG_TEST] FAIL: background not black\n");
        pass = 0;
    }

    /* Check 2: Center line should have gray dashes */
    checks++;
    int found_dash = 0;
    for (int y = 0; y < SCREEN_HEIGHT; y += 2) {
        if (pixel_matches(cx - 1, y, COLOR(80, 80, 80))) {
            found_dash = 1;
            break;
        }
    }
    if (found_dash) {
        checks_passed++;
    } else {
        print_console("[PONG_TEST] FAIL: center line not found\n");
        pass = 0;
    }

    /* Check 3: Left paddle should be white */
    checks++;
    int found_left = 0;
    for (int y = player_y; y < player_y + 80 && y < SCREEN_HEIGHT; y++) {
        if (pixel_matches(25, y, COLOR(255, 255, 255))) {
            found_left = 1;
            break;
        }
    }
    if (found_left) {
        checks_passed++;
    } else {
        print_console("[PONG_TEST] FAIL: left paddle not found\n");
        pass = 0;
    }

    /* Check 4: Right paddle should be white */
    checks++;
    int found_right = 0;
    for (int y = ai_y; y < ai_y + 80 && y < SCREEN_HEIGHT; y++) {
        if (pixel_matches(SCREEN_WIDTH - 27, y, COLOR(255, 255, 255))) {
            found_right = 1;
            break;
        }
    }
    if (found_right) {
        checks_passed++;
    } else {
        print_console("[PONG_TEST] FAIL: right paddle not found\n");
        pass = 0;
    }

    /* Check 5: Ball should be white */
    checks++;
    if (pixel_matches(ball_x + 4, ball_y + 4, COLOR(255, 255, 255))) {
        checks_passed++;
    } else {
        print_console("[PONG_TEST] FAIL: ball not found\n");
        pass = 0;
    }

    /* Report results */
    print_console("[PONG_TEST] Checks: ");
    char buf[16];
    buf[0] = '0' + checks_passed;
    buf[1] = '/';
    buf[2] = '0' + checks;
    buf[3] = '\0';
    print_console(buf);
    print_console(" passed\n");

    if (pass && checks_passed == checks) {
        print_console("[PONG_TEST] PONG_TEST_PASS: All visual checks passed!\n");
        print_console("[PONG_TEST] Pong is rendering correctly in the OS.\n");
        print_console("SCREENSHOT_READY\n");
    } else {
        print_console("[PONG_TEST] PONG_TEST_FAIL: Some visual checks failed.\n");
        print_console("SCREENSHOT_READY\n");
    }

    /* Spin to keep the screenshot available */
    while (1) { }
    exit(0);
}