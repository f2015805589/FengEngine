# ImguiPass - ImGui 渲染接入

> ImGui 是 FEngine 的编辑器 UI 层。`ImguiPass.h/cpp` 只提供一个函数：`CreateUiPSO()`，用于创建 ImGui 的图形管线状态对象；完整的 UI 初始化、每帧绘制与提交分布在 `BattleFireDirect.cpp`（`InitImGui / ShutdownImGui`）与 `main.cpp`（UIPass 事件）中。UI 最终与 3D 画面同帧合入交换链。

## 概述

```
初始化（启动时）:
  InitImGui(hwnd, device, gImGuiDescriptorHeap, srvSize)
    ├─ ImGui::CreateContext() + StyleColorsDark + AddFontDefault
    ├─ ImGui_ImplWin32_Init(hwnd)
    ├─ ImGui_ImplDX12_Init(device, 2, R8G8B8A8_UNORM, heap, cpu, gpu)
    └─ ImGui_ImplDX12_CreateDeviceObjects()
        │
        ▼ 每帧（main.cpp UIPass 事件）
  ImGui_ImplDX12_NewFrame / ImGui_ImplWin32_NewFrame / ImGui::NewFrame
    绘制主菜单栏 / DockSpace / Viewport Image / 各面板
  ImGui::Render()
    视口颜色缓冲: RENDER_TARGET → PIXEL_SHADER_RESOURCE（供 Image 采样）
    BeginRenderToSwapChain(cmd, true, false)          // 交换链 RTV，不绑深度
    SetDescriptorHeaps(gImGuiDescriptorHeap)
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd)
    EndRenderToSwapChain(cmd)
        │
        ▼
  EndFrame() → SwapD3D12Buffers()（Present）
```

- ImGui 与 3D 共用同一个 `gCommandList` 与帧 allocator，因此 UI 绘制发生在 TAA 输出之后、Present 之前；
- 输入事件通过 `WindowProc` 中的 `ImGui_ImplWin32_WndProcHandler` 注入；
- 编辑器面板（Scene/Actor/Material/Texture 等）都挂在该 UI 帧上，由 `WindowLayoutState` 持久化可见性。

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/public/ImguiPass.h` | `CreateUiPSO` 声明（引用全局 `gD3D12Device`） |
| `Engine/private/ImguiPass.cpp` | `CreateUiPSO` 实现（输入布局、混合、深度状态） |
| `Engine/private/BattleFireDirect.cpp` | `InitImGui / ShutdownImGui`、`gImGuiDescriptorHeap`（100 描述符） |
| `Engine/public/BattleFireDirect.h` | `gImGuiDescriptorHeap` 全局声明、`InitImGui/ShutdownImGui` |
| `Engine/main.cpp` | `WindowProc` 输入接入、UIPass 事件、各面板绘制、`ImGui::Render` 提交 |
| `Engine/private/ViewportManager.cpp` | 在 ImGui SRV 堆中分配视口颜色缓冲 SRV（偏移 2） |
| `ImGui/`（第三方） | `imgui.h`、`imgui_impl_win32.h`、`imgui_impl_dx12.h` |

## 架构与数据流

### 描述符堆的共享

`gImGuiDescriptorHeap`（`D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV`，`SHADER_VISIBLE`，100 个描述符）同时服务：

| 使用者 | 偏移 | 说明 |
| --- | --- | --- |
| ImGui DX12 后端 | 0 起 | 字体图集等内部资源（`ImGui_ImplDX12_Init` 传入堆起点） |
| ViewportManager 视口颜色 SRV | `s_viewportSrvOffset = 2` | `ImGui::Image` 显示 3D 结果 |
| 其他面板纹理预览 | 2 之后 | `TexturePreviewPanel` 等按需分配 |

### 每帧 UI 数据流

```
ImGui::NewFrame（三个 NewFrame 调用）
  ├─ 主菜单栏（File/View/Texture/... + FPS）
  ├─ DockSpace（ImGui::DockSpaceOverViewport("MainDockSpace")）
  ├─ Viewport 窗口: 检测 hover/尺寸 → 记录 g_viewportRect* → 计算 pendingViewportResize
  │                 → ImGui::Image((ImTextureID)GetColorSRV().ptr, displayW, displayH)
  ├─ Setting 窗口（AO/GI/TAA/分辨率/帧率滑块）
  ├─ MainLight 窗口（光照旋转、阴影模式、阴影范围、阴影开关）
  ├─ Actor/Scene/Resource/MaterialEditor/TexturePreview 等面板
  └─ ImGui::Render() 生成 DrawData
        │
        ▼
  视口颜色缓冲: RENDER_TARGET → PIXEL_SHADER_RESOURCE
  BeginRenderToSwapChain(commandList, true, false)   // 清除交换链 RTV，不绑深度
  SetDescriptorHeaps(gImGuiDescriptorHeap)
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList)
  EndRenderToSwapChain(commandList)
```

### 输入接入（WindowProc）

```cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(...);
LRESULT WindowProc(...) {
    ImGui_ImplWin32_WndProcHandler(inHWND, inMSG, inWParam, inLParam);
    if (io.WantCaptureMouse && !g_viewportHovered) return true;   // UI 优先消费输入
    ... // 相机/场景输入处理
}
```

`g_viewportHovered` 每帧由 `ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)` 更新，保证鼠标悬停 Viewport 时 3D 相机仍然响应。

## 关键实现要点

### CreateUiPSO（ImguiPass.cpp）

| 属性 | 值 | 说明 |
| --- | --- | --- |
| 输入布局 | `POSITION/TEXCOORD/NORMAL/TANGENT` 各 `R32G32B32A32_FLOAT` | 兼容场景网格顶点格式 |
| RTV 格式 | `R8G8B8A8_UNORM` | 与交换链一致 |
| DSV 格式 | `D24_UNORM_S8_UINT` | 与深度缓冲一致（虽然深度被禁用） |
| 混合 | `SRC_ALPHA / INV_SRC_ALPHA`（alpha 与颜色通道相同配置） | ImGui 标准 alpha 混合 |
| 深度 | `DepthEnable = FALSE`、写掩码 ZERO | UI 不参与深度测试 |
| 光栅化 | `CullMode = BACK`、`DepthClipEnable = TRUE` | 常规三角形 |
| 图元 | `TRIANGLE` | 顶点缓冲由 ImGui 后端提供 |

### InitImGui 细节（BattleFireDirect.cpp）

- `IMGUI_CHECKVERSION()` + `ImGui::CreateContext()`；
- `io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures`（DX12 后端要求）；
- `ImGui_ImplDX12_Init(device, 2, DXGI_FORMAT_R8G8B8A8_UNORM, srvHeap, cpuStart, gpuStart)`：**2 是交换链缓冲数**（`NumBuffers`），格式必须匹配交换链；
- `ImGui_ImplDX12_CreateDeviceObjects()` 在初始化阶段创建字体纹理与 PSO。

### 布局持久化

- `Saved/FEngineLayout.ini`：Docking 停靠位置/分割比例（`io.IniFilename` 指向 `GetSavedConfigPath()`）；
- `Saved/WindowLayout.ini`：各面板可见性开关（`WindowLayoutState::Load/Save`），启动时读回、退出时保存；
- `io.ConfigFlags`：启用 `DockingEnable`，**禁用** `ViewportsEnable`（UI 不分离到独立 OS 窗口）。

### 面板与设置联动

Setting 窗口直接驱动各 Pass 状态（下帧生效）：TAA 开关、AO 模式/参数、GI 模式/分辨率/方向/步数/半径/强度、阴影模式/范围/开关、分辨率选择、FPS 限制。MainLight 窗口驱动光照旋转与阴影配置。这些调用是渲染系统的主要运行时调参入口。

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `CreateUiPSO(rootSig, vs, ps)` | `ImguiPass.h` | 创建 UI PSO（main.cpp 用 `ndctriangle.hlsl` 的 `MainVS/MainPS` 调用，命名 `UiPso`） |
| `InitImGui(hwnd, device, srvHeap, srvDescriptorSize)` | `BattleFireDirect.h` | 初始化 ImGui 上下文与 DX12/Win32 后端 |
| `ShutdownImGui()` | `BattleFireDirect.h` | 释放 ImGui 后端、上下文与 `gImGuiDescriptorHeap` |
| `ViewportManager::GetColorSRV()` | `ViewportManager.h` | 视口颜色缓冲的 GPU SRV（供 `ImGui::Image`） |
| `ViewportManager::GetColorRTV() / GetColorTexture()` | `ViewportManager.h` | 3D 渲染目标访问 |
| `WindowLayoutState::Load() / Save()` | `main.cpp` | 面板可见性持久化 |

## 配置与调参

| 项目 | 默认 | 位置 | 说明 |
| --- | --- | --- | --- |
| 布局 ini | `Saved/FEngineLayout.ini` | main.cpp 启动段 | Docking 布局持久化 |
| 窗口布局 | `Saved/WindowLayout.ini` | `WindowLayoutState` | 面板可见性 |
| 字体 | `AddFontDefault()` | `InitImGui` | 未加载中文字体，中文面板依赖系统回退（如有乱码需自行加入字体文件） |
| 主题 | `StyleColorsDark()` | `InitImGui` | 深色主题 |
| FPS 显示 | 主菜单栏 | `ImGui::Text("FPS: %.1f", ...)` | 每帧刷新 |
| FPS 限制 | 144 | Setting 窗口 | 0=不限 |

## 已知限制与 TODO

- **多视口未启用**：`ViewportsEnable` 被显式关闭，面板无法拖出到独立 OS 窗口；
- **单字体**：只加载默认字体，中文/图标字体需要额外配置；
- **描述符堆 100 个的容量风险**：纹理预览、材质编辑器等按需分配 SRV，面板增多后可能耗尽；当前 `ViewportManager` 固定占偏移 2，无分配计数保护；
- **UI 与 3D 同队列**：UI 绘制挂在主命令队列上，长帧时 UI 延迟与 3D 一致（无独立 UI 提交）；
- **输入接管策略**：`WantCaptureMouse && !g_viewportHovered` 时吞掉输入，Docking 标签页拖拽等操作与相机控制的边界行为需要实测；
- **无 UI 层分辨率自适应**：ImGui 默认 1:1 像素映射，高 DPI 屏幕下 UI 偏小（未调用 `SetNextFrameWantCaptureKeyboard` / 缩放相关接口）。

## 维护注意事项

- **不要改动 `ImGui_ImplDX12_Init` 的缓冲数**：必须与交换链 `BufferCount = 2` 一致，否则后端会在错误的缓冲索引上渲染；
- **描述符堆必须 SHADER_VISIBLE**：`gImGuiDescriptorHeap` 创建时带 `D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE`，绘制前 `SetDescriptorHeaps` 只设置这一个堆；
- **视口颜色缓冲状态**：`ImGui::Image` 需要该资源处于 `PIXEL_SHADER_RESOURCE`；主循环在 `ImGui::Render()` 之后做一次显式转换，任何新增的“读取视口颜色”的代码都要安排在该转换前后正确的位置；
- **UI PSO 与根签名**：`CreateUiPSO` 使用全局根签名；ImGui 顶点格式是后端内部定义的（POSITION/TEXCOORD 等），若升级 ImGui 版本需核对输入布局兼容性；
- **面板代码集中在 main.cpp**：新增面板时注意 `WindowLayoutState` 的持久化字段与菜单项同步维护；面板渲染顺序影响 DockSpace 布局体验；
- **退出清理顺序**：`ShutdownImGui` 必须在 `ViewportManager::Shutdown` 与设备释放之前调用，`gImGuiDescriptorHeap` 由 `ShutdownImGui` 释放，其他持有该堆引用的模块需先释放自己的描述符。

## UI 事件时序（UIPass 事件）

```
commandList->BeginEvent(0, L"UIPass", ...)
  ImGui_ImplDX12_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
  绘制所有面板与 DockSpace
  ImGui::Render();
  // 视口颜色缓冲 RENDER_TARGET → PIXEL_SHADER_RESOURCE
  BeginRenderToSwapChain(commandList, true, false);   // 清除交换链，不绑深度
  SetDescriptorHeaps(gImGuiDescriptorHeap);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
  EndRenderToSwapChain(commandList);
commandList->EndEvent();
EndFrame();          // Close + Execute + Signal + 切换 allocator
SwapD3D12Buffers();  // Present(0, 0)
```

该事件位于 3D 渲染（BasePass → TAA）之后，所有场景工作完成、交换链 RTV 可写时执行。

## Viewport 窗口的尺寸联动

```
Viewport 窗口（ImGui::Begin("Viewport")）:
  contentAvail = ImGui::GetContentRegionAvail()
  若 (contentAvail != ViewportManager 当前宽高):
      pendingViewportWidth/Height = contentAvail
      pendingViewportResize = true          // 下一帧帧首处理
  ImGui::Image((ImTextureID)GetColorSRV().ptr, displayW, displayH)
```

- 视口尺寸由 Docking 布局决定，**下一帧帧首**执行 `ResizeViewportRenderTargets`（FlushGPU + 重建全部 Pass RT）；
- `g_viewportRectX/Y/W/H` 每帧记录窗口位置，供 `WindowProc` 判断鼠标是否在视口内（相机输入判定）；
- `ViewportManager::Resize` 内部同步更新全局 `gRenderWidth/gRenderHeight`，SceneCB 的 `screenParams` 也随之更新。

## 常见问题排查

- **UI 不显示**：检查 `ImGui_ImplDX12_Init` 是否成功、`gImGuiDescriptorHeap` 是否为 SHADER_VISIBLE、`BeginRenderToSwapChain(..., false)` 是否正确（UI 阶段不绑深度）。
- **UI 花屏/纹理错乱**：检查 `s_viewportSrvOffset = 2` 是否与 ImGui 后端内部描述符冲突（升级 ImGui 版本时重点核对）。
- **3D 画面在 UI 之下变黑**：确认视口颜色缓冲在 `ImGui::Render()` 之后被转为 `PIXEL_SHADER_RESOURCE`，且 `ImGui::Image` 使用 `GetColorSRV().ptr`。
- **鼠标无法操作 3D 相机**：检查 `g_viewportHovered` 是否被正确更新（`ImGui::IsWindowHovered(RootAndChildWindows)`），以及 `WantCaptureMouse` 分支逻辑。
- **面板布局丢失**：删除 `Saved/FEngineLayout.ini` 可恢复默认布局；面板可见性存于 `Saved/WindowLayout.ini`，两者独立。
- **中文乱码**：`AddFontDefault()` 未包含中文字形，需用 `io.Fonts->AddFontFromFileTTF` 加载中文字体并重新 `CreateDeviceObjects`。