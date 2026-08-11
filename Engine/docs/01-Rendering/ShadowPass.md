# ShadowPass - 阴影深度渲染

> ShadowPass 负责从平行光视角把场景深度渲染到 Shadow Map，为延迟光照提供阴影信息。FEngine 中存在两条阴影实现路径：独立的 `ShadowPass` 类（`Engine/private/ShadowPass.cpp`）与 `LightPass` 内嵌的子 Pass `RenderShadowMap()`。**当前主循环实际使用的是后者**（`main.cpp` 通过 `lightPass->RenderDirectLight` 驱动），`ShadowPass` 类保留为可独立复用的实现。两者共用 `shadowdepth.hlsl` 着色器。

## 概述

Shadow Map 渲染的核心思想：把相机换成平行光位置，用正交投影渲染场景深度；延迟光照阶段把片元变换到光源裁剪空间，与阴影图深度比较判断是否被遮挡。

```
                   平行光方向 L
                        │
                        ▼
         ┌─────────────────────────────┐
         │  光源正交视锥（相机中心对齐） │
         │  4096 x 4096 Shadow Map      │
         └─────────────────────────────┘
                        深度写入: mul(LightViewProjectionMatrix, 世界坐标)
                                │
                                ▼
   shadowdepth.hlsl: ShadowDepthVS（变换到光源裁剪空间）+ ShadowDepthPS（空，仅深度）
                                │
                                ▼
   lighting.hlsl: 片元重建世界坐标 → 光源裁剪空间 → 比较深度
                → Hard / PCF / PCSS 阴影因子 → LightRT（灰阶图）
```

Shadow Map 资源规格：`4096 × 4096`、`DXGI_FORMAT_R32_TYPELESS`（DSV/SRV 视图均用 `R32_FLOAT`）、单 mip、`D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL`、清除值深度 1.0。

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/public/ShadowPass.h` | `ShadowPass` 类声明（独立实现） |
| `Engine/private/ShadowPass.cpp` | 独立 ShadowPass 的资源/PSO/Render 实现 |
| `Engine/public/LightPass.h` | `LightPass::RenderShadowMap / CreateShadowPSO / ResizeShadowMap` |
| `Engine/private/LightPass.cpp` | 当前实际使用的阴影子 Pass（含 PSO 与资源管理） |
| `Engine/Shader/shadowdepth.hlsl` | 阴影深度着色器（`ShadowDepthVS / ShadowDepthPS`） |
| `Engine/private/Scene.cpp` | `CalculateLiSPSMMatrix / CalculateStandardShadowMatrix` 阴影矩阵计算 |

## 架构与数据流

### 阴影矩阵（Scene::CalculateLiSPSMMatrix）

`Scene::CalculateLiSPSMMatrix(lightDir, cameraView, cameraProj)` 当前直接返回 `CalculateStandardShadowMatrix(lightDir)`——即 **LiSPSM 入口已保留，但实现退化为标准正交阴影矩阵**：

1. `L = normalize(-lightDir)`，光源放在 `sceneCenter + L * (shadowSize * 4)`；
2. `up` 向量在接近平行时切换（`abs(dot(L, up)) > 0.99` 时改用 X 轴）；
3. 以相机为中心构造 8 个角点包围盒（`m_shadowOrthoSize` 默认 20），变换到光源空间取 AABB；
4. `XMMatrixOrthographicOffCenterLH(minX-pad, maxX+pad, minY-pad, maxY+pad, nearZ, farZ)`，其中 `pad = 5.0`、`nearZ = max(0.1, minZ - pad)`；
5. 返回 `lightView * lightProj`，写入 `SceneCBData.lightViewProjMatrix`（HLSL 中即 `LightViewProjectionMatrix`）。

### 渲染子流程（LightPass::RenderShadowMap）

```
1. RSSetViewports/ScissorRects(4096 x 4096)
2. ClearDepthStencilView(dsv, CLEAR_FLAG_DEPTH, 1.0)
3. OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle)   // 仅深度，无颜色
4. SetGraphicsRootSignature + SetPipelineState(shadowPso)
5. 绑定 SceneCB（b0，含 LightViewProjectionMatrix）
6. 遍历所有 Actor：
     绑定 Actor CB（含 ModelMatrix）→ mesh->Render()
7. Shadow Map: DEPTH_WRITE → PIXEL_SHADER_RESOURCE（供 lighting.hlsl 采样）
```

### shadowdepth.hlsl

```hlsl
VSOutput ShadowDepthVS(VertexInput input) {
    float4 positionWS = mul(ModelMatrix, float4(input.position.xyz, 1.0));
    output.position   = mul(LightViewProjectionMatrix, positionWS);  // 光源裁剪空间
    return output;
}
void ShadowDepthPS(VSOutput input) { /* 空实现，深度由光栅化写入 */ }
```

输入顶点格式与 StaticMeshComponent 一致：`POSITION/TEXCOORD/NORMAL/TANGENT` 各 `float4`。

## 关键实现要点

### 两个 PSO 的差异

| 属性 | `ShadowPass::CreateShadowPSO`（独立类） | `LightPass::CreateShadowPSO`（当前使用） |
| --- | --- | --- |
| CullMode | `BACK` | `FRONT`（渲染背面，减少自阴影伪影） |
| FrontCounterClockwise | `FALSE` | `FALSE` |
| DepthBias | `100000` | `5000` |
| SlopeScaledDepthBias | `1.0` | `2.0` |
| DepthFunc | `LESS` | `LESS` |
| NumRenderTargets | 0 | 0 |
| DSVFormat | `D32_FLOAT` | `D32_FLOAT` |

深度偏移（DepthBias + SlopeScaledDepthBias）用于防止 Shadow Acne；独立类数值更激进，实际管线使用 `5000/2.0` 配合正面剔除。

### 资源创建（ShadowPass 类）

- `CreateDescriptorHeaps()`：DSV 堆（1 个，非 shader 可见）+ SRV 堆（1 个，`SHADER_VISIBLE`）；
- `CreateShadowMapResource()`：创建 `R32_TYPELESS` 资源 → DSV（`D32_FLOAT`）→ SRV（`R32_FLOAT`，`TEXTURE2D`，Mip=1）；
- `Resize(newSize)`：`WaitForCompletionOfCommandList()` 后 `m_shadowMap.Reset()` 并重建（DSV/SRV 描述符不变，可复用堆）。

### 独立类与 LightPass 的重复

`ShadowPass::Render` 与 `LightPass::RenderShadowMap` 逻辑几乎一致（视口、清除、绑定、遍历 Actor、状态转换），区别仅在 PSO 参数与阴影图所有权。当前维护时应以 `LightPass` 的实现为准，独立类保留用于潜在的多灯阴影扩展。

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `ShadowPass(int shadowMapSize = 4096)` / `Initialize()` | `ShadowPass.h` | 构造与初始化（描述符堆 + 阴影图资源） |
| `ShadowPass::Render(cmd, pso, rootSig, scene)` | `ShadowPass.h` | 独立渲染入口 |
| `ShadowPass::GetShadowMap() / GetShadowMapSRV() / GetShadowMapSize()` | `ShadowPass.h` | 阴影图资源/SRV 句柄/尺寸访问 |
| `ShadowPass::Resize(newSize)` | `ShadowPass.h` | 阴影图分辨率变更 |
| `LightPass::RenderDirectLight(cmd, shadowPso, lightPso, rootSig, scene, depthBuffer)` | `LightPass.h` | 主循环入口（含阴影子 Pass） |
| `LightPass::GetShadowMap()` | `LightPass.h` | 当前阴影图资源 |
| `Scene::SetShadowOrthoSize(size)` / `SetShadowMode(mode)` | `Scene.h` | 阴影范围与模式配置 |

## 配置与调参

| 参数 | 默认值 | 入口 | 说明 |
| --- | --- | --- | --- |
| 阴影图分辨率 | 4096 | `LightPass` 构造参数 / `ResizeShadowMap()` | 越大越清晰，带宽与 PCF 开销增大 |
| `m_shadowOrthoSize` | 20.0 | UI `SliderFloat("Shadow Ortho Size", 10~500)` | 正交投影覆盖范围 |
| 阴影模式 | 2（PCSS） | UI `Combo("Shadow Mode")` | 0=Hard、1=PCF、2=PCSS，写入 SceneCB |
| 阴影开关 | true | UI `Checkbox("Enable Shadowmap")` | 关闭时 LightPass 整段跳过，ScreenPass 传 `nullptr` 并回退白色纹理 |
| DepthBias / SlopeBias | 5000 / 2.0 | `LightPass::CreateShadowPSO` 代码 | 调大消除 Acne，过大会导致 Peter Panning（悬浮阴影） |

UI 入口位于 `main.cpp` 的 MainLight 窗口（`showMainLightWindow`），调整后立即写入 Scene 状态，下一帧生效。

## 已知限制与 TODO

- **LiSPSM 未真正实现**：`CalculateLiSPSMMatrix` 只是 `CalculateStandardShadowMatrix` 的转发，尚无透视扭曲优化；大面积场景在极端光照角度下阴影精度不足。
- **单方向光**：ShadowPass/LightPass 只维护一张阴影图，没有级联（CSM）与多灯支持。
- **无 PCF 硬件采样**：PCF/PCSS 都在 `lighting.hlsl` 中软实现（`g_ShadowSampler` 已声明但未被使用），性能与质量均有优化空间。
- **独立 ShadowPass 类未被使用**：与 LightPass 内嵌实现重复，且两套 PSO 参数不同（剔除面与偏移），容易造成“改了一个另一个没改”的维护陷阱。
- **阴影图无 blur**：PCSS 的半影依赖采样密度，边缘在低分辨率下会有噪点，暂无后处理滤波。

## 维护注意事项

- **修改阴影矩阵时**：`Scene::Update` 与 `Scene::Render` 两处都会调用 `CalculateLiSPSMMatrix`，必须保持一致（前者用于 SceneCB，后者用于 Actor CB）。
- **阴影图状态**：渲染结束后必须转回 `PIXEL_SHADER_RESOURCE`；`RenderDirectLight` 末尾会把阴影图转回 `DEPTH_WRITE` 为下一帧准备，两个转换点都不可省略。
- **Actor CB 依赖**：阴影渲染使用 Actor 自己的 CB（含 ModelMatrix），新增影响物体变换的字段时需同步更新 `Actor::UpdateConstantBuffer`。
- **分辨率变更**：`LightPass::ResizeShadowMap(newSize)` 会释放并重建阴影图资源，但**不重建** DSV/SRV 堆（描述符仍有效），改动堆结构时需注意此假设。
- **调试建议**：把 `ScreenPass` 的 t5 输入临时替换为 `lightPass->GetShadowMap()`（`R32_FLOAT` SRV）即可在 PIX 中直接查看阴影图内容。

## 与延迟光照的衔接（lighting.hlsl）

Shadow Map 在 LightPass 子 Pass B 中以 `t1` 绑定（`LightPass::CreateInputSRVs` 中深度为 t0）。`lighting.hlsl` 的三种阴影算法：

### Hard（ShadowMode < 0.5）

`CalculateHardShadow(positionWS)`：单点采样，`(currentDepth - bias > shadowMapDepth) ? 0 : 1`，`bias = 0.001`。边界外（UV 越界或深度越界）返回 1.0（视为无阴影）。

### PCF（0.5 <= ShadowMode < 1.5）

`CalculateShadow(positionWS)`：3×3 邻域采样（`texelSize = 1/2048`，注意此函数内写死的 2048 与 4096 阴影图不一致，属于遗留参数），平均 9 次比较结果，`bias = 0.001`。

### PCSS（ShadowMode >= 1.5）

`CalculateShadowPCSS(positionWS)` 三步：

```
步骤1: FindBlockerDepth   — 16 个 Poisson 点，在 searchRadius = LIGHT_SIZE * receiverDepth * 20 内
                            统计深度小于 receiverDepth - 0.0005 的遮挡物平均深度
步骤2: EstimatePenumbra   — penumbra = LIGHT_SIZE * (receiver - blocker) / blocker
步骤3: PCF_Filter         — 25 个 Poisson 点（poissonDisk[25] 静态表），
                            滤波半径 = clamp(penumbra * 30, 1, 15) texel
```

常量：`SHADOW_MAP_SIZE = 4096.0`、`LIGHT_SIZE = 0.02`、`BLOCKER_SEARCH_SAMPLES = 16`、`PCF_SAMPLES = 25`。

LightPass 输出为**灰阶阴影因子图**（LightRT，`R16G16B16A16_FLOAT`）：`LightPS` 读取深度，`depth >= 1.0`（天空）直接输出白色，否则按 ShadowMode 选择算法并输出 `float4(shadow, shadow, shadow, 1)`。Screen.shader 中采样该图 `.r` 后乘以直接光照：`finalColor = directLighting * shadow * 6.0 + ambient`。

## 常见问题排查

- **阴影出现 Acne（自阴影噪点）**：增大 `DepthBias` 或 `SlopeScaledDepthBias`；若已使用 `LightPass` 的 PSO，注意独立 `ShadowPass` 的数值更大，两者混用会造成表现不一致。
- **阴影悬浮（Peter Panning）**：偏移过大导致，减小 DepthBias。
- **阴影边缘锯齿/块状**：提高阴影图分辨率（`ResizeShadowMap`），或切换到 PCSS 模式并增大 `LIGHT_SIZE`。
- **阴影整体消失**：检查 `m_shadowOrthoSize` 是否覆盖场景范围（UI 中可调 10~500）；检查 Actor CB 是否每帧更新（阴影渲染使用 Actor 自己的 CB）。
- **阴影闪烁（相机移动时）**：`CalculateStandardShadowMatrix` 中有纹素网格对齐逻辑（sceneCenter 对齐到 texel），若修改了该函数请保留对齐步骤。

## 后续演进建议

1. 将 `ShadowPass` 类与 `LightPass::RenderShadowMap` 合并为单一实现，消除重复；
2. 实现真正的 LiSPSM（透视扭曲）或迁移到 CSM 级联；
3. 阴影图增加 2×2 PCF 硬件采样或 VSM（方差阴影图）以支持后续模糊；
4. 为阴影模式提供每灯配置，而不是全局单一模式。

## Shadow Map 资源生命周期

### 初始化（LightPass 路径）

```
LightPass(width, height, shadowMapSize=4096)
  └─ Initialize(cmdList)
       ├─ CreateSRVHeap()          // 2 个 SRV: t0=深度, t1=ShadowMap
       ├─ CreateLightRT(cmdList)   // 延迟光照输出 RT
       └─ CreateShadowMapResource()
            ├─ DSV 堆（1 个，非 shader 可见）
            ├─ 创建 R32_TYPELESS 资源（初始 DEPTH_WRITE）
            ├─ CreateDepthStencilView（D32_FLOAT）
            └─ （SRV 在 CreateInputSRVs 中按需创建）
```

### 每帧生命周期

```
帧首: ShadowMap 处于 DEPTH_WRITE（上帧 RenderDirectLight 末尾转回）
  ├─ RenderShadowMap: ClearDepth(1.0) → 渲染 → DEPTH_WRITE → PIXEL_SHADER_RESOURCE
  ├─ RenderLighting:  CreateInputSRVs 创建 SRV（t1）→ 全屏四边形采样
  └─ RenderDirectLight 末尾: PIXEL_SHADER_RESOURCE → DEPTH_WRITE（为下一帧准备）
```

### 状态转换清单（LightPass.cpp）

| 位置 | 转换 | 目的 |
| --- | --- | --- |
| `RenderShadowMap` 末尾 | `DEPTH_WRITE → PIXEL_SHADER_RESOURCE` | lighting.hlsl 采样 |
| `RenderLighting` 开头 | LightRT `PIXEL_SHADER_RESOURCE → RENDER_TARGET` | 输出阴影因子 |
| `RenderLighting` 末尾 | LightRT `RENDER_TARGET → PIXEL_SHADER_RESOURCE` | ScreenPass 采样 |
| `RenderDirectLight` 末尾 | ShadowMap `PIXEL_SHADER_RESOURCE → DEPTH_WRITE` | 下一帧深度写入 |

## 光照旋转与阴影联动

`main.cpp` 的 MainLight 窗口提供 `Rotate X/Y/Z` 滑块（弧度，`lightRot[3]`），修改后调用：

```
g_scene->SetLightRotation(lightRot[0], lightRot[1], lightRot[2]);
```

`Scene::Update` 中初始光方向为 `(-1, -1, 1)`，经旋转矩阵变换后得到 `m_lightDirection`，再归一化写入 SceneCB 的 `lightDirection` 字段；`CalculateStandardShadowMatrix` 使用同一方向计算光源视矩阵，因此**旋转光照会同时更新直接光方向与阴影图投影**，无需额外同步。

## 数据格式速查

| 项目 | 值 |
| --- | --- |
| 阴影图格式 | `DXGI_FORMAT_R32_TYPELESS` |
| DSV 视图 | `DXGI_FORMAT_D32_FLOAT`，`TEXTURE2D` |
| SRV 视图 | `DXGI_FORMAT_R32_FLOAT`，`TEXTURE2D`，Mip 1 |
| 默认尺寸 | 4096 × 4096 |
| 清除值 | 深度 1.0，模板 0 |
| 采样器 | `g_Sampler`（s0 PointWrap），阴影比较采样器 `g_ShadowSampler`（s1）已声明未使用 |
| 深度偏移（实际使用） | DepthBias=5000，SlopeScaledDepthBias=2.0，Clamp=0 |
| 剔除（实际使用） | `CULL_MODE_FRONT`，`FrontCounterClockwise=FALSE` |
| 深度函数 | `LESS`，写掩码 `ALL` |