/*  doomgeneric_w4backend.c — static-musl backend for the GB-041 W4 ELF port
 *  surface. Provides the DG_* callbacks + main so the engine links as a static
 *  musl x86_64 ELF. Renders DG_ScreenBuffer into a malloc'd 640x400 32bpp
 *  buffer (the HobbyOS fb target). This is the W4 port surface; mapping it to a
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
#include "doomgeneric.h"

static int DG_RESX = 640;
static int DG_RESY = 400;
static uint32_t *fb; /* 640x400 B8G8R8A8, alpha=0xFF */

void DG_Init(void) {
    fb = malloc((size_t)DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
    DG_ScreenBuffer = fb;
    printf("[W4] DG_Init %dx%d fb=%p\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY, (void*)fb);
}

void DG_DrawFrame(void) {
    /* blit DG_ScreenBuffer -> fb (here same buffer; the fb IS the target). */
    if (!fb) return;
    /* ensure opaque alpha */
    for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) fb[i] |= 0xFF000000u;
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

int DG_GetKey(int *pressed, unsigned char *key) {
    (void)pressed; (void)key; return 0; /* input stays 0: no input device yet */
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
}
