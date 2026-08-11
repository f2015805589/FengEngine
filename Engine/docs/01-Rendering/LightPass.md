# LightPass - 延迟光照（阴影因子）

> LightPass 在 BasePass 之后执行，负责两件事：① 渲染 4096×4096 的平行光 Shadow Map；② 用全屏四边形从 G-Buffer 深度重建世界坐标，计算阴影因子并输出到 LightRT。注意：当前 LightPass 只输出**灰阶阴影因子**，完整的直接光/IBL 光照合成发生在 ScreenPass（`Screen.shader`）中。

## 概述

```
BasePass 输出（深度 / G-Buffer）
        │
        ▼
LightPass::RenderDirectLight(cmd, shadowPso, lightPso, rootSig, scene, depthBuffer)
        │
        ├── 子 Pass A: RenderShadowMap ──▶ Shadow Map（4096²，深度）
        │
        └── 子 Pass B: RenderLighting  ──▶ LightRT（R16G16B16A16_FLOAT 阴影因子灰阶图）
                                                   │
                                                   ▼
                              ScreenPass 采样 LightRT（t5）→ 直接光照 × 阴影
```

- 输入：`gDSRT` 深度缓冲（`R24_UNORM_X8_TYPELESS` 视图）、场景常量缓冲（含 `LightViewProjectionMatrix`、`ShadowMode`）；
- 输出：`m_lightRT`（`R16G16B16A16_FLOAT`，全屏，初始 `PIXEL_SHADER_RESOURCE`）；
- 着色器：`Engine/Shader/lighting.hlsl`（入口 `LightVS / LightPS`）；
- 阴影算法：Hard / PCF / PCSS，由 `SceneCBData.shadowMode` 运行时切换。

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/public/LightPass.h` | LightPass 类声明（尺寸、阴影图、SRV 堆、两个子 Pass） |
| `Engine/private/LightPass.cpp` | 资源创建、`RenderShadowMap`、`RenderLighting`、`RenderDirectLight` |
| `Engine/Shader/lighting.hlsl` | `LightVS/LightPS`、`ReconstructWorldPosition`、Hard/PCF/PCSS 实现 |
| `Engine/Shader/shadowdepth.hlsl` | 阴影子 Pass 的深度着色器（详见 ShadowPass.md） |
| `Engine/main.cpp` | 创建 `lightPass`（4096 阴影图）、`shadowPso`、`lightPso`，帧循环调用 |

## 架构与数据流

### 资源一览

| 资源 | 格式/规格 | 说明 |
| --- | --- | --- |
| `m_lightRT` | `R16G16B16A16_FLOAT`，视口尺寸 | 阴影因子输出（灰阶） |
| `m_shadowMap` | `R32_TYPELESS`，4096² | 阴影深度图 |
| `m_srvHeap` | 2 个描述符 | t0=深度缓冲，t1=ShadowMap |
| `m_rtvHeap` | 1 个描述符 | LightRT 的 RTV |
| `m_dsvHeap` | 1 个描述符 | ShadowMap 的 DSV |

### SRV 创建（CreateInputSRVs）

```
srvHandle = m_srvHeap 起点
  ├─ t0: 深度缓冲
  │      格式按资源格式自适应：D24_UNORM_S8_UINT / R24G8_TYPELESS → R24_UNORM_X8_TYPELESS
  │                            D32_FLOAT / R32_TYPELESS → R32_FLOAT
  └─ t1: ShadowMap → R32_FLOAT（TEXTURE2D，Mip=1）
```

### 每帧执行顺序

```
RenderDirectLight:
  1. RenderShadowMap(commandList, shadowPso, rootSignature, scene)
       - 视口 4096×4096；清深度 1.0；OMSetRenderTargets(0, null, DSV)
       - 遍历 Actor：绑定 Actor CB（b0）→ mesh->Render
       - ShadowMap → PIXEL_SHADER_RESOURCE
  2. RenderLighting(commandList, lightPso, rootSignature, depthBuffer)
       - CreateInputSRVs（重建 t0/t1 描述符）
       - LightRT → RENDER_TARGET，清黑
       - 绑定 b0 SceneCB + SRV 表（slot 1）
       - 共享全屏四边形 → DrawInstanced(6, 1, 0, 0)
       - LightRT → PIXEL_SHADER_RESOURCE
  3. ShadowMap → DEPTH_WRITE（为下一帧准备）
```

## 关键实现要点

### 世界坐标重建（lighting.hlsl）

```hlsl
float3 ReconstructWorldPosition(float2 uv, float depth) {
    float4 ndcPos;
    ndcPos.x = uv.x * 2.0f - 1.0f;
    ndcPos.y = (1.0f - uv.y) * 2.0f - 1.0f;   // Y 翻转
    ndcPos.z = depth;
    ndcPos.w = 1.0f;
    float4 viewPos = mul(InverseProjectionMatrix, ndcPos);
    viewPos /= viewPos.w;
    float4 worldPos = mul(InverseViewMatrix, viewPos);
    return worldPos.xyz;
}
```

`LightPS` 主流程：

```
depth = g_DepthBuffer.Sample(uv).r
if (depth >= 1.0) return float4(1,1,1,1);      // 天空：输出白色阴影因子
positionWS = ReconstructWorldPosition(uv, depth)
shadow = (ShadowMode < 0.5)  ? CalculateHardShadow(positionWS)
       : (ShadowMode < 1.5)  ? CalculateShadow(positionWS)   // 3x3 PCF
       :                       CalculateShadowPCSS(positionWS)
return float4(shadow, shadow, shadow, 1)
```

### 阴影因子输出约定

LightRT 中存储的是“可见度”而非“遮蔽度”：`1.0 = 完全照亮`、`0.0 = 完全阴影`。Screen.shader 中用法：

```hlsl
float shadow = ShadowMap.Sample(gSamPointWrap, input.uv).r;   // 实际是 LightRT
finalColor = directLighting * shadow * 6.0 + ambient;          // *6.0 为亮度补偿
```

`* 6.0` 是当前默认平行光强度的隐式标定，调整灯光强度时应同时审视该系数。

### PSO

- `CreateShadowPSO`：详见 ShadowPass.md（CullMode FRONT、DepthBias 5000、仅深度）；
- `CreateLightPSO`：直接复用 `CreateFullscreenPSO(rootSig, vs, ps, R16G16B16A16_FLOAT)`（禁用深度、CullNone、Position3+UV2 输入布局）。

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `LightPass(int width, int height, int shadowMapSize = 4096)` | `LightPass.h` | 构造，主循环传入视口尺寸与阴影图分辨率 |
| `LightPass::Initialize(commandList)` | `LightPass.h` | 创建 SRV 堆、LightRT、ShadowMap |
| `LightPass::RenderDirectLight(cmd, shadowPso, lightPso, rootSig, scene, depthBuffer)` | `LightPass.h` | 每帧入口（两个子 Pass） |
| `LightPass::CreateShadowPSO / CreateLightPSO` | `LightPass.h` | PSO 工厂 |
| `LightPass::GetLightRT() / GetShadowMap()` | `LightPass.h` | 输出资源访问 |
| `LightPass::Resize(w, h)` | `LightPass.h` | LightRT 分辨率变更 |
| `LightPass::ResizeShadowMap(size)` | `LightPass.h` | 阴影图分辨率变更 |
| `Scene::SetShadowMode / SetShadowmapEnabled` | `Scene.h` | 阴影模式与开关（UI 驱动） |

## 配置与调参

| 参数 | 默认 | 入口 | 说明 |
| --- | --- | --- | --- |
| 阴影图分辨率 | 4096 | 构造参数 / `ResizeShadowMap` | 影响 PCSS/PCF 采样密度 |
| 阴影模式 | 2 (PCSS) | UI MainLight 窗口 | 0=Hard、1=PCF、2=PCSS |
| 阴影开关 | true | UI `Enable Shadowmap` | 关闭时 `RenderDirectLight` 整段跳过 |
| `LIGHT_SIZE` | 0.02 | `lighting.hlsl` 常量 | 光源大小，控制软阴影范围 |
| `BLOCKER_SEARCH_SAMPLES` | 16 | `lighting.hlsl` 常量 | PCSS blocker 搜索采样数 |
| `PCF_SAMPLES` | 25 | `lighting.hlsl` 常量 | PCSS 阶段 PCF 采样数 |
| 直接光强度系数 | 6.0 | Screen.shader `finalColor` | 直接光亮度标定 |

## 已知限制与 TODO

- LightPass 只产出阴影因子，**不包含直接光颜色计算**；光照模型（Cook-Torrance）全部在 Screen.shader 中，职责划分偏弱，扩展多光源时需要重构。
- `lighting.hlsl` 中 PCF 函数内写死 `texelSize = 1/2048`，与 4096 阴影图不一致（PCSS 路径使用 `SHADOW_MAP_SIZE=4096`），是潜在精度不一致点。
- `g_ShadowSampler`（`s1`）声明为 `SamplerComparisonState` 但代码始终用 `g_Sampler` 手动比较，属于未完成特性。
- 阴影边界外默认视为无阴影（返回 1.0），场景边缘会产生“光照溢出”到包围盒外区域的效果。
- 无半分辨率优化：LightRT 为全屏 `R16G16B16A16_FLOAT`，仅存储灰度值，带宽可优化为 `R8` 单通道。

## 维护注意事项

- **新增阴影算法**：在 `LightPS` 中扩展 `ShadowMode` 分支，并把模式常量写入 `SceneCBData.shadowMode`（`FillSceneCBData` 最后一个参数 `shadowMode`，默认 2）。
- **修改 SceneCB 布局**：`lighting.hlsl` 的 `DefaultVertexCB` 有完整的偏移注释（0-175 floats），任何字段增减都必须同步维护，否则阴影矩阵/模式字段错位。
- **描述符重建**：`RenderLighting` 每帧调用 `CreateInputSRVs` 重建 t0/t1 描述符（CPU 开销很小），依赖 `m_srvHeap` 的固定顺序；若增加输入纹理，请同步更新堆大小（当前 2）。
- **深度格式适配**：`CreateInputSRVs` 按 `depthBuffer->GetDesc().Format` 自动选择 SRV 格式，替换深度缓冲格式时无需改代码，但需确认分支覆盖。
- **测试建议**：用 PIX 抓帧查看 LightRT，灰度图应能直观反映三种阴影模式差异；注意 `LightPass` 在 shadowmap 关闭时不会被调用，LightRT 保持上帧内容（主循环已用开关保护，避免误用）。

## PCSS 算法逐步说明

PCSS（Percentage Closer Soft Shadows）的目标是让阴影半影宽度随遮挡物距离动态变化：

```
// 步骤 1: 遮挡物搜索（16 采样）
searchRadius = LIGHT_SIZE * receiverDepth          // 初始搜索半径随深度增大
blockerDepth = FindBlockerDepth(uv, receiverDepth, searchRadius * 20.0)
  for i in 0..15:
    d = Sample(uv + poissonDisk[i] * searchRadius * texelSize)
    if (d < receiverDepth - 0.0005) 累加 blockerSum, count++
  无遮挡 → 返回 -1（完全照亮）

// 步骤 2: 半影估计
penumbra = LIGHT_SIZE * (receiverDepth - blockerDepth) / blockerDepth

// 步骤 3: 变半径 PCF（25 采样）
filterRadius = clamp(penumbra * 30.0, 1.0, 15.0)
shadow = 平均(25 次 (receiverDepth - 0.0005 > d ? 0 : 1))
```

参数之间的联动：`LIGHT_SIZE` 越大 → 搜索半径与半影都越大；`filterRadius` 上限 15 texel 防止过大的全屏软阴影。Poisson 盘采样点（`poissonDisk[25]`）为静态表，未按帧随机旋转，静止画面下采样图案可能可见。

## 初始化细节（LightPass::Initialize）

```
CreateSRVHeap():      m_srvHeap = 2 个 SHADER_VISIBLE 描述符
CreateLightRT(cmd):   R16G16B16A16_FLOAT 全屏 RT（初始 PIXEL_SHADER_RESOURCE）
                      RTV 堆 1 个；清除值 (0,0,0,1)
CreateShadowMap():    R32_TYPELESS 4096²（初始 DEPTH_WRITE）
                      DSV 堆 1 个（D32_FLOAT）
```

`Initialize` 接收 commandList 参数但当前未用于资源上传（阴影图与 LightRT 均无 CPU 数据拷贝），参数为接口预留。

## 与 ShadowPass 类的关系

| 维度 | `LightPass`（当前使用） | `ShadowPass` 类（备用） |
| --- | --- | --- |
| 阴影图所有权 | 内部 `m_shadowMap` | 内部 `m_shadowMap` |
| 渲染入口 | `RenderShadowMap`（子 Pass） | `Render`（独立） |
| PSO 工厂 | `CreateShadowPSO` | `CreateShadowPSO` |
| 阴影图 SRV | 每帧在 `CreateInputSRVs` 重建 | 初始化时创建一次，`GetShadowMapSRV()` 返回 |
| 是否被主循环调用 | 是 | 否 |

若未来启用 `ShadowPass` 类（如多灯），需要把 `lightPass->GetShadowMap()` 的消费路径替换为 `shadowPass->GetShadowMap()`，并确保两处 `shadowPso` 由同一工厂创建，避免参数漂移。