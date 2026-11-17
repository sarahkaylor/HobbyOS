/* src/user/cp/p8d3d12caps.c - lane C8-game (docs/cp-p8.md §5): what the P8
 * D3D12 stack (vkd3d-proton 3.1 d3d12/d3d12core from the prefix's system32,
 * native overrides, on lavapipe) offers the game, before the game asks.
 * A PE console program built like the P5 probes (tools/cp/p5/build-pe.sh's
 * recipe, by tools/cp/p8/run-p8.sh); every entry point via LoadLibrary +
 * GetProcAddress so the rows name the module that answered.
 *
 * Rows "[P8CAPS] ...": the modules and their paths, D3D12CreateDevice at
 * 11_0 and at 12_0, the highest feature level (FEATURE_LEVELS 11_0..12_2),
 * the highest shader model (asked 6_7), resource binding / tiled / heap tiers,
 * wave ops, raytracing and mesh-shader tiers, and the adapter (DXGI).
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void row(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  printf("[P8CAPS] %s\n", b);
  fflush(stdout);
}

static HMODULE load(const char *name) {
  HMODULE m = LoadLibraryA(name);
  char path[MAX_PATH] = "?";
  if (m) GetModuleFileNameA(m, path, sizeof path);
  row("module %-14s %s", name, m ? path : "LOAD FAILED");
  return m;
}

int main(void) {
  row("start (C8-game)");
  HMODULE m12 = load("d3d12.dll");
  HMODULE mcore = load("d3d12core.dll");
  HMODULE mdxgi = load("dxgi.dll");
  (void)mcore;
  if (!m12 || !mdxgi) { row("done rc=1"); return 1; }
  PFN_D3D12_CREATE_DEVICE create = (PFN_D3D12_CREATE_DEVICE)(void (*)(void))GetProcAddress(m12, "D3D12CreateDevice");
  typedef HRESULT (WINAPI *PFN_CF1)(REFIID, void **);
  PFN_CF1 mkfactory = (PFN_CF1)(void (*)(void))GetProcAddress(mdxgi, "CreateDXGIFactory1");
  if (!create || !mkfactory) { row("FAIL GetProcAddress"); row("done rc=1"); return 1; }

  IDXGIFactory1 *fac = NULL;
  IDXGIAdapter1 *ad = NULL;
  if (SUCCEEDED(mkfactory(&IID_IDXGIFactory1, (void **)&fac)) &&
      SUCCEEDED(IDXGIFactory1_EnumAdapters1(fac, 0, &ad))) {
    DXGI_ADAPTER_DESC1 d;
    if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d)))
      row("adapter: \"%ls\" vendor 0x%04x device 0x%04x dedicated %llu MiB shared %llu MiB",
          d.Description, d.VendorId, d.DeviceId,
          (unsigned long long)(d.DedicatedVideoMemory >> 20), (unsigned long long)(d.SharedSystemMemory >> 20));
  } else {
    row("adapter: CreateDXGIFactory1/EnumAdapters1 failed");
  }
  /* C6-venus (P8 stop 11): every adapter's outputs, their mode lists and the
   * memory budget -- what the game sizes its display and streaming lists from. */
  for (UINT ai = 0; fac; ai++) {
    IDXGIAdapter1 *a = NULL;
    if (FAILED(IDXGIFactory1_EnumAdapters1(fac, ai, &a))) { row("adapters: %u", ai); break; }
    DXGI_ADAPTER_DESC1 d;
    IDXGIAdapter1_GetDesc1(a, &d);
    row("adapter[%u] \"%ls\" luid %08lx:%08lx flags 0x%x", ai, d.Description,
        (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart, d.Flags);
    IDXGIAdapter3 *a3 = NULL;
    if (SUCCEEDED(IDXGIAdapter1_QueryInterface(a, &IID_IDXGIAdapter3, (void **)&a3))) {
      for (int g = 0; g < 2; g++) {
        DXGI_QUERY_VIDEO_MEMORY_INFO mi;
        memset(&mi, 0, sizeof mi);
        HRESULT hq = IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, g ? DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL
                                                                 : DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi);
        row("adapter[%u] budget %s hr=0x%08lx budget %llu MiB usage %llu MiB reservation %llu MiB", ai,
            g ? "non-local" : "local", (unsigned long)hq, (unsigned long long)(mi.Budget >> 20),
            (unsigned long long)(mi.CurrentUsage >> 20), (unsigned long long)(mi.AvailableForReservation >> 20));
      }
      IDXGIAdapter3_Release(a3);
    }
    for (UINT oi = 0;; oi++) {
      IDXGIOutput *o = NULL;
      HRESULT ho = IDXGIAdapter1_EnumOutputs(a, oi, &o);
      if (FAILED(ho)) { row("adapter[%u] outputs: %u (hr=0x%08lx)", ai, oi, (unsigned long)ho); break; }
      DXGI_OUTPUT_DESC od;
      IDXGIOutput_GetDesc(o, &od);
      row("adapter[%u] output[%u] \"%ls\" attached %d rect %ld,%ld-%ld,%ld rot %d", ai, oi, od.DeviceName,
          od.AttachedToDesktop, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
          od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, (int)od.Rotation);
      DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM};
      for (int fi = 0; fi < 3; fi++) {
        UINT n = 0;
        HRESULT hm = IDXGIOutput_GetDisplayModeList(o, fmts[fi], 0, &n, NULL);
        static DXGI_MODE_DESC md[256];
        UINT k = n > 256 ? 256 : n;
        if (SUCCEEDED(hm) && k) IDXGIOutput_GetDisplayModeList(o, fmts[fi], 0, &k, md);
        if (SUCCEEDED(hm) && k)
          row("  fmt %d: %u modes, first %ux%u@%u/%u last %ux%u@%u/%u", (int)fmts[fi], n, md[0].Width,
              md[0].Height, md[0].RefreshRate.Numerator, md[0].RefreshRate.Denominator, md[k - 1].Width,
              md[k - 1].Height, md[k - 1].RefreshRate.Numerator, md[k - 1].RefreshRate.Denominator);
        else
          row("  fmt %d: %u modes hr=0x%08lx", (int)fmts[fi], n, (unsigned long)hm);
      }
      IDXGIOutput_Release(o);
    }
    IDXGIAdapter1_Release(a);
  }

  ID3D12Device *dev12 = NULL;
  HRESULT h12 = create((IUnknown *)ad, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev12);
  row("D3D12CreateDevice(FL 12_0) hr=0x%08lx", (unsigned long)h12);
  if (dev12) ID3D12Device_Release(dev12);
  ID3D12Device *dev = NULL;
  DWORD t0 = GetTickCount();
  HRESULT h = create((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
  row("D3D12CreateDevice(FL 11_0) hr=0x%08lx in %lu ms", (unsigned long)h, (unsigned long)(GetTickCount() - t0));
  if (FAILED(h) || !dev) { row("done rc=1"); return 1; }

  D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                               D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2 };
  D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { ARRAYSIZE(want), want, 0 };
  if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl)))
    row("max feature level 0x%x", (unsigned)fl.MaxSupportedFeatureLevel);
  D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_7 };
  HRESULT hs = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm);
  if (FAILED(hs)) { sm.HighestShaderModel = D3D_SHADER_MODEL_6_5;
    hs = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm); }
  row("highest shader model 0x%x (hr=0x%08lx)", (unsigned)sm.HighestShaderModel, (unsigned long)hs);
  D3D12_FEATURE_DATA_D3D12_OPTIONS o = { 0 };
  if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o)))
    row("options: ResourceBindingTier %d TiledResourcesTier %d ResourceHeapTier %d ConservativeRaster %d ROVs %d TypedUAVLoadAdditionalFormats %d",
        o.ResourceBindingTier, o.TiledResourcesTier, o.ResourceHeapTier, o.ConservativeRasterizationTier,
        o.ROVsSupported, o.TypedUAVLoadAdditionalFormats);
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = { 0 };
  if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1)))
    row("options1: WaveOps %d WaveLaneCount %u..%u Int64ShaderOps %d", o1.WaveOps, o1.WaveLaneCountMin,
        o1.WaveLaneCountMax, o1.Int64ShaderOps);
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = { 0 };
  if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)))
    row("options5: RaytracingTier %d RenderPassesTier %d", o5.RaytracingTier, o5.RenderPassesTier);
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = { 0 };
  if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7)))
    row("options7: MeshShaderTier %d SamplerFeedbackTier %d", o7.MeshShaderTier, o7.SamplerFeedbackTier);
  ID3D12Device_Release(dev);
  row("done rc=0");
  return 0;
}
