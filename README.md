# UnityDX11FSR

> **实验性项目，不建议实际使用。** 这是一个把 AMD FidelityFX Super Resolution 接进 **D3D11 Unity 游戏**的尝试，
> 结论是：在没有真实运动矢量的情况下，时域超分只会让画面更糊，不如原生渲染 + MSAA。
> 代码保留下来作为记录，方便以后给真正的 DX12 游戏做适配时少踩坑。

A d3d11 → d3d12 bridge that drives FidelityFX Super Resolution from a `dxgi.dll` proxy.
**Experimental. It did not end up worth using** — see [why](#为什么不好用).

---

## 它做了什么

```
game (D3D11) ── Present hook ──> CopyResource(backbuffer → colorIn11)
                                   │ shared texture (D3D11 → D3D12)
                                   ▼
                             D3D12 + FFX API (ffxCreateContext / dispatch)
                                   │ upscale (color + optional mvec/depth)
                                   ▼
                             out12 ──shared──> midIn11 ──CopyResource──> backbuffer
                                   ▼
                             original Present
```

* `dxgi.dll` 代理：转发全部 dxgi 导出，挂钩 `D3D11CreateDeviceAndSwapChain` 与 `Present`；
* 不接管 swapchain：每次 Present 前后做一次共享纹理往返，原生 Present 仍正常 signal waitable object，不会死锁；
* FSR 走 FFX API（`ffxCreateContext` + `ffxDispFF`），运行时枚举 provider（实测有 4.1.2 / 3.1.5 / 2.3.4 三档，可热切换）；
* RCAS 锐化、provider 热切换、resize 重建都在里面；
* 附带一个 ImGui 菜单（Insert 键）用于调参数。

## 为什么不好用

1. **拿不到运动矢量**。时域超分（fsr2/3/4）必须要 mvec + depth。本项目尝试从 Unity 侧取
   `_CameraMotionVectorsTexture` / `_CameraDepthTexture`，但 Unity 2022.3 的 IL2CPP
   `Texture` 类里**没有** `GetNativeTexturePtr()`，拿不到原生指针，只能传零向量。
   时域部分只能靠历史帧猜 → 运动时糊 + 拖影。
2. **共享纹理格式受限**。`R32G32_FLOAT` / `R32_FLOAT`（depth/mvec 常用的格式）在这套
   `D3D11_RESOURCE_MISC_SHARED` 路径上创建失败（`hr=0x80070057`），只有 8-bit 颜色格式能共享。
3. **帧生成不可行**。`amd_fidelityfx_framegeneration_dx12.dll` 只支持 DX12，且需要自己接管
   swapchain 与 pacing；D3D11 游戏上做原生 FG 不现实，而且同样缺 mvec。

实测对比：原生 + 8x MSAA 比这套 FSR 更清晰也更流畅。

## 目录

```
src/dllmain.cpp         dxgi 代理、钩子、FSR 管线（Present / Resize / Apply）
src/ffx_upscaler.cpp    FFX API 封装（provider 枚举 / context / dispatch）
src/menu.cpp            ImGui 菜单
src/il2cpp_mvec.cpp     Unity 侧取 mvec/depth（本路线失败，保留作参考）
src/il2cpp_demosaic.cpp 去马赛克（同一 DLL 里的另一个功能，与超分无关）
build.cmd               MSVC 构建脚本
docs-notes.md           接入笔记：关键点、坑、复用清单
```

## 构建

需要自己准备（未随仓库分发）：

* **ImGui**（含 `backends/imgui_impl_dx11.*`、`imgui_impl_win32.*`）
* **MinHook**
* **AMD FidelityFX SDK 2.3.0** 的头文件（`ffx-api` 等）

放到 `vendor/imgui`、`vendor/minhook`、`vendor/ffx-include`，然后跑 `build.cmd`（MSVC x64）。
产物 `build/dxgi.dll`，与游戏 exe 同目录即可（它会作为 dxgi 代理被加载）。

## 已知限制

* 只处理单 swapchain；多 swapchain / 全屏独占未适配；
* 只在 Unity 2022.3 + D3D11 的单个游戏上验证过；
* 与 ReShade / Special K 等同样占用 `dxgi.dll` 的工具**互斥**；
* 没有任何反作弊/反调试对抗措施。

## License

BSD 2-Clause，见 [LICENSE](LICENSE)。

第三方依赖的许可证：AMD FidelityFX SDK（MIT）、Dear ImGui（MIT）、MinHook（BSD 2-Clause），
均不随本仓库分发。
