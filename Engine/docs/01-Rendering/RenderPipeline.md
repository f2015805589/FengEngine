# RenderPipeline - 渲染管线总览

> 本文描述 FEngine 基于 DirectX 12 的延迟渲染管线的整体结构：Pass 执行顺序、共享资源、帧循环与双 CommandAllocator 帧管理。管线的驱动代码位于 `Engine/main.cpp` 的主循环，各 Pass 的实现分散在 `Engine/private/` 与 `Engine/Shader/` 中。

## 概述

FEngine 当前采用 **延迟渲染（Deferred Rendering）** 流程：先由 BasePass 将场景几何写入 4 张 G-Buffer 离屏 RT（Albedo / Normal / ORM / Motion Vector），再由 LightPass 计算平行光阴影因子，随后可选的 GTAO / SSGI 两个屏幕空间效果 Pass 分别产出 AO 与间接光，最后 SkyPass 绘制天空球、ScreenPass 用 G-Buffer 做延迟光照合成，TAA 做时域抗锯齿，ImGui 作为 UI 层叠加输出到交换链。

各 Pass 之间通过**共享纹理资源**传递数据（G-Buffer、深度缓冲、Shadow Map、AO 纹理、SSGI 纹理、TAA 中间/历史缓冲），并且全部 Pass 共用一个**根签名**（root signature）与一套静态采样器。

### 渲染流程一览

```
[BeginFrame] 双 allocator 轮转 + GPU 等待
      │
      ├─ 处理窗口/视口/分辨率变更（FlushGPU + Resize）
      ├─ 延迟纹理加载（TexturePreviewPanel 待处理项）
      ├─ TAA Jitter 更新 → g_scene->SetJitterOffset()
      ├─ g_scene->Update(deltaTime)   // 相机、光照、LiSPSM 矩阵、SceneCB
      │
      ▼
 1. BasePass      G-Buffer 填充（4 张离屏 RT + 深度）        [Scene::Render]
      ▼
 2. LightPass     子 Pass A: Shadow Map 深度渲染              [shadowdepth.hlsl]
                  子 Pass B: 延迟光照（阴影因子 → LightRT）   [lighting.hlsl]
      ▼
 3. GtaoPass      Raw AO → Cross-Bilateral Blur              [GTAO.hlsl / GTAOBlur.hlsl]
      ▼
 4. SsgiPass      Raymarch → 时序累积 → Upsample → Blur H/V  [SSGI 系列 hlsl]
      ▼
 5. SkyPass       天空球 → 中间 RT（TAA 开）或视口颜色缓冲（TAA 关）
      ▼
 6. ScreenPass    延迟光照合成（直接光 + IBL + AO + SSGI lerp）
      ▼
 7. TaaPass       TAA resolve → History；TaaCopy → 视口颜色缓冲
      ▼
 8. UIPass        ImGui 绘制（ImGui_ImplDX12_RenderDrawData）
      ▼
[EndFrame] Close + Execute + Signal(fence) + 切换 allocator → Present
```

上图中 `BasePass / LightPass / GtaoPass / SsgiPass / SkyPass / ScreenPass / TaaPass / TaaCopy / UIPass` 均通过 `commandList->BeginEvent/EndEvent` 标记，可在 GPU 调试器中按事件过滤查看。

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/main.cpp` | 应用入口、初始化、主循环、Pass 调度与 UI |
| `Engine/private/BattleFireDirect.cpp` | D3D12 设备/交换链/命令队列、双 allocator 帧管理、根签名、共享全屏 VB/PSO |
| `Engine/public/BattleFireDirect.h` | 全局变量声明、`SceneCBData` 共享 CB 布局、工具函数声明 |
| `Engine/private/Scene.cpp` | 场景更新、`Scene::Render`（BasePass）、LiSPSM/阴影矩阵、离屏 RT 管理 |
| `Engine/private/LightPass.cpp` | 阴影图渲染 + 延迟光照（阴影因子） |
| `Engine/private/GtaoPass.cpp` | GTAO/SSAO 计算与模糊 |
| `Engine/private/SsgiPass.cpp` | SSGI 光线步进、时序累积、升采样与模糊 |
| `Engine/private/TaaPass.cpp` | TAA Jitter、历史缓冲、resolve 与拷贝 |
| `Engine/private/SkyPass.cpp` | 天空球几何体与渲染 |
| `Engine/private/ScreenPass.cpp` | 全屏合成 Pass 的 SRV 绑定与绘制 |
| `Engine/private/ImguiPass.cpp` | ImGui 专用 PSO 创建（`CreateUiPSO`） |
| `Engine/private/ViewportManager.cpp` | 视口颜色缓冲（最终 3D 输出目标）管理 |

## 架构与数据流

### 根签名（全局唯一）

`InitRootSignature()` 创建版本 1.1 根签名，共 3 个根参数 + 6 个静态采样器：

| 根参数 | 类型 | 绑定 | 说明 |
| --- | --- | --- | --- |
| Slot 0 | CBV | `b0` | 场景常量缓冲（`SceneCBData`，176 floats） |
| Slot 1 | 描述符表 | `t0..tUINT_MAX` | Bindless SRV 无界数组（`DESCRIPTORS_VOLATILE`） |
| Slot 2 | CBV | `b1` | 材质 CB 或各 Pass 私有 CB（GTAO/SSGI/TAA 常量） |

静态采样器：`s0` PointWrap、`s1` PointClamp、`s2` LinearWrap、`s3` LinearClamp、`s4` AnisoWrap、`s5` AnisoClamp。

### 共享资源与生产者/消费者

| 资源 | 格式 | 生产者 | 消费者 |
| --- | --- | --- | --- |
| RT0 Albedo | `R16G16B16A16_FLOAT` | BasePass | ScreenPass、SsgiPass |
| RT1 Normal | `R16G16B16A16_FLOAT` | BasePass | ScreenPass、GtaoPass、SsgiPass |
| RT2 ORM | `R16G16B16A16_FLOAT` | BasePass | ScreenPass |
| RT3 Motion Vector | `R16G16_FLOAT` | BasePass | TaaPass、SsgiPass |
| `gDSRT` 深度缓冲 | `R24G8_TYPELESS`（DSV: `D24_UNORM_S8_UINT`，SRV: `R24_UNORM_X8_TYPELESS`） | BasePass | LightPass、GtaoPass、SsgiPass、ScreenPass、TaaPass |
| Shadow Map | `R32_TYPELESS`（DSV/SRV: `R32_FLOAT`） | LightPass 子 Pass A | LightPass 子 Pass B、ScreenPass（经 LightRT） |
| LightRT 阴影因子 | `R16G16B16A16_FLOAT` | LightPass 子 Pass B | ScreenPass（t5） |
| GTAO Raw/Blurred | `R8G8B8A8_UNORM` | GtaoPass | GtaoPass Blur、ScreenPass（t6） |
| SSGI Final | `R16G16B16A16_FLOAT` | SsgiPass | ScreenPass（t7） |
| TAA 中间 RT | `R16G16B16A16_FLOAT` | SkyPass + ScreenPass | TaaPass |
| TAA History1/2 | `R16G16B16A16_FLOAT` | TaaPass | TaaPass（下一帧） |
| 视口颜色缓冲 | `R8G8B8A8_UNORM` | TAA/TaaCopy 或 ScreenPass | ImGui `ImGui::Image` 显示 |

### 深度缓冲的状态机

`gDSRT` 在整个帧内被反复转换，这是帧内资源状态管理的核心：

```
DEPTH_WRITE ──BasePass 渲染──▶ 渲染结束后转 PIXEL_SHADER_RESOURCE
     ▲                                    │
     │                                    ▼
     │                            LightPass / GtaoPass / SsgiPass 采样
     │                                    │
     │                                    ▼
     │                          ScreenPass 采样 → 转回 DEPTH_WRITE
     │                                    │
     │                     TAA 开启时：转 PIXEL_SHADER_RESOURCE 供 TAA 采样
     │                                    │
     └──── TAA 结束后转回 DEPTH_WRITE ◀───┘
```

## 关键实现要点

### 双 CommandAllocator 帧管理

`BattleFireDirect.cpp` 维护 `gCommandAllocators[2]`、`gFrameFenceValues[2]` 与 `gFrameIndex`：

- `BeginFrame()`：取当前 `gFrameIndex` 对应的 allocator，若 `gFrameFenceValues[idx]` 非 0 且 fence 尚未完成，则等待该值（即等待**两帧前**使用同一 allocator 的 GPU 工作完成），随后 `Reset()` 并返回。
- `EndFrame()`：`Close()` → `ExecuteCommandLists` → `Signal(gFence, ++gFenceValue)` → 把 fence 值记入 `gFrameFenceValues[gFrameIndex]` → `gFrameIndex = 1 - gFrameIndex` 轮转。

这样 CPU 可以比 GPU 领先约一帧运行，而不必每帧全量等待。

### 场景常量缓冲（SceneCB）

`SceneCBData`（176 floats = 704 字节）由 `FillSceneCBData()` 填充，`Scene::Update()` 每帧：

1. 计算相机视图/投影矩阵，并把 TAA Jitter 应用到投影矩阵第 2 行（`r[2].m128_f32[0/1]`）；
2. 用**不带 Jitter** 的原始投影矩阵计算 `lightViewProjMatrix`（LiSPSM）与 `currentViewProjMatrix`（供 Motion Vector 使用）；
3. 写入 `previousViewProjMatrix`（TAA 重投影）、`jitterParams`、`screenParams`、`nearFarParams`、`shadowMode`、`giType`（存放在 `skylightParams.y`）等字段；
4. 通过持久映射 `memcpy` 到上传堆。

### 分辨率/视口变更

帧首依次检查三种变更请求，统一先 `FlushGPU()` 再重建资源：

1. `g_pendingSwapChainResize`（窗口物理尺寸变化）→ `ResizeSwapChainOnly()`；
2. `pendingViewportResize`（Docking 布局变化）→ `ResizeViewportRenderTargets()`，该 lambda 依次重建 `ViewportManager`、`Scene::ResizeRenderTargets`、`LightPass/ScreenPass/TaaPass/GtaoPass/SsgiPass`；
3. `Settings::IsPendingResolutionChange()`（UI 选择分辨率）→ 按需 `ResizeSwapChainOnly()` + `ResizeViewportRenderTargets()`。

### 帧率限制

`EndFrame` 后依据 `Settings::GetInstance().GetFpsLimit()`（默认 144，0 为不限）用 `Sleep` 把 CPU 帧时间补足到目标值。

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `BeginFrame() / EndFrame()` | `BattleFireDirect.h` | 帧开始/结束，内部完成 allocator 轮转与 fence 记录 |
| `EndCommandList() / WaitForCompletionOfCommandList() / FlushGPU()` | `BattleFireDirect.h` | 提交/等待/强制同步 |
| `BeginOffscreen(commandList)` | `BattleFireDirect.h` | 设置全屏 viewport（BasePass 使用） |
| `BeginRenderToSwapChain(cmd, isClear, bindDepth)` / `EndRenderToSwapChain(cmd)` | `BattleFireDirect.h` | 交换链 RTV 绑定与 Present 状态转换 |
| `GetSharedFullscreenQuadVB(outVBV)` | `BattleFireDirect.h` | 全局共享全屏四边形 VB（Position3 + UV2，6 顶点） |
| `CreateFullscreenPSO(rootSig, vs, ps, rtvFormat, enableAlphaBlend)` | `BattleFireDirect.h` | 全屏 Pass 通用 PSO（禁用深度、CullNone） |
| `InitRootSignature()` | `BattleFireDirect.h` | 创建全局根签名（启动时调用一次） |
| `Settings::GetInstance()` | `Engine/public/Settings.h` | 分辨率、FPS 限制、TAA Jitter 缩放等全局设置 |

## 配置与调参

- **Pass 开关**：`g_scene->IsShadowmapEnabled()`、`gtaoPass->IsEnabled()`、`ssgiPass->IsEnabled()`、`taaPass->IsEnabled()` 决定对应 Pass 是否执行；UI（Setting 窗口）可实时切换。
- **渲染分辨率**：Setting 窗口下拉框 → `Settings::RequestResolutionChange()`，可同时调整窗口与视口。
- **TAA Jitter 强度**：`Settings::SetTaaJitterScale()`（默认 1.0），作用于 `TaaPass::UpdateJitter()`。
- **阴影模式**：`g_scene->SetShadowMode(0|1|2)`（Hard / PCF / PCSS），写入 `SceneCBData.shadowMode`。
- **GI 模式**：`ssgiPass->SetGIType()` + `g_scene->SetGIType()` 同步设置，`giType` 经 SceneCB 传给 Screen.shader 决定 IBL/SSGI 混合方式。

## 已知限制与 TODO

- BasePass 之后所有屏幕空间 Pass 都依赖 `gDSRT` 处于 `PIXEL_SHADER_RESOURCE`；若新增需要深度写的 Pass，必须注意插入位置与状态转换。
- 当前管线只有单一平行光，`SceneCBData.lightDirection` 由 `Scene::Update` 中的旋转矩阵（初始方向 `(-1,-1,1)`）生成，尚无多光源支持。
- `ShadowPass` 类已实现但主循环未实例化，实际阴影路径走 `LightPass::RenderShadowMap`（见 ShadowPass.md），两处实现存在重复，后续应统一。
- Viewport 尺寸变更依赖 Docking 布局驱动，首次启动时视口宽高与渲染目标可能短暂不一致。

## 维护注意事项

- **新增 Pass 时**：优先复用 `CreateFullscreenPSO` 与共享全屏 VB；若需要新纹理槽位，注意 `ScreenPass` 的 8 槽 SRV 堆（t0-t7）顺序已固定，编译器注入的 GTAO/SSGI 纹理分别是 t6/t7。
- **修改 SceneCBData 布局时**：必须同步更新所有 hlsl 中 `cbuffer SceneConstants/DefaultVertexCB` 的成员顺序与偏移注释，否则会出现数据错位（已有多处 shader 依赖 16 字节对齐的偏移注释）。
- **资源状态**：所有 Pass 结束时都应把资源转回 `PIXEL_SHADER_RESOURCE` 或 `DEPTH_WRITE`，与下一 Pass 的期望状态一致；新增分支时优先使用 `ResourceBarrier` 显式转换而非依赖假设。
- **帧管理**：不要在主循环外直接 `Reset` 当前 allocator；需要与 GPU 完全同步时使用 `FlushGPU()` 而不是 `WaitForCompletionOfCommandList()`（后者只等最近一次提交）。
## 帧循环详细流程（对应 main.cpp 主循环）

```
while (true) {
    PeekMessage(...);                    // 消息泵
    commandAllocator = BeginFrame();     // 等待上一轮该 allocator 的 GPU 完成并 Reset

    if (g_pendingSwapChainResize)        // 窗口物理尺寸变化
        { FlushGPU(); ResizeSwapChainOnly(...); }
    if (pendingViewportResize)           // Docking 布局变化
        { FlushGPU(); ResizeViewportRenderTargets(...); }
    if (Settings 有挂起的分辨率变更)
        { FlushGPU(); ResizeSwapChainOnly?; ResizeViewportRenderTargets(...); }

    if (TexturePreviewPanel 有待加载纹理)  // 延迟纹理加载
        { Reset; 处理; EndCommandList; WaitForCompletion; }

    taaPass->UpdateJitter();             // Halton 抖动
    g_scene->SetJitterOffset(...);
    g_scene->Update(deltaTime);          // 相机 + LiSPSM + SceneCB

    // ---- 3D 渲染 ----
    commandList->Reset(commandAllocator, gbufferPso);
    BeginOffscreen(commandList);
    g_scene->Render(...);                // BasePass（事件 "BasePass"）
    if (阴影开启) lightPass->RenderDirectLight(...);  // 事件 "LightPass"
    if (AO 开启)  gtaoPass->Render(...);               // 事件 "GtaoPass"
    if (SSGI 开启) ssgiPass->Render(...);              // 事件 "SsgiPass"
    skyPass->Render(...);                // 事件 "SkyPass"，目标 = 中间RT / 视口颜色缓冲
    screenPass->Render(...);             // 事件 "ScreenPass"，延迟光照合成
    // 深度缓冲转回 DEPTH_WRITE
    if (TAA 开启) {
        深度 → PIXEL_SHADER_RESOURCE;
        taaPass->RenderToSwapChain(...); // 事件 "TaaPass"（写入 History）
        taaPass->CopyToSwapChain(...);   // 事件 "TaaCopy"（History → 视口颜色缓冲）
        深度 → DEPTH_WRITE;
        g_scene->UpdatePreviousViewProjectionMatrix();
        taaPass->SwapHistoryBuffers();
    }

    // ---- UI ----
    ImGui_ImplDX12_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    绘制菜单栏 / DockSpace / Viewport Image / 各面板   // 事件 "UIPass"
    ImGui::Render();

    视口颜色缓冲 → PIXEL_SHADER_RESOURCE;
    BeginRenderToSwapChain(commandList, true, false);   // 绑定交换链 RTV，不绑深度
    SetDescriptorHeaps(gImGuiDescriptorHeap);
    ImGui_ImplDX12_RenderDrawData(...);
    EndRenderToSwapChain(commandList);

    EndFrame();                          // Close + Execute + Signal + 切换 allocator
    SwapD3D12Buffers();                  // Present(0, 0)
    // 可选的 FPS 限制 Sleep
}
```

## 初始化顺序（启动阶段）

| 步骤 | 内容 |
| --- | --- |
| 1 | `InitD3D12(hwnd, w, h)`：Debug 层、设备（Feature Level 11_0）、命令队列、双缓冲交换链、`gDSRT`、RTV/DSV 堆、ImGui SRV 堆（100 描述符） |
| 2 | `InitImGui(...)`：ImGui 上下文、Win32 + DX12 后端 |
| 3 | `ViewportManager::Initialize/Resize`：视口颜色缓冲 |
| 4 | `Scene` 创建 + 纹理异步加载（`AsyncLoadTextures`） |
| 5 | 各 Pass 构造与初始化：`LightPass(4096)`、`ScreenPass`、`SkyPass(500.0)`、`TaaPass`、`GtaoPass`、`SsgiPass` |
| 6 | `InitRootSignature()`；编译 `ndctriangle.hlsl` 创建 `BasePso`；编译 `lighting.hlsl / shadowdepth.hlsl / screen.hlsl / TAA.hlsl / GTAO.hlsl / GTAOBlur.hlsl / SSGI.hlsl / SSGIBlur.hlsl / SSGIUpsample.hlsl / TaaCopy.hlsl` 创建各 PSO |
| 7 | `ResourceManager` 扫描加载 shader/材质；`StandardPBR` 的 Pass 0（GBuffer）与 Pass 1（DeferredLighting，自动从 `Screen.shader` 获取）创建 `gbufferPso / deferredLightingPso` |
| 8 | 加载 `Sky.shader` 创建 `skyPso`；`g_scene->LoadLevel(Default.level)` |

## 各 Pass 输入/输出汇总

| Pass | 输入（SRV） | 输出（RTV） | 着色器 |
| --- | --- | --- | --- |
| BasePass | 材质纹理（Bindless t10+） | RT0-3 + 深度 | `StandardPBR.shader` Pass 0 |
| LightPass A | 无 | ShadowMap（DSV） | `shadowdepth.hlsl` |
| LightPass B | t0 深度、t1 ShadowMap | LightRT | `lighting.hlsl` |
| GtaoPass | t0 深度、t1 法线 | Raw AO → Blurred AO | `GTAO.hlsl` / `GTAOBlur.hlsl` |
| SsgiPass | t0-t6（深度Max、BaseColor、法线、噪声、深度、History、Velocity） | Raw → Temp → Final | `SSGI.hlsl` / `SSGIUpsample.hlsl` / `SSGIBlur.hlsl` |
| SkyPass | SkyCube | 中间RT / 视口颜色 | `Sky.shader` |
| ScreenPass | t0-t7（GBuffer、深度、SkyCube、LightRT、AO、SSGI） | 中间RT / 视口颜色 | `Screen.shader`（DeferredLighting） |
| TaaPass | t0 当前帧、t1 History、t2 MotionVector、t3 深度 | History | `TAA.hlsl` |
| TaaCopy | t0 History | 视口颜色 | `TaaCopy.hlsl` |

## 调试与性能分析建议

- 所有 Pass 都有 `BeginEvent` 命名，可在 PIX / RenderDoc / VS 图形调试器中按事件抓帧。
- `TAA.hlsl` 内置 `DEBUG_MODE` 宏（0=正常，1=当前帧，2=历史帧，3=Motion Vector），修改后重新编译即可快速验证 TAA 各阶段。
- 关注帧内最耗时的 Pass：默认配置下 PCSS（25 次 PCF 采样 + 16 次 blocker 搜索）与 SSGI（默认 Quarter 分辨率、64 方向、128 步）开销最大，可通过 UI 降低方向数/步数或关闭相应 Pass。
- 输出到视口颜色缓冲的最终图像由 `ViewportManager::GetColorSRV()` 提供给 ImGui，因此 3D 画面与 UI 在同一帧内完成合成，无需额外的拷贝 Pass。
