/* tools/cp/p4/xprobe.c - the X server half of P4 without Wine (lane C4-x2).
 *
 * A compat (glibc 2.31) X client built like tools/cp/x11/xsmoke.c (DT_RPATH
 * /CP/X11/lib).  tools/cp/p4/xprobe.py starts it from /XSTART.TXT in an
 * IceWM session (HB_SECOND_SCREEN=0) with /CP/X11 staged on the FAT disk
 * (`make cp-x11-disk`), and injects QMP input after its "ready" rows.
 *
 *   X1 screens/extensions  ScreenCount, XINERAMA, Composite
 *   X2 XI2 devices         every slave pointer and its valuator modes
 *   X3 raw motion          XISelectEvents(root, XI_RawMotion) on all master
 *                          devices; sums raw deltas (valuators.values) per
 *                          source device until 3 s pass without one, and
 *                          reports the master's valuator modes after
 *                          XI_DeviceChanged -- "[XP] raw ready" first
 *   X4 RandR mode set      XRRSetScreenSize + XRRSetCrtcConfig to 1280x720,
 *                          a red full-screen window, "[XP] randr hold" (the
 *                          harness screendumps), then back to the original
 *                          mode; also a refused request (a CRTC offset)
 *
 * Rows: "[XP] ..."; final "[XP] done rc=<failures>".
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define ROW(...) do { printf("[XP] " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int xerr_code;
static int on_xerr(Display *d, XErrorEvent *e) { (void)d; xerr_code = e->error_code; return 0; }

static void list_devices(Display *dpy, int which, const char *tag) {
  int n = 0;
  XIDeviceInfo *info = XIQueryDevice(dpy, which, &n);
  for (int i = 0; i < n; i++) {
    int rel = 0, abs = 0;
    for (int c = 0; c < info[i].num_classes; c++)
      if (info[i].classes[c]->type == XIValuatorClass) {
        XIValuatorClassInfo *v = (XIValuatorClassInfo *)info[i].classes[c];
        if (v->mode == XIModeRelative) rel++; else abs++;
      }
    if (info[i].use == XIMasterPointer || info[i].use == XISlavePointer)
      ROW("%s device %d \"%s\" use=%d valuators relative=%d absolute=%d", tag,
          info[i].deviceid, info[i].name, info[i].use, rel, abs);
  }
  XIFreeDeviceInfo(info);
}

static void raw_phase(Display *dpy, int xi_opcode) {
  Window root = DefaultRootWindow(dpy);
  unsigned char m[XIMaskLen(XI_LASTEVENT)] = {0};
  XIEventMask em = {XIAllMasterDevices, sizeof m, m};
  XISetMask(m, XI_RawMotion);
  XISetMask(m, XI_DeviceChanged);
  XISelectEvents(dpy, root, &em, 1);
  XSync(dpy, False);
  ROW("raw ready");
  double sx[16] = {0}, sy[16] = {0};
  long n[16] = {0}, changed = 0, t_last = 0, t0 = now_ms();
  int fd = ConnectionNumber(dpy);
  for (;;) {
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      if (ev.xcookie.type != GenericEvent || ev.xcookie.extension != xi_opcode ||
          !XGetEventData(dpy, &ev.xcookie)) continue;
      if (ev.xcookie.evtype == XI_RawMotion) {
        XIRawEvent *r = ev.xcookie.data;
        int src = r->sourceid & 15;
        const double *v = r->valuators.values;
        for (int i = 0; i < r->valuators.mask_len * 8 && i < 2; i++)
          if (XIMaskIsSet(r->valuators.mask, i)) {
            if (i == 0) sx[src] += *v; else sy[src] += *v;
            v++;
          }
        n[src]++;
        t_last = now_ms();
      } else if (ev.xcookie.evtype == XI_DeviceChanged) {
        XIDeviceChangedEvent *c = ev.xcookie.data;
        int rel = 0, abs = 0;
        for (int k = 0; k < c->num_classes; k++)
          if (c->classes[k]->type == XIValuatorClass) {
            XIValuatorClassInfo *vc = (XIValuatorClassInfo *)c->classes[k];
            if (vc->number < 2) { if (vc->mode == XIModeRelative) rel++; else abs++; }
          }
        if (changed++ < 8)
          ROW("raw DeviceChanged device=%d source=%d reason=%d master valuators 0/1: relative=%d absolute=%d",
              c->deviceid, c->sourceid, c->reason, rel, abs);
      }
      XFreeEventData(dpy, &ev.xcookie);
    }
    long t = now_ms();
    if (t_last && t - t_last > 3000) break;
    if (t - t0 > 120000) break;
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(fd, &fs);
    struct timeval tv = {0, 50000};
    select(fd + 1, &fs, NULL, NULL, &tv);
  }
  for (int s = 0; s < 16; s++)
    if (n[s]) ROW("raw total source=%d events=%ld dx=%.0f dy=%.0f", s, n[s], sx[s], sy[s]);
  if (!t_last) { ROW("FAIL raw: no XI_RawMotion"); fails++; }
}

static void randr_phase(Display *dpy) {
  Window root = DefaultRootWindow(dpy);
  XRRScreenResources *res = XRRGetScreenResources(dpy, root);
  if (!res || res->ncrtc < 1 || res->noutput < 1) { ROW("FAIL randr: no resources"); fails++; return; }
  RRCrtc crtc = res->crtcs[0];
  XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, crtc);
  RRMode orig = ci->mode, want = 0;
  int ow = DisplayWidth(dpy, 0), oh = DisplayHeight(dpy, 0);
  for (int i = 0; i < res->nmode; i++)
    if (res->modes[i].width == 1280 && res->modes[i].height == 720) want = res->modes[i].id;
  ROW("randr modes=%d current=%dx%d mode1280x720=%s", res->nmode, ow, oh, want ? "yes" : "no");
  if (!want) { fails++; return; }
  XSetErrorHandler(on_xerr);
  /* refused: a CRTC offset (the DDX shows the top-left only) */
  Status st = XRRSetCrtcConfig(dpy, res, crtc, CurrentTime, 16, 16, want, RR_Rotate_0,
                               ci->outputs, ci->noutput);
  XSync(dpy, False);
  ROW("randr refused-offset status=%d (RRSetConfigFailed=%d expected)", st, RRSetConfigFailed);
  if (st != RRSetConfigFailed) fails++;
  /* the real mode set: CRTC first (it fits the current screen), then the
     screen size, as xrandr does when shrinking */
  st = XRRSetCrtcConfig(dpy, res, crtc, CurrentTime, 0, 0, want, RR_Rotate_0,
                        ci->outputs, ci->noutput);
  XRRSetScreenSize(dpy, root, 1280, 720, 339, 191);
  XSync(dpy, False);
  XRRScreenResources *r2 = XRRGetScreenResources(dpy, root);
  XRRCrtcInfo *c2 = XRRGetCrtcInfo(dpy, r2, crtc);
  int w = 0, h = 0;
  Window rr; int x, y; unsigned bw, dep, rw, rh;
  XGetGeometry(dpy, root, &rr, &x, &y, &rw, &rh, &bw, &dep);
  w = (int)rw; h = (int)rh;
  ROW("randr set status=%d xerr=%d crtc=%ux%u root=%dx%d", st, xerr_code, c2->width, c2->height, w, h);
  if (st != RRSetConfigSuccess || w != 1280 || h != 720 || c2->width != 1280) fails++;
  XSetWindowAttributes a;
  a.override_redirect = True;
  a.background_pixel = 0xff0000;
  Window win = XCreateWindow(dpy, root, 0, 0, 1280, 720, 0, CopyFromParent, InputOutput,
                             CopyFromParent, CWOverrideRedirect | CWBackPixel, &a);
  XMapRaised(dpy, win);
  XSync(dpy, False);
  ROW("randr hold 5000 ms (red window 1280x720 at 0,0)");
  usleep(5000000);
  XDestroyWindow(dpy, win);
  XRRSetScreenSize(dpy, root, ow, oh, DisplayWidthMM(dpy, 0), DisplayHeightMM(dpy, 0));
  st = XRRSetCrtcConfig(dpy, r2, crtc, CurrentTime, 0, 0, orig, RR_Rotate_0,
                        ci->outputs, ci->noutput);
  XSync(dpy, False);
  XGetGeometry(dpy, root, &rr, &x, &y, &rw, &rh, &bw, &dep);
  ROW("randr restore status=%d root=%ux%u", st, rw, rh);
  if ((int)rw != ow || (int)rh != oh) fails++;
  XRRFreeCrtcInfo(c2);
  XRRFreeScreenResources(r2);
  XRRFreeCrtcInfo(ci);
  XRRFreeScreenResources(res);
}

int main(void) {
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) { ROW("FAIL XOpenDisplay"); ROW("done rc=1"); return 1; }
  int op, ev, er, maj = 2, min = 2;
  int xin = XQueryExtension(dpy, "XINERAMA", &op, &ev, &er);
  int comp = XQueryExtension(dpy, "Composite", &op, &ev, &er);
  ROW("screens=%d XINERAMA=%s Composite=%s", ScreenCount(dpy), xin ? "yes" : "no", comp ? "yes" : "no");
  if (ScreenCount(dpy) != 1 || !xin || !comp) fails++;
  int xi_opcode;
  if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &ev, &er) ||
      XIQueryVersion(dpy, &maj, &min) != Success) {
    ROW("FAIL XI2"); fails++;
  } else {
    list_devices(dpy, XIAllDevices, "xi2");
    raw_phase(dpy, xi_opcode);
  }
  randr_phase(dpy);
  ROW("done rc=%d", fails);
  XCloseDisplay(dpy);
  return fails;
}
