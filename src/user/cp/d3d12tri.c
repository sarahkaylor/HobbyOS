/* src/user/cp/d3d12tri.c — a D3D12 triangle for Gate P5 (lane C5-vk2,
 * docs/cp-vk.md §8; docs/cyberpunk.md P5): vkd3d-proton 3.1's d3d12.dll +
 * d3d12core.dll (with DXVK 3.1.1's dxgi.dll, the Proton pairing) under Wine ->
 * winevulkan -> lavapipe, presented in a Win32 window by winex11.  This is the
 * API path Cyberpunk 2077 takes.
 *
 *   d3d12tri.exe [seconds] [W] [H]          default: 65 640 480
 *
 * Built like d3d11tri.c (tools/cp/p5/build-pe.sh).  Entry points come from
 * LoadLibrary/GetProcAddress so the log names the module that answered.  HLSL
 * is compiled at run time by d3dcompiler_47.dll to DXBC (vkd3d-proton
 * translates DXBC itself); the triangle comes from SV_VertexID and spins via
 * a root constant.  One frame in flight, a fence per frame.
 *
 * Rows "[D3D12] ...": modules, adapter (DXGI_ADAPTER_DESC1 from
 * EnumAdapterByLuid on the device's LUID), a progress row every 5 s, RESULT,
 * "[D3D12] done rc=0".  A failing HRESULT ends with "done rc=1" and, after
 * device removal, GetDeviceRemovedReason.
 */
#define COBJMACROS
#define INITGUID
/* widl's C bindings return aggregates (GetAdapterLuid, GetCPUDescriptorHandle-
 * ForHeapStart) through the MSVC hidden out-parameter only via its inline
 * wrappers; without this the macros expand to a deliberate compile error. */
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define NBUF 2

static void row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[D3D12] ", stdout);
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

static ID3D12Device *dev;

static void die(const char *what, HRESULT hr) {
  row("FAIL %s -> hr=0x%08lx", what, (unsigned long)hr);
  if (dev) row("GetDeviceRemovedReason -> 0x%08lx", (unsigned long)ID3D12Device_GetDeviceRemovedReason(dev));
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
    "  float3 c[3] = { float3(1, 0.85, 0.1), float3(0.9, 0.2, 0.9), float3(0.1, 0.85, 0.9) };\n"
    "  float2 q = p[id];\n"
    "  V o; o.pos = float4(q.x * rot.x - q.y * rot.y, q.x * rot.y + q.y * rot.x, 0.5, 1);\n"
    "  o.col = c[id]; return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(i.col, 1); }\n";

typedef HRESULT(WINAPI *PFN_D3DCOMPILE)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *,
                                        ID3DInclude *, const char *, const char *, UINT, UINT,
                                        ID3DBlob **, ID3DBlob **);
typedef HRESULT(WINAPI *PFN_CREATEDXGIFACTORY2)(UINT, REFIID, void **);

static ID3DBlob *compile(PFN_D3DCOMPILE fn, const char *entry, const char *target) {
  ID3DBlob *code = NULL, *err = NULL;
  double t0 = now_ms();
  HRESULT hr = fn(hlsl, sizeof hlsl - 1, "d3d12tri.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
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

static void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a,
                    D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER rb = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
  rb.Transition.pResource = r;
  rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  rb.Transition.StateBefore = a;
  rb.Transition.StateAfter = b;
  ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &rb);
}

int main(int argc, char **argv) {
  int secs = argc > 1 ? atoi(argv[1]) : 65;
  UINT W = argc > 2 ? (UINT)atoi(argv[2]) : 640, H = argc > 3 ? (UINT)atoi(argv[3]) : 480;
  if (secs < 1) secs = 1;
  row("d3d12tri start (C5-vk2): %d s, %ux%u, pid %lu", secs, W, H, (unsigned long)GetCurrentProcessId());
  const char *ov = getenv("WINEDLLOVERRIDES");
  row("env WINEDLLOVERRIDES=%s VKD3D_DEBUG=%s", ov ? ov : "(unset)",
      getenv("VKD3D_DEBUG") ? getenv("VKD3D_DEBUG") : "(unset)");

  HMODULE m12 = load("d3d12.dll");
  load("d3d12core.dll");
  HMODULE mdxgi = load("dxgi.dll");
  HMODULE mcomp = load("d3dcompiler_47.dll");
  PFN_D3D12_CREATE_DEVICE create = (PFN_D3D12_CREATE_DEVICE)(void (*)(void))GetProcAddress(m12, "D3D12CreateDevice");
  PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize =
      (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void (*)(void))GetProcAddress(m12, "D3D12SerializeRootSignature");
  PFN_CREATEDXGIFACTORY2 mkfactory = (PFN_CREATEDXGIFACTORY2)(void (*)(void))GetProcAddress(mdxgi, "CreateDXGIFactory2");
  PFN_D3DCOMPILE d3dcompile = (PFN_D3DCOMPILE)(void (*)(void))GetProcAddress(mcomp, "D3DCompile");
  if (!create || !serialize || !mkfactory || !d3dcompile) { row("FAIL GetProcAddress"); row("done rc=1"); return 1; }

  HINSTANCE hi = GetModuleHandleA(NULL);
  WNDCLASSA wc = {.lpfnWndProc = wndproc, .hInstance = hi, .lpszClassName = "d3d12tri",
                  .hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW)};
  RegisterClassA(&wc);
  RECT r = {0, 0, (LONG)W, (LONG)H};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowExA(0, "d3d12tri", "d3d12tri.exe (vkd3d-proton 3.1, lavapipe)", WS_OVERLAPPEDWINDOW,
                              720, 560, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
  if (!hwnd) { row("FAIL CreateWindowEx"); row("done rc=1"); return 1; }
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);

  IDXGIFactory4 *fac;
  HR(mkfactory(0, &IID_IDXGIFactory4, (void **)&fac));
  double t0 = now_ms();
  HR(create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
  row("D3D12CreateDevice ok in %.1f ms", now_ms() - t0);
  LUID luid = ID3D12Device_GetAdapterLuid(dev);
  IDXGIAdapter1 *ad;
  HR(IDXGIFactory4_EnumAdapterByLuid(fac, luid, &IID_IDXGIAdapter1, (void **)&ad));
  DXGI_ADAPTER_DESC1 desc;
  HR(IDXGIAdapter1_GetDesc1(ad, &desc));
  char name[128];
  WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, NULL, NULL);
  row("adapter: \"%s\" vendor 0x%04x device 0x%04x dedicated video memory %llu MiB", name,
      desc.VendorId, desc.DeviceId, (unsigned long long)(desc.DedicatedVideoMemory >> 20));
  IDXGIAdapter1_Release(ad);
  D3D12_FEATURE_DATA_SHADER_MODEL smq = {D3D_SHADER_MODEL_6_0};
  if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &smq, sizeof smq)))
    row("highest shader model (asked 6.0): 0x%x", (unsigned)smq.HighestShaderModel);

  D3D12_COMMAND_QUEUE_DESC qd = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  ID3D12CommandQueue *q;
  HR(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));

  DXGI_SWAP_CHAIN_DESC1 sd = {.Width = W, .Height = H, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                              .SampleDesc = {1, 0}, .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
                              .BufferCount = NBUF, .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD};
  IDXGISwapChain1 *sc1;
  t0 = now_ms();
  HR(IDXGIFactory4_CreateSwapChainForHwnd(fac, (IUnknown *)q, hwnd, &sd, NULL, NULL, &sc1));
  IDXGISwapChain3 *sc;
  HR(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));
  row("CreateSwapChainForHwnd ok in %.1f ms (%u buffers, FLIP_DISCARD)", now_ms() - t0, NBUF);

  D3D12_DESCRIPTOR_HEAP_DESC hd = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = NBUF};
  ID3D12DescriptorHeap *heap;
  HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
  UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE h0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
  ID3D12Resource *bufs[NBUF];
  for (UINT i = 0; i < NBUF; i++) {
    HR(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bufs[i]));
    D3D12_CPU_DESCRIPTOR_HANDLE h = {h0.ptr + (SIZE_T)i * inc};
    ID3D12Device_CreateRenderTargetView(dev, bufs[i], NULL, h);
  }

  D3D12_ROOT_PARAMETER rp = {.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                             .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX};
  rp.Constants.ShaderRegister = 0;
  rp.Constants.Num32BitValues = 4;
  D3D12_ROOT_SIGNATURE_DESC rsd = {.NumParameters = 1, .pParameters = &rp};
  ID3DBlob *rsb = NULL, *rse = NULL;
  HR(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse));
  ID3D12RootSignature *rs;
  HR(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb),
                                      &IID_ID3D12RootSignature, (void **)&rs));

  ID3DBlob *vsb = compile(d3dcompile, "vs", "vs_5_0"), *psb = compile(d3dcompile, "ps", "ps_5_0");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
  pd.pRootSignature = rs;
  pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb);
  pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
  pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb);
  pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = 0xffffffffu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *pso;
  t0 = now_ms();
  HR(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
  row("CreateGraphicsPipelineState ok in %.1f ms", now_ms() - t0);

  ID3D12CommandAllocator *alloc;
  HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                         (void **)&alloc));
  ID3D12GraphicsCommandList *cl;
  HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso,
                                    &IID_ID3D12GraphicsCommandList, (void **)&cl));
  HR(ID3D12GraphicsCommandList_Close(cl));
  ID3D12Fence *fence;
  HR(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
  HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
  UINT64 fv = 0;

  enum { MAXF = 1 << 16 };
  static double tf[MAXF];
  int n = 0, win = 0;
  double start = now_ms(), last = start, next = 5000.0, worst = 0;
  const float clear[4] = {0.06f, 0.07f, 0.11f, 1.0f};
  D3D12_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
  D3D12_RECT sr = {0, 0, (LONG)W, (LONG)H};
  row("render loop: %d s", secs);
  while (!quit && now_ms() - start < secs * 1000.0) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    double f0 = now_ms();
    UINT bi = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
    HR(ID3D12CommandAllocator_Reset(alloc));
    HR(ID3D12GraphicsCommandList_Reset(cl, alloc, pso));
    barrier(cl, bufs[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE h = {h0.ptr + (SIZE_T)bi * inc};
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &h, FALSE, NULL);
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, h, clear, 0, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    float a = -(float)n * (6.2831853f / 180.0f), rot[4] = {cosf(a), sinf(a), 1.0f, 0};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 4, rot, 0);
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sr);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    barrier(cl, bufs[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    HR(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)cl};
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
    HRESULT hr = IDXGISwapChain3_Present(sc, 1, 0);
    if (FAILED(hr)) die("IDXGISwapChain3_Present", hr);
    HR(ID3D12CommandQueue_Signal(q, fence, ++fv));
    if (ID3D12Fence_GetCompletedValue(fence) < fv) {
      HR(ID3D12Fence_SetEventOnCompletion(fence, fv, ev));
      if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) die("fence wait timed out (10 s)", E_FAIL);
    }
    double f1 = now_ms();
    if (n < 3) row("frame %d: %.2f ms (back buffer %u)", n, f1 - f0, bi);
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
  CloseHandle(ev);
  ID3D12Fence_Release(fence);
  ID3D12GraphicsCommandList_Release(cl);
  ID3D12CommandAllocator_Release(alloc);
  ID3D12PipelineState_Release(pso);
  ID3D12RootSignature_Release(rs);
  for (UINT i = 0; i < NBUF; i++) ID3D12Resource_Release(bufs[i]);
  ID3D12DescriptorHeap_Release(heap);
  IDXGISwapChain3_Release(sc);
  IDXGISwapChain1_Release(sc1);
  ID3D12CommandQueue_Release(q);
  IDXGIFactory4_Release(fac);
  ID3D12Device_Release(dev);
  DestroyWindow(hwnd);
  row("teardown: clean");
  row("done rc=0");
  return 0;
}
