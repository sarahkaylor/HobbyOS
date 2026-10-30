/*
 * hw/hobbyos/hbscreen.c -- HobbyOS DDX for the X.Org server (XHOBBY.BIN):
 * output side and the DDX entry points.  Derived from hw/vfb/InitOutput.c
 * (X Consortium / X.Org license, see COPYING in the xserver tree).
 *
 * Output model: one screen, depth 24 / 32 bpp TrueColor with the standard
 * little-endian masks 0xff0000/0xff00/0xff -- byte-identical to HobbyOS's
 * B8G8R8A8 framebuffer.  fb renders into a malloc'd shadow; miext/shadow
 * tracks damage, and from the server's block handler hbShadowUpdate()
 * copies the damaged boxes into the SYS_MAP_FB mapping and presents them
 * with SYS_FLUSH_FB_RECTS (batches of <= 32 rects, the kernel's limit).
 *
 * Source of truth: third_party/x11/ddx/hobbyos/ in the HobbyOS repo;
 * build-server.sh copies it into the extracted xserver as hw/hobbyos.
 */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xos.h>
#include "scrnintstr.h"
#include "servermd.h"
#include "fb.h"
#include "colormapst.h"
#include "gcstruct.h"
#include "input.h"
#include "mipointer.h"
#include "micmap.h"
#include "dix.h"
#include "miline.h"
#include "randrstr.h"
#include "shadow.h"
#include "hobbyos.h"

/* ---- HobbyOS native calls (repo libc.a; declared here so this file
 * never pulls <libc.h>, whose macros clash with X headers) ------------- */
struct hb_fb_rect32 { int32_t x, y, w, h; };
extern void *map_fb(void);
extern int fb_ioctl(unsigned long req, void *arg);
extern int flush_fb_rects(const struct hb_fb_rect32 *rects, int count);
extern void flush_fb(void);

#define HB_FBIOGET_VSCREENINFO 0x4600
#define HB_FBIOGET_FSCREENINFO 0x4602
#define HB_FLUSH_MAX_RECTS 32

/* Linux struct fb_var_screeninfo / fb_fix_screeninfo prefixes (only the
 * leading fields are read; the buffers are sized to the full structs). */
struct hb_var_prefix { uint32_t xres, yres, xres_virtual, yres_virtual,
                       xoffset, yoffset, bits_per_pixel; };

typedef struct {
    int width, height;          /* screen size (pixels) */
    uint32_t *fb;               /* SYS_MAP_FB mapping */
    int fbStride;               /* fb row pitch in pixels */
    int fbWidth, fbHeight;      /* real framebuffer size */
    void *shadow;               /* fb's drawing target */
    int shadowStride;           /* bytes */
    CreateScreenResourcesProcPtr createScreenResources;
    CloseScreenProcPtr closeScreen;
} HbScreenRec;

static HbScreenRec hbScreen;
static Bool hbRender = TRUE;
static int hbReqWidth, hbReqHeight;     /* -screen WxH override */
unsigned long hbFlushCount, hbFlushRects;  /* stats (debugger aid) */

/* ---- fb geometry -------------------------------------------------------- */
static void
hbQueryFb(void)
{
    unsigned char var[160], fix[80];
    struct hb_var_prefix v;

    hbScreen.fbWidth = 1920;
    hbScreen.fbHeight = 1080;
    hbScreen.fbStride = 1920;
    memset(var, 0, sizeof var);
    memset(fix, 0, sizeof fix);
    if (fb_ioctl(HB_FBIOGET_VSCREENINFO, var) == 0) {
        memcpy(&v, var, sizeof v);
        if (v.xres >= 64 && v.xres <= 8192 && v.yres >= 64 && v.yres <= 8192 &&
            (v.bits_per_pixel == 32 || v.bits_per_pixel == 0)) {
            hbScreen.fbWidth = (int) v.xres;
            hbScreen.fbHeight = (int) v.yres;
            hbScreen.fbStride = (int) v.xres;
        }
    }
    if (fb_ioctl(HB_FBIOGET_FSCREENINFO, fix) == 0) {
        /* struct fb_fix_screeninfo: char id[16]; unsigned long smem_start;
         * u32 smem_len, type, type_aux, visual; u16 xpanstep, ypanstep,
         * ywrapstep; u32 line_length (offset 16+8+16+6 -> aligned 48). */
        uint32_t ll;
        memcpy(&ll, fix + 48, sizeof ll);
        if (ll >= (uint32_t) hbScreen.fbWidth * 4 && ll % 4 == 0 && ll < 65536)
            hbScreen.fbStride = (int) (ll / 4);
    }
}

/* ---- present ------------------------------------------------------------ */
static void
hbFlushBatch(struct hb_fb_rect32 *r, int n)
{
    if (n <= 0)
        return;
    if (flush_fb_rects(r, n) < 0)
        flush_fb();
    hbFlushCount++;
    hbFlushRects += n;
}

static void
hbShadowUpdate(ScreenPtr pScreen, shadowBufPtr pBuf)
{
    RegionPtr damage = DamageRegion(pBuf->pDamage);
    int nbox = RegionNumRects(damage);
    BoxPtr box = RegionRects(damage);
    PixmapPtr pix = pBuf->pPixmap;
    const unsigned char *src = pix->devPrivate.ptr;
    int srcStride = pix->devKind;
    struct hb_fb_rect32 rects[HB_FLUSH_MAX_RECTS];
    int n = 0, i;

    if (!hbScreen.fb || nbox <= 0)
        return;
    /* Many tiny boxes: present their extents instead (one copy, one rect). */
    if (nbox > 4 * HB_FLUSH_MAX_RECTS) {
        box = RegionExtents(damage);
        nbox = 1;
    }
    for (i = 0; i < nbox; i++, box++) {
        int x1 = box->x1 < 0 ? 0 : box->x1;
        int y1 = box->y1 < 0 ? 0 : box->y1;
        int x2 = box->x2 > hbScreen.fbWidth ? hbScreen.fbWidth : box->x2;
        int y2 = box->y2 > hbScreen.fbHeight ? hbScreen.fbHeight : box->y2;
        int y;

        if (x2 > hbScreen.width) x2 = hbScreen.width;
        if (y2 > hbScreen.height) y2 = hbScreen.height;
        if (x1 >= x2 || y1 >= y2)
            continue;
        for (y = y1; y < y2; y++)
            memcpy(hbScreen.fb + (size_t) y * hbScreen.fbStride + x1,
                   src + (size_t) y * srcStride + (size_t) x1 * 4,
                   (size_t) (x2 - x1) * 4);
        rects[n].x = x1;
        rects[n].y = y1;
        rects[n].w = x2 - x1;
        rects[n].h = y2 - y1;
        if (++n == HB_FLUSH_MAX_RECTS) {
            hbFlushBatch(rects, n);
            n = 0;
        }
    }
    hbFlushBatch(rects, n);
}

static Bool
hbCreateScreenResources(ScreenPtr pScreen)
{
    Bool ret;

    pScreen->CreateScreenResources = hbScreen.createScreenResources;
    ret = pScreen->CreateScreenResources(pScreen);
    pScreen->CreateScreenResources = hbCreateScreenResources;
    if (!ret)
        return FALSE;
    return shadowAdd(pScreen, pScreen->GetScreenPixmap(pScreen),
                     hbShadowUpdate, NULL, 0, NULL);
}

static Bool
hbCloseScreen(ScreenPtr pScreen)
{
    pScreen->CloseScreen = hbScreen.closeScreen;
    if (pScreen->devPrivate)
        (*pScreen->DestroyPixmap) (pScreen->devPrivate);
    pScreen->devPrivate = NULL;
    return pScreen->CloseScreen(pScreen);
}

static Bool
hbSaveScreen(ScreenPtr pScreen, int on)
{
    return TRUE;                /* no blanking hardware: nothing to do */
}

/* ---- RandR: one fixed output/mode (enough for clients that query it) --- */
static Bool
hbRRGetInfo(ScreenPtr pScreen, Rotation *rotations)
{
    *rotations = RR_Rotate_0;
    return TRUE;
}

static Bool
hbRRCrtcSet(ScreenPtr pScreen, RRCrtcPtr crtc, RRModePtr mode, int x, int y,
            Rotation rotation, int numOutput, RROutputPtr *outputs)
{
    return RRCrtcNotify(crtc, mode, x, y, rotation, NULL, numOutput, outputs);
}

static Bool
hbRRScreenSetSize(ScreenPtr pScreen, CARD16 width, CARD16 height,
                  CARD32 mmWidth, CARD32 mmHeight)
{
    /* The framebuffer is fixed; only the current size is accepted. */
    return width == pScreen->width && height == pScreen->height;
}

static Bool
hbRandRInit(ScreenPtr pScreen)
{
    rrScrPrivPtr pScrPriv;
    RRModePtr mode;
    RRCrtcPtr crtc;
    RROutputPtr output;
    xRRModeInfo modeInfo;
    char name[64];

    if (!RRScreenInit(pScreen))
        return FALSE;
    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = hbRRGetInfo;
    pScrPriv->rrCrtcSet = hbRRCrtcSet;
    pScrPriv->rrScreenSetSize = hbRRScreenSetSize;
    RRScreenSetSizeRange(pScreen, pScreen->width, pScreen->height,
                         pScreen->width, pScreen->height);
    snprintf(name, sizeof name, "%dx%d", pScreen->width, pScreen->height);
    memset(&modeInfo, 0, sizeof modeInfo);
    modeInfo.width = pScreen->width;
    modeInfo.height = pScreen->height;
    modeInfo.nameLength = strlen(name);
    mode = RRModeGet(&modeInfo, name);
    if (!mode)
        return FALSE;
    crtc = RRCrtcCreate(pScreen, NULL);
    if (!crtc)
        return FALSE;
    RRCrtcGammaSetSize(crtc, 256);
    output = RROutputCreate(pScreen, "HOBBYFB", 7, NULL);
    if (!output || !RROutputSetClones(output, NULL, 0) ||
        !RROutputSetModes(output, &mode, 1, 0) ||
        !RROutputSetCrtcs(output, &crtc, 1) ||
        !RROutputSetConnection(output, RR_Connected))
        return FALSE;
    RRCrtcNotify(crtc, mode, 0, 0, RR_Rotate_0, NULL, 1, &output);
    return TRUE;
}

/* ---- pointer screen funcs ---------------------------------------------- */
static Bool
hbCursorOffScreen(ScreenPtr *ppScreen, int *x, int *y)
{
    return FALSE;
}

static void
hbCrossScreen(ScreenPtr pScreen, Bool entering)
{
}

static miPointerScreenFuncRec hbPointerCursorFuncs = {
    hbCursorOffScreen,
    hbCrossScreen,
    miPointerWarpCursor
};

/* ---- screen init -------------------------------------------------------- */
static Bool
hbScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
    int dpi = monitorResolution ? monitorResolution : 96;
    void *fbmap;

    hbQueryFb();
    hbScreen.width = hbReqWidth ? hbReqWidth : hbScreen.fbWidth;
    hbScreen.height = hbReqHeight ? hbReqHeight : hbScreen.fbHeight;

    fbmap = map_fb();
    if (fbmap == NULL || fbmap == (void *) -1L ||
        (intptr_t) fbmap < 0 && (intptr_t) fbmap > -4096) {
        ErrorF("hobbyos: SYS_MAP_FB failed (%p)\n", fbmap);
        return FALSE;
    }
    hbScreen.fb = fbmap;
    hbScreen.shadowStride = PixmapBytePad(hbScreen.width, 24);
    hbScreen.shadow = calloc((size_t) hbScreen.shadowStride, hbScreen.height);
    if (!hbScreen.shadow)
        return FALSE;

    if (!miSetVisualTypesAndMasks(24, (1 << TrueColor), 8, TrueColor,
                                  0xff0000, 0x00ff00, 0x0000ff))
        return FALSE;
    miSetPixmapDepths();

    if (!fbScreenInit(pScreen, hbScreen.shadow, hbScreen.width,
                      hbScreen.height, dpi, dpi,
                      hbScreen.shadowStride / 4, 32))
        return FALSE;
    if (hbRender && !fbPictureInit(pScreen, 0, 0))
        return FALSE;
    if (!shadowSetup(pScreen))
        return FALSE;
    if (!hbRandRInit(pScreen))
        return FALSE;

    miDCInitialize(pScreen, &hbPointerCursorFuncs);

    pScreen->blackPixel = 0;
    pScreen->whitePixel = 0xffffff;
    pScreen->SaveScreen = hbSaveScreen;

    hbScreen.createScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = hbCreateScreenResources;
    hbScreen.closeScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = hbCloseScreen;

    if (!fbCreateDefColormap(pScreen))
        return FALSE;
    LogMessage(X_INFO, "hobbyos: screen %dx%d depth 24 (fb %dx%d pitch %d px)\n",
               hbScreen.width, hbScreen.height, hbScreen.fbWidth,
               hbScreen.fbHeight, hbScreen.fbStride);
    return TRUE;
}

void
InitOutput(ScreenInfo *screen_info, int argc, char **argv)
{
    static const int depths[][2] = {
        /* depth, bpp: the screen depth plus what RENDER wants */
        {1, 1}, {4, 8}, {8, 8}, {16, 16}, {24, 32}, {32, 32}
    };
    int i;

    for (i = 0; i < (int) (sizeof depths / sizeof depths[0]); i++) {
        screen_info->formats[i].depth = depths[i][0];
        screen_info->formats[i].bitsPerPixel = depths[i][1];
        screen_info->formats[i].scanlinePad = BITMAP_SCANLINE_PAD;
    }
    screen_info->numPixmapFormats = i;
    screen_info->imageByteOrder = IMAGE_BYTE_ORDER;
    screen_info->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screen_info->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screen_info->bitmapBitOrder = BITMAP_BIT_ORDER;

    if (AddScreen(hbScreenInit, argc, argv) == -1)
        FatalError("hobbyos: couldn't add screen\n");
}

/* ---- DDX hooks --------------------------------------------------------- */
void
ddxGiveUp(enum ExitCode error)
{
    free(hbScreen.shadow);
    hbScreen.shadow = NULL;
}

/*
 * stdio fds.  A HobbyOS program spawned without fds 0-2 still gets console
 * output from write(1/2) (the kernel's fallback for UNASSIGNED fds), but
 * then accept() hands the first client fds 0, 1, 2 -- and every ErrorF()
 * (write(2, ...)) lands inside client 3's X protocol stream.  So the
 * server pins 0-2 to /dev/null before it creates any socket (OsVendorInit
 * runs in OsInit, ahead of CreateWellKnownSockets) and the patched
 * os/log.c sends stderr text through hbConsoleWrite() instead.
 */
static Bool hbStderrIsConsole = FALSE;  /* TRUE once fd 2 is pinned */

int
hbConsoleWrite(const void *buf, size_t len)
{
    if (!hbStderrIsConsole)
        return (int) write(2, buf, len);
    /* SYS_WRITE_CONSOLE (1): the console writer libc's print() uses */
    return (int) syscall(1, (long) buf, (long) len, 0L, 0L);
}

static void
hbReserveStdio(void)
{
    int fd, tries;

    /* open() returns the lowest free fd: keep every /dev/null that lands on
     * 0-2, stop at the first one above 2.  (fcntl(F_GETFD) cannot tell an
     * unassigned console-fallback fd from a real one on HobbyOS.) */
    for (tries = 0; tries < 3; tries++) {
        fd = open("/dev/null", O_RDWR);
        if (fd < 0) {
            ErrorF("hobbyos: cannot open /dev/null to pin fds 0-2\n");
            break;
        }
        if (fd > 2) {
            close(fd);
            break;
        }
        if (fd != 2)
            continue;
        hbStderrIsConsole = TRUE;   /* fd 2 was free: stderr = console */
    }
}

void
OsVendorInit(void)
{
    static Bool done;

    if (!done) {
        hbReserveStdio();
        done = TRUE;
    }
}

void
OsVendorFatalError(const char *f, va_list args)
{
}

#if defined(DDXBEFORERESET)
void
ddxBeforeReset(void)
{
}
#endif

#if INPUTTHREAD
void
ddxInputThreadInit(void)
{
}
#endif

void
ddxUseMsg(void)
{
    ErrorF("-screen WxH            screen size (default: the framebuffer's)\n");
    ErrorF("+/-render              turn on/off RENDER extension support (default on)\n");
    ErrorF("-xkm file              compiled keymap to load (default /X11/US.XKM)\n");
    ErrorF("-verbose [n]           stderr log verbosity (default 0; 3 if n omitted)\n");
    ErrorF("-pollms n              input poll interval when idle, ms (default %d)\n",
           HB_INPUT_POLL_MS_DEFAULT);
}

int
ddxProcessArgument(int argc, char *argv[], int i)
{
    if (strcmp(argv[i], "-screen") == 0) {
        int w, h;
        if (i + 1 >= argc)
            UseMsg();
        /* accept "WxH" and Xvfb's "N WxHxD" */
        if (i + 2 < argc && strchr(argv[i + 1], 'x') == NULL &&
            sscanf(argv[i + 2], "%dx%d", &w, &h) == 2) {
            hbReqWidth = w; hbReqHeight = h;
            return 3;
        }
        if (sscanf(argv[i + 1], "%dx%d", &w, &h) != 2)
            UseMsg();
        hbReqWidth = w; hbReqHeight = h;
        return 2;
    }
    if (strcmp(argv[i], "+render") == 0) {
        hbRender = TRUE;
        return 1;
    }
    if (strcmp(argv[i], "-render") == 0) {
        hbRender = FALSE;
        return 1;
    }
    if (strcmp(argv[i], "-xkm") == 0) {
        if (i + 1 >= argc)
            UseMsg();
        XkbHobbyKeymapFile = argv[i + 1];
        return 2;
    }
    if (strcmp(argv[i], "-verbose") == 0) {
        /* Xorg-style: stderr verbosity for LogMessage (default 0) */
        int v = 3;
        if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
            v = atoi(argv[i + 1]);
            LogSetParameter(XLOG_VERBOSITY, v);
            return 2;
        }
        LogSetParameter(XLOG_VERBOSITY, v);
        return 1;
    }
    if (strcmp(argv[i], "-pollms") == 0) {
        if (i + 1 >= argc)
            UseMsg();
        hbInputPollMs = atoi(argv[i + 1]);
        if (hbInputPollMs < 1) hbInputPollMs = 1;
        if (hbInputPollMs > 1000) hbInputPollMs = 1000;
        return 2;
    }
    return 0;
}

int
hbScreenWidth(void)
{
    return hbScreen.width;
}

int
hbScreenHeight(void)
{
    return hbScreen.height;
}
