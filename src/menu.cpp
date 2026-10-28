#include "common.h"
#include "config.h"
#include "menu.h"
#include "ffx_upscaler.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace lillia {

uint64_t g_pendingVersionId = 0;

static ID3D11Device*        s_dev    = nullptr;
static ID3D11DeviceContext* s_ctx    = nullptr;
static ID3D11RenderTargetView* s_rtv = nullptr;
static HWND                 s_hwnd   = nullptr;
static WNDPROC              s_origProc = nullptr;
static bool                 s_visible = false;
static bool                 s_reqReset = false;
static FsrStats             s_stats{};

static LRESULT CALLBACK menu_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) {
        if (s_visible && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
                          msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MOUSEWHEEL ||
                          msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN))
            return 1; // 菜单打开时吞掉输入
    }
    return CallWindowProcW(s_origProc, h, msg, w, l);
}

bool menu_install_wndproc(HWND hwnd) {
    if (!hwnd || s_origProc) return true;
    s_origProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)menu_proc);
    s_hwnd = hwnd;
    return s_origProc != nullptr;
}

bool menu_init(ID3D11Device* dev, ID3D11DeviceContext* ctx, HWND hwnd) {
    s_dev = dev; s_ctx = ctx;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(dev, ctx);
    return menu_install_wndproc(hwnd);
}

void menu_shutdown() {
    if (s_origProc && s_hwnd) { SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, (LONG_PTR)s_origProc); s_origProc = nullptr; }
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; }
}

void menu_new_frame(ID3D11RenderTargetView* rtv) {
    // Insert 键切换
    static bool keyWasDown = false;
    bool key = (GetAsyncKeyState(cfg().menuKeyVk) & 0x8000) != 0;
    if (key && !keyWasDown) s_visible = !s_visible;
    keyWasDown = key;
    if (!s_visible) return;
    if (!s_dev || !s_ctx) return;   // menu 未初始化(管线失败)时严禁触碰 ImGui
    if (!rtv) return;               // RTV 缺失本帧跳过

    if (rtv != s_rtv) { if (s_rtv) s_rtv->Release(); s_rtv = rtv; rtv->AddRef(); }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void menu_render() {
    if (!s_visible || !s_dev) return;
    ImGui::SetNextWindowSize(ImVec2(420, 380), ImGuiCond_FirstUseEver);
    ImGui::Begin("Lillia FSR 4 (FidelityFX SDK 2.3.0)");
    Config& c = cfg();

    if (ImGui::CollapsingHeader("Upscaling", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Provider: %s", s_stats.provider);
        ImGui::Text("%dx%d -> %dx%d", s_stats.renderW, s_stats.renderH, s_stats.outW, s_stats.outH);
        ImGui::Separator();
        ImGui::Checkbox("Enable FSR Upscale", &c.upscaleEnabled);

        // provider 下拉 (fsr1=纯空间零拖影 / fsr4=时域ML)
        int n = FfxUpscaler::versionCount();
        if (n > 0) {
            const char* const* names = FfxUpscaler::versionNames();
            static int sel = 0;
            if (ImGui::Combo("Algorithm", &sel, names, n)) {
                g_pendingVersionId = FfxUpscaler::versionIds()[sel];
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("fsr1/spatial: no ghosting, pure spatial\n"
                                  "fsr2/3/4: temporal, better detail with motion vectors");
        }

        ImGui::SliderFloat("Render Scale", &c.renderScale, 0.30f, 1.00f);
        if (c.renderScale < 0.999f)
            ImGui::TextDisabled("  (NativeAA mode: scale inactive - game renders at display res)");
        ImGui::Checkbox("RCAS Sharpen", &c.rcasSharpen);
        if (c.rcasSharpen)
            ImGui::SliderFloat("Sharpness", &c.sharpness, 0.0f, 1.0f);
        ImGui::Checkbox("Anti-ghosting (synthetic mvec)", &c.antiGhosting);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("FVelocityFactor=0 + ShadingChange boost\nReduces trailing on camera move");
        if (ImGui::Button("Reset History (clear ghosting)"))
            menu_request_reset_history();
        if (ImGui::Button("Save INI"))
            c.save();
    }
    if (ImGui::CollapsingHeader("Stats")) {
        ImGui::Text("Present overhead: %.2f ms", s_stats.frameMs);
        ImGui::Text("Frame delta: %.2f ms", s_stats.lastDeltaMs);
        ImGui::Text("Upscale dispatches: %llu", (unsigned long long)s_stats.dispatches);
        const char* ms = s_stats.mvecStatus == 0 ? "initializing..."
                       : s_stats.mvecStatus == 1 ? "REAL mvec+depth feeding FSR"
                       : s_stats.mvecStatus == 3 ? "camera mode set, texture not ready"
                       : s_stats.mvecStatus == 2 ? "no camera" : "error";
        ImGui::Text("Motion vectors: %s", ms);
    }
    if (ImGui::CollapsingHeader("Frame Generation")) {
        ImGui::TextDisabled("Requires real motion vectors - unavailable in this engine build.");
        ImGui::TextWrapped("FSR4-FG needs camera jitter + true mvec; "
                           "synthetic zero-mvec interpolation produces severe artifacts. Disabled.");
    }
    ImGui::Text("Menu key: Insert (VK 0x2D)");
    ImGui::End();
    ImGui::Render();
    ID3D11RenderTargetView* rtv = s_rtv;
    s_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

bool menu_visible() { return s_visible; }
void menu_set_visible(bool v) { s_visible = v; }
void menu_set_stats(const FsrStats& s) { s_stats = s; }
void menu_request_reset_history() { s_reqReset = true; }
bool menu_consume_reset_history() { bool r = s_reqReset; s_reqReset = false; return r; }
void menu_resize_targets(ID3D11RenderTargetView* rtv) {
    if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; }
    if (rtv) { s_rtv = rtv; rtv->AddRef(); }
}
void menu_invalidate_targets() { if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; } }

}
