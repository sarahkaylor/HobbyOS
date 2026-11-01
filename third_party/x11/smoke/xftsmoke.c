/* xftsmoke.c -- M2 extlibs link-model smoke client (icewm.md §3 M2).
 *
 * Opens the display, opens "DejaVu Sans-10" through Xft/fontconfig, draws a
 * UTF-8 string with XftDrawStringUtf8, builds a pixmap from an inline XPM,
 * and touches each extension library IceWM links (Xcursor, Xrandr,
 * Xinerama, Xcomposite, Xdamage) plus libjpeg, so the static closure is
 * proven.  Built into build/<arch>/smoke/XFTSMOKE.BIN (never onto a disk
 * image).  Exit codes: 0 ok, 1 no display, 2 no font, 3 xpm failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/xpm.h>
#include <X11/Xft/Xft.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <fontconfig/fontconfig.h>
#include <jpeglib.h>

static const char *smoke_xpm[] = {
    "8 8 2 1",
    "  c None",
    "x c #3060c0",
    "xxxxxxxx",
    "x      x",
    "x xxxx x",
    "x x  x x",
    "x x  x x",
    "x xxxx x",
    "x      x",
    "xxxxxxxx",
};

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Library-only checks that need no server. */
    if (!FcInit()) {
        fprintf(stderr, "xftsmoke: FcInit failed\n");
    } else {
        FcPattern *pat = FcNameParse((const FcChar8 *)"DejaVu Sans-10");
        FcResult r;
        FcPattern *m;
        FcConfigSubstitute(NULL, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        m = FcFontMatch(NULL, pat, &r);
        if (m) {
            FcChar8 *file = NULL;
            FcPatternGetString(m, FC_FILE, 0, &file);
            printf("xftsmoke: fontconfig match -> %s\n", file ? (const char *)file : "(none)");
            FcPatternDestroy(m);
        }
        FcPatternDestroy(pat);
    }
    {
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        printf("xftsmoke: libjpeg %d ok\n", JPEG_LIB_VERSION);
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xftsmoke: cannot open display\n");
        return 1;
    }
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    Visual *vis = DefaultVisual(dpy, scr);
    Colormap cmap = DefaultColormap(dpy, scr);

    int ev, er, maj = 0, min = 0;
    printf("xftsmoke: randr=%d xinerama=%d composite=%d damage=%d\n",
           XRRQueryExtension(dpy, &ev, &er), XineramaIsActive(dpy),
           XCompositeQueryExtension(dpy, &ev, &er), XDamageQueryExtension(dpy, &ev, &er));
    if (XRRQueryVersion(dpy, &maj, &min))
        printf("xftsmoke: randr %d.%d\n", maj, min);
    Cursor cur = XcursorLibraryLoadCursor(dpy, "left_ptr");

    Window w = XCreateSimpleWindow(dpy, root, 40, 40, 320, 80, 1,
                                   BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    if (cur)
        XDefineCursor(dpy, w, cur);
    XSelectInput(dpy, w, ExposureMask | KeyPressMask);
    XMapWindow(dpy, w);

    XftFont *font = XftFontOpenName(dpy, scr, "DejaVu Sans-10");
    if (!font) {
        fprintf(stderr, "xftsmoke: XftFontOpenName failed\n");
        XCloseDisplay(dpy);
        return 2;
    }
    XftDraw *draw = XftDrawCreate(dpy, w, vis, cmap);
    XftColor col;
    XftColorAllocName(dpy, vis, cmap, "#202020", &col);

    Pixmap pm = None, mask = None;
    XpmAttributes xa;
    memset(&xa, 0, sizeof xa);
    int xr = XpmCreatePixmapFromData(dpy, w, (char **)smoke_xpm, &pm, &mask, &xa);
    if (xr != XpmSuccess) {
        fprintf(stderr, "xftsmoke: XpmCreatePixmapFromData: %s\n", XpmGetErrorString(xr));
        return 3;
    }

    static const char msg[] = "Hello, HobbyOS \xe2\x80\x94 Xft \xc3\xa9\xc3\xa0\xc3\xbc";
    for (int frames = 0; frames < 1;) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == Expose) {
            GC gc = DefaultGC(dpy, scr);
            XCopyArea(dpy, pm, w, gc, 0, 0, 8, 8, 8, 8);
            XftDrawStringUtf8(draw, &col, font, 24, 40, (const FcChar8 *)msg, (int)strlen(msg));
            XFlush(dpy);
            frames++;
        } else if (e.type == KeyPress) {
            break;
        }
    }
    XftColorFree(dpy, vis, cmap, &col);
    XftDrawDestroy(draw);
    XftFontClose(dpy, font);
    XpmFreeAttributes(&xa);
    XCloseDisplay(dpy);
    printf("xftsmoke: ok\n");
    return 0;
}
