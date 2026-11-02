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
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "doomgeneric.h"

static int DG_RESX = 640;
static int DG_RESY = 400;
static uint32_t *fb; /* 640x400 B8G8R8A8, alpha=0xFF */
/* GB-169: the engine draws at 640 wide while the panel is 1920, so rows are
   copied into the mapped surface one at a time (1 MB of BSS for the buffer —
   a malloc here returns NULL once DOOM's zone owns the heap). */
static uint32_t fb_static[DOOMGENERIC_RESX * DOOMGENERIC_RESY];
#define FB_PANEL_W 1920
#define FB_PANEL_H 1080
static volatile uint32_t *fbmap;
static int fbfd = -1;

void DG_Init(void) {
    fb = fb_static;
    DG_ScreenBuffer = fb;
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
