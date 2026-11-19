// cp/p0guest: D3D12 probe for the P0 ladder (Wine + vkd3d-proton on Venus vs
// the host's RADV). One Windows x64 console program, built with mingw-w64:
//
//   x86_64-w64-mingw32-g++ -O2 -static -o d3d12probe.exe d3d12probe.cpp \
//       -ld3d12 -ldxgi -ld3dcompiler_47 -luser32 -lgdi32
//
//   d3d12probe.exe features
//       adapter, feature level, shader model and every CheckFeatureSupport row
//       Cyberpunk-relevant (binding tier, wave ops, raytracing, VRS, mesh
//       shaders, sampler feedback, enhanced barriers, ...), as KEY=VALUE.
//   d3d12probe.exe present   W H SECONDS ITERS DRAWS
//       borderless window at 0,0, FLIP_DISCARD swapchain, SyncInterval 0
//       (+ALLOW_TEARING when supported); per-frame CPU timestamps.
//   d3d12probe.exe offscreen W H SECONDS ITERS DRAWS
//       same rendering into a texture, no swapchain (2 frames in flight).
//   d3d12probe.exe fence     COUNT
//       empty command list + Signal + wait, COUNT times: submit->signal
//       round-trip latency (p50/p90/p99, microseconds).
//
// ITERS is the per-pixel ALU loop (GPU load); DRAWS splits the frame into
// that many 64x64-viewport draws, each with its own root constants and
// viewport (CPU/command-stream load, which is what Venus serialises).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

static LARGE_INTEGER g_qpf;
static double now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / g_qpf.QuadPart; }
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = 0x%08lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); fflush(stdout); ExitProcess(2); } } while (0)

static IDXGIFactory4 *g_factory;
static IDXGIAdapter1 *g_adapter;
static ID3D12Device *g_dev;
static ID3D12CommandQueue *g_q;
static ID3D12Fence *g_fence;
static HANDLE g_event;
static UINT64 g_fv;

static void wait_value(UINT64 v) {
	if (g_fence->GetCompletedValue() < v) {
		CK(g_fence->SetEventOnCompletion(v, g_event));
		WaitForSingleObject(g_event, INFINITE);
	}
}

static void init_device() {
	QueryPerformanceFrequency(&g_qpf);
	CK(CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory)));
	CK(g_factory->EnumAdapters1(0, &g_adapter));
	DXGI_ADAPTER_DESC1 ad;
	g_adapter->GetDesc1(&ad);
	printf("ADAPTER=%ls\nVENDOR=0x%04x\nDEVICE=0x%04x\nDEDICATED_VIDEO_MB=%llu\nSHARED_SYSTEM_MB=%llu\n",
	       ad.Description, ad.VendorId, ad.DeviceId,
	       (unsigned long long)(ad.DedicatedVideoMemory >> 20), (unsigned long long)(ad.SharedSystemMemory >> 20));
	CK(D3D12CreateDevice(g_adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)));
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	CK(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_q)));
	CK(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
	g_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	fflush(stdout);
}

#define FEAT(name, st) st name = {}; bool ok_##name = SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_##name, &name, sizeof(name)))

static int features() {
	init_device();
	D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
				   D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2 };
	D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { 5, lv, D3D_FEATURE_LEVEL_11_0 };
	if (SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
		printf("MAX_FEATURE_LEVEL=%x_%x\n", fl.MaxSupportedFeatureLevel >> 12, (fl.MaxSupportedFeatureLevel >> 8) & 0xf);
	D3D_SHADER_MODEL sms[] = { (D3D_SHADER_MODEL)0x69, (D3D_SHADER_MODEL)0x68, (D3D_SHADER_MODEL)0x67, (D3D_SHADER_MODEL)0x66,
				   (D3D_SHADER_MODEL)0x65, (D3D_SHADER_MODEL)0x64, (D3D_SHADER_MODEL)0x60, (D3D_SHADER_MODEL)0x51 };
	for (D3D_SHADER_MODEL sm : sms) {
		D3D12_FEATURE_DATA_SHADER_MODEL s = { sm };
		if (SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &s, sizeof(s)))) {
			printf("SHADER_MODEL=%x_%x\n", s.HighestShaderModel >> 4, s.HighestShaderModel & 0xf);
			break;
		}
	}
	FEAT(D3D12_OPTIONS, D3D12_FEATURE_DATA_D3D12_OPTIONS);
	if (ok_D3D12_OPTIONS) {
		printf("RESOURCE_BINDING_TIER=%d\nTILED_RESOURCES_TIER=%d\nRESOURCE_HEAP_TIER=%d\nCONSERVATIVE_RASTER_TIER=%d\n",
		       D3D12_OPTIONS.ResourceBindingTier, D3D12_OPTIONS.TiledResourcesTier, D3D12_OPTIONS.ResourceHeapTier,
		       D3D12_OPTIONS.ConservativeRasterizationTier);
		printf("ROVS=%d\nTYPED_UAV_LOAD_ADDITIONAL_FORMATS=%d\nDOUBLE_PRECISION=%d\nVP_RT_ARRAY_INDEX_ANY_SHADER=%d\nMIN_PRECISION=%d\n",
		       D3D12_OPTIONS.ROVsSupported, D3D12_OPTIONS.TypedUAVLoadAdditionalFormats, D3D12_OPTIONS.DoublePrecisionFloatShaderOps,
		       D3D12_OPTIONS.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation, D3D12_OPTIONS.MinPrecisionSupport);
	}
	FEAT(D3D12_OPTIONS1, D3D12_FEATURE_DATA_D3D12_OPTIONS1);
	if (ok_D3D12_OPTIONS1)
		printf("WAVE_OPS=%d\nWAVE_LANES_MIN=%u\nWAVE_LANES_MAX=%u\nINT64_SHADER_OPS=%d\n", D3D12_OPTIONS1.WaveOps,
		       D3D12_OPTIONS1.WaveLaneCountMin, D3D12_OPTIONS1.WaveLaneCountMax, D3D12_OPTIONS1.Int64ShaderOps);
	FEAT(D3D12_OPTIONS2, D3D12_FEATURE_DATA_D3D12_OPTIONS2);
	if (ok_D3D12_OPTIONS2)
		printf("DEPTH_BOUNDS_TEST=%d\nPROGRAMMABLE_SAMPLE_POSITIONS_TIER=%d\n", D3D12_OPTIONS2.DepthBoundsTestSupported,
		       D3D12_OPTIONS2.ProgrammableSamplePositionsTier);
	FEAT(D3D12_OPTIONS3, D3D12_FEATURE_DATA_D3D12_OPTIONS3);
	if (ok_D3D12_OPTIONS3)
		printf("COPY_QUEUE_TIMESTAMP=%d\nVIEW_INSTANCING_TIER=%d\nBARYCENTRICS=%d\n", D3D12_OPTIONS3.CopyQueueTimestampQueriesSupported,
		       D3D12_OPTIONS3.ViewInstancingTier, D3D12_OPTIONS3.BarycentricsSupported);
	FEAT(D3D12_OPTIONS4, D3D12_FEATURE_DATA_D3D12_OPTIONS4);
	if (ok_D3D12_OPTIONS4)
		printf("NATIVE_16BIT_SHADER_OPS=%d\nMSAA64KB_ALIGNED_TEXTURE=%d\n", D3D12_OPTIONS4.Native16BitShaderOpsSupported,
		       D3D12_OPTIONS4.MSAA64KBAlignedTextureSupported);
	FEAT(D3D12_OPTIONS5, D3D12_FEATURE_DATA_D3D12_OPTIONS5);
	if (ok_D3D12_OPTIONS5)
		printf("RAYTRACING_TIER=%d\nRENDER_PASSES_TIER=%d\nSRV_ONLY_TILED_RESOURCE_TIER3=%d\n", D3D12_OPTIONS5.RaytracingTier,
		       D3D12_OPTIONS5.RenderPassesTier, D3D12_OPTIONS5.SRVOnlyTiledResourceTier3);
	FEAT(D3D12_OPTIONS6, D3D12_FEATURE_DATA_D3D12_OPTIONS6);
	if (ok_D3D12_OPTIONS6)
		printf("VRS_TIER=%d\nVRS_TILE_SIZE=%u\nVRS_ADDITIONAL_RATES=%d\n", D3D12_OPTIONS6.VariableShadingRateTier,
		       D3D12_OPTIONS6.ShadingRateImageTileSize, D3D12_OPTIONS6.AdditionalShadingRatesSupported);
	FEAT(D3D12_OPTIONS7, D3D12_FEATURE_DATA_D3D12_OPTIONS7);
	if (ok_D3D12_OPTIONS7)
		printf("MESH_SHADER_TIER=%d\nSAMPLER_FEEDBACK_TIER=%d\n", D3D12_OPTIONS7.MeshShaderTier, D3D12_OPTIONS7.SamplerFeedbackTier);
	FEAT(D3D12_OPTIONS8, D3D12_FEATURE_DATA_D3D12_OPTIONS8);
	if (ok_D3D12_OPTIONS8)
		printf("UNALIGNED_BC_TEXTURES=%d\n", D3D12_OPTIONS8.UnalignedBlockTexturesSupported);
	FEAT(D3D12_OPTIONS9, D3D12_FEATURE_DATA_D3D12_OPTIONS9);
	if (ok_D3D12_OPTIONS9)
		printf("MESH_SHADER_PIPELINE_STATS=%d\nATOMIC_INT64_TYPED=%d\nWAVE_MMA_TIER=%d\n", D3D12_OPTIONS9.MeshShaderPipelineStatsSupported,
		       D3D12_OPTIONS9.AtomicInt64OnTypedResourceSupported, D3D12_OPTIONS9.WaveMMATier);
	FEAT(D3D12_OPTIONS12, D3D12_FEATURE_DATA_D3D12_OPTIONS12);
	if (ok_D3D12_OPTIONS12)
		printf("ENHANCED_BARRIERS=%d\nRELAXED_FORMAT_CASTING=%d\n", D3D12_OPTIONS12.EnhancedBarriersSupported,
		       D3D12_OPTIONS12.RelaxedFormatCastingSupported);
	FEAT(ARCHITECTURE1, D3D12_FEATURE_DATA_ARCHITECTURE1);
	if (ok_ARCHITECTURE1)
		printf("UMA=%d\nCACHE_COHERENT_UMA=%d\nISOLATED_MMU=%d\n", ARCHITECTURE1.UMA, ARCHITECTURE1.CacheCoherentUMA, ARCHITECTURE1.IsolatedMMU);
	FEAT(GPU_VIRTUAL_ADDRESS_SUPPORT, D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT);
	if (ok_GPU_VIRTUAL_ADDRESS_SUPPORT)
		printf("VA_BITS_PER_RESOURCE=%u\nVA_BITS_PER_PROCESS=%u\n", GPU_VIRTUAL_ADDRESS_SUPPORT.MaxGPUVirtualAddressBitsPerResource,
		       GPU_VIRTUAL_ADDRESS_SUPPORT.MaxGPUVirtualAddressBitsPerProcess);
	IDXGIFactory5 *f5 = NULL;
	BOOL tearing = FALSE;
	if (SUCCEEDED(g_factory->QueryInterface(IID_PPV_ARGS(&f5))))
		f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
	printf("DXGI_ALLOW_TEARING=%d\n", tearing);
	return 0;
}

static const char *g_hlsl =
	"cbuffer C : register(b0) { float t; uint iters; float2 ofs; };\n"
	"struct VSO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
	"VSO vs(uint id : SV_VertexID) { VSO o; float2 uv = float2((id << 1) & 2, id & 2);\n"
	"  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv + ofs; return o; }\n"
	"float4 ps(VSO i) : SV_Target { float2 p = i.uv * 3 - 1.5; float c = 0; float2 k = float2(sin(t), cos(t)) * 0.7885;\n"
	"  [loop] for (uint n = 0; n < iters; n++) { p = float2(p.x * p.x - p.y * p.y, 2 * p.x * p.y) + k; c += exp(-dot(p, p)); }\n"
	"  return float4(frac(c * 0.05), frac(c * 0.013), frac(t), 1); }\n";

struct Gfx {
	ID3D12RootSignature *rs;
	ID3D12PipelineState *pso;
};

static Gfx make_pipeline() {
	Gfx g = {};
	D3D12_ROOT_PARAMETER rp = {};
	rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rp.Constants.Num32BitValues = 4;
	rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	ID3DBlob *sig = NULL, *err = NULL, *vs = NULL, *ps = NULL;
	CK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
	CK(g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g.rs)));
	if (FAILED(D3DCompile(g_hlsl, strlen(g_hlsl), "probe", NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
	    FAILED(D3DCompile(g_hlsl, strlen(g_hlsl), "probe", NULL, NULL, "ps", "ps_5_0", 0, 0, &ps, &err))) {
		printf("FAIL D3DCompile: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
		ExitProcess(2);
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = g.rs;
	pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.SampleMask = 0xffffffff;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pd.SampleDesc.Count = 1;
	CK(g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.pso)));
	return g;
}

static void record(ID3D12GraphicsCommandList *cl, const Gfx &g, ID3D12Resource *rt, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
		   int W, int H, float t, UINT iters, int draws, bool present_states) {
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = rt;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = present_states ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COPY_SOURCE;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	cl->ResourceBarrier(1, &b);
	const float clear[4] = { 0.1f, 0.1f, 0.1f, 1 };
	cl->ClearRenderTargetView(rtv, clear, 0, NULL);
	cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
	cl->SetGraphicsRootSignature(g.rs);
	cl->SetPipelineState(g.pso);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	D3D12_RECT sc = { 0, 0, W, H };
	cl->RSSetScissorRects(1, &sc);
	if (draws <= 1) {
		D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
		cl->RSSetViewports(1, &vp);
		UINT c[4]; memcpy(&c[0], &t, 4); c[1] = iters; c[2] = 0; c[3] = 0;
		cl->SetGraphicsRoot32BitConstants(0, 4, c, 0);
		cl->DrawInstanced(3, 1, 0, 0);
	} else {
		int cols = W / 64 > 0 ? W / 64 : 1, rows = H / 64 > 0 ? H / 64 : 1;
		for (int d = 0; d < draws; d++) {
			int cell = d % (cols * rows);
			D3D12_VIEWPORT vp = { (float)((cell % cols) * 64), (float)((cell / cols) * 64), 64, 64, 0, 1 };
			cl->RSSetViewports(1, &vp);
			float ox = (float)(cell % cols) * 0.01f, oy = (float)(cell / cols) * 0.01f;
			UINT c[4]; memcpy(&c[0], &t, 4); c[1] = iters; memcpy(&c[2], &ox, 4); memcpy(&c[3], &oy, 4);
			cl->SetGraphicsRoot32BitConstants(0, 4, c, 0);
			cl->DrawInstanced(3, 1, 0, 0);
		}
	}
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	b.Transition.StateAfter = present_states ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COPY_SOURCE;
	cl->ResourceBarrier(1, &b);
}

static void report(const char *mode, std::vector<double> &ft, double secs) {
	// ft: per-frame CPU intervals (s). First 10% dropped as warm-up.
	size_t skip = ft.size() / 10;
	std::vector<double> v(ft.begin() + skip, ft.end());
	if (v.empty()) { printf("RESULT mode=%s frames=0\n", mode); return; }
	std::sort(v.begin(), v.end());
	double sum = 0;
	for (double x : v) sum += x;
	double med = v[v.size() / 2], p90 = v[(size_t)(v.size() * 0.9)], p99 = v[(size_t)(v.size() * 0.99)];
	printf("RESULT mode=%s frames=%zu secs=%.2f avg_fps=%.1f median_ms=%.3f p90_ms=%.3f p99_ms=%.3f fps_from_median=%.1f\n",
	       mode, ft.size(), secs, v.size() / sum, med * 1e3, p90 * 1e3, p99 * 1e3, 1.0 / med);
	fflush(stdout);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static int run_frames(bool present, int W, int H, double secs, UINT iters, int draws) {
	init_device();
	Gfx g = make_pipeline();
	const int NB = 3;
	ID3D12Resource *rt[NB] = {};
	IDXGISwapChain3 *sc = NULL;
	BOOL tearing = FALSE;
	HWND hwnd = NULL;
	if (present) {
		WNDCLASSA wc = {};
		wc.lpfnWndProc = wndproc;
		wc.hInstance = GetModuleHandleA(NULL);
		wc.lpszClassName = "cpprobe";
		RegisterClassA(&wc);
		hwnd = CreateWindowExA(0, "cpprobe", "d3d12probe", WS_POPUP | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
		IDXGIFactory5 *f5 = NULL;
		if (SUCCEEDED(g_factory->QueryInterface(IID_PPV_ARGS(&f5))))
			f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width = W; sd.Height = H;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = NB;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
		IDXGISwapChain1 *sc1 = NULL;
		CK(g_factory->CreateSwapChainForHwnd(g_q, hwnd, &sd, NULL, NULL, &sc1));
		CK(sc1->QueryInterface(IID_PPV_ARGS(&sc)));
		for (int i = 0; i < NB; i++) CK(sc->GetBuffer(i, IID_PPV_ARGS(&rt[i])));
	} else {
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rd.SampleDesc.Count = 1;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		for (int i = 0; i < NB; i++)
			CK(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, IID_PPV_ARGS(&rt[i])));
	}
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NB, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
	ID3D12DescriptorHeap *rh;
	CK(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rh)));
	UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv[NB];
	for (int i = 0; i < NB; i++) {
		rtv[i] = rh->GetCPUDescriptorHandleForHeapStart();
		rtv[i].ptr += (SIZE_T)i * inc;
		g_dev->CreateRenderTargetView(rt[i], NULL, rtv[i]);
	}
	ID3D12CommandAllocator *ca[NB];
	UINT64 done[NB] = {};
	for (int i = 0; i < NB; i++) CK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca[i])));
	ID3D12GraphicsCommandList *cl;
	CK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca[0], NULL, IID_PPV_ARGS(&cl)));
	cl->Close();
	printf("START mode=%s W=%d H=%d iters=%u draws=%d tearing=%d\n", present ? "present" : "offscreen", W, H, iters, draws, tearing);
	fflush(stdout);
	std::vector<double> ft;
	ft.reserve(1 << 20);
	double t0 = now(), last = t0, nextsec = t0 + 1;
	int frames = 0, secframes = 0, cur = 0;
	MSG msg;
	while (now() - t0 < secs) {
		while (hwnd && PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
		int i = present ? (int)sc->GetCurrentBackBufferIndex() : cur;
		wait_value(done[i]);
		CK(ca[i]->Reset());
		CK(cl->Reset(ca[i], NULL));
		record(cl, g, rt[i], rtv[i], W, H, (float)(now() - t0), iters, draws, present);
		CK(cl->Close());
		ID3D12CommandList *lists[] = { cl };
		g_q->ExecuteCommandLists(1, lists);
		if (present) CK(sc->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0));
		CK(g_q->Signal(g_fence, ++g_fv));
		done[i] = g_fv;
		cur = (cur + 1) % NB;
		double tn = now();
		ft.push_back(tn - last);
		last = tn;
		frames++; secframes++;
		if (tn >= nextsec) { printf("SEC fps=%d\n", secframes); fflush(stdout); secframes = 0; nextsec += 1; }
	}
	wait_value(g_fv);
	report(present ? "present" : "offscreen", ft, now() - t0);
	return 0;
}

static int fence_rt(int count) {
	init_device();
	ID3D12CommandAllocator *ca;
	ID3D12GraphicsCommandList *cl;
	CK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)));
	CK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, IID_PPV_ARGS(&cl)));
	CK(cl->Close());
	std::vector<double> sig, sub;
	for (int n = 0; n < count; n++) {
		ID3D12CommandList *lists[] = { cl };
		double a = now();
		g_q->ExecuteCommandLists(1, lists);
		CK(g_q->Signal(g_fence, ++g_fv));
		double b = now();
		wait_value(g_fv);
		double c = now();
		sub.push_back(b - a);
		sig.push_back(c - a);
	}
	auto pct = [](std::vector<double> v, double p) { std::sort(v.begin(), v.end()); return v[(size_t)((v.size() - 1) * p)] * 1e6; };
	printf("RESULT mode=fence count=%d submit_us_p50=%.1f submit_us_p99=%.1f roundtrip_us_p50=%.1f roundtrip_us_p90=%.1f roundtrip_us_p99=%.1f\n",
	       count, pct(sub, 0.5), pct(sub, 0.99), pct(sig, 0.5), pct(sig, 0.9), pct(sig, 0.99));
	return 0;
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const char *m = argc > 1 ? argv[1] : "features";
	if (!strcmp(m, "features")) return features();
	if (!strcmp(m, "fence")) return fence_rt(argc > 2 ? atoi(argv[2]) : 2000);
	bool present = !strcmp(m, "present");
	if (!present && strcmp(m, "offscreen")) { printf("usage: see source header\n"); return 2; }
	int W = argc > 2 ? atoi(argv[2]) : 1280, H = argc > 3 ? atoi(argv[3]) : 720;
	double secs = argc > 4 ? atof(argv[4]) : 10;
	UINT iters = argc > 5 ? (UINT)atoi(argv[5]) : 16;
	int draws = argc > 6 ? atoi(argv[6]) : 1;
	return run_frames(present, W, H, secs, iters, draws);
}
