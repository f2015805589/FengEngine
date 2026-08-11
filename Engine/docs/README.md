# Engine/docs —— FEngine 模块文档索引

FEngine 是一个 DirectX 12 渲染引擎项目，`Engine/docs/` 下按模块分类存放引擎各系统文档，本文件为目录导航索引，[TEMPLATE.md](TEMPLATE.md) 为文档写作模板。

## 目录结构

```
Engine/docs/
├── README.md               # 文档导航索引
├── TEMPLATE.md             # 文档写作模板
├── 01-Rendering/           # RenderPipeline + 9 个 Pass（Base/Shadow/Light/GTAO/SSGI/TAA/Sky/Screen/ImGui）
├── 02-Material/            # MaterialSystem / ShaderSystem / ShadingModels
├── 03-Texture/             # TextureSystem
├── 04-Scene/               # SceneAndActor / CameraAndViewport
├── 05-Lighting/            # IBL
├── 06-Editor/              # EditorPanels
└── 07-Infrastructure/      # ResourceManager / D3D12Infrastructure / ShaderAssets
```

## 模块清单

表格说明：`类别` 为文档所属分类目录，`模块` 为模块名称，`状态` 为文档完成状态（已完成 / 待补充）。

### 01-Rendering

| 类别 | 模块 | 文档路径 | 状态 |
| --- | --- | --- | --- |
| 01-Rendering | RenderPipeline | [01-Rendering/RenderPipeline.md](01-Rendering/RenderPipeline.md) | 已完成 |
| 01-Rendering | BasePass | [01-Rendering/BasePass.md](01-Rendering/BasePass.md) | 已完成 |
| 01-Rendering | ShadowPass | [01-Rendering/ShadowPass.md](01-Rendering/ShadowPass.md) | 已完成 |
| 01-Rendering | LightPass | [01-Rendering/LightPass.md](01-Rendering/LightPass.md) | 已完成 |
| 01-Rendering | GTAOPass | [01-Rendering/GTAOPass.md](01-Rendering/GTAOPass.md) | 已完成 |
| 01-Rendering | SSGIPass | [01-Rendering/SSGIPass.md](01-Rendering/SSGIPass.md) | 已完成 |
| 01-Rendering | TAAPass | [01-Rendering/TAAPass.md](01-Rendering/TAAPass.md) | 已完成 |
| 01-Rendering | SkyPass | [01-Rendering/SkyPass.md](01-Rendering/SkyPass.md) | 已完成 |
| 01-Rendering | ScreenPass | [01-Rendering/ScreenPass.md](01-Rendering/ScreenPass.md) | 已完成 |
| 01-Rendering | ImGuiPass | [01-Rendering/ImGuiPass.md](01-Rendering/ImGuiPass.md) | 已完成 |

### 02-Material

| 类别 | 模块 | 文档路径 | 状态 |
| --- | --- | --- | --- |
| 02-Material | MaterialSystem | [02-Material/MaterialSystem.md](02-Material/MaterialSystem.md) | 已完成 |
| 02-Material | ShaderSystem | [02-Material/ShaderSystem.md](02-Material/ShaderSystem.md) | 已完成 |
| 02-Material | ShadingModels | [02-Material/ShadingModels.md](02-Material/ShadingModels.md) | 已完成 |

### 03-Texture

| 类别 | 模块 | 文档路径 | 状态 |
| --- | --- | --- | --- |
| 03-Texture | TextureSystem | [03-Texture/TextureSystem.md](03-Texture/TextureSystem.md) | 已完成 |

### 04-Scene

| 类别 | 模块 | 文档路径 | 状态 |
| --- | --- | --- | --- |
| 04-Scene | SceneAndActor | [04-Scene/SceneAndActor.md](04-Scene/SceneAndActor.md) | 已完成 |
| 04-Scene | CameraAndViewport | [04-Scene/CameraAndViewport.md](04-Scene/CameraAndViewport.md) | 已完成 |

### 05-Lighting

| 类别 | 模块 | 文档路径 | 状态 |
| --- | --- | --- | --- |
| 05-Lighting | IBL | [05-Lighting/IBL.md](05-Lighting/IBL.md) | 已完成 |

### 06-Editor

| 类别 | 模块 | 文档路径 | 状态 |
| --- | --- | --- | --- |
| 06-Editor | EditorPanels | [06-Editor/EditorPanels.md](06-Editor/EditorPanels.md) | 已完成 |

### 07-Infrastructure

| 类别 | 模块 | 文档路径 | 状态 |
| --- | --- | --- | --- |
| 07-Infrastructure | ResourceManager | [07-Infrastructure/ResourceManager.md](07-Infrastructure/ResourceManager.md) | 已完成 |
| 07-Infrastructure | D3D12Infrastructure | [07-Infrastructure/D3D12Infrastructure.md](07-Infrastructure/D3D12Infrastructure.md) | 已完成 |
| 07-Infrastructure | ShaderAssets | [07-Infrastructure/ShaderAssets.md](07-Infrastructure/ShaderAssets.md) | 已完成 |

### 迁移文档

| 类别 | 文档路径 | 状态 |
| --- | --- | --- |
| 02-Material | [HOW_TO_USE_MATERIAL_SHADER.md](02-Material/HOW_TO_USE_MATERIAL_SHADER.md) | 已完成 |
| 02-Material | [MATERIAL_SYSTEM_INTEGRATION.md](02-Material/MATERIAL_SYSTEM_INTEGRATION.md) | 已完成 |

## 阅读顺序说明

阅读顺序遵循 `Engine/README.md` 的导航协议（即仓库根目录的 [README.md](../../README.md)），按“基础设施 → 渲染管线 → 资源与材质 → 光照 → 场景 → 编辑器”的顺序推进：

1. `07-Infrastructure/`：先读基础设施文档，理解 D3D12 设备、资源管理与 Shader 资产编译流程。
2. `01-Rendering/`：按渲染管线顺序阅读 RenderPipeline 与各 Pass 文档。
3. `03-Texture/` 与 `02-Material/`：理解纹理与材质链路。
4. `05-Lighting/`：了解 IBL 光照。
5. `04-Scene/`：场景组织方式。
6. `06-Editor/`：编辑器面板。

阅读单个模块时，先参照 `TEMPLATE.md` 的章节结构定位重点内容。