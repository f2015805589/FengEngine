# IBL — 基于图像的光照资源系统

> 本文介绍 FEngine 的 IBL（Image-Based Lighting）资源体系：`IBLResources`（BRDF LUT / Irradiance / Prefiltered Environment Map 的 Compute Shader 生成）、`SphericalHarmonics`（3 阶球谐，仅头文件、尚未实现）以及 `Engine/Shader/IBL/` 与 `SHCalculation.hlsl`。**当前状态：IBL 资源尚未接入渲染管线**，运行时实际使用天空盒（SkyCube）mip 近似做环境光。

## 概述

FEngine 的 IBL 系统遵循经典的 PBR 拆分式（split-sum）方案：

- **漫反射 IBL**：对环境立方体贴图做余弦加权卷积，得到低分辨率辐照度贴图（Irradiance Map），按法线方向采样。
- **镜面反射 IBL**：对 GGX 重要性采样预过滤的环境贴图（Prefiltered Environment Map，多 mip 对应不同粗糙度），配合 BRDF 积分查找表（BRDF LUT）使用。

对应代码中的三个产物：

| 产物 | 常量 | 格式 | Shader |
| --- | --- | --- | --- |
| BRDF LUT | `BRDF_LUT_SIZE = 512` | `R16G16_FLOAT` 2D | `Shader/IBL/BRDFIntegration.hlsl` |
| Irradiance Map | `IRRADIANCE_SIZE = 32` | `R16G16B16A16_FLOAT` Cube（单 mip） | `Shader/IBL/IrradianceConvolution.hlsl` |
| Prefiltered Map | `PREFILTER_SIZE = 128`，`PREFILTER_MIP_LEVELS = 5` | `R16G16B16A16_FLOAT` Cube（5 mip） | `Shader/IBL/PrefilterEnvMap.hlsl` |

另外还有一套**球谐方案**：`SphericalHarmonics`（CPU 端从 cubemap 计算 3 阶 SH 系数，9 个 float3，通过 `SceneCBData` 传给 shader）以及对应的 GPU 计算着色器 `SHCalculation.hlsl`。当前这两者都没有实现/接入。

**接入现状**：延迟光照 `Generated_DeferredLighting.hlsl` 中 IBL 部分直接采样 `SkyCube`（`register(t4)`）——以 `roughness * 10` 作为 mip 做镜面预过滤近似、以 `maxMipLevel`（10）做辐照度近似，不使用 `IBLResources` 生成的纹理。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/public/IBLResources.h` | `IBLResources` 类声明、尺寸常量、资源访问接口 |
| `Engine/private/IBLResources.cpp` | 计算着色器编译、计算根签名、三张纹理的生成流程 |
| `Engine/public/SphericalHarmonics.h` | `SphericalHarmonics` 类声明（仅有头文件，无 .cpp 实现） |
| `Engine/Shader/IBL/BRDFIntegration.hlsl` | BRDF LUT 生成 CS（Hammersley + GGX 重要性采样，1024 样本） |
| `Engine/Shader/IBL/IrradianceConvolution.hlsl` | 辐照度卷积 CS（余弦加权，`RWTexture2DArray` 输出 6 面） |
| `Engine/Shader/IBL/PrefilterEnvMap.hlsl` | 预过滤环境贴图 CS（按 PDF 计算 mip 以减少 artifact） |
| `Engine/Shader/SHCalculation.hlsl` | 3 阶 SH 系数计算 CS（含 TODO 原子累加） |
| `Engine/Shader/Generated_DeferredLighting.hlsl` | 当前实际使用的 IBL 近似（SkyCube mip） |
| `Engine/Shader/screen.hlsl`、`Engine/Shader/Shadingmodel/Screen.shader` | 延迟光照另一实现（同样基于 SkyCube） |

## 架构与数据流

### 设计意图（目标数据流）

```text
环境 Cubemap（Scene 的 SkyTexture）
    │
    ├──> IBLResources::Initialize(commandList, environmentCubemap, rootSignature)
    │       ├── 1. CompileComputeShaders（三个 .hlsl → cs_5_0 bytecode）
    │       ├── 2. CreateComputeRootSignature（CBV b0 + SRV t0 + UAV u0 + Sampler s0）
    │       ├── 3. CreateSRVHeap / UAV Heap
    │       ├── 4. CreateBRDFLUT（Dispatch → UAV → SRV）
    │       ├── 5. CreateIrradianceMap（对 cubemap 卷积，32×32×6）
    │       └── 6. CreatePrefilteredMap（GGX 预过滤，128×128×6，5 mips）
    │
    └──> SphericalHarmonics::ComputeFromCubemap（CPU 读回，9 个 SH 系数）──> SceneCBData

延迟光照 Pass：
    ├── diffuse IBL  = IrradianceMap.Sample(normal)（或 SH 重建）
    ├── specular IBL = PrefilteredMap.SampleLevel(reflect, roughness→mip) * BRDFLUT.Sample(NdotV, roughness)
    └── ambient = (diffuseIBL + specularIBL) * Skylight * SkylightColor * ao
```

### 当前实际数据流

```text
Scene::LoadTextures ──> "SkyTexture"（Content/Cubemap/cubemap.png → DDS，cubemap）
    └──> m_skyTexture（Scene::ReturnSkyCube）
         ├──> SkyPass（天空球绘制）
         └──> Generated_DeferredLighting.hlsl（ScreenPass 的 PS）
              ├── mipLevel = roughness * maxMipLevel(10)
              ├── prefilteredColor = SkyCube.SampleLevel(gSamAnisotropicClamp, R, mipLevel)
              ├── irradiance     = SkyCube.SampleLevel(gSamAnisotropicClamp, normal, 10)
              └── ambient = (kD * irradiance * baseColor + prefilteredColor * F)
                            * Skylight * SkylightColor * ao
```

`IBLResources` 与 `SphericalHarmonics` 目前**没有任何调用方**（全仓库搜索仅命中自身头文件/实现文件），属于“已实现一半、待接入”的状态。

## 关键实现要点

### IBLResources::Initialize 六步流程（IBLResources.cpp）

1. **CompileComputeShaders**：用 `D3DCompileFromFile` 分别编译 `BRDFIntegration.hlsl`、`IrradianceConvolution.hlsl`、`PrefilterEnvMap.hlsl`，入口 `CSMain`，目标 `cs_5_0`，编译标志 `D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION`；失败时把 error blob 打印到 `std::cout` 并返回 false。
2. **CreateComputeRootSignature**：4 个根参数 —— 0: CBV(b0)（预过滤参数），1: SRV 表(t0)（环境 cubemap），2: UAV 表(u0)（输出纹理），3: Sampler 表(s0)；用 `D3DX12SerializeVersionedRootSignature`（版本 1.1）序列化并创建；随后用三个 CS bytecode 创建 `m_brdfPSO / m_irradiancePSO / m_prefilterPSO`。
3. **CreateSRVHeap**：创建 10 个描述符的 shader-visible SRV 堆（槽 0/1/2 分别放三张纹理）与 10 个描述符的 UAV 堆（计算输出）。
4. **CreateBRDFLUT**（完整实现）：
   - 创建 512×512 `R16G16_FLOAT`、`ALLOW_UNORDERED_ACCESS` 纹理，初始状态 `UNORDERED_ACCESS`；
   - 绑定 `m_brdfPSO` + 计算根签名，设置 UAV 堆，`Dispatch(512/16, 512/16, 1)`；
   - UAV → `PIXEL_SHADER_RESOURCE` 屏障后创建 SRV（槽 0）。
   - `BRDFIntegration.hlsl` 核心：`RadicalInverse_VdC` + `Hammersley` 生成低差异序列，`ImportanceSampleGGX` 采样，`GeometrySchlickGGX_IBL`（k = a²/2 的 IBL 变体），`IntegrateBRDF` 累加 1024 次得到 `float2(A, B)`（scale/bias）。
5. **CreateIrradianceMap**（**未完成**）：只创建 32×32×6 `R16G16B16A16_FLOAT` 立方体纹理（UAV → SRV 屏障 + SRV 槽 1）。源码注释明确：“这里简化处理：跳过实际的计算着色器执行”。`IrradianceConvolution.hlsl` 的卷积逻辑（球面坐标 64×64 采样、余弦加权、`irradiance = PI * irradiance / sampleCount`）已写好但从未被 dispatch。
6. **CreatePrefilteredMap**（**未完成**）：只创建 128×128×6、5 mip 的立方体纹理（UAV → SRV 屏障 + SRV 槽 2），同样跳过计算着色器。`PrefilterEnvMap.hlsl` 已实现：按 `Roughness`（cbuffer `PrefilterParams` b0）做 GGX 重要性采样，用 PDF 计算采样 mip（`0.5 * log2(saSample / saTexel)`，源分辨率硬编码 512）。

### SphericalHarmonics（仅声明，未实现）

- `Engine/public/SphericalHarmonics.h` 声明了 `Initialize(ID3D12Device*)`、`ComputeFromCubemap(commandList, cubemap, size)`（CPU 读回像素算系数）、`GetSHCoefficients()`（9 个 float3）、`EvaluateSH(direction)`；
- **没有对应的 `.cpp` 文件**，也没有任何调用方；注释描述的设计是“CPU 端从 cubemap 计算 3 阶 SH 系数（9 个 float3），通过 SceneCBData 传递给 shader”。

### SHCalculation.hlsl（GPU 版 SH，含 TODO）

- `EvaluateSHBasis` 实现 3 阶（l=0..2）基函数，系数为标准常量（0.282095 / 0.488603 / 1.092548 / 0.315392 / 0.546274）；
- `CSMain`（8×8×1）按 `dispatchThreadID.z` 处理 6 个面，用立体角权重 `4/(sqrt(temp)*temp)` 校正 cubemap 采样变形，累加 `SHCoefficients[i] += contribution`；
- **注释明确 TODO**：“需要原子累加，暂时使用简化版本”——直接对 `RWStructuredBuffer` 做非原子累加，多线程写同一地址在 D3D12 中是未定义行为；
- `CSNormalize`（9×1×1）用 `4*PI/(size²*6)` 归一化。

### 当前渲染侧近似（Generated_DeferredLighting.hlsl 第 159-179 行）

```hlsl
float maxMipLevel = 10.0;
float mipLevel = roughness * maxMipLevel;
float3 prefilteredColor = SkyCube.SampleLevel(gSamAnisotropicClamp, R, mipLevel).xyz;
float3 irradiance = SkyCube.SampleLevel(gSamAnisotropicClamp, normal.xyz, maxMipLevel).xyz;

float3 kS = F;
float3 kD = (1.0 - kS) * (1.0 - metallic);
float3 diffuseIBL = kD * irradiance * baseColor.xyz;
float3 specularIBL = prefilteredColor * F;
float3 ambient = (diffuseIBL + specularIBL) * Skylight * SkylightColor * ao;
```

`screen.hlsl` / `Shadingmodel/Screen.shader` 中有类似逻辑（`mip = (1 - orm.y) * 10` 或 `roughness→mip`）。这套近似依赖天空盒自身 mip 链，粗糙度越高采样的 mip 越模糊，视觉上是“免费的 IBL”，但无法做到物理正确的预过滤与能量守恒。

## 对外接口

`IBLResources`（`Engine/public/IBLResources.h`）：

- `bool Initialize(ID3D12GraphicsCommandList* commandList, ID3D12Resource* environmentCubemap, ID3D12RootSignature* rootSignature)`；
- `ID3D12Resource* GetBRDFLUT()`、`GetIrradianceMap()`、`GetPrefilteredMap()`；
- `ID3D12DescriptorHeap* GetSRVHeap()`；
- 常量：`BRDF_LUT_SIZE=512`、`IRRADIANCE_SIZE=32`、`PREFILTER_SIZE=128`、`PREFILTER_MIP_LEVELS=5`。

`SphericalHarmonics`（`Engine/public/SphericalHarmonics.h`，未实现）：

- `Initialize(ID3D12Device*)`、`ComputeFromCubemap(...)`、`GetSHCoefficients()`、`EvaluateSH(direction)`。

## 配置与调参

| 项 | 值 | 位置 |
| --- | --- | --- |
| BRDF LUT 尺寸 | 512×512 | `IBLResources.h`，`BRDF_LUT_SIZE` |
| BRDF LUT 采样数 | 1024 / 像素 | `BRDFIntegration.hlsl`，`SAMPLE_COUNT` |
| 辐照度贴图尺寸 | 32×32×6 | `IBLResources.h`，`IRRADIANCE_SIZE` |
| 辐照度卷积采样 | 64×64 球面采样（phi/theta） | `IrradianceConvolution.hlsl` |
| 预过滤贴图尺寸 | 128×128×6，5 mip | `IBLResources.h`，`PREFILTER_SIZE` / `PREFILTER_MIP_LEVELS` |
| 预过滤采样数 | 1024 / 像素 | `PrefilterEnvMap.hlsl`，`SAMPLE_COUNT` |
| 预过滤源分辨率 | 512（硬编码） | `PrefilterEnvMap.hlsl`，`float resolution = 512.0` |
| 当前近似 mip 上限 | 10 | `Generated_DeferredLighting.hlsl`，`maxMipLevel` |
| 环境光强度/颜色 | 场景级 Skylight（默认 1.0 / 白） | `Scene`，编辑器主光面板可调 |

## 已知限制与 TODO

- **IBL 尚未接入渲染管线**：`IBLResources` / `SphericalHarmonics` 没有任何调用方；延迟光照仍用 `SkyCube` mip 近似。
- `CreateIrradianceMap` 与 `CreatePrefilteredMap` 只创建资源并做了 UAV→SRV 屏障，**计算着色器从未执行**，产出的纹理内容未定义（未清除）。
- `SphericalHarmonics` 只有头文件声明，无实现、无调用方。
- `SHCalculation.hlsl` 的系数累加是简化版（非原子），`TODO` 注释标明需要 `InterlockedAdd` 或 GroupShared 优化。
- `PrefilterEnvMap.hlsl` 中源 cubemap 分辨率硬编码为 512，与实际资产不一致时会算错 mip。
- `IBLResources` 的 compute 根签名与主渲染根签名（`InitRootSignature`）相互独立，接入时注意两套描述符堆的切换。
- 无运行时 IBL 调试开关（无法单独开关/可视化三张纹理）。
- 计算着色器使用 `cs_5_0` 与调试编译标志（`D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION`），生产构建未做优化编译。

## 维护注意事项

- 接入 IBL 时建议顺序：先让 `CreateIrradianceMap` / `CreatePrefilteredMap` 真正执行 CS（补 UAV 绑定与 Dispatch、逐 mip 循环），再在 `Generated_DeferredLighting.hlsl` 中用 IBL 纹理替换 `SkyCube` 采样，最后用 BRDF LUT 替换 `F` 近似。
- 三张纹理的生成必须发生在环境 cubemap 上传完成之后，且整个 `Initialize` 需要在一次命令列表提交内完成（内部有 UAV→SRV 屏障序列）。
- `IBLResources` 持有自己的 SRV/UAV 堆与 compute 根签名；接入主渲染时要么把三张纹理的 SRV 合并进全局 `srvHeap`（bindless），要么在采样 Pass 单独设置该堆，二者选一避免混淆。
- 修改尺寸常量（`IRRADIANCE_SIZE` 等）时同步检查对应 .hlsl 中的 dispatch/采样逻辑与 `GetDimensions` 处理。
- 若启用 `SHCalculation.hlsl`，必须先修复原子累加问题，否则结果随机。
- 保持 `IBLResources` 与 `SphericalHarmonics` 两套方案的取舍清晰：建议二选一（贴图方案或 SH 方案）接入，避免维护两套环境光路径。