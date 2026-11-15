/*
 * tools/cp/x11/xsmoke.c — lane C4-x in-OS smoke (docs/cp-x11.md §4).
 *
 * An ORDINARY glibc 2.31 x86_64 program linked against the REAL X client
 * libraries built by tools/cp/x11/build.sh (libX11 1.8.14, libxcb 1.17.0, …),
 * found at run time through its own DT_RPATH /CP/X11/lib (never /LIB64, where
 * the RimWorld shim libX11.so.6 lives).  Inside HobbyOS it connects to
 * XHOBBY.BIN on :0, prints what the server offers (XListExtensions + the
 * version / capability queries Wine's winex11.drv and Mesa's X11 WSI make),
 * opens a window, draws, holds, and exits 0.
 *
 * All output goes to stderr, one "[XS] ..." row per line (XSESSION relays a
 * child's stderr to the serial console).  Exit: 0 ok, 1 no display, 2 the
 * window never got an Expose.
 */
#define _GNU_SOURCE 1
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/sync.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/xshmfence.h>
#include <xcb/xcb.h>
#include <xcb/present.h>
#include <xcb/dri3.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/shm.h>
#include <xcb/sync.h>

#include <dlfcn.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void say(const char *fmt, ...) {
  va_list ap;
  fputs("[XS] ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int g_xerrors;
static int on_xerror(Display *d, XErrorEvent *e) {
  char buf[128];
  XGetErrorText(d, e->error_code, buf, sizeof buf);
  say("X error: %s (code %d) request %d.%d", buf, e->error_code, e->request_code, e->minor_code);
  g_xerrors++;
  return 0;
}

static const char *lib_of(void *sym) {
  Dl_info di;
  if (sym && dladdr(sym, &di) && di.dli_fname) return di.dli_fname;
  return "?";
}

static void query_ext(Display *d, const char *name) {
  int op = 0, ev = 0, er = 0;
  Bool ok = XQueryExtension(d, name, &op, &ev, &er);
  say("ext %-22s %s opcode=%d event=%d error=%d", name, ok ? "PRESENT" : "absent ", op, ev, er);
}

static void probe_xi2(Display *d, Window root) {
  int op, ev, er, maj = 2, min = 4;
  if (!XQueryExtension(d, "XInputExtension", &op, &ev, &er)) { say("XI2: no XInputExtension"); return; }
  Status st = XIQueryVersion(d, &maj, &min);
  say("XI2: XIQueryVersion(2.4) -> %s, server %d.%d", st == Success ? "Success" : "BadRequest", maj, min);
  int ndev = 0;
  XIDeviceInfo *di = XIQueryDevice(d, XIAllDevices, &ndev);
  for (int i = 0; i < ndev; i++) {
    int nval = 0, rel = 0, abs_ = 0;
    for (int c = 0; c < di[i].num_classes; c++) {
      if (di[i].classes[c]->type == XIValuatorClass) {
        XIValuatorClassInfo *v = (XIValuatorClassInfo *)di[i].classes[c];
        nval++;
        if (v->mode == XIModeRelative) rel++; else abs_++;
      }
    }
    static const char *uses[] = {"?", "master-pointer", "master-keyboard", "slave-pointer",
                                 "slave-keyboard", "floating-slave"};
    say("XI2: device %d \"%s\" %s classes=%d valuators=%d (relative=%d absolute=%d)",
        di[i].deviceid, di[i].name, (di[i].use >= 1 && di[i].use <= 5) ? uses[di[i].use] : "?",
        di[i].num_classes, nval, rel, abs_);
  }
  if (di) XIFreeDeviceInfo(di);
  /* What winex11's raw-input path does: select XI_RawMotion on the root. */
  unsigned char m[XIMaskLen(XI_LASTEVENT)] = {0};
  XIEventMask em = {XIAllMasterDevices, sizeof m, m};
  XISetMask(m, XI_RawMotion);
  XISetMask(m, XI_RawButtonPress);
  int e0 = g_xerrors;
  XISelectEvents(d, root, &em, 1);
  XSync(d, False);
  say("XI2: XISelectEvents(root, RawMotion|RawButtonPress) -> %s", g_xerrors == e0 ? "accepted" : "X error");
}

static void probe_randr(Display *d, Window root) {
  int eb, erb, maj = 0, min = 0;
  if (!XRRQueryExtension(d, &eb, &erb) || !XRRQueryVersion(d, &maj, &min)) { say("RandR: absent"); return; }
  say("RandR: version %d.%d", maj, min);
  XRRScreenResources *r = XRRGetScreenResourcesCurrent(d, root);
  if (!r) { say("RandR: GetScreenResourcesCurrent failed"); return; }
  say("RandR: crtcs=%d outputs=%d modes=%d", r->ncrtc, r->noutput, r->nmode);
  for (int i = 0; i < r->nmode; i++)
    say("RandR: mode 0x%lx %s %ux%u", r->modes[i].id, r->modes[i].name, r->modes[i].width, r->modes[i].height);
  for (int i = 0; i < r->noutput; i++) {
    XRROutputInfo *o = XRRGetOutputInfo(d, r, r->outputs[i]);
    if (!o) continue;
    say("RandR: output %s connection=%s crtc=0x%lx nmode=%d mm=%lux%lu", o->name,
        o->connection == RR_Connected ? "connected" : "disconnected", o->crtc, o->nmode,
        o->mm_width, o->mm_height);
    XRRFreeOutputInfo(o);
  }
  for (int i = 0; i < r->ncrtc; i++) {
    XRRCrtcInfo *c = XRRGetCrtcInfo(d, r, r->crtcs[i]);
    if (!c) continue;
    say("RandR: crtc 0x%lx %dx%d+%d+%d mode=0x%lx gamma_size=%d", r->crtcs[i], c->width, c->height,
        c->x, c->y, c->mode, XRRGetCrtcGammaSize(d, r->crtcs[i]));
    XRRFreeCrtcInfo(c);
  }
  int nmon = 0;
  XRRMonitorInfo *mon = XRRGetMonitors(d, root, True, &nmon);
  say("RandR: monitors=%d", nmon);
  if (mon) XRRFreeMonitors(mon);
  XRRFreeScreenResources(r);
}

static void probe_misc(Display *d, int scr) {
  int eb, erb, maj = 0, min = 0;
  if (XRenderQueryExtension(d, &eb, &erb) && XRenderQueryVersion(d, &maj, &min)) {
    int nf = 0;
    while (XRenderFindFormat(d, 0, NULL, nf)) nf++;
    say("RENDER: version %d.%d, %d picture formats", maj, min, nf);
  } else say("RENDER: absent");
  if (XFixesQueryExtension(d, &eb, &erb) && XFixesQueryVersion(d, &maj, &min))
    say("XFIXES: version %d.%d", maj, min);
  else say("XFIXES: absent");
  say("Xcursor: XcursorSupportsARGB=%d XcursorSupportsAnim=%d", XcursorSupportsARGB(d), XcursorSupportsAnim(d));
  Bool pix = False;
  if (XShmQueryVersion(d, &maj, &min, &pix)) say("MIT-SHM: version %d.%d pixmaps=%d", maj, min, pix);
  else say("MIT-SHM: absent (XShmQueryVersion=False)");
  if (XSyncQueryExtension(d, &eb, &erb) && XSyncInitialize(d, &maj, &min)) {
    int nc = 0;
    XSyncSystemCounter *sc = XSyncListSystemCounters(d, &nc);
    say("SYNC: version %d.%d, %d system counters%s%s", maj, min, nc, nc ? ", first=" : "",
        nc ? sc[0].name : "");
    if (sc) XSyncFreeSystemCounterList(sc);
  } else say("SYNC: absent");
  if (XineramaQueryExtension(d, &eb, &erb)) {
    int n = 0;
    XineramaScreenInfo *xi = XineramaQueryScreens(d, &n);
    say("XINERAMA: present, active=%d screens=%d", XineramaIsActive(d), n);
    if (xi) XFree(xi);
  } else say("XINERAMA: absent");
  int xkbop, xkbev, xkber, xmaj = XkbMajorVersion, xmin = XkbMinorVersion;
  say("XKB: %s %d.%d", XkbQueryExtension(d, &xkbop, &xkbev, &xkber, &xmaj, &xmin) ? "present" : "absent",
      xmaj, xmin);
  (void)scr;
}

static void probe_xcb(Display *d) {
  xcb_connection_t *c = XGetXCBConnection(d);
  say("xcb: XGetXCBConnection=%p (libX11-xcb %s)", (void *)c, lib_of((void *)XGetXCBConnection));
  const xcb_query_extension_reply_t *e;
  e = xcb_get_extension_data(c, &xcb_present_id);
  if (e && e->present) {
    xcb_present_query_version_reply_t *r =
        xcb_present_query_version_reply(c, xcb_present_query_version(c, 1, 4), NULL);
    say("xcb Present: version %u.%u", r ? r->major_version : 0, r ? r->minor_version : 0);
    free(r);
  } else say("xcb Present: absent");
  e = xcb_get_extension_data(c, &xcb_dri3_id);
  if (e && e->present) {
    xcb_dri3_query_version_reply_t *r = xcb_dri3_query_version_reply(c, xcb_dri3_query_version(c, 1, 4), NULL);
    say("xcb DRI3: version %u.%u", r ? r->major_version : 0, r ? r->minor_version : 0);
    free(r);
  } else say("xcb DRI3: absent");
  e = xcb_get_extension_data(c, &xcb_shm_id);
  say("xcb MIT-SHM: %s", e && e->present ? "present" : "absent");
  e = xcb_get_extension_data(c, &xcb_composite_id);
  if (e && e->present) {
    xcb_composite_query_version_reply_t *r =
        xcb_composite_query_version_reply(c, xcb_composite_query_version(c, 0, 4), NULL);
    say("xcb Composite: version %u.%u", r ? r->major_version : 0, r ? r->minor_version : 0);
    free(r);
  } else say("xcb Composite: absent");
  e = xcb_get_extension_data(c, &xcb_damage_id);
  if (e && e->present) {
    xcb_damage_query_version_reply_t *r = xcb_damage_query_version_reply(c, xcb_damage_query_version(c, 1, 1), NULL);
    say("xcb Damage: version %u.%u", r ? r->major_version : 0, r ? r->minor_version : 0);
    free(r);
  } else say("xcb Damage: absent");
  e = xcb_get_extension_data(c, &xcb_sync_id);
  if (e && e->present) {
    xcb_sync_initialize_reply_t *r = xcb_sync_initialize_reply(c, xcb_sync_initialize(c, 3, 1), NULL);
    say("xcb SYNC: version %u.%u", r ? r->major_version : 0, r ? r->minor_version : 0);
    free(r);
  } else say("xcb SYNC: absent");
  /* libxshmfence: what Mesa's DRI3 path allocates per swapchain image. */
  int fd = xshmfence_alloc_shm();
  if (fd >= 0) {
    struct xshmfence *f = xshmfence_map_shm(fd);
    say("xshmfence: alloc_shm fd=%d map=%s", fd, f ? "ok" : "FAILED");
    if (f) {
      xshmfence_trigger(f);
      say("xshmfence: trigger -> query=%d", xshmfence_query(f));
      xshmfence_unmap_shm(f);
    }
    close(fd);
  } else say("xshmfence: alloc_shm failed (fd=%d)", fd);
}

/* KERNEL WORKAROUND (docs/cp-x11.md §4.2, reported, not fixed in this lane):
 * compat writev (src/kernel/arch/x64/trap.c compat_writev) hands each iovec
 * base straight to file_write, which memcpy()s from the user address in
 * kernel mode.  When that page belongs to an ld.so mmap of a library that has
 * not been touched from user mode yet (libX11's .rodata `pad[]` that _XSend
 * appends to every request), the kernel takes a #PF inside memcpy with IF=0
 * and the CPU never comes back (measured: XOpenDisplay hangs in its first
 * XQueryExtension).  Touching every page of every loaded object from user
 * mode first makes them resident.  XSMOKE_PREFAULT=0 disables this (to
 * reproduce the kernel bug). */
#include <link.h>
static int prefault_cb(struct dl_phdr_info *info, size_t size, void *data) {
  long *acc = data;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
    if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_R)) continue;
    uintptr_t a = (info->dlpi_addr + ph->p_vaddr) & ~(uintptr_t)4095;
    uintptr_t e = info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
    for (; a < e; a += 4096) { acc[2] += *(volatile unsigned char *)a; acc[0]++; }
  }
  acc[1]++;
  return 0;
}

int main(void) {
  long hold = getenv("XSMOKE_HOLD_MS") ? atol(getenv("XSMOKE_HOLD_MS")) : 15000;
  say("xsmoke up pid=%d DISPLAY=%s LD_LIBRARY_PATH=%s", (int)getpid(),
      getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)",
      getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "(unset)");
  const char *pfenv = getenv("XSMOKE_PREFAULT");
  if (!pfenv || *pfenv != '0') {
    long acc[3] = {0, 0, 0};
    long t = now_ms();
    dl_iterate_phdr(prefault_cb, acc);
    say("prefault: touched %ld pages in %ld loaded objects in %ld ms (kernel writev workaround)",
        acc[0], acc[1], now_ms() - t);
  } else {
    say("prefault: disabled (XSMOKE_PREFAULT=0)");
  }
  XSetErrorHandler(on_xerror);
  Display *d = XOpenDisplay(NULL);
  if (!d) { say("XOpenDisplay(NULL) FAILED"); return 1; }
  say("libX11=%s libxcb=%s libXi=%s libXrandr=%s", lib_of((void *)XOpenDisplay),
      lib_of((void *)xcb_connect), lib_of((void *)XIQueryVersion), lib_of((void *)XRRQueryVersion));
  int scr = DefaultScreen(d);
  Window root = RootWindow(d, scr);
  say("XOpenDisplay ok: \"%s\" vendor=\"%s\" release=%d proto=%d.%d screens=%d default=%d %dx%d depth=%d",
      DisplayString(d), ServerVendor(d), VendorRelease(d), ProtocolVersion(d), ProtocolRevision(d),
      ScreenCount(d), scr, DisplayWidth(d, scr), DisplayHeight(d, scr), DefaultDepth(d, scr));

  int n = 0;
  char **ext = XListExtensions(d, &n);
  say("XListExtensions: %d", n);
  for (int i = 0; i < n; i++) say("  %s", ext[i]);
  XFreeExtensionList(ext);
  static const char *want[] = {"XInputExtension", "RANDR", "RENDER", "XFIXES", "MIT-SHM", "Present",
                               "DRI3", "SYNC", "XINERAMA", "Composite", "DAMAGE", "XKEYBOARD",
                               "Generic Event Extension", "SHAPE", "BIG-REQUESTS"};
  for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) query_ext(d, want[i]);
  probe_xi2(d, root);
  probe_randr(d, root);
  probe_misc(d, scr);
  probe_xcb(d);

  /* The window: 640x400, green field, blue block, a RENDER-filled red bar,
   * core-font text. */
  unsigned long green = 0x00ff00, blue = 0x0000ff;
  Window w = XCreateSimpleWindow(d, root, 200, 150, 640, 400, 0, BlackPixel(d, scr), green);
  XStoreName(d, w, "xsmoke: real libX11 1.8.14 on HobbyOS");
  Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(d, w, &del, 1);
  XSelectInput(d, w, ExposureMask | StructureNotifyMask);
  XMapWindow(d, w);
  XSync(d, False);
  GC gc = XCreateGC(d, w, 0, NULL);
  XFontStruct *font = XLoadQueryFont(d, "fixed");
  if (font) XSetFont(d, gc, font->fid);
  XRenderPictFormat *pf = XRenderFindVisualFormat(d, DefaultVisual(d, scr));
  Picture pic = pf ? XRenderCreatePicture(d, w, pf, 0, NULL) : 0;

  int exposes = 0, fd = ConnectionNumber(d);
  long t0 = now_ms(), drawn_at = 0;
  while (now_ms() - t0 < hold + 5000) {
    while (XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);
      if (e.type == MapNotify) say("MapNotify");
      if (e.type == ConfigureNotify)
        say("ConfigureNotify %dx%d at %d,%d", e.xconfigure.width, e.xconfigure.height, e.xconfigure.x,
            e.xconfigure.y);
      if (e.type == Expose && e.xexpose.count == 0) {
        exposes++;
        XSetForeground(d, gc, blue);
        XFillRectangle(d, w, gc, 40, 40, 240, 160);
        if (pic) {
          XRenderColor red = {0xffff, 0, 0, 0xffff};
          XRenderFillRectangle(d, PictOpSrc, pic, &red, 320, 40, 280, 60);
        }
        XSetForeground(d, gc, BlackPixel(d, scr));
        const char *s1 = "HobbyOS C4-x: real libX11 1.8.14 / libxcb 1.17.0 (glibc 2.31)";
        const char *s2 = "connected to XHOBBY.BIN on :0 via /tmp/.X11-unix/X0";
        XDrawString(d, w, gc, 40, 240, s1, (int)strlen(s1));
        XDrawString(d, w, gc, 40, 260, s2, (int)strlen(s2));
        XFlush(d);
        XSync(d, False);
        if (!drawn_at) {
          drawn_at = now_ms();
          say("Expose -> window drawn (green field, blue block, RENDER red bar, text) font=%s render_pic=%s",
              font ? "fixed" : "none", pic ? "yes" : "no");
        }
      }
      if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == del) say("WM_DELETE_WINDOW");
    }
    if (drawn_at && now_ms() - drawn_at > hold) break;
    struct pollfd p = {fd, POLLIN, 0};
    poll(&p, 1, 100);
  }
  int rc = exposes ? 0 : 2;
  if (pic) XRenderFreePicture(d, pic);
  if (font) XFreeFont(d, font);
  XFreeGC(d, gc);
  XDestroyWindow(d, w);
  XSync(d, False);
  say("exposes=%d xerrors=%d", exposes, g_xerrors);
  XCloseDisplay(d);
  say("done rc=%d", rc);
  return rc;
}
