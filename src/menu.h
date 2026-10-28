#pragma once
#include <d3d11.h>
#include <cstdint>

namespace lillia {

struct FsrStats {
    float   frameMs      = 0.f;
    float   lastDeltaMs  = 0.f;
    uint64_t dispatches  = 0;
    char    provider[128] = "n/a";
    int     renderW = 0, renderH = 0, outW = 0, outH = 0;
    int     demosaicStatus = 0;
    int     demosaicHits = 0;
    int     renderersDisabled = 0;
    int     dcCount = 0;
    int     mvecStatus = 0;   // 0=init 1=纹理到手 3=模式未生效 -1=err
};

// 初始化 ImGui (gameDev11 + hwnd), 每帧在外部时钟驱动
bool menu_init(ID3D11Device* dev, ID3D11DeviceContext* ctx, HWND hwnd);
void menu_shutdown();
void menu_new_frame(ID3D11RenderTargetView* rtv);
void menu_render();                    // ImGui::Render + DX11 draw
bool menu_visible();
void menu_set_visible(bool v);
void menu_set_stats(const FsrStats& s);
void menu_request_reset_history();     // FSR history reset 请求(下一帧消费)
bool menu_consume_reset_history();
void menu_resize_targets(ID3D11RenderTargetView* rtv); // SwapChain resize 后重建 RTV
void menu_invalidate_targets();

// provider 切换请求 (dllmain 消费)
extern uint64_t g_pendingVersionId;

// WndProc 子类化安装
bool menu_install_wndproc(HWND hwnd);

}
