#pragma once
#include <cstdint>

namespace lillia {

struct Config {
    float    renderScale      = 0.75f;   // 0.5 .. 1.0
    bool     upscaleEnabled   = true;    // FSR on/off
    bool     rcasSharpen      = true;
    float    sharpness        = 0.2f;    // 0..1
    bool     disableMosaic    = true;
    bool     mosaicNukeShader = true;    // 专用 mosaic renderer 的材质换成透明 shader
    bool     mosaicProbe      = true;    // 打印 _mosaicRenderers 内容到日志
    bool     antiGhosting     = true;    // 合成 mvec 下的时域稳定增强
    int      menuKeyVk        = VK_INSERT;
    bool     vsyncOverride    = false;   // 强制 SyncInterval=0
    wchar_t  iniPath[MAX_PATH] = {};

    void load(const wchar_t* dir);
    void save() const;
};

Config& cfg();

}
