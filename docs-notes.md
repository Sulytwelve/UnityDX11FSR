# FSR 在 D3D11 游戏上的接入笔记 (lillia_fsr / dxgi proxy)

本作 `UpscaleEnabled=0` 默认关闭，代码保留，供以后真 DX12 游戏复用。

## 架构

```
game (D3D11) ── Present 钩子 ──> CopyResource(backbuffer -> colorIn11)
                                    │ 共享纹理 (D3D11 -> D3D12)
                                    ▼
                              D3D12 + FFX API (ffxCreateContext UPsCALE)
                                    │ dispatch (color + 可选 mvec/depth)
                                    ▼
                              out12 ──共享──> midIn11 ── CopyResource ──> backbuffer
                                    ▼
                              o_Present(game SC)
```

单 swapchain + 回写方案：不动游戏的 swapchain 属性，Present 前后各做一次共享纹理往返，
waitable object 仍由原生 Present 正常 signal，不会死锁。

## 关键点 (复用清单)

1. **上下文**: `ffxCreateContext(&ctx, &createDesc{FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE}, ...)`，
   `ffxQuery` 枚举 provider（本机拿到 4.1.2 / 3.1.5 / 2.3.4 三档，可运行时切换）。
2. **输入**: color/mvec/depth 走 `ffxApiGetResourceDX12(res, FFX_API_RESOURCE_STATE_*)`；
   输出走 UAV，dispatch 后用 barrier 转 COPY_SOURCE 再共享回 D3D11。
3. **共享纹理**: `D3D11_RESOURCE_MISC_SHARED` + `IDXGIResource1::CreateSharedHandle`，
   D3D12 侧 `OpenSharedHandle`。**注意**：本作 `fmt=34/41`（R32G32_FLOAT / R32_FLOAT）
   共享创建失败 `hr=0x80070057`，只有 8-bit 颜色格式可用。
   以后换游戏要先探测支持格式，别假设所有格式都能共享。
4. **抗锯齿/锐化**: RCAS 走 dispatch desc 的 sharpen 参数；时域抗拖影用
   `FVelocityFactor=0` + `FShadingChangeScale` 提升。
5. **provider 热切换**: 菜单改 provider → GPU idle → destroy context → 用新 versionId 重建。

## 本作为什么糊 (教训)

FSR 是时域算法，`mvec` 与 `depth` 是必需输入。本作两条路都断了：

* `Texture.GetNativeTexturePtr()` 在 Unity 2022.3 的 IL2CPP 里**不存在**
  （`dump.cs` 里 Texture 只剩 `CreateNativeArrayLengthOverflowException` 之类内部方法），
  拿不到 `_CameraMotionVectorsTexture` / `_CameraDepthTexture` 的原生指针。
* 传入的 zero mvec/depth 让时域部分只能猜历史帧 → 运动时糊 + 拖影，
  实测不如原生 + 8x MSAA。

结论：**没有真实 mvec 就不要开时域超分**（fsr2/3/4）。纯空间算法（fsr1/RCAS）可以，
但那就只是锐化，不如原生。

## 帧生成 (Frame Generation) 结论

* `amd_fidelityfx_framegeneration_dx12.dll` 只支持 **DX12**，本作是 D3D11，且需要自己接管
  swapchain / pacing。D3D11 游戏上做原生 FG 不现实。
* FG 同样需要 mvec + depth；本作拿不到（同上）。
* 若以后做 DX12 游戏：`ffxCreateContext(FRAME_GENERATION)` + `ffxConfigureFrameGeneration`
  + 每帧传 frameID，且必须真实的 MV/depth，否则不如不做。

## 目录结构

```
src/dllmain.cpp         dxgi 代理 + 钩子 + FSR 管线 (Apply/Resize/Present)
src/ffx_upscaler.cpp    FFX API 封装 (provider 枚举 / context / dispatch)
src/il2cpp_mvec.cpp     Unity 侧取 mvec/depth (本作失败, 保留供别的游戏参考)
src/il2cpp_demosaic.cpp 去马赛克 (本作唯一启用的功能)
src/menu.cpp            ImGui 菜单 (仅 FSR 开启时创建)
```
