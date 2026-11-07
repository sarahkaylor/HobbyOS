/*  doomgeneric_w4backend.c — static-musl backend for the GB-041 W4 ELF port
 *  surface. Provides the DG_* callbacks + main so the engine links as a static
 *  musl x86_64 ELF. Renders DG_ScreenBuffer into a fixed 640x400 32bpp BSS
 *  buffer and presents it through a mapped /dev/fb0 surface. This is the W4 port surface; mapping it to a
 *  live fb/input device is the compat integration (GB-033/GB-025), tracked
 *  against the loader. `main` boots the engine and ticks. Static-analysis/build
 *  evidence only — runtime playability is the compat gate.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "doomgeneric.h"
#include "doomkeys.h"

static int DG_RESX = 640;
static int DG_RESY = 400;
static uint32_t *fb; /* 640x400 B8G8R8A8, alpha=0xFF */
/* GB-166/GB-169: the engine draws at 640 wide while the panel is 1920, so rows
   are copied into the mapped surface one at a time.  The 1 MB staging buffer is
   fixed-size BSS, not a malloc: once DOOM's zone owns the heap, a 1 MB malloc
   returns NULL (measured in-guest: `[W4] DG_Init 640x400 fb=0x00000000`). */
static uint32_t fb_static[DOOMGENERIC_RESX * DOOMGENERIC_RESY];
#define FB_PANEL_W 1920
#define FB_PANEL_H 1080
static volatile uint32_t *fbmap;
static int fbfd = -1;

void DG_Init(void) {
    fb = fb_static;
    DG_ScreenBuffer = fb;
    /* GB-166 evidence channel: the console is the only stdout sink and it
       scrolls, so also drop the pointer where the host can read it after the
       run (mcopy the image). */
    FILE *f = fopen("/W4FB.TXT", "w");
    if (f) { fprintf(f, "fb=%p\n", (void*)fb); fclose(f); }
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd >= 0) {
        void *m = mmap(0, (size_t)FB_PANEL_W * FB_PANEL_H * 4,
                       PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
        if (m != MAP_FAILED) fbmap = (volatile uint32_t *)m;
    }
    printf("[W4] DG_Init %dx%d fb=%p map=%p\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY,
           (void*)fb, (void*)fbmap);
}

void DG_DrawFrame(void) {
    /* blit DG_ScreenBuffer -> fb (here same buffer; the fb IS the target). */
    if (!fb) return;
    /* ensure opaque alpha */
    for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) fb[i] |= 0xFF000000u;
    /* GB-169: present it — row by row, because the panel is 1920 wide and the
       engine's 640-pixel rows are contiguous in its own buffer only. */
    if (fbmap) {
        for (int y = 0; y < DOOMGENERIC_RESY; y++) {
            volatile uint32_t *dst = fbmap + (size_t)y * FB_PANEL_W;
            const uint32_t *src = fb + (size_t)y * DOOMGENERIC_RESX;
            for (int x = 0; x < DOOMGENERIC_RESX; x++) dst[x] = src[x];
        }
        fsync(fbfd);
    }
}

/* GB-168: the clock must advance or BuildNewTic never runs; the original
   stubs (`return 0`, no-op sleep) left the engine spinning until the 20 s
   console watchdog.  clock_gettime(CLOCK_MONOTONIC) is compat syscall 228
   (GB-021); musl's usleep() lands on nanosleep, now mapped to the kernel's
   millisecond sleep. */
void DG_SleepMs(uint32_t ms) {
    if (ms) usleep((useconds_t)ms * 1000u);
}

uint32_t DG_GetTicksMs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                      (uint64_t)ts.tv_nsec / 1000000u);
}

/* GB-172: evdev (Linux input-event-codes) -> doomkeys.  Only the keys the
   menu, the movement bindings and the cheats need; 0 = not forwarded. */
static unsigned char ev_to_doomkey(unsigned short code) {
    switch (code) {
    case 1:   return KEY_ESCAPE;
    case 14:  return KEY_BACKSPACE;
    case 15:  return KEY_TAB;
    case 28:  return KEY_ENTER;
    case 57:  return ' ';              /* space fires */
    case 103: return KEY_UPARROW;
    case 105: return KEY_LEFTARROW;
    case 106: return KEY_RIGHTARROW;
    case 108: return KEY_DOWNARROW;
    case 29:  return KEY_FIRE;         /* left ctrl */
    case 42: case 54: return KEY_RSHIFT;
    case 56:  return KEY_RALT;
    case 100: return KEY_LALT;
    case 59: case 60: case 61: case 62: case 63:
    case 64: case 65: case 66: case 67: case 68:
        return (unsigned char)(KEY_F1 + (code - 59));
    case 88:  return KEY_F12;
    /* digits 1..0 */
    case 2: case 3: case 4: case 5: case 6:
    case 7: case 8: case 9: case 10: case 11:
        return (unsigned char)('1' + (code - 2));
    /* q w e r t y u i o p */
    case 16: return 'q'; case 17: return 'w'; case 18: return 'e';
    case 19: return 'r'; case 20: return 't'; case 21: return 'y';
    case 22: return 'u'; case 23: return 'i'; case 24: return 'o';
    case 25: return 'p';
    /* a s d f g h j k l */
    case 30: return 'a'; case 31: return 's'; case 32: return 'd';
    case 33: return 'f'; case 34: return 'g'; case 35: return 'h';
    case 36: return 'j'; case 37: return 'k'; case 38: return 'l';
    /* z x c v b n m */
    case 44: return 'z'; case 45: return 'x'; case 46: return 'c';
    case 47: return 'v'; case 48: return 'b'; case 49: return 'n';
    case 50: return 'm';
    default:  return 0;
    }
}

static int kbd_fd = -1;
static int kbd_tried = 0;

int DG_GetKey(int *pressed, unsigned char *key) {
    struct { unsigned long sec, usec; unsigned short type, code; int value; } ev;
    if (!kbd_tried) {
        kbd_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
        kbd_tried = 1;
    }
    if (kbd_fd < 0) return 0;
    /* drain until an EV_KEY we can map; EV_SYN/EV_MSC are skipped */
    for (int guard = 0; guard < 32; guard++) {
        ssize_t n = read(kbd_fd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev) return 0;
        if (ev.type != 1) continue;                 /* EV_KEY */
        unsigned char k = ev_to_doomkey(ev.code);
        if (!k) continue;
        *pressed = (ev.value != 0);                 /* 1 down, 0 up, 2 repeat */
        *key = k;
        return 1;
    }
    return 0;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
}
