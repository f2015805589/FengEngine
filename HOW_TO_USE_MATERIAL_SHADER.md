# 如何使用新的材质Shader系统

## 修复内容

### 1. 常量缓冲区更新问题
修复了 `UpdateConstantBuffer` 函数中 `Unmap` 参数错误：
```cpp
// 修复前（错误）
inCB->Unmap(0, &writeRange); // writeRange可能导致错误

// 修复后（正确）
inCB->Unmap(0, nullptr); // nullptr表示整个映射范围都被写入
```

### 2. 渲染队列支持
shader现在支持 `RenderQueue` 标识：
- `"Deferred"` - 延迟渲染（在BasePass中使用）
- `"Forward"` - 前向渲染

## 在main.cpp中集成材质系统

### 步骤1：初始化MaterialManager并加载Shader

在 `main.cpp` 初始化部分（大约在line 100-180之间），添加：

```cpp
// 初始化MaterialManager
MaterialManager::GetInstance().Initialize(gD3D12Device);

// 加载StandardPBR shader
Shader* pbrShader = MaterialManager::GetInstance()
    .LoadShader(L"Content/Shader/StandardPBR.shader");

if (!pbrShader) {
    std::cout << "Failed to load StandardPBR shader!" << std::endl;
    return 1;
}

// 检查shader类型
if (pbrShader->IsDeferredShader()) {
    std::cout << "StandardPBR is a deferred shader" << std::endl;
} else {
    std::cout << "StandardPBR is a forward shader" << std::endl;
}
```

### 步骤2：创建材质实例

```cpp
// 创建默认PBR材质
MaterialInstance* defaultMaterial = MaterialManager::GetInstance()
    .CreateMaterial("DefaultPBR", pbrShader);

if (!defaultMaterial) {
    std::cout << "Failed to create material!" << std::endl;
    return 1;
}

// 设置材质参数
defaultMaterial->SetVector("BaseColor", XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f));
defaultMaterial->SetFloat("Roughness", 0.5f);
defaultMaterial->SetFloat("Metallic", 0.0f);

// 设置纹理（如果有的话）
// defaultMaterial->SetTexture("BaseColorTex", L"Content/Texture/brick_albedo.dds");
// defaultMaterial->SetTexture("NormalTex", L"Content/Texture/brick_normal.dds");
// defaultMaterial->SetTexture("OrmTex", L"Content/Texture/brick_orm.dds");
```

### 步骤3：为Shader创建PSO

```cpp
// 为材质shader创建PSO（延迟渲染，与BasePso相同的配置）
ID3D12PipelineState* materialPso = pbrShader->CreatePSO(gD3D12Device, rootSignature);
if (!materialPso) {
    std::cout << "Failed to create material PSO!" << std::endl;
    return 1;
}
```

### 步骤4：将材质分配给Mesh

```cpp
// 获取场景中的mesh并分配材质
if (g_scene && g_scene->GetStaticMesh()) {
    g_scene->GetStaticMesh()->SetMaterial(defaultMaterial);
    std::cout << "Material assigned to mesh" << std::endl;
}
```

### 步骤5：在渲染循环中使用材质Shader

在 `main.cpp` 的渲染循环中（BasePass部分），修改为使用材质的PSO：

```cpp
// BasePass - 使用材质shader
commandList->Reset(commandAllocator, materialPso); // 使用材质的PSO

DWORD current_time = timeGetTime();
float deltaTime = (current_time - last_time) / 1000.0f;
last_time = current_time;
g_scene->Update(deltaTime);

commandList->BeginEvent(0, L"BasePass", (UINT)(wcslen(L"BasePass")* sizeof(wchar_t)));
BeginOffscreen(commandList);
ID3D12DescriptorHeap* srvHeaps[] = { srvHeap};
commandList->SetDescriptorHeaps(_countof(srvHeaps), srvHeaps);
g_scene->Render(commandList, materialPso, rootSignature);
commandList->EndEvent();
EndCommandList();
```

## 材质参数说明

StandardPBR.shader 支持以下参数：

### 材质常量（自动生成到 b1）
- `BaseColor` (float4): 基础颜色，默认 (1, 1, 1, 1)
- `Roughness` (float): 粗糙度，范围 0.0-1.0，默认 0.5
- `Metallic` (float): 金属度，范围 0.0-1.0，默认 0.0
- `SkylightIntensity` (float): 环境光强度，范围 0.0-5.0，默认 1.0

### 纹理（自动分配到 t10, t11, t12）
- `BaseColorTex` (Texture2D): 基础颜色贴图，寄存器 t10
- `NormalTex` (Texture2D): 法线贴图，寄存器 t11
- `OrmTex` (Texture2D): ORM贴图，寄存器 t12
  - R通道：Occlusion（环境光遮蔽）
  - G通道：Roughness（粗糙度）
  - B通道：Metallic（金属度）

## PBR实现细节

StandardPBR.shader 使用业界标准的PBR（物理基础渲染）实现：

### 直接光照（Cook-Torrance BRDF）
- **D项**：GGX法线分布函数（DistributionGGX）
- **F项**：Fresnel-Schlick近似（FresnelSchlick）
- **G项**：Smith-GGX几何遮蔽函数（GeometrySmith）

### 间接光照（IBL - Image Based Lighting）
- **漫反射IBL**：使用法线方向采样环境cubemap
- **镜面反射IBL**：使用反射方向采样环境cubemap，根据粗糙度选择mip级别
- **环境光遮蔽**：使用ORM纹理的R通道
- **SkylightIntensity**：控制环境光的整体强度（0-5倍）

### 能量守恒
- 漫反射 + 镜面反射 = 1（kD + kS = 1）
- 金属材质没有漫反射（metallic = 1时，kD = 0）

## GBuffer输出格式

StandardPBR.shader的GBuffer输出与默认shader完全一致：

| 渲染目标 | 内容 | 格式 |
|---------|------|------|
| RT0 | `outAlbedo` | BaseColor (RGB) + Alpha |
| RT1 | `outNormal` | World Space Normal (RGB) + 1.0 |
| RT2 | `outSpecular` | ORM (Occlusion, Roughness, Metallic) |
| RT3 | `position` | World Position (XYZ) + 1.0 |

这确保了材质shader能够无缝集成到现有的延迟渲染管线中。

## 示例：完整初始化代码

```cpp
// 在main.cpp的初始化部分添加：

// 1. 初始化MaterialManager
MaterialManager::GetInstance().Initialize(gD3D12Device);

// 2. 加载shader
Shader* pbrShader = MaterialManager::GetInstance()
    .LoadShader(L"Content/Shader/StandardPBR.shader");

// 3. 创建PSO
ID3D12PipelineState* materialPso = pbrShader->CreatePSO(gD3D12Device, rootSignature);

// 4. 创建材质
MaterialInstance* defaultMaterial = MaterialManager::GetInstance()
    .CreateMaterial("DefaultPBR", pbrShader);

// 5. 设置材质参数
defaultMaterial->SetVector("BaseColor", XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f));
defaultMaterial->SetFloat("Roughness", 0.7f);
defaultMaterial->SetFloat("Metallic", 0.3f);
defaultMaterial->SetFloat("SkylightIntensity", 1.0f);  // 环境光强度

// 6. 分配给mesh
if (g_scene && g_scene->GetStaticMesh()) {
    g_scene->GetStaticMesh()->SetMaterial(defaultMaterial);
}

// 7. 在渲染循环中使用
// 材质shader现在是默认使用的，无需切换
commandList->Reset(commandAllocator, materialPso);
```

## 材质shader已默认启用

从最新版本开始，StandardPBR材质shader已经是默认启用的，不需要手动切换。原来的BasePso已经被替换为materialPso。

## 调试技巧

1. **检查shader是否加载成功**：
```cpp
if (pbrShader) {
    std::cout << "Shader Name: " << pbrShader->GetName() << std::endl;
    std::cout << "RenderQueue: " << pbrShader->GetRenderQueue() << std::endl;
    std::cout << "CB Size: " << pbrShader->GetConstantBufferSize() << std::endl;
}
```

2. **检查材质参数**：
```cpp
const auto& params = pbrShader->GetParameters();
for (const auto& param : params) {
    std::cout << "Param: " << param.name
              << ", Offset: " << param.byteOffset
              << ", Size: " << param.byteSize << std::endl;
}
```

3. **在材质编辑器中实时调整**：
   - 确保 `showMaterialEditor = true`
   - 在UI中选择材质并调整参数
   - 参数会实时更新到GPU

## 常见问题

### Q: 编译错误"找不到纹理"
A: 确保纹理已经通过ShaderParser自动声明，或者在HLSL中手动声明在正确的寄存器（t10+）

### Q: 常量缓冲区更新失败
A: 已修复 `UpdateConstantBuffer` 中的 `Unmap` 参数问题，使用nullptr而不是writeRange

### Q: 渲染结果与原shader不一致
A: 检查GBuffer输出格式是否匹配，StandardPBR.shader已经对齐到与ndctriangle.hlsl相同的格式

### Q: 如何添加新的材质参数
A: 在StandardPBR.shader的Properties块中添加：
```hlsl
//# float MyParam {default(1.0), min(0.0), max(2.0), ui(Slider)};
```
系统会自动生成常量缓冲区声明和材质参数绑定
