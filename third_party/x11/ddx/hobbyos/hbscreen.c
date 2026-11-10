/*
 * hw/hobbyos/hbscreen.c -- HobbyOS DDX for the X.Org server (XHOBBY.BIN):
 * output side and the DDX entry points.  Derived from hw/vfb/InitOutput.c
 * (X Consortium / X.Org license, see COPYING in the xserver tree).
 *
 * Output model: ONE framebuffer, up to TWO screens.
 *
 *   :0.0  the desktop.  Depth 24 / 32 bpp TrueColor with the standard
 *         little-endian masks 0xff0000/0xff00/0xff -- byte-identical to
 *         HobbyOS's B8G8R8A8 framebuffer -- and the whole framebuffer's size.
 *   :0.1  the 8-bit client screen.  Depth 8 PseudoColor, 640x400, presented
 *         1:1 at the framebuffer's top-left corner.  It is created when :0.0
 *         is depth 24 (or unconditionally with HB_SECOND_SCREEN=1).
 *
 * Why a second SCREEN and not an 8-bit window on :0.0: the X protocol requires
 * a window's depth to equal its parent's, so an 8-bit window cannot exist under
 * a 24-bit root (BadMatch).  Clients from the PseudoColor era ask
 * XMatchVisualInfo(dpy, screen, 8, PseudoColor, &vi) about the screen they were
 * pointed at -- id's Doom 1.10 I_Error()s otherwise -- and only a screen whose
 * ROOT is depth 8 answers that.  XFree86 shipped :0.0 + :0.1 for the same
 * reason.  HB_XDEPTH=8 still collapses this to a single whole-screen depth-8
 * screen (the pre-two-screen behaviour, kept for the XDEPTH8 probe).
 *
 * Presentation: exactly one screen owns the framebuffer.  Screen 1 owns it
 * while it has a viewable child window (pScreen->root->firstChild and friends);
 * while it does, :0.0's shadow updates are dropped -- their damage is still
 * consumed by miext/shadow, which empties the region after calling us -- and
 * when the last screen-1 window goes away the desktop is presented in FULL so
 * it reappears instead of waiting for the next damage.  The same transition
 * moves the pointer onto the game's window and hands it the keyboard (warp +
 * a synthesized motion, then a keyboard grab owned by the game's client), and
 * moves the pointer back to :0.0 when it ends; see the hand-over section.
 *
 * Both screens render into their own malloc'd shadow; miext/shadow tracks
 * damage, and from the server's block handler hbShadowUpdate() copies the
 * damaged boxes into the SYS_MAP_FB mapping and presents them with
 * SYS_FLUSH_FB_RECTS (batches of <= 32 rects, the kernel's limit).
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
#include "windowstr.h"
#include "servermd.h"
#include "fb.h"
#include "colormapst.h"
#include "gcstruct.h"
#include "input.h"
#include "inputstr.h"
#include "inpututils.h"
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
extern int sysinfo(int cmd, void *buf, int size);

/* L89: the server's own millisecond clock (kernel uptime). */
static int hbNowMs(void) { return sysinfo(1, 0, 0); }

#define HB_FBIOGET_VSCREENINFO 0x4600
#define HB_FBIOGET_FSCREENINFO 0x4602
#define HB_FLUSH_MAX_RECTS 32

/* Linux struct fb_var_screeninfo / fb_fix_screeninfo prefixes (only the
 * leading fields are read; the buffers are sized to the full structs). */
struct hb_var_prefix { uint32_t xres, yres, xres_virtual, yres_virtual,
                       xoffset, yoffset, bits_per_pixel; };

typedef struct {
    int screenNum;              /* pScreen->myNum (0 = desktop, 1 = :0.1) */
    int width, height;          /* screen size (pixels) */
    int depth;                  /* 24 (TrueColor) or 8 (PseudoColor) */
    uint32_t *fb;               /* SYS_MAP_FB mapping (shared by both screens) */
    int fbStride;               /* fb row pitch in pixels */
    int fbWidth, fbHeight;      /* real framebuffer size */
    void *shadow;               /* fb's drawing target */
    int shadowStride;           /* bytes */
    uint32_t palette[256];      /* depth 8: index -> B8G8R8A8 word */
    ColormapPtr loggedCmap;     /* depth 8: last colormap logged */
    CreateScreenResourcesProcPtr createScreenResources;
    CloseScreenProcPtr closeScreen;
} HbScreenRec;

/* Per-screen state, indexed by pScreen->myNum.  NOT pScreen->devPrivate:
 * fbScreenInit stores its screen pixmap there (hbCloseScreen destroys it), so
 * the DDX record lives here and is found from the ScreenPtr.  Two screens are
 * all this DDX ever adds (desktop + the depth-8 client screen). */
#define HB_MAX_SCREENS 2
static HbScreenRec hbScreens[HB_MAX_SCREENS];

static HbScreenRec *
hbScreenOf(ScreenPtr pScreen)
{
    if (!pScreen || pScreen->myNum < 0 || pScreen->myNum >= HB_MAX_SCREENS)
        return NULL;
    return &hbScreens[pScreen->myNum];
}

static Bool hbRender = TRUE;
static int hbReqWidth, hbReqHeight;     /* -screen WxH override */
unsigned long hbFlushCount, hbFlushRects;  /* stats (debugger aid) */

/* ---- presentation arbitration ------------------------------------------ */
/* One framebuffer, one owner.  hbOwner is the screen number whose content is
 * on the glass; hbSecondActive mirrors "screen 1 has a viewable child".  Both
 * are only touched from hbOwnershipPoll(), which runs in the server's block
 * handler BEFORE the per-screen shadow block handlers (dix's registered
 * handlers run first), so a transition is seen before either shadow update in
 * the same pass. */
static int hbOwner;
static Bool hbSecondActive;
static Bool hbFocusStolen;

/* ---- fb geometry -------------------------------------------------------- */
static void
hbQueryFb(HbScreenRec *scr)
{
    unsigned char var[160], fix[80];
    struct hb_var_prefix v;

    scr->fbWidth = 1920;
    scr->fbHeight = 1080;
    scr->fbStride = 1920;
    memset(var, 0, sizeof var);
    memset(fix, 0, sizeof fix);
    if (fb_ioctl(HB_FBIOGET_VSCREENINFO, var) == 0) {
        memcpy(&v, var, sizeof v);
        if (v.xres >= 64 && v.xres <= 8192 && v.yres >= 64 && v.yres <= 8192 &&
            (v.bits_per_pixel == 32 || v.bits_per_pixel == 0)) {
            scr->fbWidth = (int) v.xres;
            scr->fbHeight = (int) v.yres;
            scr->fbStride = (int) v.xres;
        }
    }
    if (fb_ioctl(HB_FBIOGET_FSCREENINFO, fix) == 0) {
        /* struct fb_fix_screeninfo: char id[16]; unsigned long smem_start;
         * u32 smem_len, type, type_aux, visual; u16 xpanstep, ypanstep,
         * ywrapstep; u32 line_length (offset 16+8+16+6 -> aligned 48). */
        uint32_t ll;
        memcpy(&ll, fix + 48, sizeof ll);
        if (ll >= (uint32_t) scr->fbWidth * 4 && ll % 4 == 0 && ll < 65536)
            scr->fbStride = (int) (ll / 4);
    }
}

/* ---- present ------------------------------------------------------------ */
/* Depth 8: rebuild scr->palette[] from the colormap the server has installed.
 * miCreateDefColormap installs the default map, and every XInstallColormap /
 * window-colormap install goes through miInstallColormap (fbInstallColormap),
 * so GetInstalledmiColormap() always names the map that is on screen.  The
 * table is rebuilt at EVERY present, not just on install: dix writes a cell's
 * value before it calls the screen's StoreColors, so a client that cycles its
 * palette with XStoreColors (Doom's I_SetPalette) is visible on the next
 * frame with no hooks at all. */
static void
hbLogColormap(HbScreenRec *scr, ColormapPtr cmap)
{
    if (cmap == scr->loggedCmap)
        return;
    scr->loggedCmap = cmap;
    ErrorF("hobbyos: depth8 colormap 0x%lx%s entries "
           "[0x10]=%04x/%04x/%04x [0x50]=%04x/%04x/%04x [0xA0]=%04x/%04x/%04x\n",
           (unsigned long) cmap->mid,
           cmap->mid == cmap->pScreen->defColormap ? " (default)" : "",
           cmap->red[0x10].co.local.red, cmap->red[0x10].co.local.green,
           cmap->red[0x10].co.local.blue,
           cmap->red[0x50].co.local.red, cmap->red[0x50].co.local.green,
           cmap->red[0x50].co.local.blue,
           cmap->red[0xA0].co.local.red, cmap->red[0xA0].co.local.green,
           cmap->red[0xA0].co.local.blue);
}

static void
hbLoadPalette(ScreenPtr pScreen)
{
    HbScreenRec *scr = hbScreenOf(pScreen);
    ColormapPtr cmap;
    int i, n, direct;

    if (!scr)
        return;
    cmap = GetInstalledmiColormap(pScreen);
    if (!cmap)
        return;
    hbLogColormap(scr, cmap);
    n = cmap->pVisual ? cmap->pVisual->ColormapEntries : 256;
    if (n > 256)
        n = 256;
    /* PseudoColor keeps all three components in the red table's LOCO cells
     * (green/blue are only allocated for DirectColor, where each table holds
     * its own channel); reading cmap->green here would dereference NULL. */
    direct = cmap->pVisual &&
             (cmap->pVisual->class | DynamicClass) == DirectColor;
    for (i = 0; i < n; i++) {
        unsigned int r, g, b;

        if (direct) {
            r = cmap->red[i].co.local.red;
            g = cmap->green[i].co.local.green;
            b = cmap->blue[i].co.local.blue;
        }
        else {
            r = cmap->red[i].co.local.red;
            g = cmap->red[i].co.local.green;
            b = cmap->red[i].co.local.blue;
        }
        scr->palette[i] = ((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8);
    }
    for (; i < 256; i++)
        scr->palette[i] = 0;
}

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

/* Present the WHOLE shadow of one screen (arbitration transitions use this:
 * the incoming owner has to cover whatever the outgoing owner left behind).
 * Depth 8 clears the rest of the framebuffer first, so the game is a clean
 * 640x400 (or 320x200) picture on black rather than on a frozen desktop. */
static void
hbPresentFull(ScreenPtr pScreen)
{
    HbScreenRec *scr = hbScreenOf(pScreen);
    int x, y, w, h;

    if (!scr || !scr->fb || !scr->shadow)
        return;
    w = scr->width < scr->fbWidth ? scr->width : scr->fbWidth;
    h = scr->height < scr->fbHeight ? scr->height : scr->fbHeight;
    if (scr->depth == 8) {
        hbLoadPalette(pScreen);
        for (y = 0; y < scr->fbHeight; y++)
            memset(scr->fb + (size_t) y * scr->fbStride, 0,
                   (size_t) scr->fbWidth * 4);
    }
    for (y = 0; y < h; y++) {
        uint32_t *dst = scr->fb + (size_t) y * scr->fbStride;
        const unsigned char *src = (const unsigned char *) scr->shadow +
                                   (size_t) y * scr->shadowStride;

        if (scr->depth == 8) {
            for (x = 0; x < w; x++)
                dst[x] = scr->palette[src[x]];
        }
        else
            memcpy(dst, src, (size_t) w * 4);
    }
    flush_fb();
    hbFlushCount++;
}

static void
hbShadowUpdate(ScreenPtr pScreen, shadowBufPtr pBuf)
{
    HbScreenRec *scr = hbScreenOf(pScreen);
    RegionPtr damage = DamageRegion(pBuf->pDamage);
    int nbox = RegionNumRects(damage);
    BoxPtr box = RegionRects(damage);
    PixmapPtr pix = pBuf->pPixmap;
    const unsigned char *src = pix->devPrivate.ptr;
    int srcStride = pix->devKind;
    struct hb_fb_rect32 rects[HB_FLUSH_MAX_RECTS];
    int n = 0, i;

    if (!scr || !scr->fb || nbox <= 0)
        return;
    /* Only the current owner is on the glass.  The damage region is emptied by
     * shadowRedisplay() after this returns, so dropping a frame here consumes
     * its damage instead of accumulating it. */
    if (hbOwner != scr->screenNum)
        return;
    if (scr->depth == 8)
        hbLoadPalette(pScreen);

    /* L89: price the present.  The shadow->fb copy is cheap per byte; what
     * is not cheap is the number of damage boxes, because each one is a row
     * loop and every 32 of them is another SYS_FLUSH_FB_RECTS batch, and the
     * kernel turns each rect into a synchronous TRANSFER_TO_HOST_2D +
     * RESOURCE_FLUSH.  HB_PRESPROF=1 in the server's environment prints one
     * [HBX] row per 16 presents: mean ms, mean damage boxes, mean pixels
     * copied and the batches issued, which is the split between "the copy"
     * and "the present commands". */
    static int presprof = -1;
    static unsigned long long hbx_n, hbx_ms, hbx_boxes, hbx_px, hbx_batches;
    int hbx_t0 = 0;
    unsigned long long hbx_px_frame = 0;
    unsigned hbx_batches_frame = 0;
    if (presprof < 0) {
        const char *e = getenv("HB_PRESPROF");
        presprof = (e && e[0] == '1') ? 1 : 0;
    }
    if (presprof)
        hbx_t0 = hbNowMs();

    /* Many tiny boxes: present their extents instead (one copy, one rect). */
    if (nbox > 4 * HB_FLUSH_MAX_RECTS) {
        box = RegionExtents(damage);
        nbox = 1;
    }
    for (i = 0; i < nbox; i++, box++) {
        int x1 = box->x1 < 0 ? 0 : box->x1;
        int y1 = box->y1 < 0 ? 0 : box->y1;
        int x2 = box->x2 > scr->fbWidth ? scr->fbWidth : box->x2;
        int y2 = box->y2 > scr->fbHeight ? scr->fbHeight : box->y2;
        int y;

        if (x2 > scr->width) x2 = scr->width;
        if (y2 > scr->height) y2 = scr->height;
        if (x1 >= x2 || y1 >= y2)
            continue;
        hbx_px_frame += (unsigned long long) (x2 - x1) * (unsigned long long) (y2 - y1);
        for (y = y1; y < y2; y++) {
            uint32_t *dst = scr->fb + (size_t) y * scr->fbStride + x1;

            if (scr->depth == 8) {
                /* one byte per pixel: expand the index through the palette */
                const unsigned char *src8 = src + (size_t) y * srcStride + x1;
                int npix = x2 - x1;

                while (npix-- > 0)
                    *dst++ = scr->palette[*src8++];
            }
            else
                memcpy(dst, src + (size_t) y * srcStride + (size_t) x1 * 4,
                       (size_t) (x2 - x1) * 4);
        }
        rects[n].x = x1;
        rects[n].y = y1;
        rects[n].w = x2 - x1;
        rects[n].h = y2 - y1;
        hbx_batches_frame++;
        if (++n == HB_FLUSH_MAX_RECTS) {
            hbFlushBatch(rects, n);
            n = 0;
        }
    }
    hbFlushBatch(rects, n);

    if (presprof) {
        hbx_n++;
        hbx_ms += (unsigned long long) (hbNowMs() - hbx_t0);
        hbx_boxes += (unsigned long long) nbox;
        hbx_px += hbx_px_frame;
        hbx_batches += hbx_batches_frame;
        if (hbx_n >= 16) {
            fprintf(stderr, "[HBX] upd n=%llu ms=%llu boxes=%llu px=%llu "
                    "batches=%llu\n",
                    hbx_n, hbx_ms / hbx_n, hbx_boxes / hbx_n, hbx_px / hbx_n,
                    hbx_batches / hbx_n);
            hbx_n = hbx_ms = hbx_boxes = hbx_px = hbx_batches = 0;
        }
    }
}

static Bool
hbCreateScreenResources(ScreenPtr pScreen)
{
    HbScreenRec *scr = hbScreenOf(pScreen);
    Bool ret;

    if (!scr)
        return FALSE;
    pScreen->CreateScreenResources = scr->createScreenResources;
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
    HbScreenRec *scr = hbScreenOf(pScreen);

    if (!scr)
        return FALSE;
    pScreen->CloseScreen = scr->closeScreen;
    /* shadowCloseScreen (wrapped below us) unregisters the shadow damage
     * from the screen pixmap; do it while that pixmap is still alive, or
     * DamageUnregister walks freed memory on every server exit (SIGTERM at
     * session teardown crashed in shadowRemove). */
    if (pScreen->devPrivate)
        shadowRemove(pScreen, pScreen->devPrivate);
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

/* ---- depth 8: install a mapped window's colormap ----------------------- */
/* X has no server-side "install the window's colormap when it is mapped":
 * the client is expected to call XInstallColormap, or a window manager does
 * it for the focused window.  Doom 1.10 does neither -- it creates a private
 * AllocAll colormap, sets it as the window's colormap attribute and only
 * calls XStoreColors (i_video.c UploadNewPalette) -- and the no-WM session
 * (XSESS_NO_WM=1) has no window manager to install it either.  Measured
 * before this hook: the server kept expanding every frame through the
 * DEFAULT 256-cell map (logged as "colormap 0x20 (default)"), so a Doom
 * frame came out as a handful of 0x55/0xAA colours.  Depth 8 therefore
 * installs a private window colormap when the window is realized. */
static Bool (*hbRealizeWindowOrig) (WindowPtr pWin);

static void
hbInstallWindowColormap(WindowPtr pWin)
{
    HbScreenRec *scr = hbScreenOf(pWin->drawable.pScreen);
    ColormapPtr cmap = NULL;
    Colormap mid;
    ScreenPtr pScreen = pWin->drawable.pScreen;

    if (!scr || scr->depth != 8 || !pWin->optional)
        return;
    mid = pWin->optional->colormap;
    if (mid == None || mid == pScreen->defColormap)
        return;
    if (dixLookupResourceByType((void **) &cmap, mid, RT_COLORMAP,
                                serverClient, DixUseAccess) != Success || !cmap)
        return;
    if (cmap != GetInstalledmiColormap(pScreen))
        (*pScreen->InstallColormap) (cmap);
}

static Bool
hbRealizeWindow(WindowPtr pWin)
{
    Bool ret = (*hbRealizeWindowOrig) (pWin);

    if (ret && pWin->viewable)
        hbInstallWindowColormap(pWin);
    return ret;
}

/* ---- second screen: pointer + keyboard hand-over ----------------------- */
/* A screen-1 window is managed by nobody: it is a child of :0.1's root and no
 * window manager watches that root.  Focus therefore has to be arranged here.
 *
 * Pointer: with PointerRoot focus keys go to the window under the pointer, and
 * a pointer parked on the desktop is never over the game's 320x200 window, so
 * the game receives nothing.  Warp onto the window's centre.
 *
 * miPointerWarpCursor alone is NOT enough: it updates the pointer device's
 * position but does not go through dix's motion path, so pSprite->hot and
 * pSprite->win keep naming the OLD screen's window.  With PointerRoot focus
 * dix then delivers keys to that stale window -- measured: the pointer warp
 * logged "screen 1 at 160,100" and Doom still received nothing.  So every warp
 * here is followed by a real motion event at the same position (the call the
 * evdev driver makes for a mouse move), which runs SetSpritePosition and
 * CheckMotion and repoints the sprite window (and sends the Enter/Leave pair).
 *
 * Keyboard: with IceWM running on :0.0 the focus is a screen-0 frame, not
 * PointerRoot; no amount of pointer warping puts a key into the game.  When the
 * focus is a window on the desktop screen (or None, where nothing receives
 * keys at all) it is handed to the game's window with RevertToPointerRoot, and
 * released to PointerRoot again when the game window goes away (IceWM's
 * click-to-focus takes it back on the next click).  A PointerRoot focus is left
 * alone -- the warp already routes keys to the game. */
static void
hbQueueMotion(ScreenPtr pScreen, int x, int y)
{
    ValuatorMask mask;

    if (!inputInfo.pointer)
        return;
    valuator_mask_zero(&mask);
    valuator_mask_set(&mask, 0, x);
    valuator_mask_set(&mask, 1, y);
    QueuePointerEvents(inputInfo.pointer, MotionNotify, 0,
                       POINTER_ABSOLUTE | POINTER_SCREEN, &mask);
}

static void
hbPointerToWindow(WindowPtr pWin)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    int x = pWin->drawable.x + pWin->drawable.width / 2;
    int y = pWin->drawable.y + pWin->drawable.height / 2;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= pScreen->width) x = pScreen->width - 1;
    if (y >= pScreen->height) y = pScreen->height - 1;
    if (!inputInfo.pointer)
        return;
    miPointerWarpCursor(inputInfo.pointer, pScreen, x, y);
    hbQueueMotion(pScreen, x, y);
    ErrorF("hobbyos: pointer -> screen %d at %d,%d\n",
           pScreen->myNum, x, y);
}

static void
hbPointerToScreen(ScreenPtr pScreen)
{
    int x, y;

    if (!inputInfo.pointer || !pScreen)
        return;
    x = pScreen->width / 2;
    y = pScreen->height / 2;
    miPointerWarpCursor(inputInfo.pointer, pScreen, x, y);
    hbQueueMotion(pScreen, x, y);
    ErrorF("hobbyos: pointer -> screen %d at %d,%d\n", pScreen->myNum, x, y);
}

static const char *
hbFocusName(WindowPtr w, char *buf, int size)
{
    if (w == PointerRootWin)
        return "PointerRoot";
    if (w == NoneWin || !w)
        return "None";
    snprintf(buf, size, "window 0x%lx screen %d", (unsigned long) w->drawable.id,
             w->drawable.pScreen->myNum);
    return buf;
}

static void
hbKeyboardToWindow(WindowPtr pWin)
{
    DeviceIntPtr kbd = inputInfo.keyboard;
    WindowPtr cur;
    char buf[64];

    if (!kbd || !kbd->focus)
        return;
    cur = kbd->focus->win;
    ErrorF("hobbyos: keyboard focus was %s\n", hbFocusName(cur, buf, sizeof buf));
    if (cur == PointerRootWin)
        return;                 /* the warp + motion already routes keys here */
    if (cur == pWin)
        return;                 /* already focused */
    if (cur && cur != NoneWin && cur->drawable.pScreen == pWin->drawable.pScreen)
        return;                 /* focus is already on the game's screen */
    /* A real window on the desktop screen (IceWM's frame) or None: hand the
     * keyboard to the game.  Note that a window manager re-asserts its own
     * focus within a second of the FocusOut this produces (measured: the focus
     * moved 0x800003 -> 0x5c -> 0x200002 and the game stopped receiving keys),
     * which is why PointerRoot -- where no client owns the focus and the
     * sprite window decides -- is the state to prefer, not to replace. */
    if (SetInputFocus(serverClient, kbd, pWin->drawable.id,
                      RevertToPointerRoot, CurrentTime, TRUE) == Success) {
        hbFocusStolen = TRUE;
        ErrorF("hobbyos: keyboard focus -> screen %d window 0x%lx\n",
               pWin->drawable.pScreen->myNum, (unsigned long) pWin->drawable.id);
    }
}

static void
hbKeyboardRelease(void)
{
    if (!hbFocusStolen || !inputInfo.keyboard)
        return;
    hbFocusStolen = FALSE;
    if (SetInputFocus(serverClient, inputInfo.keyboard, PointerRoot,
                      RevertToPointerRoot, CurrentTime, TRUE) == Success)
        ErrorF("hobbyos: keyboard focus -> PointerRoot\n");
}

/* ---- second screen: the keyboard grab ------------------------------- */
/* A window manager on :0.0 keeps the focus for itself: IceWM runs a
 * pointer-focus policy, so the moment the pointer leaves its windows (our warp
 * to :0.1) it puts the focus back on its own frame -- measured: PointerRoot at
 * the hand-over, then "window 0x5c screen 0" two seconds later, and the game
 * received nothing even though the sprite window was its own.  A WM only
 * manages :0.0's root, so nothing can be done with focus: the game has to own
 * the keyboard outright.  GrabDevice on behalf of the CLIENT that owns the
 * game window gives it exactly that (the same state XGrabKeyboard would
 * create, including the automatic release when the client disconnects), and
 * the desktop's keyboard comes back with UngrabDevice when the window goes. */
static ClientPtr hbGrabClient;

static void
hbGrabKeyboard(WindowPtr pWin)
{
    GrabMask mask;
    CARD8 status = GrabSuccess;
    ClientPtr client = wClient(pWin);

    if (hbGrabClient || !client || !inputInfo.keyboard)
        return;
    memset(&mask, 0, sizeof mask);
    mask.core = KeyPressMask | KeyReleaseMask;
    if (GrabDevice(client, inputInfo.keyboard, GrabModeAsync, GrabModeAsync,
                   pWin->drawable.id, TRUE, CurrentTime, &mask, CORE,
                   None, None, &status) != Success || status != GrabSuccess) {
        ErrorF("hobbyos: keyboard grab on screen %d failed (status %d)\n",
               pWin->drawable.pScreen->myNum, status);
        return;
    }
    hbGrabClient = client;
    ErrorF("hobbyos: keyboard grab -> screen %d window 0x%lx\n",
           pWin->drawable.pScreen->myNum, (unsigned long) pWin->drawable.id);
}

static void
hbUngrabKeyboard(void)
{
    GrabPtr grab;

    if (!hbGrabClient)
        return;
    hbGrabClient = NULL;
    grab = inputInfo.keyboard ? inputInfo.keyboard->deviceGrab.grab : NULL;
    if (!grab) {
        /* The client disconnected: dix freed its grab with the client, and
         * DeactivateKeyboardGrab() dereferences the grab unconditionally
         * (grab->window in DoFocusEvents) -- calling it here killed the server
         * with a fault at VA=0x18 right after the game exited. */
        ErrorF("hobbyos: keyboard grab already released by the client\n");
        return;
    }
    DeactivateKeyboardGrab(inputInfo.keyboard);
    ErrorF("hobbyos: keyboard ungrab\n");
}

/* dix's WakeupHandler calls the registered function unconditionally, so the
 * wakeup side must be a real (empty) function, not NULL. */
static void
hbOwnershipWakeup(void *blockData, int result)
{
}

/* The screen-1 child that owns the framebuffer: any viewable top-level window
 * on :0.1.  Walking firstChild/nextSib (not just firstChild) also handles a
 * second client that mapped after the first one. */
static WindowPtr
hbViewableChild(ScreenPtr pScreen)
{
    WindowPtr w;

    if (!pScreen || !pScreen->root)
        return NULL;
    for (w = pScreen->root->firstChild; w; w = w->nextSib)
        if (w->viewable)
            return w;
    return NULL;
}

/* Registered as a server block handler by InitOutput: runs once per server
 * loop pass, before the per-screen shadow block handlers.  Two state
 * transitions to notice: a screen-1 window appeared (game takes the glass) or
 * the last one went away (desktop takes it back, in full). */
static void
hbOwnershipPoll(void *blockData, void *timeout)
{
    ScreenPtr pDesktop, pGame;
    WindowPtr pWin;

    if (screenInfo.numScreens < 2)
        return;
    pDesktop = screenInfo.screens[0];
    pGame = screenInfo.screens[1];
    pWin = hbViewableChild(pGame);

    if (pWin && !hbSecondActive) {
        hbSecondActive = TRUE;
        hbOwner = pGame->myNum;
        hbPresentFull(pGame);
        ErrorF("hobbyos: present -> screen %d\n", pGame->myNum);
        hbPointerToWindow(pWin);
        hbKeyboardToWindow(pWin);
        hbGrabKeyboard(pWin);
    }
    else if (!pWin && hbSecondActive) {
        hbSecondActive = FALSE;
        hbOwner = pDesktop->myNum;
        hbPresentFull(pDesktop);
        ErrorF("hobbyos: present -> screen %d\n", pDesktop->myNum);
        hbUngrabKeyboard();
        hbKeyboardRelease();
        hbPointerToScreen(pDesktop);
    }
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
    return FALSE;               /* screens are switched by hbOwnershipPoll */
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
/* HB_XDEPTH=8 selects a depth 8 PseudoColor screen (what Doom 1.10 and other
 * 1990s X clients require); anything else, unset included, keeps the depth 24
 * TrueColor screen.  It is read from the environment rather than the command
 * line so XSESSION.BIN's /X11/XSESSION.ENV rows can switch it per disk image. */
static int
hbDepthFromEnv(void)
{
    const char *s = getenv("HB_XDEPTH");
    int d;

    if (!s || !*s)
        return 24;
    d = atoi(s);
    if (d == 8)
        return 8;
    if (d != 24)
        ErrorF("hobbyos: HB_XDEPTH=%s unsupported (8 or 24), using 24\n", s);
    return 24;
}

/* HB_SECOND_SCREEN=1 forces the depth-8 :0.1 screen even when :0.0 is already
 * depth 8 (two answers to XMatchVisualInfo; harmless, and useful for probing
 * the second-screen path without a 24-bit desktop). */
static Bool
hbSecondScreenFromEnv(void)
{
    const char *s = getenv("HB_SECOND_SCREEN");

    return s && *s && *s != '0';
}

static Bool
hbScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
    HbScreenRec *scr = hbScreenOf(pScreen);
    int dpi = monitorResolution ? monitorResolution : 96;
    void *fbmap;

    if (!scr)
        return FALSE;
    scr->screenNum = pScreen->myNum;
    /* Screen 0 is the desktop (and honours HB_XDEPTH as it always has);
     * screen 1 is always the depth-8 client screen. */
    scr->depth = pScreen->myNum == 0 ? hbDepthFromEnv() : 8;
    hbQueryFb(scr);
    if (scr->depth == 8) {
        /* The classic 640x400 8-bit screen (Xvfb's "-screen 0 640x400x8"),
         * presented 1:1 at the framebuffer's top-left corner. */
        scr->width = hbReqWidth ? hbReqWidth : 640;
        scr->height = hbReqHeight ? hbReqHeight : 400;
    }
    else {
        scr->width = hbReqWidth ? hbReqWidth : scr->fbWidth;
        scr->height = hbReqHeight ? hbReqHeight : scr->fbHeight;
    }

    fbmap = map_fb();
    if (fbmap == NULL || fbmap == (void *) -1L ||
        (intptr_t) fbmap < 0 && (intptr_t) fbmap > -4096) {
        ErrorF("hobbyos: SYS_MAP_FB failed (%p)\n", fbmap);
        return FALSE;
    }
    scr->fb = fbmap;
    scr->shadowStride = PixmapBytePad(scr->width, scr->depth);
    scr->shadow = calloc((size_t) scr->shadowStride, scr->height);
    if (!scr->shadow)
        return FALSE;

    /* miSetVisualTypesAndMasks/miSetPixmapDepths feed a global list that
     * InitVisuals CONSUMES (and clears) for the screen being initialised, so
     * each screen declares its own visual set here: depth 24 TrueColor for the
     * desktop, depth 8 PseudoColor only for :0.1 (a client that asks for 8-bit
     * finds exactly one visual, and there is no 24-bit visual to refuse it). */
    if (scr->depth == 8) {
        if (!miSetVisualTypesAndMasks(8, (1 << PseudoColor), 8, PseudoColor,
                                      0, 0, 0))
            return FALSE;
    }
    else if (!miSetVisualTypesAndMasks(24, (1 << TrueColor), 8, TrueColor,
                                       0xff0000, 0x00ff00, 0x0000ff))
        return FALSE;
    miSetPixmapDepths();

    /* fb's "width" is the frame buffer's pixel width; it derives devKind as
     * PixmapBytePad(width, root depth), which is exactly shadowStride.  The
     * bpp argument is the SCREEN's bits per pixel, not the depth: depth 24
     * runs at 32 bpp (B8G8R8A8), and fbScreenInit rejects a non-power-of-two
     * bpp (Ones(24) != 1) -- passing scr->depth here is a fatal error. */
    if (!fbScreenInit(pScreen, scr->shadow, scr->width,
                      scr->height, dpi, dpi,
                      scr->width, scr->depth == 8 ? 8 : 32))
        return FALSE;
    if (hbRender && !fbPictureInit(pScreen, 0, 0))
        return FALSE;
    if (scr->depth == 8) {
        hbRealizeWindowOrig = pScreen->RealizeWindow;
        pScreen->RealizeWindow = hbRealizeWindow;
    }
    if (!shadowSetup(pScreen))
        return FALSE;
    if (!hbRandRInit(pScreen))
        return FALSE;

    /* Pixel values, not colours: depth 8 wants palette indexes (the cursor
     * and the root weave draw with these). */
    pScreen->blackPixel = 0;
    pScreen->whitePixel = scr->depth == 8 ? 0xff : 0xffffff;

    miDCInitialize(pScreen, &hbPointerCursorFuncs);

    pScreen->SaveScreen = hbSaveScreen;

    scr->createScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = hbCreateScreenResources;
    scr->closeScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = hbCloseScreen;

    if (!fbCreateDefColormap(pScreen))
        return FALSE;
    if (scr->depth == 8) {
        /* Unconditional (ErrorF, not LogMessage): these lines are the
         * harness's markers that the 8-bit path, not the 24-bit one, is what
         * came up.  Screen 0 keeps the single-screen HB_XDEPTH=8 wording the
         * XDEPTH8 probe greps for; screen 1 announces itself as :0.1. */
        if (pScreen->myNum == 0)
            ErrorF("hobbyos: screen %dx%d depth 8 PseudoColor (fb %dx%d pitch"
                   " %d px) -- HB_XDEPTH=8\n", scr->width, scr->height,
                   scr->fbWidth, scr->fbHeight, scr->fbStride);
        else
            ErrorF("hobbyos: screen %d depth 8 PseudoColor (:0.%d) %dx%d"
                   " (fb %dx%d pitch %d px)\n", pScreen->myNum, pScreen->myNum,
                   scr->width, scr->height, scr->fbWidth, scr->fbHeight,
                   scr->fbStride);
    }
    LogMessage(X_INFO, "hobbyos: screen %d %dx%d depth %d (fb %dx%d pitch %d px)\n",
               pScreen->myNum, scr->width, scr->height, scr->depth,
               scr->fbWidth, scr->fbHeight, scr->fbStride);
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

    /* The desktop first, so it is always :0.0 and keeps its depth. */
    if (AddScreen(hbScreenInit, argc, argv) == -1)
        FatalError("hobbyos: couldn't add screen\n");

    /* Then the depth-8 client screen (:0.1).  A failure here is NOT fatal:
     * the session keeps the desktop and the launcher falls back to :0.0,
     * where Doom prints its own one-line diagnosis. */
    if (hbScreens[0].depth != 8 || hbSecondScreenFromEnv()) {
        if (AddScreen(hbScreenInit, argc, argv) == -1)
            ErrorF("hobbyos: screen 1 (depth 8 PseudoColor :0.1) not added\n");
        else
            ErrorF("hobbyos: two screens: :0.0 depth %d, :0.1 depth 8"
                   " PseudoColor\n", hbScreens[0].depth);
    }

    /* Presentation arbitration + pointer/keyboard hand-over.  A registered
     * block handler runs before the per-screen shadow block handlers, so a
     * screen switch is decided before the shadows present in the same pass. */
    RegisterBlockAndWakeupHandlers(hbOwnershipPoll, hbOwnershipWakeup, NULL);
}

/* ---- DDX hooks --------------------------------------------------------- */
void
ddxGiveUp(enum ExitCode error)
{
    int i;

    for (i = 0; i < HB_MAX_SCREENS; i++) {
        free(hbScreens[i].shadow);
        hbScreens[i].shadow = NULL;
    }
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
    ErrorF("-screen WxH            screen size (default: the framebuffer's for\n");
    ErrorF("                       :0.0, 640x400 for the depth-8 :0.1)\n");
    ErrorF("HB_XDEPTH=8            environment: make :0.0 a whole-screen depth 8\n");
    ErrorF("                       PseudoColor screen (no :0.1)\n");
    ErrorF("HB_SECOND_SCREEN=1     environment: always add the depth-8 :0.1\n");
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
    return hbScreens[0].width;
}

int
hbScreenHeight(void)
{
    return hbScreens[0].height;
}
