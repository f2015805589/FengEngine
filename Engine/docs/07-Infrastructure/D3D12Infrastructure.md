# D3D12Infrastructure — D3D12 基础设施层

> 本文介绍 FEngine 的 D3D12 基础设施：`BattleFireDirect.h/cpp` 中的设备/交换链/根签名/全局状态、多 Pass 主循环（`main.cpp`）与帧管理（双命令分配器 + Fence）。该层是所有 Pass、材质系统与 ImGui 后端的公共底座。

## 概述

`BattleFireDirect`（文件名为历史遗留，类内实际是一组全局函数与全局变量）承担以下职责：

- **设备与队列**：适配器枚举（跳过软件适配器）→ `D3D12CreateDevice`（Feature Level 11.0）→ 命令队列 → 双缓冲 `FLIP_DISCARD` 交换链；
- **资源与视图**：交换链 RTV 堆、DSV 堆、深度缓冲（`gDSRT`）、ImGui 描述符堆、双命令分配器、Fence；
- **根签名**：`InitRootSignature` 创建支持 Bindless 纹理的根签名（1.1，失败回退 1.0）；
- **共享工具**：着色器编译、常量缓冲区/缓冲区创建、场景 PSO（4 个 RTV 的 GBuffer PSO）、全屏四边形 PSO、`SceneCBData` 填充；
- **帧管理**：`BeginFrame / EndFrame / SwapD3D12Buffers` 构成每帧生命周期；`BeginRenderToSwapChain / EndRenderToSwapChain` 管理交换链渲染；
- **分辨率**：`ResizeSwapChainAndDepthBuffer / ResizeSwapChainOnly` 两套调整路径；
- **ImGui**：`InitImGui / ShutdownImGui`。

渲染管线的多 Pass 主循环（BasePass → LightPass → GTAO → SSGI → SkyPass → ScreenPass → TAA → UIPass）由 `main.cpp` 编排，详见“多 Pass 主循环”一节。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/public/BattleFireDirect.h` | 全局变量声明、工具函数声明、`SceneCBData` 结构、`FillSceneCBData` |
| `Engine/private/BattleFireDirect.cpp` | 上述全部实现（约 1025 行） |
| `Engine/main.cpp` | WinMain：窗口、初始化顺序、多 Pass 主循环、UI、清理 |
| `Engine/public/ImguiPass.h`、`Engine/private/ImguiPass.cpp` | ImGui 专用 PSO（`CreateUiPSO`） |
| `Engine/public/ViewportManager.h/cpp` | 视口颜色/深度缓冲（`gDSRT` 的所有者） |

## 架构与数据流

### 初始化顺序（main.cpp WinMain）

```text
RegisterClassEx / CreateWindow（1280x720）
→ InitD3D12(hwnd, w, h)          // 设备/队列/交换链/深度/RTV/DSV/双 allocator/Fence/ImGui heap
→ Settings::Initialize(w, h)
→ InitImGui(hwnd, device, gImGuiDescriptorHeap, srvDescSize)
    ├── io.ConfigFlags |= DockingEnable；&~= ViewportsEnable
    └── io.IniFilename = Saved/Config/FEngineLayout.ini
→ ViewportManager::Initialize + Resize(1280,720)
→ MaterialManager / TextureManager / TextureCompressor / TexturePreviewPanel 初始化
→ new Scene(w, h) + AsyncLoadTextures + Scene::Initialize
→ LightPass(4096 阴影图) / ScreenPass / SkyPass / TaaPass / GtaoPass / SsgiPass 初始化
→ InitRootSignature + 各 PSO（Base/gbuffer/light/shadow/screen/taa/gtao/ssgi/ui…）
→ 材质系统：LoadShader(StandardPBR) → CreateMaterial(DefaultPBR) → EndCommandList + Wait
→ Sky.shader 加载（独立 PSO，input layout 与场景 mesh 不同）
→ LoadLevel(Content/Level/Default.level)
→ 主循环
```

### 每帧生命周期

```text
BeginFrame()                          // 双 allocator 轮转：等待上一轮 fence，Reset allocator
├── 窗口 WM_SIZE / 视口 / 分辨率请求处理（FlushGPU + Resize*）
├── 纹理延迟加载（TexturePreviewPanel::ProcessPendingLoad）
├── TAA jitter 更新 → g_scene->SetJitterOffset
├── g_scene->Update(deltaTime)        // 场景 CB 填充
├── BasePass：commandList->Reset(allocator, gbufferPso) + BeginOffscreen + Scene::Render
├── LightPass（shadowmap 开启时）：RenderDirectLight（阴影图 + 直接光照）
├── GtaoPass（IsEnabled）：深度 + 法线 → AO
├── SsgiPass（IsEnabled）：深度 + BaseColor + Normal + Velocity → SSGI
├── SkyPass（skyPso 有效）：天空球 → 视口颜色缓冲 / TAA 中间 RT
├── ScreenPass：GBuffer + 阴影 + AO + SSGI + 天空 → 视口颜色缓冲 / 中间 RT
├── TaaPass（IsEnabled）：运动矢量 + 历史 → 中间 RT → TaaCopy 到视口颜色缓冲
├── UIPass：ImGui 帧 → 面板 → RenderDrawData 到交换链
EndFrame()                            // Close + Execute + Signal(fence) + 记录 gFrameFenceValues
SwapD3D12Buffers()                    // Present(0,0)
（可选）FPS 限制 Sleep
```

### 全局状态（BattleFireDirect.cpp 第 9-30 行）

```cpp
ID3D12Device* gD3D12Device;          ID3D12CommandQueue* gCommandQueue;
IDXGISwapChain3* gSwapChain;         ID3D12Resource* gDSRT, * gColorRTs[2];
int gCurrentRTIndex;                 ID3D12DescriptorHeap* gSwapChainRTVHeap;
ID3D12DescriptorHeap* gSwapChainDSVHeap;  UINT gRTVDescriptorSize, gDSVDescriptorSize;
ID3D12CommandAllocator* gCommandAllocators[2];  UINT64 gFrameFenceValues[2];
int gFrameIndex;                     ID3D12GraphicsCommandList* gCommandList;
ID3D12Fence* gFence;                 HANDLE gFenceEvent;  UINT64 gFenceValue;
int gRenderWidth = 1280, gRenderHeight = 720;   HWND gMainHWND;
```

## 关键实现要点

### InitD3D12（设备/交换链/堆）

- Debug 模式下启用 `ID3D12Debug` 与 `DXGI_CREATE_FACTORY_DEBUG`；
- 适配器枚举：跳过 `DXGI_ADAPTER_FLAG_SOFTWARE`，用 `D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0)` 试创建；
- 交换链：2 个缓冲、`R8G8B8A8_UNORM`、`DXGI_SWAP_EFFECT_FLIP_DISCARD`、窗口模式；
- 深度缓冲：`R24G8_TYPELESS`（DSV 用 `D24_UNORM_S8_UINT`），允许 DSV + SRV 双用；
- 描述符堆：RTV 堆 2 个描述符（双缓冲）、DSV 堆 1 个、ImGui SRV 堆 100 个（shader visible）；
- 帧同步：两个 `D3D12_COMMAND_LIST_TYPE_DIRECT` allocator + 1 个 Fence + 事件。

### InitRootSignature（Bindless 支持）

- 检查 `D3D12_FEATURE_ROOT_SIGNATURE`，优先版本 1.1，失败回退 1.0；
- 1.1 版本：3 个根参数 —— slot 0: CBV b0（场景 CB，`SHADER_VISIBILITY_ALL`）、slot 1: 描述符表（`NumDescriptors = UINT_MAX` 无界数组，`DESCRIPTORS_VOLATILE`，从 t0 开始，Pixel 可见）、slot 2: CBV b1（材质 CB，Pixel 可见）；
- 6 个静态采样器：`pointWarp/pointClamp/linearWarp/linearClamp/anisotropicWarp/anisotropicClamp`（s0-s5）；
- 1.0 回退：SRV 表 1000 个描述符（不支持无界），3 个根参数结构相同。

### 共享工具函数

- `CreateShaderFromFile`：`D3DCompileFromFile`，入口/目标由调用方指定，固定 `D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION`，失败 `printf` 错误 blob；
- `CreateConstantBufferObject`：`D3D12_HEAP_TYPE_UPLOAD` 的 Buffer；`UpdateConstantBuffer`：Map+memcpy+Unmap（一次性）；
- `CreateBufferObject`：DEFAULT 堆 + UPLOAD 临时缓冲 + `CopyBufferRegion` + 状态转换到 `inFinalResourceState`（顶点/索引缓冲通用）；
- `CreateScenePSO`：4 个 RTV（RT0-2 `R16G16B16A16_FLOAT`、RT3 `R16G16_FLOAT`）、DSV `D24_UNORM_S8_UINT`、`FrontCounterClockwise=TRUE`、深度测试开启、**全部 RT 关闭混合**（GBuffer 直接覆盖写）、4 分量 float 输入布局；
- `GetSharedFullscreenQuadVB`：全局单例全屏四边形（Position3+UV2，6 顶点，UPLOAD 堆），首次调用创建；
- `CreateFullscreenPSO`：Position3+UV2 输入布局、`CULL_MODE_NONE`、深度关闭、可选 alpha 混合、单个 RTV 格式参数化；
- `FillSceneCBData`：填充 `SceneCBData`（176 floats）：投影/视图/模型/法线矩阵、光照方向、相机位置、skylightParams（x=强度, y=giType）、逆矩阵、skylightColor、lightViewProj、prevViewProj、jitterParams、screenParams、nearFarParams、currentViewProj、shadowMode；法线矩阵 = `transpose(inverse(model))`，奇异时回退单位矩阵。

### 帧管理（双 allocator）

- `BeginFrame`：按 `gFrameIndex` 取 allocator，若 `gFrameFenceValues[idx] > 0` 且 fence 未完成则等待事件；Reset 后设为当前 allocator 并返回；
- `EndFrame`：Close → ExecuteCommandLists → `gFenceValue++` → Signal → 记录 `gFrameFenceValues[gFrameIndex]` → `gFrameIndex = 1 - gFrameIndex`（轮转）；
- `WaitForCompletionOfCommandList`：等 `gFenceValue`；`FlushGPU`：Signal 新值并阻塞等待（全同步用）；
- 注意：`EndCommandList`（初始化阶段使用）与 `EndFrame` 都执行 Close/Execute/Signal，但前者不轮转 allocator 也不记录帧 fence 值。

### 交换链渲染辅助

- `BeginRenderToSwapChain(commandList, isClear, bindDepth)`：`GetCurrentBackBufferIndex` → PRESENT→RENDER_TARGET 屏障 → 绑定 RTV（可选 DSV）→ 可选清除 → 设置视口/裁剪（`gRenderWidth/Height`）；
- `EndRenderToSwapChain`：RENDER_TARGET→PRESENT 屏障；
- `SwapD3D12Buffers`：`Present(0, 0)`（无 VSync 参数）；
- `ResizeSwapChainAndDepthBuffer`：释放旧 RT/深度 → `ResizeBuffers(2, w, h, R8G8B8A8_UNORM, 0)` → 重建 RT/深度/DSV → 更新 `gRenderWidth/Height` → 可选 `SetWindowPos`；
- `ResizeSwapChainOnly`：只 ResizeBuffers + 重建 RTV，**不更新** `gRenderWidth/Height`（由 ViewportManager 维护，见 `CameraAndViewport.md`）。

### ImGui 集成

- `InitImGui`：`ImGui::CreateContext`、`StyleColorsDark`、`ImGui_ImplWin32_Init`、`ImGui_ImplDX12_Init(device, 2, R8G8B8A8_UNORM, srvHeap, cpuStart, gpuStart)`、`CreateDeviceObjects`；
- `ShutdownImGui`：依次 Shutdown 后端、销毁上下文、释放 `gImGuiDescriptorHeap`；
- `CreateUiPSO`（ImguiPass.cpp）：alpha 混合（SRC_ALPHA/INV_SRC_ALPHA）、深度关闭、4 分量 float 输入布局。

## 对外接口

`BattleFireDirect.h` 完整函数清单：

- 初始化/资源：`InitD3D12(HWND, w, h)`、`InitRootSignature()`、`InitResourceBarrier(...)`；
- 着色器/缓冲：`CreateShaderFromFile(...)`、`CreateConstantBufferObject(int)`、`UpdateConstantBuffer(...)`、`CreateBufferObject(...)`、`CreateScenePSO(...)`；
- 命令/帧：`GetCommandList()`、`GetCommandAllocator()`、`WaitForCompletionOfCommandList()`、`FlushGPU()`、`EndCommandList()`、`BeginFrame()`、`EndFrame()`、`BeginOffscreen(...)`、`BeginRenderToSwapChain(...)`、`EndRenderToSwapChain(...)`、`SwapD3D12Buffers()`、`GetCurrentSwapChainRTV()`；
- 分辨率：`ResizeSwapChainAndDepthBuffer(w, h, resizeWindow)`、`ResizeSwapChainOnly(w, h)`、`GetRenderWidth()`、`GetRenderHeight()`；
- 共享资源：`GetSharedFullscreenQuadVB(D3D12_VERTEX_BUFFER_VIEW&)`、`CreateFullscreenPSO(...)`；
- ImGui：`InitImGui(...)`、`ShutdownImGui()`；
- 共享 CB：`FillSceneCBData(...)`、`SceneCBData` 结构。

## 配置与调参

| 项 | 值 | 位置 |
| --- | --- | --- |
| Feature Level | 11.0 | `InitD3D12` |
| 交换链 | 双缓冲、FLIP_DISCARD、R8G8B8A8_UNORM | `InitD3D12` |
| Present | `Present(0, 0)`（无 VSync） | `SwapD3D12Buffers` |
| 深度缓冲 | R24G8_TYPELESS / D24_UNORM_S8_UINT | `InitD3D12` / `ResizeSwapChainAndDepthBuffer` |
| ImGui SRV 堆 | 100 描述符 | `InitD3D12` |
| 全局 SRV 堆（场景） | 1000 描述符 | `Scene::CreateSRVHeap` |
| 静态采样器 | 6 个（s0-s5） | `GetStaticSamplers` |
| 初始渲染分辨率 | 1280×720（`gRenderWidth/Height`） | `BattleFireDirect.cpp` |
| 编译标志 | DEBUG + SKIP_OPTIMIZATION（无条件） | `CreateShaderFromFile`、`CompileHLSLString` |
| 阴影图尺寸 | 4096（LightPass 构造参数） | `main.cpp` |

## 已知限制与 TODO

- 单命令队列、单命令列表：所有 Pass 串行提交，无并行录制；`BeginFrame` 的 allocator 等待会阻塞 CPU。
- 无 VSync/自适应帧率（Present 固定 `(0,0)`），帧率限制靠 CPU Sleep。
- 资源状态转换全部手工维护（`InitResourceBarrier`），没有资源状态追踪器；新增 Pass 时漏掉屏障会直接产生 GPU 错误（Debug Layer 可查）。
- `gDSRT` 生命周期跨两个模块（创建于 `BattleFireDirect`/`ResizeSwapChainAndDepthBuffer`，重建于 `ViewportManager`，释放于 `ViewportManager::ReleaseResources`），所有权分散，容易误释放。
- 无条件使用 `D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION`，Release 构建也没有优化着色器；无离线着色器缓存（每次启动重新编译，见 `ShaderAssets.md`）。
- 根签名 1.1 的 Bindless 表使用 `DESCRIPTORS_VOLATILE`，需要保证描述符内容在命令执行期间有效；目前依赖全局 `srvHeap` 常驻。
- `ResizeSwapChainAndDepthBuffer` 与 `ResizeSwapChainOnly` 职责重叠，前者已基本被后者 + ViewportManager 取代。
- 无 GPU 崩溃恢复（Device Removed 处理）、无资源泄漏检测工具（Debug 层之外）。

## 维护注意事项

- 新增全局资源时，明确写入 `BattleFireDirect.h` 的 extern 声明并统一命名规范（`g` 前缀）；避免在多个 .cpp 里各自声明 `extern`，防止声明不一致。
- **SceneCBData 是全局契约**：`BattleFireDirect.h` 的结构体布局必须与 `ShaderParser::GenerateHLSLCode` 生成的 HLSL CB（`DefaultVertexCB`，含 `ReservedMemory[1020]` 占位）保持一致；字段顺序/大小变化会导致所有 shader 错位。
- 修改 PSO 公共参数（RTV 数量、格式、混合、光栅化）时，检查所有调用方：`CreateScenePSO` 影响 BasePass/Forward；`CreateFullscreenPSO` 影响 Screen/TAA/GTAO/SSGI/Sky 等全屏 Pass；`CreateUiPSO` 只影响 ImGui。
- 帧管理约定：初始化阶段用 `EndCommandList + WaitForCompletionOfCommandList`；运行阶段用 `BeginFrame / EndFrame / SwapD3D12Buffers`，不要混用，否则 allocator 轮转与 fence 记录会错乱。
- `FlushGPU` 开销大（阻塞 CPU），只用于分辨率变更、资源重建等需要完全同步的场景。
- 修改 `InitD3D12` 中的描述符堆数量时，同步检查各模块的硬编码槽位（场景 SRV t0-t12、ImGui heap 的视口偏移 2、纹理预览 `IMGUI_TEXTURE_SLOT_START=10`）。
- Debug 层依赖 `_DEBUG` 宏（`DX12_ENABLE_DEBUG_LAYER`），Release 构建会跳过；调试 GPU 问题时确认当前构建配置。