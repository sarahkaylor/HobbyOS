/* src/user/cp/rawinput.c - Gate P4 raw-input / mouselook probe (lane C4-x2,
 * docs/cyberpunk.md §6 P4).
 *
 * A Windows x86_64 program (console subsystem, so its rows reach the serial
 * log through the inherited stdout; it still opens a real window), built with
 * clang -target x86_64-windows against Wine 11.0's headers and PE import libs
 * (tools/cp/p4/build-rawinput.sh; no MinGW).
 *
 *   rawinput.exe [clip_ms]   the mouselook probe:
 *     1. opens a 640x480 window, brings it to the front, and registers for
 *        raw mouse input (usage page 1 / usage 2, RIDEV_INPUTSINK);
 *     2. phase "free": WM_INPUT with the cursor unclipped;
 *     3. phase "clip": ClipCursor() to the window's client area (what a game
 *        does for mouselook; Wine answers with an X pointer grab confined to
 *        a clip window and selects XI_RawMotion on the root), parks the
 *        cursor in the middle, and sums WM_INPUT deltas;
 *     4. releases the clip and prints "[RI] done rc=<failures>".
 *   Each phase prints "[RI] phase <name> ready ..." when it starts listening
 *   and "[RI] phase <name> total n=<packets> rel=<relative packets>
 *   abs=<absolute packets> dx=<sum> dy=<sum> ..." after 3 s without input
 *   (or after clip_ms, default 90 s, with none).  The host harness injects
 *   QMP relative motion after a "ready" row and compares the sums.
 *
 *   rawinput.exe hold <ms>   keeps the desktop (explorer) alive for <ms>.
 *
 *   rawinput.exe modes       the display-mode probe: EnumDisplaySettings,
 *     ChangeDisplaySettingsEx to 1280x720 (CDS_FULLSCREEN), the metrics Wine
 *     then reports, a 4 s hold (screendump), and the restore.
 *
 * Every row is one WriteFile (kernel console rows cannot splice into it).
 */
#include <windows.h>

static int fails;
static HWND g_hwnd;

static void put(const char *s) {
  DWORD n = 0;
  int len = 0;
  while (s[len]) len++;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)len, &n, NULL);
}

/* A tiny row builder: text and signed decimals, one WriteFile per row. */
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
static void r_x(struct row *r, unsigned long long v) {
  char t[20];
  int k = 0;
  do { unsigned d = (unsigned)(v & 15); t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v && k < 19);
  r_s(r, "0x");
  while (k && r->n < (int)sizeof r->b - 3) r->b[r->n++] = t[--k];
}
static void r_end(struct row *r) {
  r->b[r->n++] = '\r';
  r->b[r->n++] = '\n';
  r->b[r->n] = 0;
  put(r->b);
  r->n = 0;
}

/* ---- raw input accounting ------------------------------------------- */
static struct {
  long long n, rel, abs, dx, dy, ax, ay, buttons;
  DWORD last_tick;
} acc;
static int verbose_rows = 40;   /* first N WM_INPUT packets are printed */

static void on_input(HRAWINPUT h) {
  RAWINPUT ri;
  UINT sz = sizeof ri;
  if (GetRawInputData(h, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1)
    return;
  if (ri.header.dwType != RIM_TYPEMOUSE)
    return;
  RAWMOUSE *m = &ri.data.mouse;
  acc.n++;
  acc.last_tick = GetTickCount();
  if (m->usButtonFlags) acc.buttons++;
  if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
    acc.abs++;
    acc.ax = m->lLastX;
    acc.ay = m->lLastY;
  } else {
    acc.rel++;
    acc.dx += m->lLastX;
    acc.dy += m->lLastY;
  }
  if (verbose_rows > 0) {
    verbose_rows--;
    struct row r = {{0}, 0};
    r_s(&r, "[RI] input flags="); r_x(&r, m->usFlags);
    r_s(&r, " x="); r_d(&r, m->lLastX);
    r_s(&r, " y="); r_d(&r, m->lLastY);
    r_s(&r, " btn="); r_x(&r, m->usButtonFlags);
    r_end(&r);
  }
}

/* The fullscreen window of the modes probe: pure magenta, no frame. */
static LRESULT CALLBACK fullproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH b = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(dc, &rc, b);
    DeleteObject(b);
    EndPaint(h, &ps);
    return 0;
  }
  return DefWindowProcA(h, msg, wp, lp);
}

static void pump(DWORD idle_ms, DWORD max_ms);

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_INPUT:
    on_input((HRAWINPUT)lp);
    return DefWindowProcA(h, msg, wp, lp);
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH b = CreateSolidBrush(RGB(0, 96, 160));
    FillRect(dc, &rc, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, 16, 16, "HobbyOS P4 raw-input probe", 26);
    EndPaint(h, &ps);
    return 0;
  }
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, msg, wp, lp);
}

/* Pump messages until `idle_ms` have passed since the last WM_INPUT (once
 * at least one arrived), or `max_ms` in total. */
static void pump(DWORD idle_ms, DWORD max_ms) {
  DWORD t0 = GetTickCount();
  for (;;) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    DWORD now = GetTickCount();
    if (acc.n > 0 && now - acc.last_tick >= idle_ms) break;
    if (now - t0 >= max_ms) break;
    MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
  }
}

static void phase(const char *name, DWORD max_ms, const RECT *clip) {
  struct row r = {{0}, 0};
  acc.n = acc.rel = acc.abs = acc.dx = acc.dy = acc.ax = acc.ay = acc.buttons = 0;
  verbose_rows = 40;
  POINT p0;
  GetCursorPos(&p0);
  r_s(&r, "[RI] phase "); r_s(&r, name); r_s(&r, " ready cursor=");
  r_d(&r, p0.x); r_s(&r, ","); r_d(&r, p0.y);
  if (clip) {
    r_s(&r, " clip="); r_d(&r, clip->left); r_s(&r, ","); r_d(&r, clip->top);
    r_s(&r, ","); r_d(&r, clip->right); r_s(&r, ","); r_d(&r, clip->bottom);
  }
  r_end(&r);
  pump(3000, max_ms);
  POINT p1;
  GetCursorPos(&p1);
  r_s(&r, "[RI] phase "); r_s(&r, name); r_s(&r, " total n="); r_d(&r, acc.n);
  r_s(&r, " rel="); r_d(&r, acc.rel); r_s(&r, " abs="); r_d(&r, acc.abs);
  r_s(&r, " dx="); r_d(&r, acc.dx); r_s(&r, " dy="); r_d(&r, acc.dy);
  r_s(&r, " lastabs="); r_d(&r, acc.ax); r_s(&r, ","); r_d(&r, acc.ay);
  r_s(&r, " cursor="); r_d(&r, p1.x); r_s(&r, ","); r_d(&r, p1.y);
  if (clip) {
    int inside = p1.x >= clip->left && p1.x < clip->right &&
                 p1.y >= clip->top && p1.y < clip->bottom;
    r_s(&r, inside ? " inside=1" : " inside=0");
    if (!inside) fails++;
  }
  r_end(&r);
}

static int run_mouselook(DWORD max_ms) {
  struct row r = {{0}, 0};
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
  wc.lpszClassName = "HobbyRawInput";
  RegisterClassA(&wc);
  g_hwnd = CreateWindowExA(0, "HobbyRawInput", "P4 raw-input probe",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 150, 640, 480,
                           NULL, NULL, wc.hInstance, NULL);
  if (!g_hwnd) {
    r_s(&r, "[RI] FAIL CreateWindowEx err="); r_d(&r, (long long)GetLastError()); r_end(&r);
    return 1;
  }
  ShowWindow(g_hwnd, SW_SHOW);
  UpdateWindow(g_hwnd);
  SetForegroundWindow(g_hwnd);
  pump(0, 1500);

  RAWINPUTDEVICE rid;
  rid.usUsagePage = 1;
  rid.usUsage = 2;
  rid.dwFlags = RIDEV_INPUTSINK;
  rid.hwndTarget = g_hwnd;
  BOOL ok = RegisterRawInputDevices(&rid, 1, sizeof rid);
  r_s(&r, ok ? "[RI] PASS RegisterRawInputDevices" : "[RI] FAIL RegisterRawInputDevices err=");
  if (!ok) { r_d(&r, (long long)GetLastError()); fails++; }
  r_end(&r);

  RECT wr, cr;
  GetWindowRect(g_hwnd, &wr);
  GetClientRect(g_hwnd, &cr);
  POINT tl = {cr.left, cr.top}, br = {cr.right, cr.bottom};
  ClientToScreen(g_hwnd, &tl);
  ClientToScreen(g_hwnd, &br);
  RECT clip = {tl.x, tl.y, br.x, br.y};
  r_s(&r, "[RI] window hwnd="); r_x(&r, (unsigned long long)(ULONG_PTR)g_hwnd);
  r_s(&r, " rect="); r_d(&r, wr.left); r_s(&r, ","); r_d(&r, wr.top); r_s(&r, ",");
  r_d(&r, wr.right); r_s(&r, ","); r_d(&r, wr.bottom);
  r_s(&r, " fg="); r_s(&r, GetForegroundWindow() == g_hwnd ? "1" : "0");
  r_s(&r, " screen="); r_d(&r, GetSystemMetrics(SM_CXSCREEN)); r_s(&r, "x");
  r_d(&r, GetSystemMetrics(SM_CYSCREEN));
  r_end(&r);

  /* Start both phases with the cursor over the window: X only reports core
     motion to the window under the pointer (or to a grab). */
  SetCursorPos((clip.left + clip.right) / 2, (clip.top + clip.bottom) / 2);
  pump(0, 300);
  phase("free", max_ms, NULL);

  SetCursorPos((clip.left + clip.right) / 2, (clip.top + clip.bottom) / 2);
  ok = ClipCursor(&clip);
  RECT got;
  GetClipCursor(&got);
  r_s(&r, ok ? "[RI] PASS ClipCursor" : "[RI] FAIL ClipCursor err=");
  if (!ok) { r_d(&r, (long long)GetLastError()); fails++; }
  r_s(&r, " got="); r_d(&r, got.left); r_s(&r, ","); r_d(&r, got.top); r_s(&r, ",");
  r_d(&r, got.right); r_s(&r, ","); r_d(&r, got.bottom);
  r_end(&r);
  pump(0, 1000);    /* let winex11 grab and enable XI2 raw motion */
  phase("clip", max_ms, &clip);

  ClipCursor(NULL);
  pump(0, 500);
  DestroyWindow(g_hwnd);
  pump(0, 500);
  return 0;
}

static int run_modes(void) {
  struct row r = {{0}, 0};
  DEVMODEA dm;
  int i, n = 0, has720 = 0;
  for (i = 0;; i++) {
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsA(NULL, (DWORD)i, &dm)) break;
    n++;
    if (dm.dmPelsWidth == 1280 && dm.dmPelsHeight == 720) has720 = 1;
    if (i < 40) {
      r_s(&r, "[RI] mode "); r_d(&r, i); r_s(&r, " "); r_d(&r, dm.dmPelsWidth); r_s(&r, "x");
      r_d(&r, dm.dmPelsHeight); r_s(&r, "x"); r_d(&r, dm.dmBitsPerPel); r_s(&r, " @");
      r_d(&r, dm.dmDisplayFrequency); r_end(&r);
    }
  }
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
  r_s(&r, "[RI] modes count="); r_d(&r, n); r_s(&r, " current="); r_d(&r, dm.dmPelsWidth);
  r_s(&r, "x"); r_d(&r, dm.dmPelsHeight); r_s(&r, " has1280x720="); r_d(&r, has720);
  r_end(&r);

  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  dm.dmPelsWidth = 1280;
  dm.dmPelsHeight = 720;
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
  LONG rc = ChangeDisplaySettingsExA(NULL, &dm, NULL, CDS_FULLSCREEN, NULL);
  r_s(&r, "[RI] ChangeDisplaySettingsEx 1280x720 rc="); r_d(&r, rc);
  r_s(&r, " metrics="); r_d(&r, GetSystemMetrics(SM_CXSCREEN)); r_s(&r, "x");
  r_d(&r, GetSystemMetrics(SM_CYSCREEN));
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
  r_s(&r, " current="); r_d(&r, dm.dmPelsWidth); r_s(&r, "x"); r_d(&r, dm.dmPelsHeight);
  r_end(&r);
  if (rc == DISP_CHANGE_SUCCESSFUL) {
    /* A borderless window covering the new mode, as a fullscreen game makes
       one: Wine asks the WM for _NET_WM_STATE_FULLSCREEN. */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = fullproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "HobbyFull";
    RegisterClassA(&wc);
    HWND fw = CreateWindowExA(WS_EX_TOPMOST, "HobbyFull", "P4 fullscreen", WS_POPUP | WS_VISIBLE,
                              0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                              NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(fw);
    pump(0, 1500);
    RECT fr = {0, 0, 0, 0};
    if (fw) GetWindowRect(fw, &fr);
    r_s(&r, "[RI] fullscreen window rect="); r_d(&r, fr.left); r_s(&r, ","); r_d(&r, fr.top);
    r_s(&r, ","); r_d(&r, fr.right); r_s(&r, ","); r_d(&r, fr.bottom);
    r_s(&r, " fg="); r_s(&r, GetForegroundWindow() == fw ? "1" : "0"); r_end(&r);
    r_s(&r, "[RI] modes hold 4000 ms"); r_end(&r);
    pump(0, 4000);
    if (fw) DestroyWindow(fw);
    pump(0, 300);
  }
  LONG rc2 = ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
  r_s(&r, "[RI] restore rc="); r_d(&r, rc2); r_s(&r, " current="); r_d(&r, dm.dmPelsWidth);
  r_s(&r, "x"); r_d(&r, dm.dmPelsHeight); r_end(&r);
  /* A clean refusal (DISP_CHANGE_BADMODE / _FAILED) is an acceptable answer
     too; what must not happen is a hang or a wrong "current" afterwards. */
  if (rc2 != DISP_CHANGE_SUCCESSFUL) fails++;
  return 0;
}

int main(int argc, char **argv) {
  struct row r = {{0}, 0};
  DWORD max_ms = 90000;
  int modes = 0;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == 'm') modes = 1;
    else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
      DWORD v = 0;
      for (const char *c = argv[i]; *c >= '0' && *c <= '9'; c++) v = v * 10 + (DWORD)(*c - '0');
      max_ms = v;
    }
  }
  if (argc > 1 && argv[1][0] == 'h') {
    /* "hold": keep a user32 client alive so the desktop process (explorer,
       which owns winex11's display setup) stays up across the gate's steps;
       win32u waits only 10 s for explorer when the first client starts it. */
    HWND d = GetDesktopWindow();
    r_s(&r, "[RI] hold desktop="); r_x(&r, (unsigned long long)(ULONG_PTR)d);
    r_s(&r, " ms="); r_d(&r, max_ms); r_end(&r);
    Sleep(max_ms);
    return 0;
  }
  r_s(&r, "[RI] rawinput probe (C4-x2) mode="); r_s(&r, modes ? "modes" : "mouselook"); r_end(&r);
  if (modes) run_modes();
  else run_mouselook(max_ms);
  r_s(&r, "[RI] done rc="); r_d(&r, fails); r_end(&r);
  return fails;
}
