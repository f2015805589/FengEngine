# SceneAndActor — 场景与 Actor 体系

> 本文覆盖 FEngine 的场景组织方式：Actor-Component 模式的雏形（`Actor` + `StaticMeshComponent` + `MaterialInstance`）、`Scene` 对 Actor 列表 / 相机 / 灯光 / Skylight 的管理、`.mesh` 与 `.level` 资产格式，以及基于 FBX SDK 的网格导入流程。文中的类名、函数名、文件路径均来自当前源码。

## 概述

FEngine 的场景系统采用“场景（`Scene`）— 实体（`Actor`）— 组件（`StaticMeshComponent`）”的分层结构，但目前仍处于 Actor-Component 模式的早期形态：

- `Actor` 是一个可放置实体，持有 `Transform`（位置/旋转/缩放）、`MeshAssetInfo`（`.mesh` 资产描述）、`StaticMeshComponent*`（渲染网格）与 `MaterialInstance*`（材质），并为每个 Actor 维护一份独立的常量缓冲区（b0）。
- `StaticMeshComponent` 负责网格数据（VBO/IBO）与 FBX 导入，一个组件内部可以包含多个 `SubMesh`（按 FBX 节点名索引）。
- `Scene` 是场景的总管理器：维护 `std::vector<Actor*>` 列表、内置相机 `Camera m_camera`、平行光方向（由 `m_lightRotation` 旋转推导）、Skylight（强度 + 颜色）、阴影参数（正交范围 / 模式 / 开关）、GI 类型，并负责 `.mesh` / `.level` 的加载与保存。
- 场景使用延迟渲染的 GBuffer 输出：`Scene::Render` 将全部 Actor 绘制到 4 张离屏 RT（Albedo / Normal / ORM / MotionVector），后续由 LightPass / ScreenPass 消费。

当前场景只有一个方向光（主光），没有点光/聚光灯组件；Skylight 是场景级的“环境光”参数，通过 `SceneCBData` 传给所有着色器。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/public/Actor.h` | `Transform`、`MeshAssetInfo`、`Actor` 类声明 |
| `Engine/public/StaticMeshComponent.h` | `StaticMeshComponentVertexData`、`SubMesh`、`StaticMeshComponent` 声明 |
| `Engine/public/Scene.h` | `Scene` 类声明（Actor 管理 / Level 管理 / 阴影矩阵 / SRV 堆 / TAA 参数） |
| `Engine/private/Actor.cpp` | Transform 矩阵、`.mesh` 读写、Actor 常量缓冲区创建与更新 |
| `Engine/private/StaticMeshComponent.cpp` | FBX SDK 导入、顶点/索引缓冲创建、渲染 |
| `Engine/private/Scene.cpp` | 离屏 RT 创建、纹理加载、场景更新/渲染、Actor/Level 管理、阴影矩阵 |
| `Engine/Content/Actor/Mesh/sphere.mesh` | `.mesh` 资产示例（引用 `Content/Actor/FBX/sphere.fbx`） |
| `Engine/Content/Level/Default.level` | `.level` 资产示例（引用 sphere.mesh + DefaultPBR 材质） |

## 架构与数据流

### 场景对象关系

```text
Scene
├── Camera m_camera（内置编辑器相机）
├── StaticMeshComponent m_staticMesh（旧式单网格，向后兼容回退路径）
├── std::vector<Actor*> m_actors
│   └── Actor
│       ├── Transform m_transform（position / rotation / scale）
│       ├── MeshAssetInfo m_meshAssetInfo（.mesh 描述符）
│       ├── StaticMeshComponent* m_mesh（FBX 导入后的 GPU 网格）
│       ├── MaterialInstance* m_material（材质参数 + Shader + PSO）
│       └── ID3D12Resource* m_constantBuffer（独立 b0 CB）
├── 灯光状态：m_lightRotation / m_lightDirection（唯一平行光）
├── Skylight：m_skylightIntensity / m_skylightColor
└── 阴影：m_shadowOrthoSize / m_shadowMode / m_shadowmapEnabled
```

### 帧数据流

1. `main.cpp` 主循环调用 `g_scene->Update(deltaTime)`：
   - 相机更新（`m_camera.Update`）；
   - 由 `m_lightRotation` 旋转初始方向 `(-1,-1,1)` 得到 `m_lightDirection`；
   - 计算 TAA Jitter 后的投影矩阵、逆矩阵、`lightViewProjMatrix`（`CalculateLiSPSMMatrix`）；
   - 通过 `FillSceneCBData` 填充场景级 `SceneCBData`，`memcpy` 到持久映射的 `m_constantBuffer`；
   - 遍历所有 Actor 调用 `actor->UpdateConstantBuffer(...)`，保证在任意 Pass 之前 CB 已就绪。
2. `main.cpp` 调用 `g_scene->Render(commandList, gbufferPso, rootSignature)`：
   - 将 4 个离屏 RT 从 `PIXEL_SHADER_RESOURCE` 转为 `RENDER_TARGET` 并绑定（同时绑定深度缓冲）；
   - 清除 RT 与深度缓冲；
   - 若 `m_actors` 非空：遍历每个 Actor，按其 `MaterialInstance->GetShader()->GetPSO(0)` 切换 PSO（无材质时回退默认 PSO），绑定材质 CB（slot 2）与 Actor 自己的 CB（b0），调用 `mesh->Render` 发出 `DrawIndexedInstanced`；
   - 若没有 Actor：走旧的单网格路径 `m_staticMesh.Render`；
   - 结束后将 RT 转回 `PIXEL_SHADER_RESOURCE`、深度缓冲转回 `PIXEL_SHADER_RESOURCE`（供后续 LightPass/GTAO/SSGI 采样）。
3. `.level` 资产加载在材质系统初始化之后进行：`g_scene->LoadLevel(GetEnginePath() + L"Content\\Level\\Default.level")`。

### 资产加载数据流

```text
.level ──> [Actor_N] 段 ──> Actor + Transform（位置/旋转/缩放）
              └─> MeshAsset=*.mesh ──> MeshAssetInfo（.mesh 解析）
                      └─> FBXPath=*.fbx ──> StaticMeshComponent::InitFromFile
                                              └─> FBX SDK 导入 ──> VBO + IBO ──> SubMesh
Material=DefaultPBR ──> MaterialManager::GetMaterial ──> actor->SetMaterial
```

## 关键实现要点

### Transform 与模型矩阵

- `Transform::GetModelMatrix()` 按 `缩放 * 旋转 * 平移` 组合；旋转使用 `XMMatrixRotationRollPitchYaw(pitch, yaw, roll)`（ZXY 顺序），输入为**角度制**（`XMConvertToRadians` 转换）。
- `Actor::UpdateModelMatrix()` 是空实现（注释说明渲染时动态计算），矩阵由 `GetModelMatrix()` 实时返回。

### Actor 常量缓冲区（b0）

- `Actor::CreateConstantBuffer(ID3D12Device*)` 调用共享工具 `CreateConstantBufferObject(sizeof(SceneCBData))`（UPLOAD 堆），并**持久映射**，初始填充单位矩阵。
- `Actor::UpdateConstantBuffer(...)` 与 `Scene::Update` 使用同一个 `FillSceneCBData`（`BattleFireDirect.cpp` 中实现），只是 model 矩阵换成 Actor 自己的 `GetModelMatrix()`。
- 析构时只释放 CB；`m_mesh` / `m_material` 由外部管理，Actor 不删除（见已知限制）。

### FBX 导入（StaticMeshComponent）

- `InitFromFile` 流程：文件存在性检查 → `FbxManager::Create` → `FbxIOSettings` → `LoadPluginsDirectory`（优先 exe 目录，其次 exe 同级的 `plugins/`）→ `FbxImporter::Initialize` → `Import(scene)` → `ParseFBXScene`。
- `ParseFBXScene` / `ProcessFBXNode` 递归遍历节点属性，遇到 `FbxNodeAttribute::eMesh` 调用 `ProcessFBXMesh`。
- `ProcessFBXMesh` 关键处理：
  - 坐标转换：`vtx.mPosition = (pos[0], pos[2], pos[1])`（YZ 交换），法线同样交换 YZ；UV 的 V 翻转（`1.0 - uv[1]`）；
  - 法线取 `GetPolygonVertexNormal` 并归一化；切线取 `GetElementTangent(0)`，同样做 YZ 交换；
  - FBX 无切线数据时用 Gram-Schmidt 正交化从法线生成默认切线（参考向量：法线接近 Y 轴时用 X 轴，否则用 Y 轴）；
  - 多边形使用 fan 三角化（`polyVertIndices[0], j, j+1`）；
  - 每节点生成一个 `SubMesh`（32 位索引，`DXGI_FORMAT_R32_UINT`），存入 `mSubMeshes[nodeName]`；
  - 顶点缓冲使用 `CreateBufferObject`（DEFAULT 堆 + upload 拷贝），索引缓冲用 `D3D12_RESOURCE_STATE_INDEX_BUFFER`。
- `Render`：绑定 VBO 视图，若材质有待加载纹理则先 `material->LoadTexturesFromPaths`，再 `material->Bind(commandList, rootSignature, 2)`（slot 2 对应 b1），最后遍历 `mSubMeshes` 逐个 `DrawIndexedInstanced`。

### .mesh 资产格式

`MeshAssetInfo::SaveToFile / LoadFromFile` 使用 `[MeshAsset]` 段 + `Key=Value` 文本格式（`#` 开头为注释）：

```ini
[MeshAsset]
Name=Sphere
FBXPath=Content/Actor/FBX/sphere.fbx
DefaultMaterial=DefaultPBR
```

注意：`Actor::LoadFromMeshFile` 只解析 `.mesh` 描述符，**不加载 Transform**（注释明确说明 Transform 由 `.level` 设置），网格本体由调用方负责调用 `InitFromFile`。

### .level 资产格式

`Scene::SaveLevel / LoadLevel` 使用 `[Level]` + `[Actor_N]` 段。`SaveLevel` 直接遍历 `m_actors` 写出；`LoadLevel` 逐段解析，遇到 `[Actor_N]` 创建新 `Actor`，`Name` 键创建 Actor 并分配 CB，随后解析 `MeshAsset / Material / Position / Rotation / Scale`，由 `FinalizeAndAddActor` 完成最终组装（设置 Transform → 读 `.mesh` → 加载 FBX → 挂材质 → 加入 `m_actors`）。示例：

```ini
[Level]
Name=Default
Description=Default level

[Actor_0]
Name=Sphere_1
MeshAsset=Content/Actor/Mesh/sphere.mesh
Material=DefaultPBR
Position=0,0,0
Rotation=0,0,0
Scale=1,1,1
```

### 阴影矩阵与 911 行硬编码

- `Scene::CalculateLiSPSMMatrix` 目前直接返回 `CalculateStandardShadowMatrix(lightDir)`（LiSPSM 尚未真正实现）。
- `CalculateStandardShadowMatrix` 以相机为中心构建固定范围正交投影：
  - 范围 `shadowSize = m_shadowOrthoSize`；
  - **第 911 行**：`const float shadowMapResolution = 2048.0f;  // TODO: 从LightPass获取实际分辨率` —— 阴影分辨率被硬编码为 2048，纹素对齐（texel snapping）也依赖该值。注意 `main.cpp` 中 `LightPass` 实际以 4096 创建阴影图（`new LightPass(viewportWidth, viewportHeight, 4096)`），两者不一致是已知 TODO。
  - 光源位置 = 场景中心 + 光方向 × `shadowSize * 4.0f`；近平面 `max(0.1, minZ - 5)`，远平面 `maxZ + 5`，含 5.0 填充。

### Bindless 纹理 SRV 槽位

- `Scene::CreateSRVHeap` 创建 1000 个描述符的全局 SRV 堆（`srvHeap`，shader visible）。
- `Scene::CreateTextureSRV` 固定布局：t0=SkyTexture（cubemap）、t1=BaseColor、t2=Normal、t3=Orm，t4-t9 预留（“跳过 t4-t9，为材质纹理预留槽位”），t10-t12 为材质 BaseColor/Normal/ORM 纹理。
- `AllocateBindlessSRVSlot / FreeBindlessSRVSlot / CreateBindlessTextureSRV` 提供从 t10 开始的 990 个 bindless 槽位分配（`BINDLESS_START_SLOT = 10`），由材质系统使用。

### 纹理异步加载

- `Scene::AsyncLoadTextures()` 用 `std::async(std::launch::async, &Scene::LoadTextures, this)` 启动后台加载；`LoadTextures` 依次加载 `Content/Texture/color.png`、`Content/Cubemap/cubemap.png`、`Content/Texture/normal.png`、`Content/Texture/orm.png`（PNG 无 DDS 时会先转 DDS：WIC 加载 → 生成 mip → BC3 压缩 → 保存 DDS）。
- `main.cpp` 中用 `Sleep(100)` 临时等待异步加载（源码注释承认“没有提供等待接口”），随后才 `Scene::Initialize`。

## 对外接口

`Scene` 主要公开接口（`Engine/public/Scene.h`）：

- Actor 管理：`CreateActor`、`LoadActorFromMeshFile`、`RemoveActor`、`GetActors`、`GetActorByName`。
- Level 管理：`LoadLevel`、`SaveLevel`。
- 相机：`GetCamera`、`SetCameraLookSpeed`、`SetCameraMoveSpeed`、`HandleInput`。
- 灯光/Skylight：`SetLightRotation`、`SetSkylightIntensity/Color`、`GetSkylightIntensity/Color`。
- 阴影：`SetShadowOrthoSize`、`SetShadowMode`（0=Hard,1=PCF,2=PCSS）、`SetShadowmapEnabled`。
- GI：`SetGIType`（0=Close,1=SSGI）、`GetGIType`。
- 渲染：`Initialize`、`Update`、`Render`、`CreateOffscreenRTs`、`ResizeRenderTargets`、`GetConstantBuffer`、`GetMotionVectorRT`、`ReturnSkyCube`。
- SRV：`CreateSRVHeap`、`CreateTextureSRV`、`UpdateTextureSRV`、`AllocateBindlessSRVSlot`、`FreeBindlessSRVSlot`、`CreateBindlessTextureSRV`、`GetGlobalSRVHeap`。
- TAA：`SetJitterOffset`、`GetJitterOffset`、`UpdatePreviousViewProjectionMatrix`。

`Actor` 主要公开接口（`Engine/public/Actor.h`）：`LoadFromMeshFile`、`Set/GetTransform`、`Set/GetPosition/Rotation/Scale`、`GetModelMatrix`、`GetMesh/SetMesh`、`GetMaterial/SetMaterial`、`CreateConstantBuffer`、`UpdateConstantBuffer`、`IsSelected/SetSelected`（编辑器用）。

## 配置与调参

| 参数 | 默认值 | 位置 |
| --- | --- | --- |
| 相机 FOV / near / far | 45°（弧度转换）/ 0.1 / 1000 | `Scene` 构造函数，`m_camera(...)` |
| 相机移动/旋转速度 | 50 / 0.005 | `Camera.cpp`；编辑器面板可调 |
| 初始灯光方向 | `(-1,-1,1)`（归一化） | `Scene` 构造函数 |
| Skylight 强度 | 1.0 | `Scene.h`，`m_skylightIntensity` |
| Skylight 颜色 | (1,1,1) | `Scene.h`，`m_skylightColor` |
| 阴影正交范围 | 20.0 | `Scene.h`，`m_shadowOrthoSize` |
| 阴影模式 | 2（PCSS） | `Scene.h`，`m_shadowMode` |
| 阴影分辨率 | 2048（硬编码，TODO） | `Scene.cpp` 第 911 行 |
| 阴影图实际分辨率 | 4096 | `main.cpp`：`new LightPass(..., 4096)` |
| GI 类型 | 0（Close） | `Scene.h`，`m_giType` |
| 离屏 RT 格式 | RT0-2: R16G16B16A16_FLOAT，RT3: R16G16_FLOAT | `Scene::CreateOffscreenRTs` |

编辑器面板（设置窗口 / 主光窗口）会把这些值通过 `Set*` 接口实时写入 `Scene`。

## 已知限制与 TODO

- `Scene.cpp` 第 911 行：`shadowMapResolution = 2048.0f` 硬编码，TODO 标注“从 LightPass 获取实际分辨率”，与 LightPass 的 4096 不一致。
- `CalculateLiSPSMMatrix` 未实现 LiSPSM，直接退化为标准正交阴影矩阵（`CalculateStandardShadowMatrix`）。
- `Actor::LoadFromMeshFile` 不加载 Transform；`.mesh` 中也没有 Transform 字段，Transform 只存在于 `.level`。
- `Actor::UpdateModelMatrix()` 是空实现（历史遗留）。
- `Actor` 析构不释放 `m_mesh`；`Scene::RemoveActor` 只 `delete` Actor，会导致 `StaticMeshComponent` 泄漏（mesh 由谁所有不明确）。
- `LoadLevel` 不清空已有 `m_actors`，重复加载会追加 Actor。
- `.level` 只保存 Actor（名称/网格/材质/Transform），不保存相机、灯光旋转、Skylight、阴影等场景级参数。
- 场景只有一个平行光，没有点光/聚光灯、没有光照组件化。
- 异步纹理加载与 CommandList 存在竞态风险（`main.cpp` 用 `Sleep(100)` 规避，注释承认无等待接口）。
- 旧式 `m_staticMesh` 单网格路径仍保留，与 Actor 系统并存。

## 维护注意事项

- **顶点布局同步**：`StaticMeshComponentVertexData` 是 Position/Texcoord/Normal/Tangent 各 4 分量；修改它必须同步 `CreateScenePSO`（`BattleFireDirect.cpp`）与 `CreateUiPSO`（`ImguiPass.cpp`）的 input layout，以及 `.shader` 中 `struct VertexData`。
- **CB 布局同步**：`SceneCBData`（176 floats，见 `BattleFireDirect.h`）被 `Scene::Update`、`Actor::UpdateConstantBuffer`、`FillSceneCBData` 以及 `ShaderParser::GenerateHLSLCode` 生成的 HLSL 共用，任何字段增删都要同步这几处，并保持 16 字节对齐。
- 新增网格资产时先创建 `.mesh`（引用相对 `Engine/` 的 FBX 路径），再在 `Actor Creator` 或 `.level` 中引用；路径统一用 `PathUtils` 的 `GetEnginePath()` 拼接，不要写死绝对路径。
- FBX 导入使用 eByPolygonVertex 逐多边形顶点展开（无顶点索引复用优化），大模型会产生较多顶点；调整导入逻辑时注意 `polyVertIndices` 与 fan 三角化的对应关系。
- 修改阴影矩阵时同时检查两处分辨率：`Scene::CalculateStandardShadowMatrix`（纹素对齐）与 `LightPass` 阴影图尺寸。