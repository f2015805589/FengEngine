# ScreenPass - 全屏合成（延迟光照）

> ScreenPass 是延迟渲染的“光照合成”阶段：绑定 8 张输入 SRV（G-Buffer、深度、SkyCube、LightRT 阴影因子、GTAO、SSGI），以全屏四边形执行 `Screen.shader` 的 DeferredLighting Pass，把直接光照（Cook-Torrance）+ IBL 环境光 + AO + SSGI 混合成最终场景颜色。输出到 TAA 中间 RT 或视口颜色缓冲。

## 概述

```
输入 SRV（ScreenPass 8 槽堆）:
  t0 BaseColor  t1 Normal  t2 ORM  t3 深度
  t4 SkyCube    t5 LightRT(阴影因子)  t6 GTAO  t7 SSGI
        │
        ▼
ScreenPass::Render(cmd, deferredLightingPso, rootSig, rt0, rt1, rt2,
                   depthBuffer, skyTexture, shadowMap, gtaoTexture, ssgiTexture)
  ├─ CreateInputSRVs(...)             // 每帧重建 8 个描述符
  ├─ 绑定 b0 SceneCB + SRV 表（slot 1）
  └─ 共享全屏四边形 → DrawInstanced(6, 1, 0, 0)
        │
        ▼
Screen.shader PS（DeferredLighting）:
  深度重建世界坐标 → ORM 解包 + ShadingModelID
  直接光: BRDF(shadingModelID, N, V, L, albedo, metallic, roughness)
  IBL:   prefiltered(R, roughness*8) × E + irradiance(N, mip8) × DiffuseColor
  AO:    ao = clamp(materialAO * GTAO, 0.03, 1) → MultiBounceAO
  SSGI:  giType>0.5 ? lerp(ambient, ssgi, ssgiWeight) : ambient
  输出:  directLighting * shadow * 6.0 + ambient；天空 alpha=0
```

- 注意：主循环传入的 PSO 是 `deferredLightingPso`（StandardPBR 的 Pass 1，由 ShaderParser 自动从 `Screen.shader` 生成），而不是 `screen.hlsl` 编译的 `screenPso`（后者仅保留兼容）；
- ScreenPass 不拥有光照算法，只负责 SRV 绑定与全屏绘制；
- PSO 开启 alpha 混合（`SRC_ALPHA / INV_SRC_ALPHA`），实现“天空透明、场景覆盖”的合成协议。

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/public/ScreenPass.h` | ScreenPass 类、8 槽 SRV 堆、默认纹理 |
| `Engine/private/ScreenPass.cpp` | `CreateInputSRVs`、`Render`、默认白/黑纹理上传 |
| `Engine/Shader/Shadingmodel/Screen.shader` | 实际光照合成 shader（UE5 风格 BRDF + IBL + AO + SSGI 混合） |
| `Engine/Shader/screen.hlsl` | 遗留合成 shader（早期版本，当前未被调度） |
| `Engine/Shader/Generated_DeferredLighting.hlsl` | ShaderParser 生成的 Pass 1 编译产物（内容与 Screen.shader 同步较旧） |
| `Engine/private/ShaderParser.cpp` | 单 Pass shader 自动附加 Screen.shader 第二 Pass 的逻辑 |
| `Engine/private/BattleFireDirect.cpp` | `CreateFullscreenPSO`（alpha 混合开关） |

## 架构与数据流

### 8 槽 SRV 堆布局（CreateInputSRVs）

| 槽位 | 内容 | 格式 | 兜底 |
| --- | --- | --- | --- |
| t0 | RT0 Albedo | `R16G16B16A16_FLOAT` | — |
| t1 | RT1 Normal | `R16G16B16A16_FLOAT` | — |
| t2 | RT2 ORM | `R16G16B16A16_FLOAT` | — |
| t3 | gDSRT 深度 | `R24_UNORM_X8_TYPELESS` | — |
| t4 | SkyCube | 资源格式，`TEXTURECUBE` | — |
| t5 | LightRT（阴影因子） | `R16G16B16A16_FLOAT` | 阴影关闭 → 1×1 白色纹理 |
| t6 | GTAO Blurred | `R8G8B8A8_UNORM` | 关闭 → 1×1 白色纹理 |
| t7 | SSGI Final | `R16G16B16A16_FLOAT` | 关闭 → 1×1 黑色纹理 |

默认纹理：`CreateDefaultWhiteTexture()`（1×1 `R8G8B8A8_UNORM`，像素 255,255,255,255）与 `CreateDefaultBlackTexture()`（1×1 `R16G16B16A16_FLOAT`，全 0），均用临时 command list + fence 上传。

### 渲染流程

```
ScreenPass::Render:
  1. CreateInputSRVs(cmd, rt0..rt2, depth, skyTexture, shadowMap, gtao, ssgi)
  2. SetGraphicsRootSignature / SetPipelineState(pso)     // deferredLightingPso
  3. SetGraphicsRootConstantBufferView(0, SceneCB)
  4. SetDescriptorHeaps(m_srvHeap)；SetGraphicsRootDescriptorTable(1, 起点)
  5. GetSharedFullscreenQuadVB → DrawInstanced(6, 1, 0, 0)
```

## 关键实现要点

### Screen.shader 的光照模型（UE5 风格）

| 函数 | 说明 |
| --- | --- |
| `D_GGX(a2, NoH)` | UE 版 GGX NDF（`a2 = Pow4(roughness)`） |
| `Vis_SmithJointApprox(a2, NoV, NoL)` | Smith Joint 可见性（含 1/(4·NoV·NoL)） |
| `F_Schlick(F0, VoH)` | Schlick 菲涅尔 |
| `Diffuse_Lambert(DiffuseColor)` | 漫反射 `albedo*(1-metallic)/π` |
| `ComputeEnvBRDFApprox(NoV, Roughness)` | 拟合曲线近似预计算 BRDF LUT（Karis） |
| `ComputeGGXSpecEnergyTerms(...)` | `E = F0*scale + bias`、`W = 1 + F0*(1/E - 1)` 多重散射能量补偿 |
| `ComputeEnergyPreservation / Conservation` | 漫反射能量守恒与镜面补偿（基于亮度） |
| `MultiBounceAO(ao, albedo)` | Frostbite 多反弹 AO 近似（见下文） |
| `DefaultBRDF(...)` / `BRDF(shadingModelID, ...)` | 按 `ShadingModelID` 分发（当前仅 case 1） |

### 合成公式（PS 主体）

```hlsl
float materialAO = orm.r;  float roughness = orm.g;  float metallic = orm.b;
uint shadingModelID = uint(orm.a * 255.0 + 0.5);

// AO：材质 AO × GTAO，夹到 [0.03, 1]
float gtao = GTAOTexture.Sample(gSamPointClamp, uv).r;
float ao = clamp(materialAO * gtao, 0.03, 1);

// SSGI：rgb=间接光, a=命中率
float4 ssgiData = SSGITexture.Sample(gSamPointClamp, uv);
float3 ssgi = ssgiData.rgb;  float ssgiWeight = ssgiData.a;

// 直接光
float3 directLighting = BRDF(shadingModelID, N, V, L, albedo, metallic, roughness);

// IBL（UE5 风格）
float3 prefilteredColor = SkyCube.SampleLevel(gSamAnisotropicClamp, R, roughness * 8).rgb;
float3 irradiance      = SkyCube.SampleLevel(gSamAnisotropicClamp, N, 8).rgb;
float3 diffuseIBL  = irradiance * DiffuseColor * ComputeEnergyPreservation(E);
float3 specularIBL = prefilteredColor * E * ComputeEnergyConservation(E);
float3 ambient = (diffuseIBL + specularIBL) * Skylight;

// SSGI/IBL 混合：giType 通过 SceneCB 的 _Padding0.x（即 skylightParams.y）传入
ambient = (giType > 0.5) ? lerp(ambient, ssgi, ssgiWeight) : ambient;
ambient *= MultiBounceAO(ao, baseColor.xyz);

// 组合
if (depth < 1.0) { finalColor = directLighting * shadow * 6.0 + ambient; alpha = 1.0; }
else             { finalColor = 0; alpha = 0.0; }   // 天空透明
```

### MultiBounceAO（多反弹 AO）

Screen.shader 内置 Frostbite 风格的多反弹近似，把单次遮蔽值修正为多反弹后的更亮结果：

```hlsl
float3 MultiBounceAO(float ao, float3 albedo) {
    float3 a = 2.0404 * albedo - 0.3324;
    float3 b = -4.7951 * albedo + 0.6417;
    float3 c = 2.7552 * albedo + 0.6903;
    return max(ao, ((ao * a + b) * ao + c) * ao);
}
```

亮色材质（高 albedo）的多反弹增益更大，符合物理直觉。

### giType 的传递路径

```
main.cpp: ssgiPass->SetGIType(v); g_scene->SetGIType(v);
Scene::Update → FillSceneCBData(..., giType)
  → out.skylightParams = XMFLOAT4(intensity, giType, 0, 0)   // 偏移 73 = shader 的 _Padding0.x
Screen.shader: ambient = (_Padding0.x > 0.5) ? lerp(ambient, ssgi, ssgiWeight) : ambient;
```

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `ScreenPass::Initialize(w, h)` | `ScreenPass.h` | 创建 SRV 堆与默认纹理 |
| `ScreenPass::Render(cmd, pso, rootSig, rt0, rt1, rt2, depth, skyTexture, shadowMap, gtao, ssgi)` | `ScreenPass.h` | 每帧合成入口 |
| `ScreenPass::CreatePSO(rootSig, vs, ps)` | `ScreenPass.h` | `CreateFullscreenPSO(..., R8G8B8A8_UNORM, alphaBlend=true)` |
| `ScreenPass::SetSceneConstantBuffer / SetMaterialConstantBuffer` | `ScreenPass.h` | 绑定 CB（材质 CB 当前未在 Render 中使用，仅匹配根签名） |
| `ScreenPass::Resize(w, h)` | `ScreenPass.h` | 更新视口尺寸（无资源重建） |

## 配置与调参

| 参数 | 默认 | 入口 | 说明 |
| --- | --- | --- | --- |
| 直接光强度系数 | 6.0 | Screen.shader `finalColor` | 直接光照亮度标定 |
| IBL mip 上限 | 8.0 | `maxMipLevel = 8.0` | 预过滤近似粒度 |
| AO 下限 | 0.03 | `clamp(materialAO * gtao, 0.03, 1)` | 防止完全黑暗 |
| Skylight 强度 | 1.0 | `Scene::SetSkylightIntensity` | 环境光整体强度 |
| Skylight 颜色 | (1,1,1) | `Scene::SetSkylightColor` | 环境光 Tint |
| SSGI 混合 | 命中率 | `giType` + SSGI alpha | 命中率=0 时纯 IBL |

## 已知限制与 TODO

- **IBL 为运行时近似**：`prefilteredColor / irradiance` 直接对 SkyCube 按 mip 采样，未使用 `IBLResources` 预计算的 BRDF LUT / 辐照度 / 预过滤贴图（`IBLResources` 类已实现但主循环从未实例化，资源实际未生成），能量项用 `ComputeEnvBRDFApprox` 拟合曲线近似；
- **无曝光/色调映射**：合成输出为线性 HDR 值直接写入 `R16/R8` 目标，未做 ACES/Filmic 映射（TAA 在 Reinhard 空间做裁剪，但输出仍是线性）；
- **screen.hlsl 与 Generated_DeferredLighting.hlsl 是旧版**：前者未被调度，后者内容滞后于 Screen.shader（缺少 GTAO/SSGI 注入与 ShadingModelID 分发），排查问题时以 `Screen.shader` 为准；
- **阴影图采样器**：`gSamPointWrap` 采样阴影因子（无滤波），配合 PCSS 已足够，但换 PCF 模式时建议使用线性采样；
- **混合协议脆弱**：天空透明依赖 PSO alpha 混合 + 深度判断 `depth >= 1.0`，任何一方被改动都会导致天空变黑或场景透底。

## 维护注意事项

- **SRV 槽位顺序是硬约定**：t0-t7 顺序由 `CreateInputSRVs` 固定，Screen.shader 的 `register(tN)` 与之对应；新增输入纹理需要同时改堆大小（当前 8）、SRV 创建顺序与 shader 声明。
- **GTAO/SSGI 槽位由编译器注入**：`GTAOTexture`（t6）与 `SSGITexture`（t7）的声明由 ShaderParser 注入生成文件，手写 hlsl 中不应重复声明同寄存器。
- **pso 参数**：主循环传 `deferredLightingPso`（StandardPBR Pass 1）而非 `screenPso`；`ScreenPass::CreatePSO` 创建的 PSO 目前只在启动时校验用，维护时不要依赖它作为最终合成 PSO。
- **场景 CB 的 `_Padding0.x`**：Screen.shader 通过它读取 giType，实际对应 `SceneCBData.skylightParams.y`；修改 SceneCB 布局时保持该映射（或改为显式字段）。
- **状态约定**：ScreenPass 采样深度时深度必须处于 `PIXEL_SHADER_RESOURCE`；主循环在 ScreenPass 之后立即转回 `DEPTH_WRITE`，新增读取深度的 Pass 需安排在转换之前。
- **调试建议**：逐槽替换 SRV（如把 t6 换成白色纹理）可快速定位 AO/GI 合成问题；用 PIX 查看 `Screen.shader` 的中间结果需要临时在 PS 中输出特定项（如 `return float4(ao,ao,ao,1)`）。