#include "common.h"
#include "config.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

namespace lillia {

static Config g_cfg;

Config& cfg() { return g_cfg; }

struct IniPair { const char* key; void* ptr; enum { F, I, B } type; };

void Config::load(const wchar_t* dir) {
    swprintf_s(iniPath, L"%s\\lillia_fsr.ini", dir);
    char pathA[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, iniPath, -1, pathA, MAX_PATH, nullptr, nullptr);
    if (!PathFileExistsA(pathA)) { save(); return; }
    auto gf = [pathA](const char* k, const char* def) {
        char buf[64]; GetPrivateProfileStringA("FSR", k, def, buf, 64, pathA); return std::string(buf);
    };
    renderScale    = (float)atof(gf("RenderScale", "0.75").c_str());
    if (renderScale < 0.30f) renderScale = 0.30f;
    if (renderScale > 1.00f) renderScale = 1.00f;
    upscaleEnabled = atoi(gf("UpscaleEnabled", "0").c_str()) != 0;
    rcasSharpen    = atoi(gf("RCASSharpen", "1").c_str()) != 0;
    sharpness      = (float)atof(gf("Sharpness", "0.2").c_str());
    if (sharpness < 0.f) sharpness = 0.f;
    if (sharpness > 1.f) sharpness = 1.f;
    disableMosaic  = atoi(gf("DisableMosaic", "1").c_str()) != 0;
    mosaicNukeShader = atoi(gf("MosaicNukeShader", "1").c_str()) != 0;
    mosaicProbe    = atoi(gf("MosaicProbe", "1").c_str()) != 0;
    antiGhosting   = atoi(gf("AntiGhosting", "1").c_str()) != 0;
    menuKeyVk      = atoi(gf("MenuKeyVK", "45").c_str()); // VK_INSERT
    vsyncOverride  = atoi(gf("VSyncForceOff", "0").c_str()) != 0;
}

void Config::save() const {
    if (!iniPath[0]) return;
    char pathA[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, iniPath, -1, pathA, MAX_PATH, nullptr, nullptr);
    char buf[64];
    auto w = [&](const char* k, const char* v) { WritePrivateProfileStringA("FSR", k, v, pathA); };
    sprintf_s(buf, "%.3f", renderScale);       w("RenderScale", buf);
    w("UpscaleEnabled", upscaleEnabled ? "1" : "0");
    w("RCASSharpen", rcasSharpen ? "1" : "0");
    sprintf_s(buf, "%.3f", sharpness);         w("Sharpness", buf);
    w("DisableMosaic", disableMosaic ? "1" : "0");
    w("MosaicNukeShader", mosaicNukeShader ? "1" : "0");
    w("MosaicProbe", mosaicProbe ? "1" : "0");
    w("AntiGhosting", antiGhosting ? "1" : "0");
    sprintf_s(buf, "%d", menuKeyVk);           w("MenuKeyVK", buf);
    w("VSyncForceOff", vsyncOverride ? "1" : "0");
}

}
