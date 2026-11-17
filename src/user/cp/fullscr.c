/* src/user/cp/fullscr.c - Wine fullscreen mode-switch probe (lane C4-x3,
 * docs/cyberpunk.md §7.2e, docs/cp-x3.md §4).
 *
 * A Windows x86_64 console program built like the P5 probes
 * (tools/cp/p5/build-pe.sh: clang -target x86_64-windows, Wine's headers and
 * PE import libs).  It does what a game's exclusive-fullscreen start does with
 * plain user32/gdi32, so the X side (winex11's XRandR 1.4 handler -> XHOBBY's
 * RandR, IceWM's _NET_WM_STATE_FULLSCREEN) is exercised without D3D:
 *
 *   fullscr.exe run [hold_ms]
 *     M1 EnumDisplaySettings lists 1280x720 and 1920x1080; the current mode
 *     M2 ChangeDisplaySettingsEx(1280x720, CDS_FULLSCREEN): rc, metrics,
 *        ENUM_CURRENT_SETTINGS
 *     M3 a WS_POPUP TOPMOST window covering the mode, painted with GDI (so
 *        through winex11's window surface: XShmPutImage when MIT-SHM is up):
 *        64x64 corner markers red (top-left) / green (top-right) / blue
 *        (bottom-left) / white (bottom-right), 8 colour bars between; rows
 *        "[FS] hold <tag> <w>x<h>" while the harness screendumps
 *     M4 the same at 1920x1080 (switch "back" while still fullscreen)
 *     M5 1280x720 again, then EXIT WITHOUT RESTORING: Windows (and Wine's
 *        explorer display-settings restorer, programs/explorer/desktop.c)
 *        must put the desktop mode back when a CDS_FULLSCREEN process exits
 *   fullscr.exe check [wait_ms]
 *     M6 polls ENUM_CURRENT_SETTINGS until it is the registry (desktop) mode
 *        again, and reports how long that took
 *
 * Rows "[FS] ...", one WriteFile each; last row "[FS] done rc=<failures>".
 */
#include <windows.h>

static int fails;

struct row { char b[512]; int n; };
static void r_s(struct row *r, const char *s) {
  while (*s && r->n < (int)sizeof r->b - 3) r->b[r->n++] = *s++;
}
static void r_d(struct row *r, long long v) {
  char t[24];
  int k = 0;
  unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
  if (v < 0) r_s(r, "-");
  do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 23);
  while (k && r->n < (int)sizeof r->b - 3) r->b[r->n++] = t[--k];
}
static void r_end(struct row *r) {
  DWORD n = 0;
  r->b[r->n++] = '\r';
  r->b[r->n++] = '\n';
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), r->b, (DWORD)r->n, &n, NULL);
  r->n = 0;
}
static void say(const char *s) { struct row r = {{0}, 0}; r_s(&r, s); r_end(&r); }

static void pump(DWORD ms) {
  DWORD t0 = GetTickCount();
  for (;;) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&m);
      DispatchMessageA(&m);
    }
    DWORD el = GetTickCount() - t0;
    if (el >= ms) break;
    MsgWaitForMultipleObjects(0, NULL, FALSE, ms - el < 50 ? ms - el : 50, QS_ALLINPUT);
  }
}

static void cur_mode(DWORD *w, DWORD *h) {
  DEVMODEA dm;
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
  *w = dm.dmPelsWidth;
  *h = dm.dmPelsHeight;
}

static void reg_mode(DWORD *w, DWORD *h) {
  DEVMODEA dm;
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &dm);
  *w = dm.dmPelsWidth;
  *h = dm.dmPelsHeight;
}

/* The pattern: what the harness looks for on the glass. */
static void paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT rc;
  GetClientRect(h, &rc);
  int w = rc.right, ht = rc.bottom;
  static const COLORREF bars[8] = {RGB(255, 255, 0), RGB(0, 255, 255), RGB(255, 0, 255), RGB(255, 128, 0),
                                   RGB(128, 0, 255), RGB(0, 128, 0), RGB(128, 128, 128), RGB(0, 0, 128)};
  HBRUSH bk = CreateSolidBrush(RGB(16, 16, 16));
  FillRect(dc, &rc, bk);
  DeleteObject(bk);
  for (int i = 0; i < 8; i++) {
    RECT b = {64 + (w - 128) * i / 8, 96, 64 + (w - 128) * (i + 1) / 8, ht - 96};
    HBRUSH br = CreateSolidBrush(bars[i]);
    FillRect(dc, &b, br);
    DeleteObject(br);
  }
  struct { int x, y; COLORREF c; } mk[4] = {
    {0, 0, RGB(255, 0, 0)}, {w - 64, 0, RGB(0, 255, 0)},
    {0, ht - 64, RGB(0, 0, 255)}, {w - 64, ht - 64, RGB(255, 255, 255)}};
  for (int i = 0; i < 4; i++) {
    RECT m = {mk[i].x, mk[i].y, mk[i].x + 64, mk[i].y + 64};
    HBRUSH br = CreateSolidBrush(mk[i].c);
    FillRect(dc, &m, br);
    DeleteObject(br);
  }
  EndPaint(h, &ps);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) { paint(h); return 0; }
  if (msg == WM_ERASEBKGND) return 1;
  return DefWindowProcA(h, msg, wp, lp);
}

static LONG set_mode(DWORD w, DWORD h, const char *tag) {
  DEVMODEA dm;
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  dm.dmPelsWidth = w;
  dm.dmPelsHeight = h;
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
  DWORD t0 = GetTickCount();
  LONG rc = ChangeDisplaySettingsExA(NULL, &dm, NULL, CDS_FULLSCREEN, NULL);
  DWORD ms = GetTickCount() - t0;
  DWORD cw, ch;
  cur_mode(&cw, &ch);
  struct row r = {{0}, 0};
  r_s(&r, "[FS] "); r_s(&r, tag); r_s(&r, " ChangeDisplaySettingsEx "); r_d(&r, w); r_s(&r, "x");
  r_d(&r, h); r_s(&r, " rc="); r_d(&r, rc); r_s(&r, " in "); r_d(&r, ms); r_s(&r, " ms metrics=");
  r_d(&r, GetSystemMetrics(SM_CXSCREEN)); r_s(&r, "x"); r_d(&r, GetSystemMetrics(SM_CYSCREEN));
  r_s(&r, " current="); r_d(&r, cw); r_s(&r, "x"); r_d(&r, ch);
  r_end(&r);
  if (rc != DISP_CHANGE_SUCCESSFUL || cw != w || ch != h ||
      GetSystemMetrics(SM_CXSCREEN) != (int)w || GetSystemMetrics(SM_CYSCREEN) != (int)h) {
    fails++;
    say("[FS] FAIL mode set");
  }
  return rc;
}

static void cover(HWND fw, DWORD w, DWORD h, const char *tag, DWORD hold) {
  SetWindowPos(fw, HWND_TOPMOST, 0, 0, (int)w, (int)h, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
  InvalidateRect(fw, NULL, TRUE);
  SetForegroundWindow(fw);
  /* The window manager answers the resize asynchronously (IceWM keeps a
     _NET_WM_STATE_FULLSCREEN window at the monitor size it last saw, and
     winex11 takes the rect back from the ConfigureNotify), so wait for the
     rect to settle and report how long that took (run 1, shrinking from
     1920x1080: still 1920x1080 after 1.5 s). */
  RECT wr = {0, 0, 0, 0}, cr = {0, 0, 0, 0};
  DWORD t0 = GetTickCount(), settle = 0;
  for (;;) {
    pump(100);
    GetWindowRect(fw, &wr);
    settle = GetTickCount() - t0;
    if ((wr.left == 0 && wr.top == 0 && wr.right == (LONG)w && wr.bottom == (LONG)h) || settle >= 8000)
      break;
  }
  /* Watch it for 3 s: a window manager or a late ConfigureNotify can move it
     again after it first matched (run 2, M5: matched at 100 ms, back to the
     old 1920x1080 by 1.1 s).  Every change is a row. */
  RECT last = wr;
  for (int i = 0; i < 30; i++) {
    pump(100);
    RECT q;
    GetWindowRect(fw, &q);
    if (q.left != last.left || q.top != last.top || q.right != last.right || q.bottom != last.bottom) {
      struct row c = {{0}, 0};
      r_s(&c, "[FS] "); r_s(&c, tag); r_s(&c, " rect changed at +"); r_d(&c, (long long)(GetTickCount() - t0));
      r_s(&c, " ms -> "); r_d(&c, q.left); r_s(&c, ","); r_d(&c, q.top); r_s(&c, ",");
      r_d(&c, q.right); r_s(&c, ","); r_d(&c, q.bottom);
      r_end(&c);
      last = q;
    }
  }
  GetWindowRect(fw, &wr);
  GetClientRect(fw, &cr);
  struct row r = {{0}, 0};
  r_s(&r, "[FS] "); r_s(&r, tag); r_s(&r, " window rect="); r_d(&r, wr.left); r_s(&r, ",");
  r_d(&r, wr.top); r_s(&r, ","); r_d(&r, wr.right); r_s(&r, ","); r_d(&r, wr.bottom);
  r_s(&r, " client="); r_d(&r, cr.right); r_s(&r, "x"); r_d(&r, cr.bottom);
  r_s(&r, " fg="); r_s(&r, GetForegroundWindow() == fw ? "1" : "0");
  r_s(&r, " settled_ms="); r_d(&r, settle);
  r_end(&r);
  if (wr.left != 0 || wr.top != 0 || wr.right != (LONG)w || wr.bottom != (LONG)h) {
    fails++;
    say("[FS] FAIL window does not cover the mode");
  }
  r_s(&r, "[FS] hold "); r_s(&r, tag); r_s(&r, " "); r_d(&r, w); r_s(&r, "x"); r_d(&r, h);
  r_s(&r, " "); r_d(&r, hold); r_s(&r, " ms");
  r_end(&r);
  pump(hold);
}

static int run(DWORD hold) {
  struct row r = {{0}, 0};
  DEVMODEA dm;
  int n = 0, has720 = 0, has1080 = 0;
  for (int i = 0;; i++) {
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsA(NULL, (DWORD)i, &dm)) break;
    n++;
    if (dm.dmPelsWidth == 1280 && dm.dmPelsHeight == 720) has720 = 1;
    if (dm.dmPelsWidth == 1920 && dm.dmPelsHeight == 1080) has1080 = 1;
  }
  DWORD cw, ch, rw, rh;
  cur_mode(&cw, &ch);
  reg_mode(&rw, &rh);
  r_s(&r, "[FS] M1 modes="); r_d(&r, n); r_s(&r, " has1280x720="); r_d(&r, has720);
  r_s(&r, " has1920x1080="); r_d(&r, has1080); r_s(&r, " current="); r_d(&r, cw); r_s(&r, "x");
  r_d(&r, ch); r_s(&r, " registry="); r_d(&r, rw); r_s(&r, "x"); r_d(&r, rh);
  r_end(&r);
  if (!has720 || !has1080) { fails++; say("[FS] FAIL M1 modes missing"); }

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "HobbyFullscr";
  wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
  RegisterClassA(&wc);

  if (set_mode(1280, 720, "M2") != DISP_CHANGE_SUCCESSFUL) {
    r_s(&r, "[FS] done rc="); r_d(&r, fails); r_end(&r);
    return fails;
  }
  HWND fw = CreateWindowExA(WS_EX_TOPMOST, "HobbyFullscr", "C4-x3 fullscreen", WS_POPUP | WS_VISIBLE,
                            0, 0, 1280, 720, NULL, NULL, wc.hInstance, NULL);
  if (!fw) { fails++; say("[FS] FAIL CreateWindowEx"); }
  cover(fw, 1280, 720, "M3", hold);
  set_mode(1920, 1080, "M4");
  cover(fw, 1920, 1080, "M4", hold);
  set_mode(1280, 720, "M5");
  cover(fw, 1280, 720, "M5", hold);
  r_s(&r, "[FS] M5 exit without restoring (desktop mode was "); r_d(&r, rw); r_s(&r, "x"); r_d(&r, rh);
  r_s(&r, ")"); r_end(&r);
  r_s(&r, "[FS] done rc="); r_d(&r, fails); r_end(&r);
  ExitProcess((UINT)fails); /* no ChangeDisplaySettings(NULL), no DestroyWindow */
  return fails;
}

static int check(DWORD wait) {
  DWORD rw, rh, cw = 0, ch = 0, t0 = GetTickCount(), el = 0;
  reg_mode(&rw, &rh);
  for (;;) {
    cur_mode(&cw, &ch);
    el = GetTickCount() - t0;
    if ((cw == rw && ch == rh) || el >= wait) break;
    Sleep(200);
  }
  struct row r = {{0}, 0};
  r_s(&r, "[FS] M6 check current="); r_d(&r, cw); r_s(&r, "x"); r_d(&r, ch); r_s(&r, " registry=");
  r_d(&r, rw); r_s(&r, "x"); r_d(&r, rh); r_s(&r, " metrics="); r_d(&r, GetSystemMetrics(SM_CXSCREEN));
  r_s(&r, "x"); r_d(&r, GetSystemMetrics(SM_CYSCREEN)); r_s(&r, " after "); r_d(&r, el); r_s(&r, " ms");
  r_end(&r);
  if (cw != rw || ch != rh) { fails++; say("[FS] FAIL M6 desktop mode not restored"); }
  r_s(&r, "[FS] done rc="); r_d(&r, fails); r_end(&r);
  return fails;
}

int main(int argc, char **argv) {
  struct row r = {{0}, 0};
  DWORD ms = argc > 2 ? (DWORD)atoi(argv[2]) : 5000;
  r_s(&r, "[FS] fullscr probe (C4-x3) mode="); r_s(&r, argc > 1 ? argv[1] : "run"); r_s(&r, " pid=");
  r_d(&r, (long long)GetCurrentProcessId()); r_end(&r);
  if (argc > 1 && !strcmp(argv[1], "check")) return check(argc > 2 ? ms : 15000);
  return run(ms);
}
