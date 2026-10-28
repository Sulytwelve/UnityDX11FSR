#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace lillia {

// FSR 4.x (FidelityFX SDK 2.3.0, ffx_api) upscaler on a background D3D12 device.
// Inputs/outputs cross the D3D11 game device via shared NT handles.
class FfxUpscaler {
public:
    bool init(ID3D12Device* dev12, ID3D12CommandQueue* queue,
              uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
              uint64_t versionId = 0);
    void shutdown();

    // Dispatch one upscale. color12/out12 are D3D12 resources opened/wrapped on dev12.
    // mvec12/depth12 nullable: null -> zero-filled static inputs
    bool dispatch(ID3D12GraphicsCommandList* cl,
                  ID3D12Resource* color12, ID3D12Resource* out12,
                  ID3D12Resource* mvec12, ID3D12Resource* depth12,
                  uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
                  float deltaMs, bool sharpen, float sharpness, bool reset);

    // zero-filled static inputs (depth/mvec) sized to render resolution
    bool createStaticInputs(uint32_t renderW, uint32_t renderH);
    void destroyStaticInputs();

    const char* providerName() const { return m_providerName; }
    uint64_t    dispatchCount() const { return m_dispatches; }
    uint64_t    activeVersionId() const { return m_versionId; }

    // 运行时调参 (FFX_API_CONFIGURE_UPSCALE_KEY_*)
    bool configureFloat(uint64_t key, float value);

    // 版本(provider)枚举: 静态缓存, 菜单读取
    static int  listVersions(ID3D12Device* dev12);
    static const char* const* versionNames() { return s_versionNames; }
    static const uint64_t* versionIds() { return s_versionIds; }
    static int  versionCount() { return s_versionCount; }

private:
    void*        m_loader          = nullptr;
    void*        m_ctx             = nullptr;
    void*        m_dev12           = nullptr;
    void*        m_queue12         = nullptr;
    ID3D12Resource* m_zeroDepth    = nullptr;
    ID3D12Resource* m_zeroMvec     = nullptr;
    uint64_t     m_dispatches      = 0;
    uint64_t     m_versionId       = 0;
    char         m_providerName[128] = {};

    static uint64_t s_versionIds[16];
    static const char* s_versionNames[16];
    static int    s_versionCount;
};

}
