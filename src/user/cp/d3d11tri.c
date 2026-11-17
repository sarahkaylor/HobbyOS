/* src/user/cp/d3d11tri.c — a D3D11 triangle for Gate P5 (lane C5-vk2,
 * docs/cp-vk.md §8; docs/cyberpunk.md P5): DXVK 3.1.1's d3d11.dll + dxgi.dll
 * under Wine -> winevulkan -> lavapipe, presented in a Win32 window by winex11.
 *
 *   d3d11tri.exe [seconds] [W] [H]          default: 65 640 480
 *
 * A Windows x86_64 console program (tools/cp/p5/build-pe.sh: clang
 * -target x86_64-windows + Wine's headers and import libs, no MinGW).  The D3D
 * entry points are resolved with LoadLibrary/GetProcAddress so the log names
 * the module that actually answered (DXVK's native DLL vs Wine's builtin).
 * The shaders are HLSL compiled at run time by d3dcompiler_47.dll (Wine's
 * builtin, vkd3d-shader); the triangle comes from SV_VertexID (no vertex
 * buffer, no input layout) and spins through a constant buffer.
 *
 * Rows "[D3D11] ...": modules + adapter (DXGI_ADAPTER_DESC: DXVK reports the
 * Vulkan device name, "llvmpipe ..."), a progress row every 5 s, RESULT, and
 * "[D3D11] done rc=0".  Any failing HRESULT ends with "done rc=1".
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[D3D11] ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(ap);
}

static double now_ms(void) {
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1e3 / (double)f.QuadPart;
}

static void die(const char *what, HRESULT hr) {
  row("FAIL %s -> hr=0x%08lx", what, (unsigned long)hr);
  row("done rc=1");
  fflush(stdout);
  ExitProcess(1);
}
#define HR(expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) die(#expr, hr_); } while (0)

static HMODULE load(const char *name) {
  HMODULE m = LoadLibraryA(name);
  if (!m) { row("FAIL LoadLibrary %s: error %lu", name, (unsigned long)GetLastError()); row("done rc=1"); ExitProcess(1); }
  char path[MAX_PATH] = "?";
  GetModuleFileNameA(m, path, sizeof path);
  row("module %-18s %s", name, path);
  return m;
}

static volatile int quit;
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_CLOSE) { quit = 1; return 0; }
  if (msg == WM_PAINT) { ValidateRect(h, NULL); return 0; }
  return DefWindowProcA(h, msg, wp, lp);
}

static const char hlsl[] =
    "cbuffer C : register(b0) { float4 rot; };\n"
    "struct V { float4 pos : SV_Position; float3 col : COLOR0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "  float2 p[3] = { float2(0.0, 0.75), float2(0.70, -0.55), float2(-0.70, -0.55) };\n"
    "  float3 c[3] = { float3(1, 0.15, 0.1), float3(0.1, 0.9, 0.2), float3(0.15, 0.3, 1) };\n"
    "  float2 q = p[id];\n"
    "  V o; o.pos = float4(q.x * rot.x - q.y * rot.y, (q.x * rot.y + q.y * rot.x) * rot.z, 0.5, 1);\n"
    "  o.col = c[id]; return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(i.col, 1); }\n";

typedef HRESULT(WINAPI *PFN_D3DCOMPILE)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *,
                                        ID3DInclude *, const char *, const char *, UINT, UINT,
                                        ID3DBlob **, ID3DBlob **);

static ID3DBlob *compile(PFN_D3DCOMPILE fn, const char *entry, const char *target) {
  ID3DBlob *code = NULL, *err = NULL;
  double t0 = now_ms();
  HRESULT hr = fn(hlsl, sizeof hlsl - 1, "d3d11tri.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
  if (FAILED(hr)) {
    row("FAIL D3DCompile %s %s hr=0x%08lx: %s", entry, target, (unsigned long)hr,
        err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no log)");
    row("done rc=1");
    ExitProcess(1);
  }
  row("D3DCompile %s/%s: %lu bytes DXBC in %.1f ms", entry, target,
      (unsigned long)ID3D10Blob_GetBufferSize(code), now_ms() - t0);
  return code;
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
  int secs = argc > 1 ? atoi(argv[1]) : 65;
  UINT W = argc > 2 ? (UINT)atoi(argv[2]) : 640, H = argc > 3 ? (UINT)atoi(argv[3]) : 480;
  /* W = fs (C6-p7, Gate P7): a borderless WS_POPUP covering the whole screen */
  const int fs = argc > 2 && !strcmp(argv[2], "fs");
  if (fs) {
    W = (UINT)GetSystemMetrics(SM_CXSCREEN);
    H = (UINT)GetSystemMetrics(SM_CYSCREEN);
  }
  if (secs < 1) secs = 1;
  row("d3d11tri start (C5-vk2): %d s, %ux%u, pid %lu", secs, W, H, (unsigned long)GetCurrentProcessId());
  const char *ov = getenv("WINEDLLOVERRIDES");
  row("env WINEDLLOVERRIDES=%s DXVK_LOG_LEVEL=%s", ov ? ov : "(unset)",
      getenv("DXVK_LOG_LEVEL") ? getenv("DXVK_LOG_LEVEL") : "(unset)");

  HMODULE md3d = load("d3d11.dll");
  load("dxgi.dll");
  HMODULE mcomp = load("d3dcompiler_47.dll");
  PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN create =
      (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(void (*)(void))GetProcAddress(md3d, "D3D11CreateDeviceAndSwapChain");
  PFN_D3DCOMPILE d3dcompile = (PFN_D3DCOMPILE)(void (*)(void))GetProcAddress(mcomp, "D3DCompile");
  if (!create || !d3dcompile) { row("FAIL GetProcAddress"); row("done rc=1"); return 1; }

  HINSTANCE hi = GetModuleHandleA(NULL);
  WNDCLASSA wc = {.lpfnWndProc = wndproc, .hInstance = hi, .lpszClassName = "d3d11tri",
                  .hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW)};
  RegisterClassA(&wc);
  RECT r = {0, 0, (LONG)W, (LONG)H};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = fs ? CreateWindowExA(WS_EX_TOPMOST, "d3d11tri", "d3d11tri.exe fullscreen", WS_POPUP, 0, 0, (int)W,
                                   (int)H, NULL, NULL, hi, NULL)
                 : CreateWindowExA(0, "d3d11tri", "d3d11tri.exe (DXVK 3.1.1, lavapipe)", WS_OVERLAPPEDWINDOW,
                                   720, 40, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
  if (!hwnd) { row("FAIL CreateWindowEx"); row("done rc=1"); return 1; }
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);

  DXGI_SWAP_CHAIN_DESC sd = {0};
  sd.BufferDesc.Width = W;
  sd.BufferDesc.Height = H;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.OutputWindow = hwnd;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ID3D11Device *dev = NULL;
  ID3D11DeviceContext *ctx = NULL;
  IDXGISwapChain *sc = NULL;
  D3D_FEATURE_LEVEL fl = 0, want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  double t0 = now_ms();
  HR(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 2, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx));
  row("D3D11CreateDeviceAndSwapChain ok in %.1f ms, feature level 0x%x", now_ms() - t0, (unsigned)fl);

  IDXGIDevice *xd = NULL;
  IDXGIAdapter *ad = NULL;
  HR(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd));
  HR(IDXGIDevice_GetAdapter(xd, &ad));
  DXGI_ADAPTER_DESC desc;
  HR(IDXGIAdapter_GetDesc(ad, &desc));
  char name[128];
  WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, NULL, NULL);
  row("adapter: \"%s\" vendor 0x%04x device 0x%04x dedicated video memory %llu MiB", name,
      desc.VendorId, desc.DeviceId, (unsigned long long)(desc.DedicatedVideoMemory >> 20));
  IDXGIAdapter_Release(ad);
  IDXGIDevice_Release(xd);

  ID3DBlob *vsb = compile(d3dcompile, "vs", "vs_5_0"), *psb = compile(d3dcompile, "ps", "ps_5_0");
  ID3D11VertexShader *vs;
  ID3D11PixelShader *ps;
  HR(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs));
  HR(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps));
  D3D11_BUFFER_DESC bd = {.ByteWidth = 16, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER};
  ID3D11Buffer *cb;
  HR(ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb));
  ID3D11Texture2D *bb;
  HR(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb));
  ID3D11RenderTargetView *rtv;
  HR(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv));
  ID3D11Texture2D_Release(bb);

  enum { MAXF = 1 << 16 };
  static double tf[MAXF];
  int n = 0, win = 0;
  double start = now_ms(), last = start, next = 5000.0, worst = 0;
  const float clear[4] = {0.06f, 0.07f, 0.11f, 1.0f};
  row("render loop: %d s", secs);
  while (!quit && now_ms() - start < secs * 1000.0) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    double f0 = now_ms();
    float a = (float)n * (6.2831853f / 180.0f), rot[4] = {cosf(a), sinf(a), 1.0f, 0};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, rot, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    HRESULT hr = IDXGISwapChain_Present(sc, 1, 0);
    if (FAILED(hr)) die("IDXGISwapChain_Present", hr);
    double f1 = now_ms();
    if (n < 3) row("frame %d: %.2f ms", n, f1 - f0);
    if (n < MAXF) tf[n] = f1 - f0;
    if (n > 0 && f1 - f0 > worst) worst = f1 - f0;
    n++; win++;
    if (f1 - start >= next) {
      row("t=%.0fs frames=%d window fps=%.2f", (f1 - start) / 1000.0, n, win * 1000.0 / (f1 - last));
      win = 0; last = f1; next += 5000.0;
    }
  }
  double wall = now_ms() - start;
  int k = n < MAXF ? n : MAXF;
  qsort(tf, (size_t)k, sizeof(double), cmp_d);
  row("RESULT frames=%d wall=%.1f ms fps=%.2f | per-frame median %.2f p90 %.2f worst(after frame 0) %.2f ms",
      n, wall, n * 1000.0 / wall, k ? tf[k / 2] : 0, k ? tf[(k * 9) / 10] : 0, worst);
  ID3D11DeviceContext_ClearState(ctx);
  ID3D11RenderTargetView_Release(rtv);
  ID3D11Buffer_Release(cb);
  ID3D11PixelShader_Release(ps);
  ID3D11VertexShader_Release(vs);
  ID3D10Blob_Release(vsb);
  ID3D10Blob_Release(psb);
  IDXGISwapChain_Release(sc);
  ID3D11DeviceContext_Release(ctx);
  ID3D11Device_Release(dev);
  DestroyWindow(hwnd);
  row("teardown: clean");
  row("done rc=0");
  return 0;
}
