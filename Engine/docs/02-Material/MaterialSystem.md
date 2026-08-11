# 材质系统总览（MaterialSystem）

> 本文依据 `Engine/private/Material/`、`Engine/public/Material/` 实际源码编写，说明材质资产格式、加载流程、MaterialInstance/MaterialManager 职责，以及材质系统与 ShaderSystem 的关系。配套阅读：`02-Material/ShaderSystem.md`、`02-Material/ShadingModels.md`、`02-Material/HOW_TO_USE_MATERIAL_SHADER.md`、`02-Material/MATERIAL_SYSTEM_INTEGRATION.md`。

## 概述

材质系统负责三件事：

1. **Shader 资产管理**：加载 `.shader`（Unity ShaderLab 风格）或旧的 `.shader.ast` XML，解析参数、生成 HLSL、编译 VS/PS、创建 PSO（详见 ShaderSystem.md）。
2. **材质实例管理**：`.material` / `.ast` 资产保存材质参数（颜色、粗糙度、金属度、纹理引用），运行时由 `MaterialInstance` 打包进 256 字节常量缓冲区（b1）并绑定到渲染管线。
3. **编辑与调试**：`MaterialEditorPanel`（ImGui）提供材质选择、参数实时调整、保存/加载、应用到 Mesh 的完整工作流。

核心类共 5 个：`MaterialManager`（单例）、`MaterialInstance`、`Shader`、`ShaderParameter`（+ `ShaderParser`）、`MaterialEditorPanel`。纹理加载复用 `TextureManager`（见 03-Texture/TextureSystem.md）。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/public/Material/MaterialManager.h` / `Engine/private/Material/MaterialManager.cpp` | 管理器单例：shader/材质缓存、加载/保存、纹理中转 |
| `Engine/public/Material/MaterialInstance.h` / `Engine/private/Material/MaterialInstance.cpp` | 材质实例：CPU 参数表、常量缓冲区打包、GPU 绑定、Bindless 纹理索引 |
| `Engine/public/Material/Shader.h` / `Engine/private/Material/Shader.cpp` | Shader 资产：XML/Unity 风格解析、HLSL 生成、D3DCompile、PSO 创建、ShadingModel 注册与 Screen 注入 |
| `Engine/public/Material/ShaderParameter.h` | 参数类型枚举（Float/Vector2/3/4/Int/Bool/Texture2D/TextureCube/Matrix4x4）与字节大小、默认值 union |
| `Engine/public/Material/ShaderParser.h` / `Engine/private/Material/ShaderParser.cpp` | `.shader` 解析器与 HLSL 代码生成器 |
| `Engine/public/Material/MaterialEditorPanel.h` / `Engine/private/Material/MaterialEditorPanel.cpp` | ImGui 材质编辑器面板 |
| `Engine/private/BattleFireDirect.cpp` | 根签名：slot0=b0 场景 CB、slot1=Bindless SRV 表、slot2=b1 材质 CB |
| `Engine/private/Scene.cpp` | Bindless SRV 槽位分配（t10 起 990 槽）与 Render 时材质绑定 |
| `Engine/private/StaticMeshComponent.cpp` | 持有 `MaterialInstance*`，渲染时绑定到槽位 2 |
| `Engine/main.cpp` | 启动流程：初始化、扫描、加载 shader/材质、分配默认材质 |
| `Engine/Shader/StandardPBR.shader`、`Engine/Shader/ToonPBR.shader` | Unity 风格 shader 资产（Deferred） |
| `Content/Materials/DefaultPBR.ast`、`BlueRough.material`、`RedMetal.material`、`toonmaterial.material` | 材质实例资产样例 |

## 架构与数据流

### 启动时序（main.cpp）

1. `MaterialManager::GetInstance().Initialize(gD3D12Device)`（main.cpp 约 240 行），随后初始化 `TextureManager` 与 `TexturePreviewPanel`。
2. `InitRootSignature()` 之后调用 `MaterialManager::GetInstance().SetRootSignature(rootSignature)`，供后续按需创建 PSO。
3. `ResourceManager::ScanAndLoadAllResources()` 扫描 `Engine/Shader/*.shader`（排除 `screen.shader` 与 `Sky.shader`）与 `Content/Materials/*.material`。
4. 步骤 2：循环加载非 `StandardPBR` 的 shader，逐个 `CompileShaders()` 并为每个 Pass `CreatePSO()`（此时 ToonPBR 的 ShadingModel 会被注册）。
5. 步骤 3：加载 `StandardPBR.shader`，Pass 0 创建 `gbufferPso`（BasePass 使用），Pass 1 创建 `deferredLightingPso`（ScreenPass 使用）。
6. 步骤 4：遍历 `GetAllMaterialResources()` 调用 `LoadMaterial()`；再显式加载 `Content/Materials/DefaultPBR.ast` 作为默认材质，`g_scene->GetStaticMesh()->SetMaterial(defaultMaterial)`。
7. 渲染循环：BasePass 使用 `gbufferPso`，`g_scene->Render()` 内部由 `StaticMeshComponent::Render` 绑定材质；ScreenPass 使用 `deferredLightingPso`。

### 材质加载流程（LoadMaterial）

```
LoadMaterial(path)
 ├─ 预解析 XML（MSXML）：<Material name> 与 <Shader> 引用名
 ├─ GetShader(shaderName)；未缓存则 LoadShader(L"Engine/Shader/<name>.shader")
 ├─ new MaterialInstance(name, shader)
 │   ├─ 分配 CPU 缓冲区（shader->GetConstantBufferSize() 字节）
 │   └─ InitializeDefaultParameters()：从 Shader 参数默认值填充各参数表
 ├─ material->LoadFromXML(path)：覆盖 Parameters 与 Textures
 ├─ material->Initialize(device)：CreateConstantBufferObject(256) + Map + 初始 UpdateConstantBuffer
 └─ 若全局 gCommandList 可用：LoadTexturesFromPaths() 加载纹理并分配 Bindless SRV
```

### 渲染数据流

```
BasePass: commandList->Reset(commandAllocator, gbufferPso)
  └─ g_scene->Render(commandList, gbufferPso, rootSignature)
       └─ StaticMeshComponent::Render
            ├─ 若有待加载纹理：material->LoadTexturesFromPaths(commandList)
            └─ material->Bind(commandList, rootSignature, 2)  // slot 2 = b1
                 ├─ dirty 则 PackConstantBuffer + memcpy 到映射内存
                 └─ SetGraphicsRootConstantBufferView(2, cb->GetGPUVirtualAddress())
```

材质常量缓冲布局（生成后 HLSL，见 `Engine/Shader/Shader_Cache/StandardPBR_GBufferPass.hlsl`）：

| 成员 | 类型 | Offset |
| --- | --- | --- |
| `BaseColor` | float4 | 0 |
| `Roughness` | float | 16 |
| `Metallic` | float | 20 |
| `BaseColorTexIndex` / `NormalTexIndex` / `OrmTexIndex` | uint | 24 / 28 / 32 |
| `_Padding[13]` | float4 | 36（声明至 244，编译器补齐到 256） |

## 关键实现要点

### 资产格式：.material / .ast（UTF-8 XML）

```xml
<?xml version="1.0" encoding="utf-8"?>
<Material name="DefaultPBR">
  <Shader>StandardPBR</Shader>
  <Parameters>
    <Parameter name="BaseColor" type="Vector4">1 1 1 1</Parameter>
    <Parameter name="Roughness" type="Float">0.5</Parameter>
    <Parameter name="Metallic" type="Float">0</Parameter>
  </Parameters>
  <Textures>
    <Texture name="BaseColorTex" slot="t10">Content/Texture/color.texture.ast</Texture>
    <Texture name="NormalTex" slot="t11">Content/Texture/normal.texture.ast</Texture>
    <Texture name="OrmTex" slot="t12">Content/Texture/orm.texture.ast</Texture>
  </Textures>
</Material>
```

- 解析使用 MSXML6（`IXMLDOMDocument2` / `DOMDocument60`），`LoadFromXML` 与 `SaveToXML` 成对实现。
- 纹理路径统一以 UTF-8 字符串存储，读写时经 `MultiByteToWideChar` / `WideCharToMultiByte(CP_UTF8)` 转换。
- `ResourceManager` 只扫描 `.material` 后缀；`.ast` 文件（如 `DefaultPBR.ast`）在 main.cpp 中显式按路径加载。

### CPU 参数存储与脏标记

`MaterialInstance` 用 6 个 map 存参数：`m_floatParams` / `m_vectorParams`(XMFLOAT4) / `m_vector3Params`(XMFLOAT3) / `m_intParams` / `m_boolParams` / `m_textureParams`(路径)。所有 `SetXxx` 都会置 `m_isDirty = true`；纹理路径变更后由 `LoadTexturesFromPaths` 延迟加载。

### 常量缓冲区打包（PackConstantBuffer）

按 `ShaderParameter.byteOffset` 把各参数写入 CPU 缓冲区；纹理参数写入 `m_textureSRVIndices` 中的 **Bindless SRV 索引（uint）**，默认 0（约定 0 号槽为默认纹理）。`UpdateConstantBuffer()` 一次性 `memcpy` 到持久映射的 upload 堆。

### Bindless 纹理绑定

`LoadTexturesFromPaths` 流程：

1. `TextureManager::LoadTexture(texPath)` 得到 `TextureAsset`（GPU 资源 + SRV）。
2. `Scene::AllocateBindlessSRVSlot()` 分配全局槽位（从 t10 开始，共 990 个，t0-t9 保留给场景纹理）。
3. `Scene::CreateBindlessTextureSRV(srvIndex, resource)` 在场景 SRV 堆创建 SRV。
4. `SetTextureSRVIndex(name, srvIndex - 10)`：shader 中 `g_BindlessTextures[1024]` 从 t10 起，故存相对索引。

根签名的 slot1 是 `NumDescriptors = UINT_MAX` 的无界描述符表（`D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE`），对应 HLSL 中的 `Texture2D g_BindlessTextures[1024] : register(t10)`。

### 材质编辑器（MaterialEditorPanel）

`RenderUI()` 组织为：材质下拉选择器（选中即 `OnApplyToMesh`）→ 参数编辑器（Float 走 Slider、Vector 走 ColorPicker/Drag，Int/Bool 走 Input/Checkbox）→ 纹理槽位（`RenderTextureSlots`）→ 控制按钮（保存/加载/新建/应用）。`OnApplyToMesh()` 通过 `SetScene` / `SetTargetActor` 把材质写回 Mesh 或 Actor。

## 对外接口

### MaterialManager（单例）

| 接口 | 说明 |
| --- | --- |
| `Initialize(ID3D12Device*)` / `Shutdown()` | 初始化/释放全部 shader、材质与纹理缓存 |
| `SetRootSignature(...)` / `GetRootSignature()` | 供 PSO 创建 |
| `LoadShader(const std::wstring&)` | 按扩展名分发：`.shader.ast` → `LoadFromXML`，`.shader` → `LoadFromShaderFile`；按文件名缓存 |
| `GetShader(name)` / `GetAllShaderNames()` / `ClearShaderCache(name)` | 查询与缓存管理 |
| `CompileAndCreateAllShadersPSO()` | 两阶段：先编译非 Screen（注册 ShadingModel），后编译 Screen |
| `LoadMaterial(path)` / `ReloadMaterial(path)` / `CreateMaterial(name, shader)` / `GetMaterial(name)` / `SaveMaterial(mat, path)` | 材质生命周期与持久化 |
| `LoadTexture(path)` | 中转 `TextureManager` 加载 `.texture.ast` / png / dds |

### MaterialInstance

- 参数：`SetFloat/SetVector/SetVector3/SetInt/SetBool/SetTexture` 与对应 `GetXxx`。
- 纹理：`SetTextureResource(name, res, slot)`（旧固定槽方式）、`SetTextureSRVIndex(name, index)`（Bindless 方式）。
- GPU：`Initialize(device)`、`UpdateConstantBuffer()`、`Bind(commandList, rootSig, materialCBSlot)`、`BindTextures(device, srvHeap, descriptorSize)`（兼容旧路径）。
- 状态：`MarkDirty()/IsDirty()`、`MarkTexturesDirty()/IsTexturesDirty()`、`HasPendingTextures()`、`LoadTexturesFromPaths(commandList)`。

## 配置与调参

- **绑定槽位**：`Bind(..., 2)` 在 `StaticMeshComponent::Render` 与 `Scene::Render` 中硬编码为 slot 2，对应根签名 b1；如需改槽位需同时改 `InitRootSignature()` 与绑定处。
- **CB 大小**：固定 256 字节（`Shader::CalculateConstantBufferSize` 按 256 对齐），生成 HLSL 时不足 256 用 `_Padding` 补足；新增参数超过 256 字节需同步改生成逻辑。
- **默认值**：`InitializeDefaultParameters()` 从 shader `Properties` 的 `default(...)` 读取；运行时 `LoadFromXML` 覆盖。
- **纹理默认路径**：旧 XML（`StandardPBR.material`）在 `<Default>` 中写 `Content/Texture/Default/White.dds`；新 `.shader` 的纹理属性无默认路径，由编辑器指定。
- **场景级 Skylight**：`SkylightIntensity` 已不是材质参数（main.cpp 注释明确），改为 `g_scene->SetSkylightIntensity(1.0f)` 走场景 CB。

## 已知限制与TODO

- 材质文件扩展名不统一：`.material` 与 `.ast` 并存，`ResourceManager` 只扫描前者，`.ast` 需要显式路径加载。
- 编辑器参数控件只完整实现 Float / Vector3 / Vector4 / Int / Bool / Texture；`Vector2`、`Matrix4x4` 无专属控件。
- `LoadTexturesFromPaths` 依赖全局 `gCommandList`（extern 变量）的时序：若为 NULL 则纹理延迟到渲染帧内补加载。
- `MaterialManager::m_textures` 缓存不增加引用计数，资源生命周期由 `TextureManager` 负责，Shutdown 顺序敏感（先材质后纹理）。
- 每个 Actor 每帧单独 `SetGraphicsRootConstantBufferView` 绑定，未做材质合批。
- `RenderQueue` 目前只有 `"Deferred"` / `"Forward"` 两个取值，`IsDeferredShader()` 以字符串比较判断。

## 维护注意事项

- 所有资产文件必须 UTF-8 编码（`HOW_TO_USE_MATERIAL_SHADER.md` 的 FAQ 明确 XML 解析失败先查编码）。
- MSXML 的 `CoInitialize`/`CoUninitialize` 在 `LoadFromXML`、`SaveToXML`、`LoadMaterial` 中成对出现，修改时保持配对，避免 COM 状态泄漏。
- 修改 shader 的 `Properties` 后，材质 CB 布局由生成器自动重建；但旧 `.shader.ast` XML 里的 `offset` 是手写的，两者不能混改。
- 新增材质资产放入 `Content/Materials/`（会被扫描）并沿用 `<Shader>` 引用名；shader 必须先于材质加载（LoadMaterial 找不到时会尝试 `Engine/Shader/<name>.shader`）。
- 编辑器中「保存」调用 `SaveMaterial → SaveToXML`，会覆盖原文件，保存前确认路径。
- 调试日志：`Engine/Shader/Shader_Cache/log/material_load.txt` 与 `<材质名>_error.txt`；生成 HLSL 在 `Engine/Shader/Shader_Cache/<Shader>_<Pass>.hlsl`，排查绑定问题时先看这两个产物。
