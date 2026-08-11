# CameraAndViewport — 相机与视口管理

> 本文介绍 FEngine 的编辑器相机（`Camera`）与视口管理（`ViewportManager`）。相机负责视角/输入/视图矩阵；视口是内嵌在 ImGui Docking 布局中的渲染目标，3D 结果输出到独立颜色缓冲后由 `ImGui::Image` 显示。

## 概述

FEngine 的“视口”不是一个独立的原生窗口，而是主窗口内一个 ImGui 窗口（标题为 `Viewport`）。渲染流程与视口的关系如下：

- `ViewportManager`（单例）持有视口的颜色缓冲（`R8G8B8A8_UNORM`，RTV + SRV）与深度缓冲（`R24G8_TYPELESS`，DSV + SRV，即全局 `gDSRT`），并提供按视口尺寸重建资源的 `Resize`。
- 3D 管线（BasePass → LightPass → GTAO → SSGI → SkyPass → ScreenPass → TAA）最终写入视口颜色缓冲；该缓冲随后转为 SRV，在 `Viewport` 窗口中通过 `ImGui::Image` 贴图显示。
- `Camera` 是场景内置的编辑器相机（透视投影，`XMMatrixPerspectiveFovLH`），支持 WASD/QE 平移、鼠标拖拽旋转、滚轮前进后退，并受 ImGui 焦点捕获约束（鼠标在 ImGui 面板上时不抢相机输入）。
- 视口尺寸由 Docking 布局驱动：每帧读取 `Viewport` 窗口的 `GetContentRegionAvail()`，尺寸变化时延迟一帧调用 `ViewportManager::Resize` 以及各 Pass 的 `Resize`。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/public/Camera.h` | `Camera` 类声明（投影参数、输入状态、速度） |
| `Engine/private/Camera.cpp` | 视图矩阵更新、WASD/鼠标/滚轮输入处理 |
| `Engine/public/ViewportManager.h` | `ViewportManager` 单例声明（颜色/深度缓冲、RTV/SRV） |
| `Engine/private/ViewportManager.cpp` | 视口资源创建、`Resize`、与 ImGui SRV 堆的对接 |
| `Engine/main.cpp` | `Viewport` 窗口、DockSpace、尺寸变更队列、输入转发（`WindowProc`） |

## 架构与数据流

### 视口渲染链路

```text
ImGui DockSpace（MainDockSpace）
└── "Viewport" 窗口（无内边距）
    └── 每帧获取 contentAvail 尺寸
        └── 尺寸变化 → pendingViewportResize = true（延迟一帧）
            └── ViewportManager::Resize(w, h)
                ├── 重建 m_colorRT（视口颜色缓冲）
                ├── 重建 gDSRT（视口深度缓冲）
                └── 重建 RTV / DSV / SRV（SRV 位于 gImGuiDescriptorHeap 偏移 2）
                    └── 同步 gRenderWidth / gRenderHeight
3D Pass 写入视口颜色缓冲（viewportColorRTV）
    └── UIPass：颜色缓冲 RENDER_TARGET → PIXEL_SHADER_RESOURCE
        └── ImGui::Image(GetColorSRV().ptr, ...) 显示
```

### 输入数据流

```text
WindowProc（main.cpp）
├── ImGui_ImplWin32_WndProcHandler（ImGui 先处理）
├── 若 ImGui::WantCaptureMouse 且 !g_viewportHovered → 拦截鼠标消息
│   （WM_LBUTTONUP / WM_RBUTTONUP 始终转发，保证相机拖拽状态能清除）
└── g_scene->HandleInput → Camera::HandleInput（鼠标按下/移动/滚轮）
每帧 Camera::Update(deltaTime)：GetAsyncKeyState 轮询 WASD/QE + 计算视图矩阵
```

### 视口尺寸的三种变更来源

1. **窗口物理尺寸变化**（拖动窗口边缘）：`WindowProc` 收到 `WM_SIZE` 后只置位 `g_pendingSwapChainResize`，帧首调用 `ResizeSwapChainOnly` 调整交换链；视口尺寸由下一帧 Docking 布局自然跟随。
2. **Docking 布局变化**（拖动分隔条/停靠面板）：`Viewport` 窗口的 `GetContentRegionAvail()` 变化 → 置位 `pendingViewportResize` → 下一帧帧首调用 `ResizeViewportRenderTargets`（见下）。
3. **分辨率下拉选择**（Settings 面板）：`Settings::RequestResolutionChange` → 帧首 `FlushGPU` 后 `ResizeSwapChainOnly`（同时调整窗口）再 `ResizeViewportRenderTargets`。

`main.cpp` 第 711 行的 `ResizeViewportRenderTargets` lambda 是所有视口尺寸变更的统一出口：

```cpp
auto ResizeViewportRenderTargets = [&](int newWidth, int newHeight) {
    ViewportManager::GetInstance().Resize(newWidth, newHeight);
    g_scene->ResizeRenderTargets(newWidth, newHeight);
    lightPass->Resize(newWidth, newHeight);
    screenPass->Resize(newWidth, newHeight);
    taaPass->Resize(newWidth, newHeight);
    gtaoPass->Resize(newWidth, newHeight);
    ssgiPass->Resize(newWidth, newHeight);
    Settings::GetInstance().SetResolution(newWidth, newHeight);
};
```

## 关键实现要点

### Camera

- **构造**：`Camera(fov, aspectRatio, nearZ, farZ)`，默认位置 `(0,0,-10)`，朝向 `+Z`；`m_moveSpeed=50`、`m_lookSpeed=0.005`（`SetLookSpeed` 会乘以 0.001）、`m_zoomSpeed=5`。
- **视图矩阵**：`Update` 中由欧拉角 `m_rotation`（弧度）构造旋转矩阵，前向 = `(0,0,1)` 经旋转并归一化，右向 = `forward × up`；视图矩阵用 `XMMatrixLookAtLH(pos, pos+forward, up)`。
- **WASD/QE 移动**：在 `Update` 中每帧用 `GetAsyncKeyState` 轮询（W/S 沿 forward、A/D 沿 right、Q/E 垂直升降），速度 = `m_moveSpeed * deltaTime`；仅在 `!ImGui::GetIO().WantCaptureKeyboard || g_viewportHovered` 时响应，避免与 ImGui 输入冲突：

```cpp
if (!ImGui::GetIO().WantCaptureKeyboard || g_viewportHovered) {
    float move = m_moveSpeed * deltaTime;
    if (GetAsyncKeyState('W') & 0x8000) { /* 沿 m_forward 移动 */ }
    // ... S/A/D/Q/E 同理
}
```

- **鼠标旋转**（`HandleInput`）：
  - 左键拖拽：只绕 Y 轴（偏航），`m_rotation.y -= deltaX * m_lookSpeed`；
  - 右键拖拽：偏航 + 俯仰，俯仰钳制到 `[-PI/2 + 0.1, PI/2 - 0.1]`；
  - 滚轮：沿前向移动 `zDelta * 0.01 * m_zoomSpeed`。
- **ImGui 冲突处理**：`isButtonUp`（鼠标按键抬起）始终处理，否则在 `WantCaptureMouse && !g_viewportHovered` 时直接返回，防止拖拽 Docking 分隔条后 `m_isMouseDown` 残留。
- **宽高比**：`SetAspectRatio` 会重建投影矩阵（分辨率/视口尺寸变化时由 `Scene::ResizeRenderTargets` 调用）。

### ViewportManager

- 单例（`GetInstance()`），`Initialize(device, imguiSrvHeap)` 创建 1 个描述符的 RTV 堆并记录 descriptor 大小。
- `Resize(width, height)`：
  - 先 `WaitForCompletionOfCommandList()` 等待 GPU，再同步全局渲染分辨率 `gRenderWidth / gRenderHeight`（外部通过 `extern` 声明）；
  - 创建 `m_colorRT`（`DXGI_FORMAT_R8G8B8A8_UNORM`，`ALLOW_RENDER_TARGET`，命名为 `ViewportColorRT`）；
  - 释放旧的 `gDSRT` 并重建深度缓冲（`R24G8_TYPELESS`，DSV 视图格式 `D24_UNORM_S8_UINT`，命名为 `ViewportDepthRT`）——**`gDSRT` 的所有权在 ViewportManager**；
  - RTV 写入 `m_rtvHeap` 起始处；DSV 复用全局 `gSwapChainDSVHeap` 的第 0 个描述符；
  - SRV 写入 ImGui 描述符堆：`s_viewportSrvOffset = 2`（硬编码偏移），保存 GPU 句柄到 `m_colorSRV` 供 `ImGui::Image` 使用。
- `Shutdown/ReleaseResources`：释放颜色 RT、RTV 堆与 `gDSRT`。

### main.cpp 集成

- `WindowProc`：`WM_SIZE` 只标记 `g_pendingSwapChainResize`（窗口物理尺寸），视口尺寸由下一帧 Docking 布局自动跟随。
- 每帧 UI 中 `Viewport` 窗口的关键代码：

```cpp
ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
g_viewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
// 记录窗口矩形，供 WindowProc 实时命中判定
g_viewportRectX = vpPos.x; g_viewportRectY = vpPos.y;
g_viewportRectW = vpSize.x; g_viewportRectH = vpSize.y;
// 尺寸变化 → 延迟一帧重建
if (contentAvail 与当前视口尺寸不同) { pendingViewportResize = true; }
// 显示 3D 结果
ImGui::Image((ImTextureID)ViewportManager::GetInstance().GetColorSRV().ptr, ...);
ImGui::End();
```

- `g_viewportHovered` 同时用于：相机输入判定（`Camera.cpp`、`WindowProc`）与 ImGui 捕获冲突规避。
- 分辨率下拉（Settings 面板）走 `Settings::RequestResolutionChange` → 帧首 `FlushGPU` + `ResizeSwapChainOnly`（窗口）+ `ResizeViewportRenderTargets`（视口）。
- UIPass 末尾：视口颜色缓冲从 `RENDER_TARGET` 转为 `PIXEL_SHADER_RESOURCE` 屏障后，`BeginRenderToSwapChain` 绑定交换链，设置 `gImGuiDescriptorHeap`，`ImGui_ImplDX12_RenderDrawData` 绘制所有窗口（含 Viewport 贴图），最后 `EndRenderToSwapChain`。

## 对外接口

`Camera`（`Engine/public/Camera.h`）：

- `Update(float deltaTime)`、`HandleInput(HWND, UINT, WPARAM, LPARAM)`；
- `GetViewMatrix()`、`GetProjectionMatrix()`、`GetPosition()`、`GetForward()`；
- `SetPosition(x,y,z)`、`SetRotation(pitch, yaw)`、`SetAspectRatio(float)`、`SetLookSpeed`、`SetMoveSpeed`；
- `GetNearPlane()`、`GetFarPlane()`（TAA/阴影使用）。

`ViewportManager`（`Engine/public/ViewportManager.h`）：

- `Initialize(ID3D12Device*, ID3D12DescriptorHeap* imguiSrvHeap)`、`Shutdown()`；
- `Resize(int width, int height)`；
- `GetWidth()`、`GetHeight()`、`GetColorTexture()`、`GetColorRTV()`、`GetColorSRV()`。

## 配置与调参

| 参数 | 默认值 | 位置 |
| --- | --- | --- |
| 相机位置 / 朝向 | (0,0,-10)，+Z | `Camera` 构造函数 |
| FOV / near / far | 45° / 0.1 / 1000 | `Scene` 构造函数传入 |
| 移动速度 | 50 | `Camera.cpp`；Settings 面板 “Move Speed” 可调 |
| 旋转速度 | 0.005（`SetLookSpeed` 内 ×0.001） | `Camera.cpp`；Settings 面板 “Mouse Speed” 可调 |
| 缩放速度 | 5.0 | `Camera.cpp`（无 UI） |
| 俯仰角钳制 | ±(π/2 − 0.1) | `Camera::HandleInput` |
| 视口颜色格式 | R8G8B8A8_UNORM | `ViewportManager::Resize` |
| 视口深度格式 | R24G8_TYPELESS / D24_UNORM_S8_UINT | `ViewportManager::Resize` |
| 视口 SRV 槽位 | `s_viewportSrvOffset = 2`（ImGui 堆） | `ViewportManager.h` |
| 初始视口尺寸 | 1280×720 | `main.cpp` WinMain |

## 已知限制与 TODO

- 相机只有编辑器式的第一人称控制（左键偏航 / 右键俯仰+偏航 / WASD 平移），没有 Orbit / 第三人称模式，也没有 FOV 的运行时 UI 调节。
- 俯仰角被钳制在 ±(π/2−0.1)，无法完全垂直俯视；滚轮“缩放”实际是沿前向平移，不是改变 FOV。
- 视口尺寸变更采用“延迟一帧”策略，Docking 拖动时资源重建会有 1 帧滞后。
- `s_viewportSrvOffset = 2` 是硬编码偏移，与 `gImGuiDescriptorHeap` 的其它使用者（ImGui 字体纹理、纹理预览等）共享布局，新增占用槽位的模块时必须核对偏移，否则画面串扰。
- `gDSRT` 的所有权在 `ViewportManager`（`ReleaseResources` 中释放），但大量 Pass 以全局变量方式引用它；一旦 ViewportManager 重建深度缓冲，旧指针必须全部同步失效。
- 相机状态不参与 `.level` 序列化，重启后回到默认视角。
- `ResizeSwapChainOnly` 只调整交换链，不更新 `gRenderWidth/gRenderHeight`（由 ViewportManager 维护），两个“分辨率”概念容易混淆。
- `Camera::HandleInput` 的鼠标消息坐标取自 `lParam`（客户区坐标），与 ImGui 的坐标体系混用时需小心缩放/DPI 场景（当前未处理 DPI 感知）。

## 维护注意事项

- **不要在其他地方释放 `gDSRT`**：深度缓冲由 `ViewportManager::ReleaseResources` / `Resize` 统一管理，外部只允许通过 `gSwapChainDSVHeap` 创建 DSV 视图。
- 修改视口格式时需同步：`ViewportManager::Resize`（RTV/SRV）、`ScreenPass`/`SkyPass`/`TAA` 输出的 RTV 格式、ImGui 的 `ImGui_ImplDX12_Init` 后端格式（`R8G8B8A8_UNORM`）。
- 输入转发规则（鼠标在 ImGui 面板上时拦截、按键抬起始终转发）是相机与编辑器共存的关键，新增输入事件时保持该约定。
- 新增“渲染分辨率”相关功能时，明确是修改交换链（`ResizeSwapChainOnly`）还是视口（`ViewportManager::Resize`），不要直接改 `gRenderWidth/gRenderHeight`。
- 相机速度/按键绑定目前硬编码在 `Camera.cpp`，如要做按键配置需要先提取到 `Settings`。
- 在 `Viewport` 窗口内新增叠加 UI（如 Gizmo）时，注意 `ImGui::Image` 的显示尺寸应跟随 `ViewportManager` 的当前尺寸，而不是窗口内容尺寸（两者相差 1 帧）。