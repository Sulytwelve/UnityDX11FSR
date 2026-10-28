#pragma once
#include <cstdint>

namespace lillia {

// 线程模型 (v16):
//   worker线程: 仅 il2cpp 元数据解析 (类/方法查找, 线程安全)
//   主线程 (demosaic Update hook 内): mvec_poll_mainthread() 节流调用
//     - Camera.main.depthTextureMode = Depth|MotionVectors
//     - GetGlobalTexture -> GetNativeTexturePtr -> SRV/AddRef 缓存 (防悬挂)
//   Present线程: mvec_fetch() 纯缓存读 + D3D11 操作 (与游戏渲染同线程)
void mvec_init();
void mvec_stop();
void mvec_notify_rendering();                          // Present hook: 渲染已开始
void mvec_set_device(void* dev11);                     // 管线创建时提供 (SRV 创建用)
void mvec_poll_mainthread();                            // 主线程 Unity 调用 (节流)
void mvec_fetch(void** outMvecSRV, void** outDepthTex); // Present 线程纯读
int  mvec_status();   // 0=未解析 1=纹理到手 2=模式已设无纹理 3=解析完待设 -1=错

}