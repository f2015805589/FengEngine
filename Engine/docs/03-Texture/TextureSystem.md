# 纹理系统（TextureSystem）

> 本文依据 `Engine/private/Texture/`、`Engine/public/Texture/` 实际源码编写，说明 `.texture.ast` 资产格式、`TextureManager` 加载与 SRV 管理、`TextureCompressor` 三路压缩（GPU 计算着色器 / CPU DirectXTex / NVTT 外部进程）、`TexturePreviewPanel` 预览面板与 `TextureCache` 缓存机制。

## 概述

纹理系统承担「源图 → 压缩资产 → GPU 资源 → Bindless SRV」的完整管线：

1. **资产化**：源图（PNG/JPG/DDS/HDR）导入后生成 `.texture.ast`（UTF-8 XML），记录源路径、源文件 MD5、压缩格式（默认 BC3）、mip/sRGB 设置与缓存 DDS 路径。
2. **缓存**：压缩结果以 DDS 形式缓存在源图同目录的 `TextureCache/` 子目录；`SourceHash` 变化自动失效重压。
3. **三路压缩**：NVTT 外部进程（默认，质量最高）→ DirectXTex CPU 压缩（回退/离线）→ GPU 计算着色器压缩（BC1/BC3/BC5，运行时快速路径）。
4. **运行时**：`TextureManager` 单例持有 1000 槽 SRV 堆，`TextureAsset` 加载到 GPU 并分配 SRV；材质系统通过 Bindless 索引引用（`MaterialInstance::LoadTexturesFromPaths` → `Scene::AllocateBindlessSRVSlot`，见 MaterialSystem.md）。
5. **编辑器**：`TexturePreviewPanel`（ImGui）提供通道可视化（RGBA/R/G/B/A/Normal/Luminance）、mip 浏览、曝光/Gamma、缩放平移，以及 UE 风格的「选格式 → Apply 重压缩 → Save Asset」流程。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/public/Texture/TextureAsset.h` / `Engine/private/Texture/TextureAsset.cpp` | 纹理资产：XML 读写、缓存验证、GPU 加载/卸载、压缩调度 |
| `Engine/public/Texture/TextureManager.h` / `Engine/private/Texture/TextureManager.cpp` | 管理器单例：加载缓存、SRV 堆与槽位分配、异步加载、缓存清理 |
| `Engine/public/Texture/TextureCompressor.h` / `Engine/private/Texture/TextureCompressor.cpp` | 压缩器：GPU 计算着色器 / CPU DirectXTex / NVTT 三路 |
| `Engine/public/Texture/TexturePreviewPanel.h` / `Engine/private/Texture/TexturePreviewPanel.cpp` | ImGui 预览面板 + 通道可视化 shader + 压缩设置 UI |
| `Engine/Shader/Compression/BC1Compress.hlsl` / `BC3Compress.hlsl` / `BC5Compress.hlsl` | GPU 压缩计算着色器 |
| `Engine/Shader/Utility/TexturePreview.hlsl` | 预览通道可视化 shader（面板另有内嵌同款代码） |
| `Engine/public/PathUtils.h` | `GetProjectRoot()`：相对/绝对路径换算依据 |
| `Content/Texture/*.texture.ast` 与 `Content/Texture/Default/*.texture.ast` | 资产样例（color/normal/orm/White） |
| `Tools/NVIDIA Texture Tools/nvtt_export.exe` | NVTT 压缩外部工具（可选，缺失自动回退） |

## 架构与数据流

### .texture.ast 资产格式（UTF-8 XML）

```xml
<?xml version="1.0" encoding="utf-8"?>
<TextureAsset name="color" version="1.0">
  <Source>
    <Path>Content\Texture\color.png</Path>
    <SourceHash>421f040e458a44921704e3b726768cc6</SourceHash>
  </Source>
  <TextureType>Texture2D</TextureType>
  <Compression>
    <Format>BC3</Format>
    <GenerateMips>true</GenerateMips>
    <sRGB>true</sRGB>
  </Compression>
  <Cache>
    <DdsPath>...\TextureCache\color.dds</DdsPath>
    <CacheValid>true</CacheValid>
  </Cache>
  <RuntimeInfo>  <!-- 仅加载后写入 -->
    <Width>4096</Width><Height>4096</Height>
    <MipLevels>13</MipLevels><MemorySize>22369648</MemorySize>
  </RuntimeInfo>
</TextureAsset>
```

- `ParseAssetXML` 用 MSXML6 解析；路径经 `ToAbsolutePath/ToRelativePath`（基于 `GetProjectRoot()`）归一化，支持资产内保存相对路径、运行时换算绝对路径。
- 解析末尾做缓存验证：`m_cacheValid && SourceHash != MD5(源文件)` 时置 `m_cacheValid = false`（源图变更自动重压）。

### 加载流程（TextureManager::LoadTexture）

```
LoadTexture(path) → LoadTextureInternal(path)
 ├─ 路径/名称双缓存查重（m_pathToName / m_textures）
 ├─ 扩展名分发：
 │   ├─ .texture.ast → texture->LoadFromAssetFile(path)
 │   ├─ .dds        → ImportFromSource（format=None，不重压）
 │   └─ 其他源文件   → ImportFromSource（默认 BC3 + mips + sRGB）
 ├─ 若 m_commandList 有效：texture->LoadToGPU(device, commandList)
 └─ 存入 m_textures / m_pathToName 并返回
```

`TextureAsset::LoadToGPU` 优先级：`m_cacheValid && 缓存 DDS 存在` → `LoadDDSFromCache`；否则 `LoadAndCompressSource`（压缩后落盘缓存再加载）。`LoadSourceToGPU` 是预览专用路径：直接加载源图、不压缩、只 1 级 mip。

### 压缩三路（LoadAndCompressSource）

```
LoadAndCompressSource
 ├─ [1] NVTT：s_useNVTT && format != None && IsNVTTAvailable()
 │       └─ CompressWithNVTT（子进程 nvtt_export.exe，60s 超时，失败回退）
 ├─ [2] DirectXTex CPU：
 │       LoadFromWICFile/DDSFile/HDRFile → GenerateMipMaps(TEX_FILTER_DEFAULT)
 │       → DirectX::Compress(TEX_COMPRESS_DEFAULT | TEX_COMPRESS_PARALLEL)
 │       → SaveToDDSFile(缓存路径)
 └─ [3] 加载 DDS 缓存 → GPU 资源（UpdateSubresources + 状态转换）
```

`TextureCompressor::CompressGPU` 是独立运行时的快速路径（见下），不在 `LoadAndCompressSource` 默认链路中。

### GPU 资源与 SRV

- `TextureManager` 初始化创建 `MAX_TEXTURES = 1000` 槽的 shader-visible CBV_SRV_UAV 堆；`AllocateSRVIndex/FreeSRVIndex` 环形分配（`m_srvSlotUsed` + `m_nextFreeSlot`，互斥锁保护）。
- `TextureAsset::CreateSRV` 按 `TextureType` 创建 2D / Cube / 2DArray SRV，保存 CPU/GPU 句柄与槽位索引；`UnloadFromGPU` 释放槽位。
- 内存估算 `CalculateMemorySize`：按 mip 链逐级累计（BC1 8B/块，BC3/5/7/6H 16B/块）。

## 关键实现要点

### TextureCompressor 三路实现

**GPU 压缩（CompressGPU / CompressMipLevel）**

- 支持 BC1/BC3/BC5（`SupportsGPUCompression` 返回 true；BC7/BC6H 复杂度高，走 CPU）。
- 计算着色器来自 `Engine/Shader/Compression/BCxCompress.hlsl`（`CSMain`，cs_5_0），编译失败不致命（回退 CPU）。
- 专用根签名：b0=`CompressionParams` CBV、t0=源纹理 SRV、u0=输出 UAV、s0=采样器；线程组 `THREAD_GROUP_SIZE = 8`（8×8 线程，每线程压一个 4×4 块），`Dispatch((blockCountX+7)/8, (blockCountY+7)/8, 1)`。
- `CompressionParams`：textureWidth/Height、blockCountX/Y、mipLevel、isSRGB。
- `CompressGPUWithMips` 目前是 TODO（返回 false）。

**CPU 压缩（CompressCPU / CompressAndSaveDDS）**

- DirectXTex `DirectX::Compress`；`TextureCompressionQuality`：Fast/Normal 走默认，High 对 BC7 加 `TEX_COMPRESS_BC7_QUICK`；并行 `TEX_COMPRESS_PARALLEL`。

**NVTT 压缩（CompressWithNVTT）**

- 路径默认 `Tools\NVIDIA Texture Tools\nvtt_export.exe`（`SetNVTTPath` 可改）；`IsNVTTAvailable` 检查文件存在。
- 命令行：`nvtt_export.exe -o <out.dds> --format bc3 [--mips] [--srgb] --quality <0-3> <input.png>`。
- `CreateProcessW` 隐藏窗口启动，`WaitForSingleObject` 60 秒超时；失败/超时/退出码非 0 均回退 `CompressAndSaveDDS(..., High)`。
- 质量等级 `NVTTQuality`：Fastest=0 / Normal=1 / Production=2（默认）/ Highest=3。
- 开关：`TextureAsset::s_useNVTT`（静态，默认 true），`SetUseNVTT/GetUseNVTT` 控制，预览面板可切换。

### 缓存机制（TextureCache）

- `GenerateCachePath()`：源图同目录下建 `TextureCache\`，产物为 `<源文件名>.dds`。
- `TextureManager::m_cacheDir` 默认声明为 `Engine/TextureCache/`，但 `Initialize` 实际改为 **exe 目录下的 `TextureCache\`**（`GetModuleFileNameW` + `PathRemoveFileSpecW`），二者不一致需注意。
- `ClearCache()` 删除缓存目录下所有 `.dds`；`RebuildCache(commandList)` 对所有已加载纹理 `UnloadFromGPU + LoadToGPU`；`ValidateCache(assetPath)` 用临时资产检查 `IsCacheValid()`。
- 缓存失效三条件：`CacheValid=false`、缓存 DDS 不存在、`SourceHash` 与当前源文件 MD5 不一致。

### 预览面板（TexturePreviewPanel）

- 单例；`Initialize` 创建预览资源（内嵌 HLSL shader：`channelMode` 0-7 对应 RGBA/RGB/R/G/B/A/Normal/Luminance，`exposure/gamma/mipLevel/uvOffset/uvScale`；1024×1024 RT；根签名 b0 CBV + t0 SRV + 静态线性采样器）。
- `SetTexturePath` 采用**延迟加载**：只记录路径，渲染帧间隙由 `ProcessPendingLoad()`（main.cpp 每帧检查 `HasPendingLoad()`）执行，避免帧中阻塞压缩。
- `RenderUI`：View 菜单（信息/压缩面板、Fit、Reset）、Channel 菜单、工具栏（通道、Mip Slider、Zoom、BC6H 时 Exposure、Alpha 棋盘格）；`RenderTextureView` 支持滚轮缩放、中键平移、像素坐标 Tooltip；`RenderInfoPanel` 显示尺寸/格式/mip/显存。
- `RenderCompressionSettings`：NVTT 开关与质量、格式 Combo（None/BC1/BC3/BC5/BC7/BC6H）、GenerateMips、sRGB、**Apply** 按钮 → `ApplyCompression(format, device, cmdList)`（置 CacheValid=false → UnloadFromGPU → 重新压缩加载）→ `CreatePreviewSRV` 刷新；「Save Asset File」把资产 XML 写到源图同名的 `.texture.ast`。
- `CreatePreviewSRV` 在 ImGui 描述符堆固定槽位 `IMGUI_TEXTURE_SLOT_START = 10` 创建 SRV 供 `ImGui::Image` 显示；`RenderTextureToPreviewRT` 预留但未实现。

## 对外接口

### TextureManager（单例）

| 接口 | 说明 |
| --- | --- |
| `Initialize(device)` / `Shutdown()` | 创建 SRV 堆（1000 槽）、缓存目录 |
| `LoadTexture(path)` / `LoadTextureAsync(path)` | 同步/异步加载（std::async + 互斥锁） |
| `ImportTexture(sourcePath, desc)` | 导入新资产（UE 风格：format=None 不压缩，仅原图预览） |
| `GetTexture(name)` / `GetTextureByPath(path)` / `UnloadTexture` / `UnloadAllTextures` | 查询与卸载 |
| `AllocateSRVIndex()` / `FreeSRVIndex(i)` / `GetSRVHeap()` / `GetSRVCPUHandle/GetSRVGPUHandle(i)` | SRV 堆管理 |
| `GetCompressor()` / `SetCommandList` / `GetCommandList` | 压缩器与命令列表访问 |
| `GetCacheDirectory()` / `ClearCache()` / `RebuildCache(cmdList)` / `ValidateCache(path)` | 缓存管理 |
| `GetLoadedTextureCount()` / `GetTotalMemoryUsage()` / `GetAllTextureNames()` | 统计 |
| 常量 `MAX_TEXTURES = 1000` | SRV 堆上限 |

### TextureAsset

- 加载/保存：`LoadFromAssetFile` / `ImportFromSource` / `SaveAssetFile`。
- GPU：`LoadToGPU` / `LoadSourceToGPU` / `ApplyCompression(format, device, cmdList)` / `Recompress` / `UnloadFromGPU`。
- Getter：`GetResource` / `GetSRV` / `GetSRVIndex` / `GetWidth/Height/MipLevels/Format/MemorySize` / `IsLoaded` / `IsCacheValid` / `GetSourcePath` / `GetCachePath`。
- 静态：`GetDXGIFormat(format, sRGB)`、`GetFormatName`、`GetTypeName`、`CalculateMemorySize`、`s_useNVTT/SetUseNVTT`。

### TextureCompressor

- `CompressGPU` / `CompressGPUWithMips`（TODO）/ `CompressCPU` / `CompressAndSaveDDS` / `CompressWithNVTT`。
- 配置：`SetNVTTPath` / `SetNVTTQuality` / `IsNVTTAvailable` / `GetNVTTQualityName`。
- 静态：`GetBlockSize` / `GetCompressedFormat` / `SupportsGPUCompression`。

## 配置与调参

- **默认压缩设置**：`TextureAssetDesc` 默认 `format=BC3`、`quality=Normal`、`generateMips=true`、`sRGB=true`、`filter=Trilinear`、`address=Wrap`、`maxAnisotropy=16`（采样设置当前仅在描述中，GPU 采样器由管线静态采样器决定）。
- **格式映射**（`GetDXGIFormat`）：None→R8G8B8A8_UNORM(_SRGB)；BC1/BC3/BC7→UNORM(_SRGB)；BC5→BC5_UNORM（无 sRGB）；BC6H→BC6H_UF16（HDR，无 sRGB）。
- **质量策略**：预览面板里「Use NVIDIA Texture Tools」勾选 + Quality（默认 Production）；关闭或工具缺失时用 DirectXTex。
- **压缩耗时**：NVTT 与 CPU 压缩在主线程执行，大图（如 4096² PNG）会卡顿；`LoadTextureAsync` 提供异步入口，但 `ApplyCompression` 仍是同步的。

## 已知限制与TODO

- `CompressGPUWithMips` 未实现（返回 false）；GPU 压缩只覆盖单 mip 的 BC1/BC3/BC5，且 `CompressGPU` 主链路在运行时代码中未被 `LoadAndCompressSource` 调用。
- `m_bc7ShaderBlob/m_bc7PSO/m_bc6hShaderBlob/m_bc6hPSO` 成员已声明，但 `CompileComputeShaders` 只编译 BC1/BC3/BC5。
- `TextureManager::m_cacheDir` 头文件默认值（`Engine/TextureCache/`）与 `Initialize` 实际写入的 exe 目录 `TextureCache\` 不一致。
- `.texture.ast` 中 `DdsPath` 会写成绝对路径（`WriteAssetXML` 用 `ToRelativePath` 已尽量归一化，但旧资产仍含绝对路径），换机器需靠 `ToAbsolutePath` 兜底。
- 预览面板的「通道可视化」在 `RenderTextureView` 中仅通过 ImGui tint 近似实现（R/G/B 通道），完整通道 shader（内嵌代码/TexturePreview.hlsl）已编译但 `RenderTextureToPreviewRT` 未接入。
- `LoadTextureAsync` 的 future 结果需调用方持有，内部未跟踪未完成任务；压缩/加载均要求 `m_commandList` 非空，否则只缓存资产不加载 GPU。
- 重复导入同名纹理按名称去重，不同目录同名源文件会互相覆盖缓存条目。

## 维护注意事项

- 资产与源文件编码：`.texture.ast` 必须 UTF-8；源文件路径变化或 MD5 变化都会触发缓存失效重压。
- 修改压缩格式/质量后，需点击预览面板「Apply」或调用 `ApplyCompression` 才会重压；直接改 XML 不生效（`CacheValid` 校验）。
- `nvtt_export.exe` 升级/更换后验证命令行兼容性（`-o/--format/--mips/--srgb/--quality`），失败路径会自动回退 DirectXTex，但日志只打控制台。
- GPU 压缩计算着色器在 `Engine/Shader/Compression/`，新增格式需同时补 shader、PSO 与 `SupportsGPUCompression`。
- 资源释放顺序：`TexturePreviewPanel::Shutdown` → `TextureManager::Shutdown`（先于 `MaterialManager::Shutdown`），因为材质纹理引用的 SRV 槽位由 TextureManager 管理。
- 排查显示问题先看 `TexturePreviewPanel` 的 `CreatePreviewSRV` 日志（ImGui 堆槽位 10）与 `TextureManager` 的 `Loaded texture` 日志；SRV 槽位耗尽时 `AllocateSRVIndex` 返回 `UINT_MAX`。
*** End Patch