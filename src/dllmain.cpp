#include "common.h"
#include "config.h"
#include "ffx_upscaler.h"
#include "menu.h"
#include "il2cpp_demosaic.h"
#include "il2cpp_mvec.h"

#include <dxgi.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <chrono>

#include "MinHook.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace lillia {

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainFactory(IDXGIFactory* f, IUnknown* dev,
                                                           DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);
static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc1, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc,
    IDXGIOutput* restrictOut, IDXGISwapChain1** out);
static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags);
static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT bufferCount,
                                                  UINT w, UINT h, DXGI_FORMAT format, UINT flags);

// ---------------- dxgi 转发 ----------------
static HMODULE s_realDxgi = nullptr;

bool proxy_init() {
    if (s_realDxgi) return true;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wcscat_s(sys, L"\\dxgi.dll");
    s_realDxgi = LoadLibraryW(sys);
    return s_realDxgi != nullptr;
}

void* proxy_proc(const char* name) {
    return proxy_init() ? (void*)GetProcAddress(s_realDxgi, name) : nullptr;
}

// ---------------- FSR 管线状态 ----------------
struct Pipeline {
    ID3D11Device*           dev11      = nullptr;
    ID3D11Device1*          dev11_1    = nullptr;   // OpenSharedResource1
    ID3D11DeviceContext*    ctx11      = nullptr;
    ID3D11Device5*          dev11_5    = nullptr;   // OpenSharedFence
    ID3D11DeviceContext4*   ctx11_4    = nullptr;   // GPU Wait/Signal
    ID3D12Device*           dev12      = nullptr;
    ID3D12Device1*          dev12_1    = nullptr;   // CreateSharedHandle
    ID3D12CommandQueue*     queue12    = nullptr;
    ID3D12CommandAllocator* alloc12    = nullptr;
    ID3D12GraphicsCommandList* cl12    = nullptr;
    ID3D12Fence* fenceA12  = nullptr; // 11 signal -> 12 wait
    ID3D12Fence* fenceB12  = nullptr; // 12 signal -> 11 wait
    ID3D11Fence* fenceA11  = nullptr;
    ID3D11Fence* fenceB11  = nullptr;
    uint64_t fenceVal = 0;
    ID3D12Resource* color12 = nullptr; // game frame @ render res (RT, shared)
    ID3D12Resource* out12   = nullptr; // FSR 输出 (私有 UAV, 不共享)
    ID3D12Resource* mid12   = nullptr; // 中转 (RT, shared) — out12 的跨 API 桥
    ID3D11Resource* colorIn11 = nullptr;
    ID3D11Resource* midIn11   = nullptr;
    // 真 mvec/depth 通道 (11 create -> 12 open)
    ID3D11Resource* mvecShared11 = nullptr;
    ID3D11Resource* depthShared11 = nullptr;
    ID3D12Resource* mvecShared12 = nullptr;
    ID3D12Resource* depthShared12 = nullptr;
    // mvec 转换 CS (Unity [0,1]uv-mvec -> 像素 delta RG16F)
    ID3D11ComputeShader* mvecCS = nullptr;
    ID3D11Buffer* mvecCB = nullptr;
    ID3D11UnorderedAccessView* mvecUAV = nullptr;
    IDXGISwapChain*  sc   = nullptr;       // 游戏 SC (低分辨率, 渲染用, Present 被吞)
    IDXGISwapChain3* sc1  = nullptr;
    IDXGISwapChain*  hiddenSC   = nullptr; // 隐藏 SC (全尺寸, 唯一真正 Present)
    IDXGISwapChain3* hiddenSC3  = nullptr;
    uint32_t bbW = 0, bbH = 0;
    uint32_t renderW = 0, renderH = 0;
    DXGI_FORMAT bbFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D11RenderTargetView* rtvs[8] = {};
    FfxUpscaler ffx;
    bool ffxReady = false;
    std::chrono::steady_clock::time_point lastFrame{};
    float lastDeltaMs = 16.6f;
    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER t0{}, t1{};
    bool frameTimingValid = false;
};
static Pipeline P;

static void pipeline_destroy_resources() {
    if (P.ffxReady) { P.ffx.shutdown(); P.ffxReady = false; }
    for (auto& r : P.rtvs) { if (r) { r->Release(); r = nullptr; } }
    if (P.colorIn11) { P.colorIn11->Release(); P.colorIn11 = nullptr; }
    if (P.midIn11)   { P.midIn11->Release();   P.midIn11 = nullptr; }
    if (P.mvecShared11) { P.mvecShared11->Release(); P.mvecShared11 = nullptr; }
    if (P.depthShared11){ P.depthShared11->Release(); P.depthShared11 = nullptr; }
    if (P.mvecShared12) { P.mvecShared12->Release(); P.mvecShared12 = nullptr; }
    if (P.depthShared12){ P.depthShared12->Release(); P.depthShared12 = nullptr; }
    if (P.mvecUAV) { P.mvecUAV->Release(); P.mvecUAV = nullptr; }
    if (P.mvecCS)  { P.mvecCS->Release();  P.mvecCS = nullptr; }
    if (P.mvecCB)  { P.mvecCB->Release();  P.mvecCB = nullptr; }
    if (P.color12)   { P.color12->Release();   P.color12 = nullptr; }
    if (P.out12)     { P.out12->Release();     P.out12 = nullptr; }
    if (P.mid12)     { P.mid12->Release();     P.mid12 = nullptr; }
}

static bool wait_gpu_idle_12() {
    if (!P.queue12 || !P.fenceB12) return true;
    P.fenceVal++;
    if (FAILED(P.queue12->Signal(P.fenceB12, P.fenceVal))) return false;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    P.fenceB12->SetEventOnCompletion(P.fenceVal, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);
    return true;
}

static void pipeline_shutdown() {
    wait_gpu_idle_12();
    pipeline_destroy_resources();
    if (P.fenceA11) { P.fenceA11->Release(); P.fenceA11 = nullptr; }
    if (P.fenceB11) { P.fenceB11->Release(); P.fenceB11 = nullptr; }
    if (P.fenceA12) { P.fenceA12->Release(); P.fenceA12 = nullptr; }
    if (P.fenceB12) { P.fenceB12->Release(); P.fenceB12 = nullptr; }
    if (P.cl12)   { P.cl12->Release();   P.cl12 = nullptr; }
    if (P.alloc12){ P.alloc12->Release(); P.alloc12 = nullptr; }
    if (P.queue12){ P.queue12->Release(); P.queue12 = nullptr; }
    if (P.dev12_1){ P.dev12_1->Release(); P.dev12_1 = nullptr; }
    if (P.dev12)  { P.dev12->Release();  P.dev12 = nullptr; }
    if (P.ctx11_4){ P.ctx11_4->Release(); P.ctx11_4 = nullptr; }
    if (P.dev11_5){ P.dev11_5->Release(); P.dev11_5 = nullptr; }
    if (P.dev11_1){ P.dev11_1->Release(); P.dev11_1 = nullptr; }
    if (P.ctx11)  { P.ctx11->Release();  P.ctx11 = nullptr; }
    if (P.dev11)  { P.dev11->Release();  P.dev11 = nullptr; }
    if (P.sc1)    { P.sc1->Release();    P.sc1 = nullptr; }
    if (P.sc)     { P.sc->Release();     P.sc = nullptr; }
    // hiddenSC 保留 — 属于我们, 与 D3D 管线生命周期无关
}

static bool pipeline_build_resources(uint32_t rw, uint32_t rh, uint32_t bbW, uint32_t bbH) {
    P.bbW = bbW; P.bbH = bbH;
    P.renderW = rw; P.renderH = rh;
    LOG_I("pipeline build: render=%ux%u display=%ux%u", rw, rh, bbW, bbH);

    D3D12_HEAP_PROPERTIES dhp{}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto mkTex = [&](uint32_t w, uint32_t h, bool uav) -> ID3D12Resource* {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = P.bbFmt;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                 | (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        D3D12_CLEAR_VALUE cv{}; cv.Format = P.bbFmt;
        ID3D12Resource* res = nullptr;
        // D3D12_HEAP_FLAG_SHARED => NT handle 可共享 (committed + shared)
        HRESULT hr = P.dev12->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_SHARED, &td,
            D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(&res));
        if (FAILED(hr)) { LOG_E("shared tex create failed hr=0x%08lX (%ux%u uav=%d)", hr, w, h, uav); return nullptr; }
        return res;
    };
    auto mkTexPrivate = [&](uint32_t w, uint32_t h) -> ID3D12Resource* {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = P.bbFmt;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* res = nullptr;
        HRESULT hr = P.dev12->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res));
        if (FAILED(hr)) { LOG_E("private uav tex create failed hr=0x%08lX (%ux%u)", hr, w, h); return nullptr; }
        return res;
    };
    P.color12 = mkTex(rw, rh, false);
    P.out12   = mkTexPrivate(bbW, bbH);   // UAV 私有: D3D11 侧打不开 RT+UAV 共享纹理
    P.mid12   = mkTex(bbW, bbH, false);   // RT 共享中转 (与 color12 同 flags, 已验证可开)
    if (!P.color12 || !P.out12 || !P.mid12) return false;

    // NT shared handles (ID3D12Device1::CreateSharedHandle)
    HANDLE nt_color = nullptr, nt_mid = nullptr;
    SECURITY_ATTRIBUTES sa{};
    if (FAILED(P.dev12_1->CreateSharedHandle(P.color12, &sa, GENERIC_ALL, nullptr, &nt_color)) ||
        FAILED(P.dev12_1->CreateSharedHandle(P.mid12, &sa, GENERIC_ALL, nullptr, &nt_mid))) {
        LOG_E("CreateSharedHandle(textures) failed");
        return false;
    }

    auto open11 = [&](HANDLE h, ID3D11Resource** out11) -> bool {
        return SUCCEEDED(P.dev11_1->OpenSharedResource1(h, __uuidof(ID3D11Resource), (void**)out11));
    };
    if (!open11(nt_color, &P.colorIn11)) { LOG_E("open color shared on 11 failed"); return false; }
    if (!open11(nt_mid, &P.midIn11))     { LOG_E("open mid shared on 11 failed"); return false; }

    // 11 create -> 12 open: 真 mvec/depth 通道
    auto mkShared11 = [&](DXGI_FORMAT fmt, bool uav, ID3D11Resource** t11, ID3D12Resource** t12) -> bool {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = rw; d.Height = rh;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ID3D11Texture2D* tex = nullptr;
        HRESULT chr = P.dev11->CreateTexture2D(&d, nullptr, &tex);
        if (FAILED(chr)) {
            LOG_E("shared11 fmt=%u create failed hr=0x%08lX", (unsigned)fmt, (unsigned)chr); return false;
        }
        *t11 = tex;
        IDXGIResource1* r1 = nullptr;
        HANDLE h = nullptr;
        bool ok = SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1));
        if (ok) { ok = SUCCEEDED(r1->GetSharedHandle(&h)); r1->Release(); }
        if (ok) ok = SUCCEEDED(P.dev12_1->OpenSharedHandle(h, IID_PPV_ARGS(t12)));
        if (!ok) { LOG_E("shared11 %u open on 12 failed", (unsigned)fmt); return false; }
        return true;
    };
    bool mvecOK = mkShared11(DXGI_FORMAT_R16G16_FLOAT, true, &P.mvecShared11, &P.mvecShared12);
    bool depthOK = mkShared11(DXGI_FORMAT_R32_FLOAT, false, &P.depthShared11, &P.depthShared12);
    if (!mvecOK || !depthOK) { LOG_W("mvec/depth shared channels unavailable (zero fallback)"); }
    else {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R16G16_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        P.dev11->CreateUnorderedAccessView(P.mvecShared11, &ud, &P.mvecUAV);
        LOG_I("mvec/depth shared channels ready (%ux%u)", rw, rh);
    }

    FfxUpscaler::listVersions(P.dev12);
    if (!P.ffx.init(P.dev12, P.queue12, rw, rh, bbW, bbH, g_pendingVersionId)) {
        LOG_W("FFX context init failed - upscale disabled, passthrough");
        return false;
    }
    P.ffxReady = true;
    return true;
}

// ---------------- Present / Resize hooks ----------------
typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn o_Present = nullptr;
typedef HRESULT (STDMETHODCALLTYPE *ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static ResizeBuffersFn o_ResizeBuffers = nullptr;
typedef HRESULT (STDMETHODCALLTYPE *CreateSwapChainFn)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static CreateSwapChainFn o_CreateSwapChain = nullptr;
typedef decltype(&D3D11CreateDeviceAndSwapChain) D3D11CreateDeviceAndSwapChainFn;
static D3D11CreateDeviceAndSwapChainFn o_D3D11CreateSC = nullptr;

static UINT cur_backbuffer_index() {
    return P.sc1 ? P.sc1->GetCurrentBackBufferIndex() : 0;
}
static UINT hidden_backbuffer_index() {
    return P.hiddenSC3 ? P.hiddenSC3->GetCurrentBackBufferIndex() : 0;
}

static bool ensure_pipeline(IDXGISwapChain* sc) {
    if (P.dev11) return sc == P.sc;      // 已初始化: 只认主 gameSC
    DXGI_SWAP_CHAIN_DESC gd{};
    if (FAILED(sc->GetDesc(&gd))) return false;
    if (!gd.BufferDesc.Width || !gd.BufferDesc.Height) { LOG_E("gameSC zero size"); return false; }

    P.bbFmt = gd.BufferDesc.Format;
    if (P.bbFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) P.bbFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (P.bbFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)  P.bbFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (P.bbFmt != DXGI_FORMAT_B8G8R8A8_UNORM && P.bbFmt != DXGI_FORMAT_R8G8B8A8_UNORM)
        LOG_W("unusual backbuffer format %u", (unsigned)P.bbFmt);

    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&P.dev11))) {
        LOG_E("swapchain GetDevice(D3D11) failed");
        return false;
    }
    P.dev11->GetImmediateContext(&P.ctx11);
    P.dev11->QueryInterface(__uuidof(ID3D11Device1), (void**)&P.dev11_1);
    P.dev11->QueryInterface(__uuidof(ID3D11Device5), (void**)&P.dev11_5);
    P.ctx11->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&P.ctx11_4);
    if (!P.dev11_1 || !P.dev11_5 || !P.ctx11_4) { LOG_E("D3D11.1/5/Context4 unavailable"); return false; }
    P.sc = sc; P.sc->AddRef();
    sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&P.sc1);
    if (!P.sc1) LOG_W("IDXGISwapChain3 unavailable - menu RTV index fixed at 0");

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    P.dev11->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    dxgiDev->GetAdapter(&adapter);
    dxgiDev->Release();
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&P.dev12));
    adapter->Release();
    if (FAILED(hr)) { LOG_E("D3D12CreateDevice failed hr=0x%08lX", hr); return false; }
    P.dev12->QueryInterface(__uuidof(ID3D12Device1), (void**)&P.dev12_1);
    if (!P.dev12_1) { LOG_E("ID3D12Device1 unavailable"); return false; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    if (FAILED(P.dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&P.queue12)))) return false;
    P.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&P.alloc12));
    P.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, P.alloc12, nullptr, IID_PPV_ARGS(&P.cl12));
    P.cl12->Close();

    auto mkFence = [&](ID3D12Fence** f12, ID3D11Fence** f11) -> bool {
        if (FAILED(P.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(f12)))) return false;
        HANDLE h = nullptr;
        SECURITY_ATTRIBUTES sa{};
        if (FAILED(P.dev12_1->CreateSharedHandle(*f12, &sa, GENERIC_ALL, nullptr, &h))) return false;
        return SUCCEEDED(P.dev11_5->OpenSharedFence(h, __uuidof(ID3D11Fence), (void**)f11));
    };
    if (!mkFence(&P.fenceA12, &P.fenceA11)) { LOG_E("fenceA create/open failed"); return false; }
    if (!mkFence(&P.fenceB12, &P.fenceB11)) { LOG_E("fenceB create/open failed"); return false; }

    // mvec 转换 CS: Unity uv-mvec(0.5=静止) -> 像素 delta (RG16F)
    static const char kCs[] =
        "Texture2D<float4> gSrc : register(t0);\n"
        "RWTexture2D<float2> gDst : register(u0);\n"
        "cbuffer cb : register(b0) { float4 gParam; }; // xy = ±renderSize, zw pad\n"
        "[numthreads(8,8,1)]\n"
        "void cs_main(uint3 tid : SV_DispatchThreadID) {\n"
        "  float2 u = gSrc[tid.xy].xy;\n"
        "  float2 pix = (u * 2.0 - 1.0) * gParam.xy;\n"
        "  gDst[tid.xy] = pix;\n"
        "}\n";
    {
        ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
        HRESULT cshr = D3DCompile(kCs, sizeof(kCs) - 1, nullptr, nullptr, nullptr,
                                  "cs_main", "cs_5_0", 0, 0, &code, &err);
        if (SUCCEEDED(cshr)) {
            P.dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &P.mvecCS);
            code->Release();
        } else {
            if (err) { LOG_E("mvec CS compile: %s", (char*)err->GetBufferPointer()); err->Release(); }
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA init{};
        float zeros[4] = {};
        init.pSysMem = zeros;
        P.dev11->CreateBuffer(&bd, &init, &P.mvecCB);
    }

    // menu 先于 FFX 管线初始化: 管线失败时菜单仍可用 (Insert 不再崩)
    mvec_set_device((void*)P.dev11);
    menu_init(P.dev11, P.ctx11, gd.OutputWindow);
    QueryPerformanceFrequency(&P.qpcFreq);
    P.lastFrame = std::chrono::steady_clock::now();

    // 单 SC 保真模式: render == display (NativeAA + RCAS)
    if (!pipeline_build_resources(gd.BufferDesc.Width, gd.BufferDesc.Height,
                                  gd.BufferDesc.Width, gd.BufferDesc.Height)) {
        LOG_W("pipeline build failed - menu-only mode, present passthrough");
        return false;
    }
    LOG_I("pipeline ready (fmt=%u)", (unsigned)P.bbFmt);
    return true;
}

// RTV 缓存: 主表 -> hiddenSC(全尺寸, 正常模式), 副表 -> gameSC(降级模式)
static ID3D11RenderTargetView* rtv_hidden(UINT idx) {
    if (idx >= 8 || !P.hiddenSC) return nullptr;
    if (P.rtvs[idx]) return P.rtvs[idx];
    ID3D11Resource* bb = nullptr;
    if (FAILED(P.hiddenSC->GetBuffer(idx, __uuidof(ID3D11Resource), (void**)&bb))) return nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = P.dev11->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
    if (FAILED(hr)) return nullptr;
    P.rtvs[idx] = rtv;
    return rtv;
}
static ID3D11RenderTargetView* s_rtvGame[8] = {};
static ID3D11RenderTargetView* rtv_game(UINT idx) {
    if (idx >= 8 || !P.sc) return nullptr;
    if (s_rtvGame[idx]) return s_rtvGame[idx];
    ID3D11Resource* bb = nullptr;
    if (FAILED(P.sc->GetBuffer(idx, __uuidof(ID3D11Resource), (void**)&bb))) return nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = P.dev11->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
    if (FAILED(hr)) return nullptr;
    s_rtvGame[idx] = rtv;
    return rtv;
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags) {
    // 入口诊断: 前5帧 + 之后每600帧打一条
    static uint64_t s_calls = 0;
    if (s_calls < 5 || (s_calls % 600) == 0)
        LOG_I("Present call #%llu sc=%p (game=%p) Sync=%u Flags=0x%x",
              (unsigned long long)s_calls, (void*)sc, (void*)P.sc, SyncInterval, Flags);
    s_calls++;

    // 渲染确认信号 (#60 ≈ 首场景稳定): il2cpp 元数据已初始化, 解锁 demosaic/mvec 扫描
    if (s_calls == 60) {
        demosaic_notify_rendering();
        mvec_notify_rendering();
    }

    // 陌生 SC 直通 (hiddenSC 现在不参与 Present 路径, 一切 SC 都直通判断之外)
    if (P.sc && sc != P.sc)
        return o_Present(sc, SyncInterval, Flags);
    if (Flags & DXGI_PRESENT_TEST) return S_OK;

    // 超分关闭时彻底直通: 不建 D3D12 设备 / 不建 FFX 上下文 / 不出菜单, 零开销
    if (!cfg().upscaleEnabled)
        return o_Present(sc, cfg().vsyncOverride ? 0 : SyncInterval, Flags);

    if (!ensure_pipeline(sc)) return o_Present(sc, SyncInterval, Flags);

    if (!P.ffxReady || !cfg().upscaleEnabled) {
        menu_new_frame(rtv_game(cur_backbuffer_index()));
        menu_render();
        return o_Present(sc, cfg().vsyncOverride ? 0 : SyncInterval, Flags);
    }

    QueryPerformanceCounter(&P.t0);
    auto now = std::chrono::steady_clock::now();
    if (P.frameTimingValid) P.lastDeltaMs = std::chrono::duration<float, std::milli>(now - P.lastFrame).count();
    P.lastFrame = now; P.frameTimingValid = true;

    // 单 SC 回写方案: gameSC 正常 Present (waitable object 正常 signal, 无死锁)
    UINT cur = cur_backbuffer_index();
    ID3D11Resource* gameBB = nullptr;
    if (FAILED(sc->GetBuffer(cur, __uuidof(ID3D11Resource), (void**)&gameBB)) || !gameBB) {
        LOG_E("GetBuffer failed - passthrough frame");
        return o_Present(sc, SyncInterval, Flags);
    }

    P.fenceVal++;
    uint64_t v = P.fenceVal;

    // 1) game11: backbuffer -> colorShared (同尺寸拷贝)
    P.ctx11->CopyResource(P.colorIn11, gameBB);

    // 1b) 真 mvec/depth: 主线程已缓存 SRV/纹理, 此处纯 D3D11 操作 (无 il2cpp 调用)
    ID3D12Resource* mvecIn12 = nullptr;
    ID3D12Resource* depthIn12 = nullptr;
    {
        void *mvSRV = nullptr, *dpTex = nullptr;
        mvec_fetch(&mvSRV, &dpTex);
        if (mvSRV && P.mvecCS && P.mvecCB && P.mvecUAV) {
            ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)mvSRV;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(P.ctx11->Map(P.mvecCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                float* cbf = (float*)mapped.pData;
                cbf[0] = (float)P.renderW; cbf[1] = (float)P.renderH;
                cbf[2] = 0.f; cbf[3] = 0.f;
                P.ctx11->Unmap(P.mvecCB, 0);
            }
            ID3D11ShaderResourceView* srvs[] = { srv };
            ID3D11UnorderedAccessView* uavs[] = { P.mvecUAV };
            P.ctx11->CSSetShader(P.mvecCS, nullptr, 0);
            P.ctx11->CSSetConstantBuffers(0, 1, &P.mvecCB);
            P.ctx11->CSSetShaderResources(0, 1, srvs);
            P.ctx11->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            UINT gx = (P.renderW + 7) / 8, gy = (P.renderH + 7) / 8;
            P.ctx11->Dispatch(gx, gy, 1);
            ID3D11ShaderResourceView* nullSRV[1] = {};
            ID3D11UnorderedAccessView* nullUAV[1] = {};
            P.ctx11->CSSetShaderResources(0, 1, nullSRV);
            P.ctx11->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
            mvecIn12 = P.mvecShared12;
        }
        if (dpTex && P.depthShared11) {
            D3D11_TEXTURE2D_DESC sd{}, sd2{};
            ((ID3D11Texture2D*)dpTex)->GetDesc(&sd);
            ((ID3D11Texture2D*)P.depthShared11)->GetDesc(&sd2);
            if (sd.Format == sd2.Format && sd.Width == sd2.Width && sd.Height == sd2.Height) {
                P.ctx11->CopyResource(P.depthShared11, (ID3D11Resource*)dpTex);
                depthIn12 = P.depthShared12;
            } else {
                static bool warned = false;
                if (!warned) {
                    LOG_W("[mvec] depth fmt mismatch: unity=%u(%ux%u) shared=%u - depth off",
                          (unsigned)sd.Format, sd.Width, sd.Height, (unsigned)sd2.Format);
                    warned = true;
                }
            }
        }
    }

    // 2) 纯 GPU 同步: 11 signal -> 12 wait
    P.ctx11_4->Signal(P.fenceA11, v);
    P.queue12->Wait(P.fenceA12, v);

    // provider 热切换 (菜单 Apply): GPU idle 后重建 FFX context
    if (P.ffxReady && g_pendingVersionId != P.ffx.activeVersionId()) {
        wait_gpu_idle_12();
        P.ctx11->Flush();
        P.ffx.destroyStaticInputs();
        P.ffx.shutdown();
        if (P.ffx.init(P.dev12, P.queue12, P.renderW, P.renderH, P.bbW, P.bbH, g_pendingVersionId)) {
            LOG_I("provider switched -> '%s'", P.ffx.providerName());
        } else {
            LOG_E("provider switch failed, retry default");
            P.ffx.init(P.dev12, P.queue12, P.renderW, P.renderH, P.bbW, P.bbH, 0);
        }
    }

    // 3) 12: FSR upscale (含抗拖影参数应用)
    {
        static bool s_lastAnti = false;
        bool anti = cfg().antiGhosting;
        if (anti != s_lastAnti) {
            s_lastAnti = anti;
            P.ffx.configureFloat(0 /*FVELOCITYFACTOR*/, anti ? 0.0f : 1.0f);
            P.ffx.configureFloat(2 /*FSHADINGCHANGESCALE*/, anti ? 2.0f : 1.0f);
            LOG_I("anti-ghosting %s (velocityFactor=%s shadingChange=%.1f)",
                  anti ? "on" : "off", anti ? "0" : "1", anti ? 2.f : 1.f);
        }
    }
    bool reset = menu_consume_reset_history();
    P.alloc12->Reset();
    P.cl12->Reset(P.alloc12, nullptr);
    {
        // 输入侧: COMMON -> SRV (color + 可选 mvec/depth); 输出: COMMON -> UAV
        D3D12_RESOURCE_BARRIER bs[4]{};
        int nb = 0;
        bs[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[nb].Transition = { P.color12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        nb++;
        bs[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[nb].Transition = { P.out12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        nb++;
        if (mvecIn12) {
            bs[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bs[nb].Transition = { mvecIn12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
            nb++;
        }
        if (depthIn12) {
            bs[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bs[nb].Transition = { depthIn12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
            nb++;
        }
        P.cl12->ResourceBarrier(nb, bs);
    }
    bool ok = P.ffx.dispatch(P.cl12, P.color12, P.out12,
                             mvecIn12, depthIn12,
                             P.renderW, P.renderH, P.bbW, P.bbH,
                             P.lastDeltaMs, cfg().rcasSharpen, cfg().sharpness, reset);
    {
        // color: SRV -> COMMON; out: UAV -> COPY_SOURCE; mid: COMMON -> COPY_DEST;
        // mvec/depth: SRV -> COMMON (D3D11 互操作约定)
        D3D12_RESOURCE_BARRIER bs[5]{};
        int nb = 0;
        bs[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[nb].Transition = { P.color12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON };
        nb++;
        bs[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[nb].Transition = { P.out12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
        nb++;
        bs[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[nb].Transition = { P.mid12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST };
        nb++;
        if (mvecIn12) {
            bs[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bs[nb].Transition = { mvecIn12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON };
            nb++;
        }
        if (depthIn12) {
            bs[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bs[nb].Transition = { depthIn12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON };
            nb++;
        }
        P.cl12->ResourceBarrier(nb, bs);
    }
    {
        // out12(私有UAV) -> mid12(共享RT) : 跨 API 桥
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0; src.pResource = P.out12;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0; dst.pResource = P.mid12;
        P.cl12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    {
        // out: COPY_SOURCE -> COMMON; mid: COPY_DEST -> COMMON (D3D11 互操作约定)
        D3D12_RESOURCE_BARRIER bs[2]{};
        bs[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[0].Transition = { P.out12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON };
        bs[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[1].Transition = { P.mid12, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON };
        P.cl12->ResourceBarrier(2, bs);
    }
    P.cl12->Close();
    ID3D12CommandList* cls[] = { P.cl12 };
    P.queue12->ExecuteCommandLists(1, cls);

    if (ok) {
        // 4) 12 signal -> game11 GPU wait -> mid 写回 gameBB
        P.queue12->Signal(P.fenceB12, v);
        P.ctx11_4->Wait(P.fenceB11, v);
        P.ctx11->CopyResource(gameBB, P.midIn11);
    } else {
        // dispatch 失败也要画菜单(只警告一次, 防日志刷爆)
        static bool s_warned = false;
        if (!s_warned) { LOG_W("fsr dispatch failed - passthrough frames"); s_warned = true; }
        menu_new_frame(rtv_game(cur));
        menu_render();
        gameBB->Release();
        return o_Present(sc, SyncInterval, Flags);
    }

    // 5) ImGui overlay 画在 gameBB
    menu_new_frame(rtv_game(cur));
    menu_render();

    gameBB->Release();
    QueryPerformanceCounter(&P.t1);

    FsrStats st{};
    st.frameMs = (float)(P.t1.QuadPart - P.t0.QuadPart) * 1000.f / P.qpcFreq.QuadPart;
    st.lastDeltaMs = P.lastDeltaMs;
    st.dispatches = P.ffx.dispatchCount();
    st.renderW = P.renderW; st.renderH = P.renderH;
    st.outW = P.bbW; st.outH = P.bbH;
    strncpy_s(st.provider, P.ffx.providerName(), _TRUNCATE);
    st.demosaicStatus = demosaic_status();
    st.demosaicHits = demosaic_hits();
    st.renderersDisabled = demosaic_renderers_disabled();
    st.dcCount = demosaic_dc_count();
    st.mvecStatus = mvec_status();
    menu_set_stats(st);

    // 6) gameSC 正常 present (waitable object 正常 signal, 无死锁)
    return o_Present(sc, cfg().vsyncOverride ? 0 : SyncInterval, Flags);
}

static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT bufferCount,
                                                  UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    if ((P.sc && sc != P.sc) || !P.dev11)
        return o_ResizeBuffers(sc, bufferCount, w, h, format, flags);

    // 游戏期望尺寸 (w,h; 0 = 沿用当前)
    UINT dw = w, dh = h;
    if ((!dw || !dh) && P.sc) {
        DXGI_SWAP_CHAIN_DESC gd{};
        if (SUCCEEDED(P.sc->GetDesc(&gd))) { dw = gd.BufferDesc.Width; dh = gd.BufferDesc.Height; }
    }
    LOG_I("ResizeBuffers display=%ux%u", dw, dh);
    wait_gpu_idle_12();
    P.ctx11->Flush();
    pipeline_destroy_resources();
    menu_invalidate_targets();
    for (auto& r : s_rtvGame) { if (r) { r->Release(); r = nullptr; } }

    if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        P.bbFmt = (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (format != DXGI_FORMAT_UNKNOWN)
        P.bbFmt = format;

    HRESULT hr = o_ResizeBuffers(sc, bufferCount, w, h, format, flags);
    if (FAILED(hr)) return hr;
    if (dw && dh) {
        if (!pipeline_build_resources(dw, dh, dw, dh)) LOG_W("rebuild after resize failed - passthrough");
    }
    return hr;
}

// Present1: IDXGISwapChain2 vtable 槽 20, Unity waitable-SC 路径
typedef HRESULT (STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain*, UINT, UINT,
    const DXGI_PRESENT_PARAMETERS*);
static Present1Fn o_Present1 = nullptr;

static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags,
                                             const DXGI_PRESENT_PARAMETERS* params) {
    if (sc == P.hiddenSC || (P.sc && sc != P.sc))
        return o_Present1(sc, SyncInterval, Flags, params);
    // 走主 Present 管线 (忽略 dirty rects, 全帧合成)
    return hk_Present(sc, SyncInterval, Flags);
}

// ---------------- 安装 hook ----------------
static void install_swapchain_hooks(IDXGISwapChain* sc) {
    void** vft = *(void***)sc;
    // IDXGISwapChain vtable: 0-2 IUnknown, 3-6 IDXGIObject, 7 GetDevice,
    // 8 Present, 9 GetBuffer, 10 GetContainingOutput, 11 GetDesc, 12 ResizeBuffers,
    // 13 ResizeTarget, ... 20 Present1 (IDXGISwapChain1 追加区)
    if (!o_Present) {
        MH_CreateHook((void*)vft[8], (void*)hk_Present, (void**)&o_Present);
        MH_EnableHook((void*)vft[8]);
        LOG_I("Present hooked @ vtable[8]");
    }
    if (!o_ResizeBuffers) {
        MH_CreateHook((void*)vft[13], (void*)hk_ResizeBuffers, (void**)&o_ResizeBuffers);
        MH_EnableHook((void*)vft[13]);
        LOG_I("ResizeBuffers hooked @ vtable[13]");
    }
    if (!o_Present1) {
        MH_CreateHook((void*)vft[21], (void*)hk_Present1, (void**)&o_Present1);
        MH_EnableHook((void*)vft[21]);
        LOG_I("Present1 hooked @ vtable[21]");
    }
}

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainFactory(IDXGIFactory* f, IUnknown* dev,
                                                            DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    // 单 SC 模式: 透传创建, 仅装 vtable hook
    HRESULT hr = o_CreateSwapChain(f, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out) install_swapchain_hooks(*out);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *CreateSwapChainForHwndFn)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
static CreateSwapChainForHwndFn o_CreateSwapChainForHwnd = nullptr;

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc1, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc,
    IDXGIOutput* restrictOut, IDXGISwapChain1** out) {
    // 单 SC 模式: 透传创建, 仅装 vtable hook (不建 hiddenSC, 不改尺寸)
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, desc1, fsDesc, restrictOut, out);
    if (SUCCEEDED(hr) && out && *out) {
        install_swapchain_hooks(*out);
        DXGI_SWAP_CHAIN_DESC1 d{};
        if (SUCCEEDED((*out)->GetDesc1(&d)))
            LOG_I("gameSC created (ForHwnd): %ux%u fmt=%u flags=0x%x",
                  d.Width, d.Height, (unsigned)d.Format, d.Flags);
    }
    return hr;
}

static bool hook_factory(IDXGIFactory* f) {
    if (o_CreateSwapChain && o_CreateSwapChainForHwnd) return true;
    void** vft = *(void***)f;
    bool any = false;
    // IDXGIFactory::CreateSwapChain = slot 10
    if (!o_CreateSwapChain &&
        MH_CreateHook((void*)vft[10], (void*)hk_CreateSwapChainFactory, (void**)&o_CreateSwapChain) == MH_OK) {
        MH_EnableHook((void*)vft[10]);
        LOG_I("factory CreateSwapChain hooked");
        any = true;
    }
    // IDXGIFactory2::CreateSwapChainForHwnd = slot 15
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
        void** vft2 = *(void***)f2;
        if (!o_CreateSwapChainForHwnd &&
            MH_CreateHook((void*)vft2[15], (void*)hk_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd) == MH_OK) {
            MH_EnableHook((void*)vft2[15]);
            LOG_I("factory2 CreateSwapChainForHwnd hooked");
            any = true;
        }
        f2->Release();
    }
    return any;
}

static HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    HRESULT hr = o_D3D11CreateSC(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
        SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) install_swapchain_hooks(*ppSwapChain);
    return hr;
}

// ---------------- 初始化 ----------------
static std::once_flag s_earlyOnce;

static void module_dir(wchar_t* dir, size_t cch) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&module_dir, &self);
    GetModuleFileNameW(self, dir, (DWORD)cch);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
}

// 同步早初始化: 在 dxgi 转发 stub 第一次被调用时执行.
// 必须在返回 factory 给游戏之前装好 factory/d3d11 hook, 否则错过 swapchain 创建.
static void early_init() {
    std::call_once(s_earlyOnce, [] {
        wchar_t dir[MAX_PATH];
        module_dir(dir, MAX_PATH);
        wchar_t logp[MAX_PATH]; swprintf_s(logp, L"%s\\lillia_fsr.log", dir);
        log_set_file(logp);
        cfg().load(dir);
        LOG_I("== lillia_fsr early_init ==");

        if (MH_Initialize() != MH_OK) { LOG_E("MH_Initialize failed"); return; }

        // 路径 A: d3d11.dll 导出 (D3D11CreateDeviceAndSwapChain 内部会建 swapchain)
        HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (d3d11) {
            void* fn = (void*)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
            if (fn && !o_D3D11CreateSC &&
                MH_CreateHook(fn, (void*)hk_D3D11CreateDeviceAndSwapChain, (void**)&o_D3D11CreateSC) == MH_OK) {
                MH_EnableHook(fn);
                LOG_I("D3D11CreateDeviceAndSwapChain hooked");
            }
        } else {
            LOG_W("d3d11.dll not loaded at early_init");
        }

        // ffx 依赖部署 + 去马赛克线程 (后台)
        std::thread([] {
            wchar_t dir2[MAX_PATH];
            module_dir(dir2, MAX_PATH);
            wchar_t src[MAX_PATH], dst[MAX_PATH];
            const wchar_t* deps[] = { L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll",
                                      L"amd_fidelityfx_framegeneration_dx12.dll" };
            for (auto d : deps) {
                swprintf_s(dst, L"%s\\%s", dir2, d);
                if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES) {
                    swprintf_s(src, L"%s\\ffx\\%s", dir2, d);
                    if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) CopyFileW(src, dst, FALSE);
                }
            }
            demosaic_start();
            // 串行化: 等 demosaic 元数据解析完成后再启 mvec (并发读 il2cpp metadata 会拿到半初始化类)
            for (int i = 0; i < 120 && demosaic_status() == 0; i++) Sleep(500);
            if (cfg().upscaleEnabled) mvec_init();
        }).detach();
    });
}

// factory 创建回调(由 dxgi 转发 stub 调用)
static void on_factory_created(IUnknown* factory) {
    IDXGIFactory* f = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory), (void**)&f))) {
        hook_factory(f);
        f->Release();
    }
}

} // namespace lillia

// ---------------- dxgi 导出转发 (内部 proxy_ 名 + def 别名) ----------------
extern "C" {

HRESULT WINAPI proxy_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    lillia::early_init();
    static auto fn = (HRESULT(WINAPI*)(REFIID, void**))lillia::proxy_proc("CreateDXGIFactory");
    HRESULT hr = fn(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) lillia::on_factory_created((IUnknown*)*ppFactory);
    return hr;
}

HRESULT WINAPI proxy_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    lillia::early_init();
    static auto fn = (HRESULT(WINAPI*)(REFIID, void**))lillia::proxy_proc("CreateDXGIFactory1");
    HRESULT hr = fn(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) lillia::on_factory_created((IUnknown*)*ppFactory);
    return hr;
}

HRESULT WINAPI proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
    lillia::early_init();
    static auto fn = (HRESULT(WINAPI*)(UINT, REFIID, void**))lillia::proxy_proc("CreateDXGIFactory2");
    HRESULT hr = fn(flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) lillia::on_factory_created((IUnknown*)*ppFactory);
    return hr;
}

HRESULT WINAPI proxy_DXGIDeclareAdapterRemovalSupport() {
    lillia::early_init();
    static auto fn = (HRESULT(WINAPI*)())lillia::proxy_proc("DXGIDeclareAdapterRemovalSupport");
    return fn();
}

HRESULT WINAPI proxy_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** ppDebug) {
    lillia::early_init();
    static auto fn = (HRESULT(WINAPI*)(UINT, REFIID, void**))lillia::proxy_proc("DXGIGetDebugInterface1");
    return fn(flags, riid, ppDebug);
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        // 早初始化不在这里做: LoaderLock 限制(不能 CreateThread/LoadLibrary).
        // dxgi 导出 stub 第一次被游戏调用时同步执行 early_init.
    } else if (reason == DLL_PROCESS_DETACH) {
        lillia::demosaic_stop();
    }
    return TRUE;
}
