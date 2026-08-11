# ResourceManager — 资源管理与路径/设置基础设施

> 本文介绍 FEngine 的基础设施三件套：资源管理（`ResourceManager`：Content/Engine 双层资源浏览、shader/material 扫描）、路径工具（`PathUtils.h`：基于 exe 位置动态推算项目根目录）、运行时设置（`Settings`：分辨率/帧率等单例配置）。资源管理器同时承担“编辑器资源浏览器”的 UI 职责（双面板文件树）。

## 概述

FEngine 的资源体系分两个层级，模仿 UE 的 Content/Engine 划分：

- **Content 层**（`FEngine/Content/`）：项目资源，优先级高（纹理、模型、材质等）；
- **Engine 层**（`FEngine/Engine/`）：引擎自带资源，优先级低（引擎 Shader、默认资产）。

`ResourceManager` 以 `ResourceLayer` 枚举区分两层，启动时扫描两层的 `.shader` / `.material` 文件并构建完整目录树（`FileTreeNode`），在编辑器“Resource Manager”窗口中提供左侧文件夹树 + 右侧平铺视图的双面板浏览。需要说明的是：**它当前定位是资源浏览器 + 统计面板，不负责实际的 shader/material 加载**（加载由 `MaterialManager` 在 `main.cpp` 中手动完成）。

`PathUtils.h` 提供全项目统一的路径入口（`GetProjectRoot / GetEnginePath / GetContentPath / GetSavedPath / GetSavedConfigPath`），消除硬编码绝对路径；`Settings` 提供分辨率预设、延迟分辨率变更、帧率限制等运行时配置，供设置面板与主循环消费。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/public/ResourceManager.h` | `ResourceLayer`、`ResourceInfo`、`FileTreeNode`、`ResourceManager` 声明 |
| `Engine/private/ResourceManager.cpp` | 扫描/加载、目录树构建、双面板 UI（`ShowResourceWindow`） |
| `Engine/public/PathUtils.h` | `GetProjectRoot` 等纯 inline 路径工具 |
| `Engine/public/Settings.h`、`Engine/private/Settings.cpp` | 分辨率选项/回调、FPS 限制、相机速度等运行时设置 |
| `Engine/main.cpp` | `ResourceManager::Initialize` + `ScanAndLoadAllResources` 调用、设置面板集成 |
| `Saved/Config/` | 运行时配置目录（`FEngineLayout.ini`、`WindowLayout.ini`） |

## 架构与数据流

### 初始化与扫描

```text
main.cpp 启动序列：
Settings::GetInstance().Initialize(1280, 720)      // 分辨率预设
ResourceManager::GetInstance().Initialize(device, rootSig)
    └── 记录 m_contentPath = GetContentPath()、m_enginePath = GetEnginePath()
ResourceManager::GetInstance().ScanAndLoadAllResources()
    ├── ScanShaders(Engine/Shader/, Engine)          // .shader
    ├── ScanShaders(Content/Shaders/, Content)       // .shader
    ├── ScanMaterials(Engine/Shader/, Engine)        // .material
    ├── ScanMaterials(Content/Materials/, Content)   // .material
    ├── ScanCompleteDirectory(Content/) → m_contentRoot（目录树）
    └── ScanCompleteDirectory(Engine/)  → m_engineRoot（目录树）
```

### 编辑器窗口数据流

```text
ShowResourceWindow(&open)
├── 首次打开：m_rightCurrent = m_contentRoot（默认 Content 根）
├── 工具栏：..(Up) / Content / Engine 切换
├── 左侧 RenderFolderTree：递归渲染两个根（Content 在前、Engine 在后）
│       └── 单击节点 → m_leftSelected
└── 右侧 RenderTileView(m_rightCurrent)
        ├── 单击文件/目录 → m_rightSelected（高亮）
        ├── 双击目录 → 进入（m_rightCurrent = child，左侧联动 m_leftSelected）
        ├── 双击纹理 → TexturePreviewPanel::SetTexturePath(child->fullPath)
        └── Hover → 完整路径 + 类型 tooltip
```

### Settings 数据流

```text
设置面板（Combo）→ RequestResolutionChange(w, h, fromUI)
    └── 置位 m_pendingResolutionChange（延迟）
主循环帧首 → IsPendingResolutionChange() 为真
    ├── FlushGPU()
    ├── fromUI=true → ResizeSwapChainOnly（调整窗口）
    ├── ResizeViewportRenderTargets(w, h)（内部调用 Settings::SetResolution 同步宽高）
    └── ClearPendingResolutionChange()
FPS 限制：Settings::SetFpsLimit → 主循环帧末 Sleep 到目标帧时间
```

## 关键实现要点

### 双层资源扫描（ResourceManager.cpp）

- `ScanShaders` / `ScanMaterials` 复用私有 `ScanDirectory`：递归遍历目录，按扩展名（`.shader` / `.material`）收集 `ResourceInfo`（名称、完整路径、层级、类型、isLoaded=false）；
- 扫描时**排除** `screen.shader` / `Screen.shader` / `sky.shader` / `Sky.shader`（注释：不需要预编译，仅用于特殊 Pass）；
- `ScanCompleteDirectory` 递归构建 `FileTreeNode`（目录/文件、扩展名、fullPath），子节点排序规则：**文件夹在前、文件在后，各自按名称排序**；
- `ExtractResourceName` 从路径提取不带扩展名的文件名（UTF-8 转换）。

### 双面板 UI（ShowResourceWindow）

- 布局：`BeginChild("##FolderTree", 窗口宽*0.30f)` + `SameLine` + `BeginChild("##TileView")`；
- 左侧 `RenderFolderTree`：仅渲染目录（`ImGuiTreeNodeFlags_OpenOnArrow | OpenOnDoubleClick`，叶子加 `Leaf`），单击设置 `m_leftSelected`；
- 右侧 `RenderTileView`：网格平铺（固定列数），文件夹按钮前缀 `[D]`，选中项改变背景色（`ImGui::PushStyleColor`）；
- 导航状态：`m_leftSelected`（左树选中）、`m_rightCurrent`（右面板当前目录）、`m_rightSelected`（右面板高亮）；`FindParentNode` 用于 `.. (Up)`；
- 双击纹理文件（`IsTextureFile`：png/jpg/jpeg/dds/bmp/tga/hdr/ast）→ `TexturePreviewPanel::SetTexturePath`（延迟加载，见 EditorPanels 文档）。

### PathUtils（纯 inline 头文件）

- `GetProjectRoot()`：`GetModuleFileNameW` 拿到 exe 路径 → 取 exe 目录 → 检查同级是否有 `Engine/` 目录；没有则向上回溯最多 5 级查找 `Engine/`；仍找不到则回退到旧的三级回溯逻辑。结果用 `static` 缓存；
- 派生路径：`GetEnginePath()`（`root\Engine\`）、`GetContentPath()`（`root\Content\`）、`GetSavedPath()`（`root\Saved\`，不存在自动 `CreateDirectoryW`）、`GetSavedConfigPath()`（`root\Saved\Config\`，自动创建）；
- `WToA`：`WideCharToMultiByte(CP_UTF8, ...)` 的 narrow 转换辅助。

### Settings（单例）

- 分辨率预设：`1280x720 (720p)`、`1920x1080 (1080p)`、`2560x1440 (1440p)`、`2560x1600 (2K)`；`Initialize` 时按当前宽高匹配索引；
- `SetResolution` 成功后调用 `NotifyResolutionChanged`（`std::function` 回调列表，目前 `main.cpp` 未注册回调，主要靠 `SetResolution` 同步宽高）；
- `RequestResolutionChange(width, height, fromUI)`：只置位 pending 标志，实际变更由主循环帧首执行；`fromUI=true` 时 `m_shouldResizeWindow=true`（同时调整窗口大小），窗口拖动时 false；
- FPS 限制：`m_fpsLimit` 默认 144，0 表示无限制；
- 相机速度：`m_mouseSpeed=5.0`、`m_moveSpeed=50.0`（面板当前直接改 `Scene`，未走 Settings）；
- TAA：`m_taaJitterScale=1.0`（预留，未看到消费方）。

## 对外接口

`ResourceManager`（`Engine/public/ResourceManager.h`）：

- `Initialize(ID3D12Device*, ID3D12RootSignature*)`、`ScanAndLoadAllResources()`；
- `ScanShaders / ScanMaterials / ScanCompleteDirectory`；
- `GetAllShaderResources() / GetAllMaterialResources()`、`GetTotalShaderCount() / GetLoadedShaderCount()`、`GetTotalMaterialCount() / GetLoadedMaterialCount()`；
- `GetContentRoot() / GetEngineRoot()`（`FileTreeNode*`）；
- `ShowResourceWindow(bool* open)`。

`PathUtils.h`（全部 inline）：`GetProjectRoot`、`GetEnginePath`、`GetContentPath`、`GetSavedPath`、`GetSavedConfigPath`、`WToA`。

`Settings`（`Engine/public/Settings.h`）：`Initialize`、`GetWidth/Height/AspectRatio`、`SetResolution`、`SetResolutionByIndex`、`GetResolutionOptions`、`GetCurrentResolutionIndex`、`RegisterResolutionChangedCallback`、`Get/SetFpsLimit`、`Get/SetMouseSpeed/MoveSpeed`、`Get/SetTaaJitterScale`、`IsPendingResolutionChange / GetPendingResolution / ShouldResizeWindow / ClearPendingResolutionChange / RequestResolutionChange`。

## 配置与调参

| 项 | 默认/取值 | 位置 |
| --- | --- | --- |
| Content 层路径 | `<root>/Content/` | `PathUtils.h` |
| Engine 层路径 | `<root>/Engine/` | `PathUtils.h` |
| 配置目录 | `<root>/Saved/Config/`（自动创建） | `PathUtils.h` |
| shader 扫描目录 | Engine: `Engine/Shader/`；Content: `Content/Shaders/` | `ResourceManager::ScanAndLoadAllResources` |
| material 扫描目录 | Engine: `Engine/Shader/`；Content: `Content/Materials/` | 同上 |
| 排除文件 | `screen.shader`、`sky.shader`（大小写各一） | `ScanDirectory` |
| 分辨率预设 | 720p / 1080p / 1440p / 2K | `Settings.cpp` |
| 默认分辨率 | 1280×720 | `Settings.h` / `main.cpp` |
| FPS 限制 | 144（0=无限制） | `Settings.h` |
| 相机速度默认 | 5.0 / 50.0 | `Settings.h`（面板直接改 Scene） |
| 资源窗口布局 | 左 30% 树 / 右 70% 平铺 | `ShowResourceWindow` |

## 已知限制与 TODO

- `ResourceManager` 只扫描和浏览，**不加载** shader/material（`ScanAndLoadAllResources` 的日志明确输出 “Shaders need to be loaded manually in main.cpp”）；`ResourceInfo.isLoaded` 也没有被任何加载路径更新。
- 无资源搜索、收藏、过滤（除扩展名平铺高亮外）；无 Content 层的写入/导入能力（纹理导入走 Texture 菜单，与资源管理器无关）。
- `FileTreeNode` 内存由手写递归析构管理（`~FileTreeNode` 循环 delete），扫描会整树重建（先 delete 旧根），大目录树时存在停顿。
- `Settings` 的相机速度、TAA Jitter Scale 目前没有 UI 消费方；`RegisterResolutionChangedCallback` 注册机制存在但 `main.cpp` 未使用。
- `Settings` 本身不做磁盘持久化（分辨率/FPS 每次启动恢复默认）；持久化只存在于 `Saved/Config/*.ini`（ImGui 布局与窗口可见性，见 `EditorPanels.md`）。
- 扫描是启动时一次性快照，运行中新增/删除资源不会自动刷新（需要重新调用 `ScanAndLoadAllResources`，UI 未暴露）。
- 资源路径统一以宽字符存储（`std::wstring`），名称转 UTF-8 `std::string`，非 UTF-8 文件名可能乱码。

## 维护注意事项

- **所有新路径代码必须走 `PathUtils`**：不要硬编码 `Engine\` / `Content\` 前缀；exe 输出位置变化时 `GetProjectRoot` 的回溯逻辑（最多 5 级 + 3 级回退）已覆盖常见布局，改动构建输出目录后验证一次。
- 新增资源类型（如 `.mesh`、`.level`）时，在 `ScanAndLoadAllResources` 中仿照 `ScanMaterials` 增加扫描入口，并同步更新 `ResourceInfo.type` 语义与 UI。
- 修改 `Settings` 的分辨率流程时，保持“请求 → 帧首延迟执行 → 清标志”的三段式，避免在 ImGui 回调中直接重建渲染资源。
- `ScanDirectory` 与 `ScanCompleteDirectory` 是两套遍历（前者按扩展名收集，后者建树），改动遍历逻辑时注意保持一致（如跳过 `..`、路径分隔符用 `/` 拼接）。
- `ResourceManager` 窗口的导航状态（`m_leftSelected` 等）跨帧保存，窗口关闭再打开会保留位置；若后续支持多窗口实例，需要把状态移出单例。
- 资源统计计数（`GetLoadedShaderCount` 等）依赖 `ResourceInfo.isLoaded` 被正确置位——目前没有置位方，统计仅表示“已扫描”数量，接入加载流程时记得更新。