# 着色模型（ShadingModels）

> 本文依据 `Engine/Shader/StandardPBR.shader`、`Engine/Shader/ToonPBR.shader`、`Engine/Shader/Shadingmodel/Screen.shader` 与 `Engine/private/Material/Shader.cpp` 的注入逻辑编写，说明 StandardPBR（shadingModelID=1）与 ToonPBR（shadingModelID=2）两种着色模型的定义、GBuffer 编码，以及 Screen.shader 的 UE5 风格延迟光照与 BRDF switch 机制。

## 概述

FEngine 的着色模型采用「GBuffer 携带 ID + 延迟光照统一 BRDF switch」架构：

- 材质 shader（Deferred）在 Pass 0 填充 GBuffer，其中 `ORM.a` 通道写入 `shadingModelID / 255.0f`。
- 延迟光照由 `Screen.shader` 的 PS 完成：从 GBuffer 解包 ID，调用 `BRDF(int shadingModelID, N, V, L, albedo, metallic, roughness)` 分派到具体 BRDF。
- `BRDF()` 原文件只实现 `case 1: DefaultBRDF(...)`；带 `ShadingModel{}` 块的 shader（如 ToonPBR）加载时会把其 BRDF 函数体与 `case N` **字符串注入**到生成的 `Screen.hlsl`（见 ShaderSystem.md）。
- 光照代码整体为 UE5 风格：GGX（Roughness^4）、Smith Joint 可见性、Fresnel-Schlick、Split-sum IBL、多重散射能量补偿、Frostbite MultiBounce AO。

目前正式注册的模型：`1 = StandardPBR`（默认，写死在 StandardPBR.shader），`2 = ToonPBR`（由 ToonPBR.shader 的 ShadingModel 块注册）。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/Shader/StandardPBR.shader` | 模型 1：GBuffer Pass（shadingModelID=1），参数 Properties |
| `Engine/Shader/ToonPBR.shader` | 模型 2：ShadingModel 块（shadingmodel=2）+ ToonBRDF 定义 + GBufferToonPass |
| `Engine/Shader/Shadingmodel/Screen.shader` | 延迟光照 Pass：UE5 风格 BRDF 函数集 + BRDF switch（原生仅 case 1）+ IBL/SSGI/GTAO 合成 |
| `Engine/private/Material/ShaderParser.cpp` | `ParseShadingModel` 解析 ShadingModel 块；`ParseShaderFile` 为 Deferred shader 自动挂载 Screen Pass |
| `Engine/private/Material/Shader.cpp` | `g_shadingModels` 全局注册表；Screen.hlsl 的 BRDF 函数与 case 注入；`CheckAndRecompileScreen` |
| `Engine/Shader/Shader_Cache/Screen.hlsl` | 生成产物（含注入后的 ToonBRDF 与 case 2），调试用 |

## 架构与数据流

### GBuffer 编码（Pass 0）

四个离屏 RT（`Scene::CreateOffscreenRTs`）：

| RT | 格式 | 内容 |
| --- | --- | --- |
| RT0 | R16G16B16A16_FLOAT | BaseColor (RGB) + 1.0 |
| RT1 | R16G16B16A16_FLOAT | World Normal (RGB) + 1.0 |
| RT2 | R16G16B16A16_FLOAT | ORM：R=AO、G=Roughness、B=Metallic、A=ShadingModelID/255 |
| RT3 | R16G16_FLOAT | Motion Vector（当前帧 NDC − 上一帧 NDC）× 0.5 |

材质 shader 的 GBuffer PS 流程（StandardPBR/ToonPBR 相同）：

1. Bindless 采样：`SAMPLE_TEXTURE(BaseColorTexIndex, gSamAnisotropicWarp, uv)` 等三个纹理。
2. 参数合成：`finalBaseColor = sampledBaseColor * BaseColor.xyz`；`ao = sampledOrm.r`；`roughness = saturate(sampledOrm.g + Roughness)`；`metallic = saturate(sampledOrm.b + Metallic)`。
3. TBN 法线：切线正交化（Gram-Schmidt）+ `cross(N, T) * T.w` 构造 B，`transpose(TBN)` 后乘以 `tangentNormal * 2 - 1`。
4. 写 `SV_TARGET0..3`；ShadingModelID 写入 `ORM.a`（`shadingModelID / 255.0f`），延迟阶段用 `uint(orm.a * 255.0 + 0.5)` 解包。

### 延迟光照数据流（Screen.shader PS）

```
采样 GBuffer（t0-t3）+ SkyCube(t4) + ShadowMap(t5) + GTAO(t6) + SSGI(t7)
 → UV→NDC→InverseProjectionMatrix→InverseViewMatrix 重构世界坐标
 → 解包 shadingModelID / roughness / metallic / materialAO
 → ao = clamp(materialAO * gtao, 0.03, 1)
 → 直接光：directLighting = BRDF(shadingModelID, N, V, L, baseColor, metallic, roughness)
 → IBL：prefiltered mip = roughness * 8；irradiance = 最高 mip；split-sum 能量项
 → finalColor = directLighting * shadow * 6.0 + ambient（含 MultiBounceAO、SSGI lerp）
 → depth >= 1.0 输出 alpha=0（天空区域透出 SkyPass）
```

- 阴影：`ShadowMap.Sample(gSamPointWrap, uv).r`，来自 LightPass 输出；关闭阴影时 ScreenPass 注入 1x1 白色纹理。
- GI 模式：`_Padding0.x = giType`（0=ambient，1=SSGI），SSGI 时按 `ssgiWeight`（SSGI 纹理 alpha 通道）在 IBL 与 SSGI 之间 `lerp`。
- 天空：`depth < 1.0` 才计算光照，否则输出 `(0,0,0,0)` 并启用 Alpha 混合，让后置 SkyPass 透出。

## 关键实现要点

### StandardPBR（shadingModelID = 1）

- `Engine/Shader/StandardPBR.shader`：`RenderQueue "Deferred"`，Properties 为 `BaseColor`(float4, ColorPicker)、`Roughness`(Slider 0-1)、`Metallic`(Slider 0-1)、`BaseColorTex/NormalTex/OrmTex`(Texture2D)。
- 单 Pass `GBufferPass`，PS 中硬编码 `int shadingModelID = 1;`，写入 `gbuffer.ORM = float4(ao, finalRoughness, finalMetallic, shadingModelID / 255.0f);`。
- VS 输出 TAA 所需 `currentPositionCS`（当前帧不带 Jitter 的 VP）与 `previousPositionCS`（上一帧 VP），用于 Motion Vector。

### ToonPBR（shadingModelID = 2）

`Engine/Shader/ToonPBR.shader` 顶部 ShadingModel 块：

```
ShadingModel
{
    shadingmodel=2;
    BRDF=ToonBRDF(N, V, L, albedo, metallic, roughness);
    BRDF
    {
        float3 ToonBRDF(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness)
        {
            float NdotL = max(dot(N, L), 0.0);
            float toonLevels = 4.0;
            float toonShading = floor(NdotL * toonLevels) / toonLevels;
            float NdotV = max(dot(N, V), 0.0);
            float rimLight = pow(1.0 - NdotV, 3.0) * 0.5;
            float3 diffuse = albedo * toonShading;
            float3 rim = float3(1.0, 1.0, 1.0) * rimLight;
            return diffuse + rim;
        }
    }
}
```

- 卡通特征：4 级亮度量化（`floor(NdotL*4)/4`）+ 0.5 强度的三次方边缘光；不依赖 roughness/metallic。
- `Shader::LoadFromShaderFile` 注册 ID=2 后置 `g_screenNeedsRecompile=true` 并删除 Screen.hlsl 缓存；生成 Screen.hlsl 时把 `ToonBRDF` 函数体插到 `BRDF` switch 前、`case 2: return ToonBRDF(...);` 插到 `default` 前。
- 材质资产 `Content/Materials/toonmaterial.material` 引用 `ToonPBR` shader。

### BRDF switch（重点限制）

`Screen.shader` 原文件：

```hlsl
float3 BRDF(int shadingModelID, float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness){
    switch(shadingModelID){
        case 1: return DefaultBRDF(N, V, L, albedo, metallic, roughness);
        //后面的会自动生成
        default: return float3(0, 0, 0);
    }
}
```

- **原生只实现 case 1**；case 2 由 ToonPBR 加载后的代码注入生成（生成产物 `Engine/Shader/Shader_Cache/Screen.hlsl` 可验证）。
- 未注册的 ID 落入 `default: return float3(0, 0, 0)`，像素显示为黑色（仅环境光），排查时先确认 ID 已写入 `g_shadingModels`。

### UE5 风格光照函数集（Screen.shader）

| 函数 | 说明 |
| --- | --- |
| `F_Schlick(F0, VoH)` | Fresnel-Schlick 近似 |
| `FresnelSchlickRoughness(cosTheta, F0, roughness)` | 带粗糙度的 Fresnel（IBL 用） |
| `D_GGX(a2, NoH)` | UE 版 GGX NDF，`a2 = Pow4(Roughness)` |
| `Vis_SmithJointApprox(a2, NoV, NoL)` | Smith Joint 可见性（已含 1/(4·NoL·NoV)） |
| `Diffuse_Lambert(DiffuseColor)` | 漫反射 = DiffuseColor / PI |
| `ComputeEnvBRDFApprox(NoV, Roughness)` | Karis 拟合曲线近似 EnvBRDF LUT |
| `Luminance(color)` | Rec.709 亮度（0.2126/0.7152/0.0722） |
| `ComputeGGXSpecEnergyTerms` | 单次散射方向反照率 E = F0·scale + bias；多重散射补偿 W = 1 + F0·(1/E−1) |
| `ComputeEnergyPreservation / ComputeEnergyConservation` | 漫反射能量守恒（1−Luminance(E)）与镜面能量补偿（W） |
| `MultiBounceAO(ao, albedo)` | Frostbite 多次反弹 AO 近似（a/b/c 系数） |
| `DefaultBRDF(N, V, L, albedo, metallic, roughness)` | 模型 1 主体：`DiffuseColor = albedo*(1-metallic)`、`SpecularColor = lerp(0.04, albedo, metallic)`、`specular = D*Vis*F`，应用能量补偿后 `(diffuse + specular) * NoL` |

IBL 部分（PS 内）：

- `maxMipLevel = 8.0`；`prefilteredColor = SkyCube.SampleLevel(..., R, roughness * 8)`。
- `irradiance = SkyCube.SampleLevel(..., N, 8)`（最高 mip 近似漫反射辐照度）。
- `diffuseIBL = irradiance * DiffuseColor * ComputeEnergyPreservation(...)`；`specularIBL = prefilteredColor * EnergyTerms.E * ComputeEnergyConservation(...)`。
- 环境光 = `(diffuseIBL + specularIBL) * Skylight`，再乘 `MultiBounceAO`。

## 对外接口

- 引擎侧无 C++ 着色模型枚举；ID 只在 shader 文本与 `ShaderParser::ShadingModelDefinition.shadingModelID` 中存在。
- `ShaderParser::ParseShadingModel(content, pos)`：解析 `shadingmodel=ID`、`BRDF=调用;`、`BRDF{函数}`。
- `ShaderParser::HasShadingModel() / GetShadingModel()`：查询解析结果。
- `Shader.cpp` 全局 `std::map<int, ShaderParser::ShadingModelDefinition> g_shadingModels`：跨 shader 注册表（按 ID 去重）。
- `CheckAndRecompileScreen(device, rootSig, matMgr)`：新模型注册后触发 Screen 重编译。

## 配置与调参

- **新增着色模型**：新建 `.shader`，写 `RenderQueue "Deferred"` + `ShadingModel { shadingmodel=N; BRDF=MyBRDF(N,V,L,albedo,metallic,roughness); BRDF{...} }` + GBuffer Pass（`gbuffer.ORM.a = N/255.0f`），放入 `Engine/Shader/` 即可被扫描注册。
- **ID 约束**：`0 < ID <= 255`（GBuffer alpha 为 8 位归一化）；不能与已注册 ID 重复。
- **卡通参数**：`toonLevels`（量化级数）与 `rimLight` 强度（0.5）在 `ToonBRDF` 函数体内硬编码，调参需改 shader 文本。
- **光照强度**：直接光缩放系数 `6.0`（`directLighting * shadow * 6.0`）在 Screen.shader PS 内硬编码。
- **IBL mip 上限**：`maxMipLevel = 8.0` 需与预过滤环境贴图的 mip 数匹配。
- **AO 下限**：`ao = clamp(materialAO * gtao, 0.03, 1)`，0.03 为最低环境光。

## 已知限制与TODO

- **BRDF switch 目前只实现 case 1**（原生代码）；case 2 依赖加载期字符串注入，`Screen.hlsl` 缓存被删除前新模型不会生效。
- ToonBRDF 不参与 IBL（环境光仍走 DefaultLit 的能量模型），且无描边、无色调分离的阴影分级贴图控制。
- 注入依赖精确锚点字符串；Screen.shader 中重命名 `BRDF` 函数或修改 `default` 分支会静默注入失败（只打 `[ERROR]` 日志）。
- `default: return float3(0,0,0)` 没有兜底光照，ID 未注册时直接光为 0。
- 金属度/粗糙度在 Toon 模型中被忽略，GBuffer 仍照常写入（便于切回 StandardPBR）。
- 每个模型需手写一份 GBuffer Pass（StandardPBR 与 ToonPBR 的 VS/PS 大量重复），未抽象共用模板。

## 维护注意事项

- 修改任一模型后必须删除 `Engine/Shader/Shader_Cache/Screen.hlsl`（或依赖 `CheckAndRecompileScreen` 的置位逻辑）再验证，否则看到的是旧注入结果。
- 验证注入是否成功：检查 `Engine/Shader/Shader_Cache/Screen.hlsl` 中 `// ShadingModel N` 注释与 `case N:` 是否存在。
- GBuffer 布局（RT 格式、ORM 通道含义）是材质 shader 与 Screen.shader 的契约，改动任一侧必须同步另一侧；`Scene::CreateOffscreenRTs` 的格式数组是权威定义。
- `ORM.a` 的 ShadingModelID 用归一化 8 位编码（ID/255），解包用 `uint(orm.a * 255.0 + 0.5)`，两边公式必须一致。
- 旧文档 `HOW_TO_USE_MATERIAL_SHADER.md` 的 GBuffer 表格（RT3=position）已过时：当前 RT3 为 Motion Vector，深度采样走 `DepthTexture(t3)` + 逆投影重构。
*** End Patch