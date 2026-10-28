#include "common.h"
#include "ffx_upscaler.h"

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

namespace lillia {

static PfnFfxCreateContext  s_CreateContext;
static PfnFfxDestroyContext s_DestroyContext;
static PfnFfxConfigure      s_Configure;
static PfnFfxQuery          s_Query;
static PfnFfxDispatch       s_Dispatch;

static void ffxMsg(uint32_t type, const wchar_t* message) {
    if (type == FFX_API_MESSAGE_TYPE_ERROR)   LOG_E("[ffx] %ls", message);
    else if (type == FFX_API_MESSAGE_TYPE_WARNING) LOG_W("[ffx] %ls", message);
    else LOG_D("[ffx] %ls", message);
}

uint64_t FfxUpscaler::s_versionIds[16] = {};
const char* FfxUpscaler::s_versionNames[16] = {};
int FfxUpscaler::s_versionCount = 0;

int FfxUpscaler::listVersions(ID3D12Device* dev12) {
    if (s_versionCount > 0) return s_versionCount;
    void* loader = (void*)LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
    if (!loader) { LOG_E("loader dll missing for version query"); return 0; }
    ffxFunctions fns{};
    ffxLoadFunctions(&fns, loader);
    if (!fns.Query) return 0;

    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = dev12;
    uint64_t count = 0;
    q.outputCount = &count;
    if (fns.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) {
        LOG_W("GetVersions returned 0");
        return 0;
    }
    if (count > 16) count = 16;
    s_versionCount = (int)count;
    q.outputCount = &count;
    q.versionIds = s_versionIds;
    q.versionNames = s_versionNames;
    if (fns.Query(nullptr, &q.header) != FFX_API_RETURN_OK) { s_versionCount = 0; return 0; }
    for (int i = 0; i < s_versionCount; i++)
        LOG_I("[ffx] provider[%d] id=%llu name='%s'", i, (unsigned long long)s_versionIds[i],
              s_versionNames[i] ? s_versionNames[i] : "?");
    return s_versionCount;
}

bool FfxUpscaler::init(ID3D12Device* dev12, ID3D12CommandQueue* queue,
                       uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
                       uint64_t versionId) {
    shutdown();
    if (!m_loader) {
        m_loader = (void*)LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
        if (!m_loader) { LOG_E("amd_fidelityfx_loader_dx12.dll load failed (%lu)", GetLastError()); return false; }
        ffxFunctions fns{};
        ffxLoadFunctions(&fns, m_loader);
        s_CreateContext  = fns.CreateContext;
        s_DestroyContext = fns.DestroyContext;
        s_Configure      = fns.Configure;
        s_Query          = fns.Query;
        s_Dispatch       = fns.Dispatch;
        if (!s_CreateContext || !s_Dispatch) { LOG_E("ffx loader exports missing"); return false; }
    }

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = dev12;

    ffxOverrideVersion override{};
    override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override.versionId = versionId;
    if (versionId) backend.header.pNext = &override.header;

    ffxCreateContextDescUpscale ups{};
    ups.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    ups.header.pNext = &backend.header;
    ups.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
              | FFX_UPSCALE_ENABLE_DEPTH_INVERTED
              | FFX_UPSCALE_ENABLE_DEPTH_INFINITE
              | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    ups.maxRenderSize  = { renderW, renderH };
    ups.maxUpscaleSize = { outW, outH };
    ups.fpMessage = &ffxMsg;

    ffxContext ctx = nullptr;
    ffxReturnCode_t rc = s_CreateContext(&ctx, &ups.header, nullptr);
    if (rc != FFX_API_RETURN_OK) { LOG_E("ffxCreateContext(upscale,vid=%llu) rc=%u", (unsigned long long)versionId, rc); return false; }
    m_ctx = ctx;
    m_dev12 = dev12; m_queue12 = queue;
    m_versionId = versionId;

    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (s_Query(&ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
        strncpy_s(m_providerName, pv.versionName, _TRUNCATE);
    LOG_I("FSR upscaler context created: '%s' (vid=%llu) render=%ux%u out=%ux%u",
          m_providerName, (unsigned long long)versionId, renderW, renderH, outW, outH);
    return true;
}

bool FfxUpscaler::configureFloat(uint64_t key, float value) {
    if (!m_ctx || !s_Configure) return false;
    ffxConfigureDescUpscaleKeyValue kv{};
    kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
    kv.key = key;
    kv.ptr = &value;
    ffxReturnCode_t rc = s_Configure(&m_ctx, &kv.header);
    if (rc != FFX_API_RETURN_OK) { LOG_E("ffxConfigure(key=%llu) rc=%u", (unsigned long long)key, rc); return false; }
    return true;
}

void FfxUpscaler::shutdown() {
    if (m_ctx) {
        s_DestroyContext(&m_ctx, nullptr);
        m_ctx = nullptr;
        LOG_I("FSR upscaler context destroyed");
    }
    destroyStaticInputs();
}

bool FfxUpscaler::createStaticInputs(uint32_t renderW, uint32_t renderH) {
    destroyStaticInputs();
    ID3D12Device* dev = (ID3D12Device*)m_dev12;
    if (!dev) return false;

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = renderW; td.Height = renderH;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    // 注: 无 ALLOW_DEPTH_STENCIL flag 的资源禁用 DepthStencil clear value (E_INVALIDARG),
    // 此纹理仅为 FSR 输入数据平面, copy 清零, clear value 传 null
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_zeroDepth));
    if (FAILED(hr)) { LOG_E("zero depth create failed hr=0x%08lX", hr); return false; }

    td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = renderW; td.Height = renderH;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.SampleDesc.Count = 1;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_zeroMvec));
    if (FAILED(hr)) { LOG_E("zero mvec create failed hr=0x%08lX", hr); return false; }

    // DEFAULT heap 资源不保证清零, 用 upload buffer 显式清零
    const size_t depthBytes = (size_t)renderW * renderH * 4;
    const size_t mvecBytes  = (size_t)renderW * renderH * 4;
    D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub{};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = depthBytes + mvecBytes + 256;
    ub.Height = 1; ub.DepthOrArraySize = 1; ub.MipLevels = 1;
    ub.SampleDesc.Count = 1; ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    hr = dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr)) { LOG_E("upload buffer failed hr=0x%08lX", hr); return false; }

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    memset(mapped, 0, depthBytes + mvecBytes);
    upload->Unmap(0, nullptr);

    ID3D12CommandAllocator* alloc = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));

    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    fp.Offset = 0; fp.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    fp.Footprint.Width = renderW; fp.Footprint.Height = renderH; fp.Footprint.Depth = 1;
    fp.Footprint.RowPitch = renderW * 4;
    src.PlacedFootprint = fp; src.pResource = upload;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    dst.pResource = m_zeroDepth;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    fp.Offset = depthBytes; fp.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
    fp.Footprint.RowPitch = renderW * 4;
    src.PlacedFootprint = fp;
    dst.pResource = m_zeroMvec;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER bar[2]{};
    bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[0].Transition.pResource = m_zeroDepth;
    bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bar[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[1].Transition.pResource = m_zeroMvec;
    bar[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(2, bar);
    cl->Close();
    ID3D12CommandList* cls[] = { cl };
    ((ID3D12CommandQueue*)m_queue12)->ExecuteCommandLists(1, cls);
    // 等待完成: 阻塞 fence (一次性初始化, CPU wait 可接受)
    ID3D12Fence* f = nullptr;
    if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ((ID3D12CommandQueue*)m_queue12)->Signal(f, 1);
        f->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 2000);
        CloseHandle(ev);
        f->Release();
    }
    cl->Release(); alloc->Release(); upload->Release();
    LOG_I("static zero depth/mvec inputs ready (%ux%u)", renderW, renderH);
    return true;
}

void FfxUpscaler::destroyStaticInputs() {
    if (m_zeroDepth) { m_zeroDepth->Release(); m_zeroDepth = nullptr; }
    if (m_zeroMvec)  { m_zeroMvec->Release();  m_zeroMvec = nullptr; }
}

bool FfxUpscaler::dispatch(ID3D12GraphicsCommandList* cl,
                           ID3D12Resource* color12, ID3D12Resource* out12,
                           ID3D12Resource* mvec12, ID3D12Resource* depth12,
                           uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
                           float deltaMs, bool sharpen, float sharpness, bool reset) {
    if (!m_ctx || !color12 || !out12 || !cl) return false;
    if (!m_zeroDepth && !createStaticInputs(renderW, renderH)) return false;

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;

    d.color = ffxApiGetResourceDX12(color12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = depth12 ? ffxApiGetResourceDX12(depth12, FFX_API_RESOURCE_STATE_COMPUTE_READ)
                      : ffxApiGetResourceDX12(m_zeroDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = mvec12 ? ffxApiGetResourceDX12(mvec12, FFX_API_RESOURCE_STATE_COMPUTE_READ)
                             : ffxApiGetResourceDX12(m_zeroMvec, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.exposure = {};                      // auto exposure
    d.reactive = {};
    d.transparencyAndComposition = {};
    d.output = ffxApiGetResourceDX12(out12, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    d.jitterOffset = { 0.f, 0.f };        // 无相机 jitter (v1)
    // 真 mvec 时输入已是像素 delta (CS 转换过), scale=1; zero mvec 时无所谓
    d.motionVectorScale = { 1.f, 1.f };
    d.renderSize  = { renderW, renderH };
    d.upscaleSize = { outW, outH };
    d.enableSharpening = sharpen;
    d.sharpness = sharpness;
    d.frameTimeDelta = deltaMs;
    d.preExposure = 1.0f;
    d.reset = reset;
    d.cameraNear = 0.1f;
    d.cameraFar  = 1000.0f;
    d.cameraFovAngleVertical = 1.0472f;   // 60deg placeholder, static scene
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

    ffxReturnCode_t rc = s_Dispatch(&m_ctx, &d.header);
    if (rc != FFX_API_RETURN_OK) { LOG_E("ffxDispatch(upscale) rc=%u", rc); return false; }
    m_dispatches++;
    return true;
}

}
