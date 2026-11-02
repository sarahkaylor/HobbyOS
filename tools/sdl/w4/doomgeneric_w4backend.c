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

void DG_SleepMs(uint32_t ms) { (void)ms; }

uint32_t DG_GetTicksMs(void) { return 0; }

int DG_GetKey(int *pressed, unsigned char *key) {
    (void)pressed; (void)key; return 0; /* input stays 0: no input device yet */
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
}
