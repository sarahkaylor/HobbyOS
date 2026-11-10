/* src/user/x11/apps/xputbench/main.c — how long does one XPutImage take?
 *
 * The hosted RimWorld window reaches XHOBBY through one XPutImage per
 * presented frame (HBHOST blits the app's shared surface into its X window),
 * and L88 measured that put at 135 ms for a 1280x720 sub-rect of a 1920x1080
 * image.  The in-guest AF_UNIX wire moves the same 3.7 MB in 1-4.5 ms
 * (tools/gate linux_probe xwire), so the 135 ms is either the client library's
 * request construction, the server's PutImage, or the server's present --
 * and those need different fixes.  This client measures the pieces
 * separately, on a bare session with no window manager and no game:
 *
 *   [XPB] put-whole     XPutImage of a full 1280x720 ZPixmap, contiguous
 *   [XPB] put-subrect   XPutImage of 1280x720 from a 1920x1080 XImage at
 *                       offset (0,16) -- exactly HBHOST's call
 *   [XPB] put-scanline  XPutImage of 1280x1 from the same XImage
 *   [XPB] roundtrip     XNoOp + XSync: the cost of one round trip with no
 *                       payload, which bounds "the server is slow" from below
 *
 * Each case reports both what the call costs (the client, which blocks when
 * the socket ring fills) and what the following XSync costs (the rest of the
 * server's work).  If `put` is large and `sync` is small the client library is
 * doing the work; if `put` is small and `sync` is large it is the server; if
 * both scale with the rect it is the bytes on the wire.
 *
 * Timing: the guest's CLOCK_MONOTONIC ticks at 1 ms, so each case runs until
 * it has accumulated at least TARGET_MS and reports us/iteration.
 *
 * Run: tools/rw/l89-xputbench.py (stages this BIN as /XSTART.TXT on a copy of
 * disk.img and boots the bare X session, XSESS_NO_WM=1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* sysinfo(1, ...) is the HobbyOS native millisecond tick. */
extern unsigned long long sysinfo(int cmd, unsigned long long arg, unsigned long long arg2);

#define TARGET_MS 400
#define WIN_W 1280
#define WIN_H 720
#define SURF_W 1920
#define SURF_H 1080

static Display *dpy;
static Window win;

static unsigned long long now_ms(void) { return sysinfo(1, 0, 0); }

/* One case: run `fn` until TARGET_MS have elapsed.  Reports the average cost
 * of the call and of the XSync that follows it, in microseconds. */
static void
case_run(const char *name, void (*fn)(void *), void *arg, int do_sync)
{
    unsigned long long t0, t_call = 0, t_sync = 0;
    unsigned long n = 0, c0, c1, csync;

    /* warm up: one iteration outside the window (map/expose work happens
     * on the first put, and must not be part of the average). */
    fn(arg);
    if (do_sync) XSync(dpy, False);

    t0 = now_ms();
    while (now_ms() - t0 < TARGET_MS || n == 0) {
        c0 = now_ms();
        fn(arg);
        c1 = now_ms();
        t_call += c1 - c0;
        csync = c1;
        if (do_sync) {
            XSync(dpy, False);
            csync = now_ms();
            t_sync += csync - c1;
        }
        n++;
        if (n <= 6)
            printf("[XPB]   it %s #%lu call=%llu us sync=%llu us\n",
                   name, n - 1, (c1 - c0) * 1000ull, (csync - c1) * 1000ull);
        if (n > 200000) break;
    }
    printf("[XPB] %-14s n=%lu  put=%llu us  sync=%llu us\n",
           name, n, t_call * 1000ull / n, t_sync * 1000ull / n);
    fflush(stdout);
}

struct putarg { Drawable d; XImage *img; int sx, sy, w, h; };

static void put_it(void *p) {
    struct putarg *a = (struct putarg *)p;
    XPutImage(dpy, a->d, DefaultGC(dpy, 0), a->img, a->sx, a->sy,
              a->sx, a->sy, (unsigned)a->w, (unsigned)a->h);
}

struct copyarg { Drawable src, dst; int w, h; };

static void copy_it(void *p) {
    struct copyarg *a = (struct copyarg *)p;
    XCopyArea(dpy, a->src, a->dst, DefaultGC(dpy, 0), 0, 0,
              (unsigned)a->w, (unsigned)a->h, 0, 0);
}

static void noop_it(void *p) { (void)p; XNoOp(dpy); }

int main(void) {
    int screen, depth, rc = 0;
    XImage *whole, *sub;
    unsigned *buf;
    struct putarg a;

    dpy = XOpenDisplay(NULL);
    if (!dpy) { printf("[XPB] FAIL XOpenDisplay\n"); return 1; }
    screen = DefaultScreen(dpy);
    depth = DefaultDepth(dpy, screen);
    printf("[XPB] display %s: screen %d %dx%d depth %d\n",
           DisplayString(dpy), screen, DisplayWidth(dpy, screen),
           DisplayHeight(dpy, screen), depth);
    printf("[XPB] maxreq=%ld extmaxreq=%ld\n",
           XMaxRequestSize(dpy), XExtendedMaxRequestSize(dpy));

    /* L89: the ring experiment.  XPB_SNDBUF=<bytes> asks the kernel for a send
     * ring that big on this connection (grow-only, capped); unset leaves the
     * 512 KiB default.  Same binary, same case list, so the A/B is one env
     * variable -- that is the whole point of the lane. */
    {
        const char *e = getenv("XPB_SNDBUF");
        int want = e ? atoi(e) : 0;
        int fd = ConnectionNumber(dpy);
        int got = 0, glen = (int)sizeof got;
        if (want > 0)
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &want, (int)sizeof want);
        if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &got, &glen) != 0) got = -1;
        printf("[XPB] xring fd=%d want=%d got=%d\n", fd, want, got);
    }

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, WIN_W, WIN_H,
                              0, 0, 0);
    XMapWindow(dpy, win);
    XSync(dpy, False);

    /* contiguous 1280x720 image */
    whole = XCreateImage(dpy, DefaultVisual(dpy, screen), depth, ZPixmap, 0,
                         NULL, WIN_W, WIN_H, 32, 0);
    if (!whole) { printf("[XPB] FAIL XCreateImage whole\n"); return 1; }
    whole->data = malloc((size_t)whole->bytes_per_line * WIN_H);
    memset(whole->data, 0x40, (size_t)whole->bytes_per_line * WIN_H);

    /* 1920x1080 image, so a 1280x720 put is a strided sub-rect (HBHOST's) */
    sub = XCreateImage(dpy, DefaultVisual(dpy, screen), depth, ZPixmap, 0,
                       NULL, SURF_W, SURF_H, 32, 0);
    if (!sub) { printf("[XPB] FAIL XCreateImage sub\n"); return 1; }
    buf = malloc((size_t)sub->bytes_per_line * SURF_H);
    memset(buf, 0x80, (size_t)sub->bytes_per_line * SURF_H);
    sub->data = (char *)buf;

    XSync(dpy, False);

    a.d = win; a.img = whole; a.sx = 0; a.sy = 0; a.w = WIN_W; a.h = WIN_H;
    printf("[XPB] whole bytes_per_line=%d\n", whole->bytes_per_line);
    case_run("put-whole", put_it, &a, 1);

    a.d = win; a.img = sub; a.sx = 0; a.sy = 16; a.w = WIN_W; a.h = WIN_H;
    printf("[XPB] subrect bytes_per_line=%d (stride %d)\n",
           sub->bytes_per_line, SURF_W * 4);
    case_run("put-subrect", put_it, &a, 1);

    a.d = win; a.img = sub; a.sx = 0; a.sy = 16; a.w = WIN_W; a.h = 1;
    case_run("put-scanline", put_it, &a, 1);

    case_run("roundtrip", noop_it, NULL, 1);

    /* Same payload into a PIXMAP: the server does the same fb copy and the
     * same request read, but nothing is presented (no shadow->fb, no
     * SYS_FLUSH_FB_RECTS).  If this is fast, the present is the cost; if it
     * is also ~113 ms, the request/transport is. */
    {
        Pixmap pm = XCreatePixmap(dpy, win, WIN_W, WIN_H, depth);
        struct putarg pa = { pm, sub, 0, 16, WIN_W, WIN_H };
        XSync(dpy, False);
        case_run("put-pixmap", put_it, &pa, 1);

        /* And the present alone: copy the pixmap to the window. */
        struct copyarg ca = { pm, win, WIN_W, WIN_H };
        XSync(dpy, False);
        case_run("copy-present", copy_it, &ca, 1);
    }

    /* Is the put itself asynchronous?  Issue N puts with no sync and time
     * only the last sync: if that is still ~N x the cost above, the server is
     * the one doing the work and the client only queues. */
    {
        unsigned long long t0, t1, t2;
        struct putarg b = { win, sub, 0, 16, WIN_W, WIN_H };
        int n = 4;
        XSync(dpy, False);
        t0 = now_ms();
        for (int i = 0; i < n; i++) put_it(&b);
        t1 = now_ms();
        XSync(dpy, False);
        t2 = now_ms();
        printf("[XPB] queued %d puts: issue=%llu us (%.0f us/put) "
               "sync=%llu us (%.0f us/put)\n", n,
               (t1 - t0) * 1000ull, (double)(t1 - t0) * 1000.0 / n,
               (t2 - t1) * 1000ull, (double)(t2 - t1) * 1000.0 / n);
    }

    printf("[XPB] done rc=%d\n", rc);
    fflush(stdout);
    return rc;
}
