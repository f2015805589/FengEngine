# EditorPanels — 编辑器面板体系

> 本文介绍 FEngine 基于 Dear ImGui（Docking）实现的编辑器 UI：设置面板、主光面板、Actor 创建器、场景层级、资源管理器、纹理预览、材质编辑器、GTAO/SSGI 调试面板，以及布局持久化机制。大部分面板代码内联在 `Engine/main.cpp` 的 UIPass 中，部分复杂面板独立成类（`ResourceManager`、`TexturePreviewPanel`、`MaterialEditorPanel`）。

## 概述

FEngine 的编辑器 UI 全部在主窗口内以 ImGui 窗口形式呈现，启用 `ImGuiConfigFlags_DockingEnable`（禁用独立视口 `ViewportsEnable`）：

- 顶部主菜单栏（`Window` / `Texture` 菜单 + FPS 显示）控制各面板的开关；
- 全窗口 `DockSpaceOverViewport("MainDockSpace")` 提供停靠布局；
- `Viewport` 窗口显示 3D 渲染结果（见 `CameraAndViewport.md`）；
- 面板分为三类：**内联面板**（设置 / 主光 / Actor 创建器 / 场景层级 / Actor 属性，直接在 `main.cpp` 的 UIPass 中编写）、**独立面板类**（资源管理器 `ResourceManager`、纹理预览 `TexturePreviewPanel`、材质编辑器 `MaterialEditorPanel`）；
- 布局持久化分两层：ImGui 自身的 docking 布局写到 `Saved/Config/FEngineLayout.ini`；各面板的开关状态由 `WindowLayoutState` 写到 `Saved/Config/WindowLayout.ini`。

UI 渲染顺序在每帧最后（UIPass）：`ImGui::Render` → 视口颜色缓冲转 `PIXEL_SHADER_RESOURCE` → `BeginRenderToSwapChain`（绑定交换链 RTV）→ 设置 `gImGuiDescriptorHeap` → `ImGui_ImplDX12_RenderDrawData` → `EndRenderToSwapChain`。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/main.cpp` | 主菜单、DockSpace、Viewport 窗口、设置/主光/Actor 创建器/场景层级/Actor 属性面板、`WindowLayoutState` |
| `Engine/public/ImguiPass.h`、`Engine/private/ImguiPass.cpp` | ImGui 专用 PSO 创建（`CreateUiPSO`） |
| `Engine/public/ResourceManager.h`、`Engine/private/ResourceManager.cpp` | 资源管理器面板（双面板浏览、双层目录树） |
| `Engine/public/Texture/TexturePreviewPanel.h`、`Engine/private/Texture/TexturePreviewPanel.cpp` | 纹理预览面板（通道/展开模式/缩放/压缩） |
| `Engine/public/Material/MaterialEditorPanel.h`、`Engine/private/Material/MaterialEditorPanel.cpp` | 材质编辑器面板（参数/纹理槽/保存加载） |
| `Saved/Config/FEngineLayout.ini` | ImGui docking 布局持久化文件 |
| `Saved/Config/WindowLayout.ini` | 面板可见性持久化文件 |

## 架构与数据流

### 面板开关与渲染

```text
主菜单栏（Window 菜单）
├── Settings         → showSettingWindow（内联）
├── Main Light       → showMainLightWindow（内联）
├── Actor Creator    → showActorWindow（内联）
├── Scene            → showSceneWindow（内联）
├── Resource Manager → ResourceManager::GetInstance().ShowResourceWindow(&showResourceWindow)
└── Texture Preview  → TexturePreviewPanel::GetInstance().Show() + RenderUI()
Texture 菜单
├── Import Texture...（文件对话框 → TextureManager::ImportTexture → 预览）
└── Texture Preview Panel

UIPass 每帧：
1. ImGui_ImplDX12_NewFrame / ImGui_ImplWin32_NewFrame / ImGui::NewFrame
2. BeginMainMenuBar → 菜单项
3. DockSpaceOverViewport
4. Viewport 窗口（记录 g_viewportHovered / 尺寸）
5. 各面板按 show* 标志渲染（内联面板 / 独立面板类）
6. ImGui::Render → 提交到交换链
```

### 面板 → 引擎状态

面板控件直接调用引擎接口修改运行时状态，例如：

- 设置面板 → `g_scene->SetCameraLookSpeed/MoveSpeed`、`Settings::SetFpsLimit`、`taaPass->SetEnabled`、`gtaoPass->SetAOType/Radius/Intensity/SliceCount/StepsPerSlice`、`ssgiPass->SetGIType/ResolutionScale/DirectionCount/StepCount/Radius/Intensity`、`g_scene->SetGIType`、`Settings::RequestResolutionChange`；
- 主光面板 → `g_scene->SetLightRotation`、`SetSkylightIntensity`、`SetSkylightColor`、`SetShadowOrthoSize`、`SetShadowMode`、`SetShadowmapEnabled`；
- Actor 创建器 → `new Actor` + `LoadFromMeshFile` + `StaticMeshComponent::InitFromFile` + 默认 `DefaultPBR` 材质，push 进 `g_scene->GetActors()`；
- 场景层级 → 选中 `Actor*`（`selectedActor`），双击打开 Actor 属性面板；
- Actor 属性面板 → `SetPosition/Rotation/Scale`、打开材质编辑器（`g_materialEditor->SetTargetActor + SetSelectedMaterial + Show`）。

## 关键实现要点

### 设置面板（Setting window）

- 鼠标/移动速度：SliderFloat 0.1-100，实时写入 `Scene` 相机；
- FPS 限制：Combo（60/120/144/180/Unlimited），写入 `Settings::SetFpsLimit`，主循环用 `timeBeginPeriod(1)` + `Sleep` 实现帧限；
- TAA：`Enable TAA` 开关 → `taaPass->SetEnabled`；
- AO：`AO` Combo（Close/SSAO/GTAO）→ `gtaoPass->SetAOType`；开启时显示 Radius、Intensity，GTAO 额外显示 Slices（1-8）、Steps/Slice（1-8）；
- GI：`GI` Combo（Close/SSGI）→ `ssgiPass->SetGIType` + `g_scene->SetGIType`；SSGI 开启时显示 Resolution（Full/Half/Quarter）、Directions（8-64）、Steps（4-256）、Radius、Intensity；
- 分辨率：从 `Settings::GetResolutionOptions()` 构建下拉，选择后 `Settings::RequestResolutionChange(width, height, true)`（fromUI=true 会同时调整窗口大小）。

### 主光面板（MainLight window）

- `Light Rotation Control`：Reset 按钮 + Rotate X/Y/Z Slider（弧度，范围 ±10），每帧调用 `g_scene->SetLightRotation`；
- Skylight（UE 风格）：Intensity Slider（0-5）+ `ColorEdit3` Tint 颜色；
- Shadow：`Shadow Ortho Size`（10-500，提示 “Smaller = Higher Resolution”）、`Shadow Mode` Combo（Hard/PCF/PCSS）、`Enable Shadowmap` Checkbox。

### Actor 创建器（Actor Creator）

- 通用 lambda `CreateActorFromMesh(meshPath, prefix, counter)`：`WaitForCompletionOfCommandList` → 重置命令列表 → 创建 Actor + CB → 设默认 Transform → `LoadFromMeshFile`（.mesh）→ 拼绝对 FBX 路径 → `InitFromFile` 导入 → 挂 `DefaultPBR` 材质 → push 进 `g_scene->GetActors()` → 提交并等待；
- “Add Sphere” / “Add Box” 按钮分别加载 `Engine/Content/Actor/Mesh/sphere.mesh` 与 `box.mesh`，命名 `Sphere_N` / `Box_N`；
- 显示当前 Actor 数量。

### 场景层级（Scene 窗口）与 Actor 属性

- `Scene` 窗口遍历 `g_scene->GetActors()`，`Selectable(actorName, selectedActor == actor, AllowDoubleClick)`：单击选中，双击打开 `Actor Properties`；
- `Actor Properties` 面板：`Material` CollapsingHeader（显示 `.mesh` 的默认材质名，“Edit Material” 按钮打开材质编辑器并绑定目标 Actor）；`Transform` CollapsingHeader（Position DragFloat、Rotation DragFloat（度，±180）、Scale DragFloat（0.01-10））。

### 资源管理器（Resource Manager）

- 双面板布局：左侧 30% 文件夹树（`RenderFolderTree`，先 Content 树后 Engine 树），右侧 70% 平铺视图（`RenderTileView`）；
- 顶部工具栏：`.. (Up)` 返回上级（`FindParentNode`）、`Content` / `Engine` 根切换；
- 平铺项以按钮呈现：`[D] 目录名` / 文件名；单击选中，双击目录进入（左右联动），双击纹理文件调用 `TexturePreviewPanel::SetTexturePath` 打开预览；Hover 显示完整路径与类型 tooltip；
- 详见 `07-Infrastructure/ResourceManager.md`。

### 纹理预览（TexturePreviewPanel）

- 工具栏：通道模式（RGBA/RGB/R/G/B/A/Normal/Luminance）、Cubemap 展开模式（Cross/Horizontal/Vertical/Sphere）、mip 选择、曝光（HDR）、gamma（默认 2.2）、缩放/平移（拖拽）、`fit to window`、Alpha 棋盘格背景；
- 独立渲染：`RenderTextureToPreviewRT` 用全屏四边形 + `Engine/Shader/Utility/TexturePreview.hlsl` 把纹理画到预览 RT，再经 ImGui 描述符堆显示（`IMGUI_TEXTURE_SLOT_START = 10`）；
- 信息面板与压缩面板（`TextureCompressionFormat` BC1/BC3/BC5 等 + 质量 + mips + sRGB，可重新压缩）；
- 延迟加载：`SetTexturePath` 只记录路径，主循环帧首 `HasPendingLoad → ProcessPendingLoad`（在 GPU 空闲时执行，避免命令列表冲突）。

### 材质编辑器（MaterialEditorPanel）

- `RenderUI`：材质选择器（Combo，切换时自动 `OnApplyToMesh`）→ 参数编辑器（按 `ShaderParameterType` 分发到 `RenderFloat/Vector/Int/Bool/TextureParameter`，UI 控件类型来自参数的 `uiWidget`：Slider/ColorPicker/TexturePicker 等）→ 纹理槽 → 控制按钮；
- 控制按钮：Save / Load（文件对话框）、Create New Material（`OnCreateNewMaterial`）、Apply to Mesh（`OnApplyToMesh`，应用到 `m_targetActor` 或 `m_scene->GetStaticMesh()`）；
- 窗口内 `ImGuiWindowFlags_AlwaysAutoResize`；从 Actor 属性面板打开时 `SetTargetActor(selectedActor)`。

### 布局持久化

- **Docking 布局**：`io.IniFilename` 设为 `Saved/Config/FEngineLayout.ini`（`GetSavedConfigPath()` 拼接，UTF-8 转换后写入 ImGui IO），ImGui 自动保存停靠位置/分割比例；
- **面板可见性**：`WindowLayoutState`（`main.cpp` 第 72-119 行）保存 8 个 `show*` 标志；`Load()` 在启动时读取 `Saved/Config/WindowLayout.ini`（`key=0/1` 格式），`Save()` 在退出时写出 `[Windows]` 段；材质编辑器的可见状态通过 `g_materialEditor->Show()` 同步；
- 注释明确指出：ImGui 的 ini 不记录窗口是否打开，因此需要这套单独管理可见性的机制。

## 对外接口

- `ResourceManager`：`Initialize(device, rootSig)`、`ScanAndLoadAllResources()`、`ShowResourceWindow(bool*)`、`GetAllShaderResources/MaterialResources`、`GetContentRoot/GetEngineRoot`、`GetTotalShaderCount/GetLoadedShaderCount` 等（详见 `ResourceManager.md`）；
- `TexturePreviewPanel`：`Initialize`、`Shutdown`、`Show/Hide/IsVisible`、`SetTexture(TextureAsset*)`、`SetTexturePath(wstring)`、`HasPendingLoad/ProcessPendingLoad`、`GetCurrentTexture`；
- `MaterialEditorPanel`：`RenderUI()`、`SetSelectedMaterial(name)`、`SetScene(Scene*)`、`SetTargetActor(Actor*)`、`Show/Hide/IsVisible`；
- `CreateUiPSO(rootSignature, vs, ps)`：创建 ImGui 专用 PSO（alpha 混合、深度关闭、4 分量 float 输入布局）。

## 配置与调参

| 项 | 默认/范围 | 位置 |
| --- | --- | --- |
| Docking 布局文件 | `Saved/Config/FEngineLayout.ini` | `main.cpp`（`io.IniFilename`） |
| 面板可见性文件 | `Saved/Config/WindowLayout.ini` | `main.cpp`（`WindowLayoutState`） |
| 初始窗口尺寸 | 1280×720 | `main.cpp` WinMain |
| 相机速度范围 | 0.1-100 | 设置面板 |
| FPS 限制选项 | 60/120/144/180/Unlimited（默认 144） | 设置面板 + `Settings::GetFpsLimit` |
| AO 类型 | Close/SSAO/GTAO | 设置面板 |
| GI 类型 | Close/SSGI | 设置面板 |
| 分辨率预设 | 720p/1080p/1440p/2K | `Settings.cpp` `Initialize` |
| 纹理导入默认压缩 | BC3 + mips + sRGB | `main.cpp` Texture 菜单 |

## 已知限制与 TODO

- 除三个独立面板类外，设置/主光/Actor 创建器/场景层级等面板全部内联在 `main.cpp`（约 400 行 UI 代码），后续应拆分到独立 Panel 类。
- 面板状态大量使用 `static` 局部变量（`lightRot`、`skylightIntensity` 等），不随布局持久化，每次启动回到默认值。
- `WindowLayoutState` 只持久化可见性；面板内的参数值、选中 Actor、选中材质均不持久化。
- 资源管理器目前只负责浏览与纹理预览，shader/material 的“加载”仍需在 `main.cpp` 手动完成（`ResourceManager.cpp` 注释也确认了这一点）。
- 材质编辑器与资源管理器之间没有拖拽互通；纹理导入仅提供 BC3 预设路径。
- 无撤销/重做、无场景保存按钮（`.level` 保存只能走 `Scene::SaveLevel` 接口，UI 未暴露）。
- 无多视口/多相机切换、无 Gizmo（移动/旋转/缩放手柄），Actor 变换只能通过属性面板数值修改。
- 中文 UI 与主题定制尚未做；UI 文本为英文。

## 维护注意事项

- 新增面板时同步修改三处：`WindowLayoutState` 结构体、`Load()/Save()` 的 key 解析、主菜单 `MenuItem`；否则可见性无法持久化。
- ImGui 描述符堆（`gImGuiDescriptorHeap`）槽位由多模块共享：ImGui 字体、视口 SRV（偏移 2）、纹理预览（`IMGUI_TEXTURE_SLOT_START=10`）；新增 `ImGui::Image` 前先核对槽位偏移。
- UI 中的 `ImGui::Image` 只能使用 `gImGuiDescriptorHeap` 内的 GPU 句柄；使用前必须确保对应的资源处于 `PIXEL_SHADER_RESOURCE` 状态（UIPass 开头有专门屏障）。
- 面板中会触发 GPU 工作的操作（导入纹理、创建 Actor、重新压缩）必须遵循“帧首/GPU 空闲时执行”的约定（参考 `TexturePreviewPanel::ProcessPendingLoad` 与 `CreateActorFromMesh`）。
- 修改 `Settings`（分辨率/帧率）时注意 `RequestResolutionChange` 的延迟执行语义：不要在 ImGui 回调内直接重建资源。
- `MaterialEditorPanel` 的 `m_showWindow` 与 `showMaterialEditorFromActor` 是两套可见性状态，关闭窗口时记得同步（`main.cpp` 已处理，新增入口时保持一致）。