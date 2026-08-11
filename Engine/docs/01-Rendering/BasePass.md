# BasePass - G-Buffer 填充

> BasePass 是延迟渲染管线的第一站：把所有 Actor 的网格以材质 Pass 0（GBuffer 填充）渲染到 4 张离屏 RT（Albedo / Normal / ORM / Motion Vector）并写入场景深度缓冲。FEngine 中没有独立的 `BasePass` 类，该阶段由 `Scene::Render()` 实现，主循环中以事件名 `"BasePass"` 标记。

## 概述

BasePass 的职责：

1. 将 4 张离屏 RT 与深度缓冲绑定为渲染目标并清除；
2. 遍历场景中所有 Actor，为每个 Actor 绑定其材质对应的 PSO（`Shader::GetPSO(0)`）、材质常量缓冲（b1）与 Actor 独立场景常量缓冲（b0）；
3. 像素着色器输出 G-Buffer：`BaseColor`（RGB）、`Normal`（世界空间）、`ORM`（AO/Roughness/Metallic/ShadingModelID）、`MotionVector`（屏幕空间速度）；
4. 渲染完成后把所有 RT 与深度缓冲转换为 `PIXEL_SHADER_RESOURCE`，供后续 LightPass / GTAO / SSGI / ScreenPass / TAA 采样。

G-Buffer 数据流：

```
                     ┌──────────────────────────────────────┐
 Actor 网格 ──▶ BasePass ──▶ RT0 Albedo ──▶ ScreenPass(直接光/IBL), SsgiPass(间接光)
    │                    └──▶ RT1 Normal ──▶ ScreenPass, GtaoPass(法线半球), SsgiPass
    │                    └──▶ RT2 ORM    ──▶ ScreenPass(材质参数)
    │                    └──▶ RT3 Motion ──▶ TaaPass(重投影), SsgiPass(历史重投影)
    └─────────────────────▶ gDSRT 深度  ──▶ LightPass/GTAO/SSGI/ScreenPass/TAA
```

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/private/Scene.cpp` | `Scene::Render()` 即 BasePass 主体；`CreateOffscreenRTs()` 创建 4 张离屏 RT |
| `Engine/public/Scene.h` | `m_offscreenRTs`、`GetMotionVectorRT()`、Jitter 相关接口 |
| `Engine/Shader/StandardPBR.shader` | 默认材质 shader，Pass 0 名为 `GBufferPass`（实际执行的 G-Buffer 代码） |
| `Engine/Shader/Generated_GBufferPass.hlsl` | Shader 解析器生成的 Pass 0 编译产物（含自动生成材质常量/纹理声明） |
| `Engine/Shader/standard_pbr.hlsl` | 材质系统基准 PBR shader 源（旧版内嵌 GBuffer 写法，被 StandardPBR.shader 取代） |
| `Engine/Shader/gbuffer.hlsl` | 遗留的默认 shader（占位实现），当前管线不再使用 |
| `Engine/private/BattleFireDirect.cpp` | `CreateScenePSO()` 创建 4 RT 输出 PSO；`BeginOffscreen()` 设置视口 |

## 架构与数据流

### 离屏 RT 的创建

`Scene::CreateOffscreenRTs(width, height, rtvHeap, rtvDescriptorSize)` 创建 4 张纹理，均为 `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET`，初始状态 `PIXEL_SHADER_RESOURCE`：

| 槽位 | 用途 | 格式 | 数据内容 |
| --- | --- | --- | --- |
| RT0 | Albedo | `R16G16B16A16_FLOAT` | 材质 BaseColor × 纹理采样 |
| RT1 | Normal | `R16G16B16A16_FLOAT` | 世界空间法线（`[-1,1]`，无需解码） |
| RT2 | ORM | `R16G16B16A16_FLOAT` | `(AO, Roughness, Metallic, ShadingModelID/255)` |
| RT3 | Motion Vector | `R16G16_FLOAT` | 屏幕空间速度（`(currentNDC - previousNDC) * 0.5`） |

### BasePass 渲染流程（Scene::Render）

```
1. 取 4 个 RTV 句柄（m_rtvHeap 偏移 0..3）
2. 4 张 RT: PIXEL_SHADER_RESOURCE → RENDER_TARGET（深度缓冲保持 DEPTH_WRITE）
3. OMSetRenderTargets(4, rtvHandles, FALSE, &dsvHandle)
4. SetGraphicsRootSignature(rootSignature)
5. SetGraphicsRootDescriptorTable(1, 全局 srvHeap 起点)   // Bindless 纹理表
6. 清除 4 张 RT（黑色）+ 清除深度（1.0）
7. 遍历 m_actors:
   a. 材质/Shader 存在 → SetPipelineState(shader->GetPSO(0))
      （否则回退到传入的默认 gbufferPso）
   b. material->Bind(commandList, rootSignature, 2)      // b1 材质 CB
   c. actor->UpdateConstantBuffer(...)                   // 每 Actor 独立 CB（b0）
   d. SetGraphicsRootConstantBufferView(0, actor CB)
   e. mesh->Render(commandList, rootSignature)           // 一个 DrawCall
8. 无 Actor 时回退 m_staticMesh.Render（旧单 Mesh 路径）
9. 4 张 RT: RENDER_TARGET → PIXEL_SHADER_RESOURCE
10. gDSRT: DEPTH_WRITE → PIXEL_SHADER_RESOURCE
```

### PSO 配置（CreateScenePSO）

- 输入布局：`POSITION / TEXCOORD / NORMAL / TANGENT` 均为 `R32G32B32A32_FLOAT`（场景网格格式）；
- 4 个 RTV 格式：3 × `R16G16B16A16_FLOAT` + 1 × `R16G16_FLOAT`，`NumRenderTargets = 4`；
- DSV：`D24_UNORM_S8_UINT`；深度测试 `LESS_EQUAL`、写入开启；
- 光栅化：`CullMode = BACK`、`FrontCounterClockwise = TRUE`、`DepthClipEnable = TRUE`；
- 混合：**全部 RT 关闭混合**（直接覆盖写入）。

## 关键实现要点

### G-Buffer 像素输出（StandardPBR.shader Pass 0）

顶点着色器 `MainVS` 除常规 MVP 变换外，还计算：

- `tangentWS`：切线变换到世界空间（`mul(float4(tangent.xyz, 0), ModelMatrix)`）；
- `currentPositionCS = mul(CurrentViewProjectionMatrix, positionWS)`：**不带 Jitter** 的当前帧裁剪空间位置；
- `previousPositionCS = mul(PreviousViewProjectionMatrix, positionWS)`：上一帧位置（当前假设物体静态，未使用上一帧 ModelMatrix）。

像素着色器 `MainPS`：

```hlsl
float3 sampledBaseColor = SAMPLE_TEXTURE(BaseColorTexIndex, gSamAnisotropicWarp, uv).xyz;
float4 sampledNormal   = SAMPLE_TEXTURE(NormalTexIndex,    gSamAnisotropicWarp, uv);
float3 sampledOrm      = SAMPLE_TEXTURE(OrmTexIndex,       gSamAnisotropicWarp, uv).xyz;

float3 finalBaseColor = sampledBaseColor * BaseColor.xyz;
float  ao = sampledOrm.r;
float  finalRoughness = saturate(sampledOrm.g + Roughness);
float  finalMetallic  = saturate(sampledOrm.b + Metallic);

// TBN 矩阵与法线贴图解码
float3 tangentNormal = sampledNormal.xyz * 2.0 - 1.0;
float3 normalWS = normalize(mul(TBN, tangentNormal));

gbuffer.BaseColor  = float4(finalBaseColor, 1.0);
gbuffer.Normal     = float4(normalWS, 1.0);
gbuffer.ORM        = float4(ao, finalRoughness, finalMetallic, 1.0/255.0); // ShadingModelID=1
// Motion Vector
float2 mv = (currentNDC - previousNDC) * 0.5f;
gbuffer.MotionVector = mv;
```

要点：

- 纹理通过 **Bindless 槽位**（`t10+`）动态索引访问，纹理索引放在材质 CB 中，由 `MaterialInstance::Bind` 绑定；
- 粗糙度/金属度采用**相加调制**（`sampledOrm.g + Roughness`），与旧版 `standard_pbr.hlsl` 的相乘调制不同；
- `ShadingModelID` 编码在 ORM 的 alpha 通道（`id/255.0`），Screen.shader 中按 `uint(orm.a * 255.0 + 0.5)` 解包并 `switch` 选择 BRDF；
- Motion Vector 使用**无 Jitter** 的 `CurrentViewProjectionMatrix` 计算，避免 TAA 抖动混入速度场导致重影。

### 材质系统集成

- `RenderQueue "Deferred"`：StandardPBR 声明延迟渲染队列；
- 每个 Actor 的材质决定其 Pass 0 PSO（`shader->GetPSO(0)`），因此多材质场景会自动按材质切换 PSO；
- 若材质有未加载纹理（`material->HasPendingTextures()`），在绑定前调用 `material->LoadTexturesFromPaths(commandList)` 即时加载；
- 全局 SRV 堆（TextureManager 的堆）在 BasePass 前由主循环统一 `SetDescriptorHeaps`。

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `Scene::Render(commandList, pso, rootSignature)` | `Engine/public/Scene.h` | BasePass 主体，`pso` 为默认 GBuffer PSO 回退 |
| `Scene::CreateOffscreenRTs(w, h, rtvHeap, rtvSize)` | `Engine/public/Scene.h` | 创建 4 张离屏 RT（RTV 已创建） |
| `Scene::GetMotionVectorRT()` | `Engine/public/Scene.h` | 返回 RT3（`m_offscreenRTs[3]`） |
| `Scene::ResizeRenderTargets(w, h)` | `Engine/public/Scene.h` | 分辨率变更时重建 RT |
| `Scene::SetJitterOffset(x, y)` | `Engine/public/Scene.h` | 记录当前/上一帧 Jitter，供 SceneCB 使用 |
| `Scene::UpdatePreviousViewProjectionMatrix()` | `Engine/public/Scene.h` | 帧末保存当前帧 VP 矩阵（供下一帧 Motion Vector） |
| `CreateScenePSO(rootSig, vs, ps)` | `BattleFireDirect.h` | 创建 4 RT 输出的 GBuffer PSO |

## 配置与调参

- **材质参数**：`BaseColor / Roughness / Metallic` 在材质编辑器（MaterialEditorPanel）中修改，写入材质 CB（b1），无需重编译 shader；
- **纹理**：`BaseColorTex / NormalTex / OrmTex` 通过材质文件的纹理路径加载（如 `Content/Texture/color.dds` 等），支持 `GenerateMips` 与 BC 压缩导入；
- **TAA Jitter**：`Settings::GetTaaJitterScale()` 可整体缩放抖动幅度；Jitter 在 `Scene::Update` 中应用到投影矩阵 `r[2]` 行，BasePass 顶点位置随之亚像素偏移；
- **渲染分辨率**：跟随 `ResizeViewportRenderTargets()` 统一重建（`Scene::ResizeRenderTargets`）。

## 已知限制与 TODO

- **静态物体假设**：Motion Vector 使用上一帧 VP 矩阵 × 当前帧 ModelMatrix，动态物体/骨骼动画会产生错误速度；`previousPositionCS` 计算处有注释注明需要上一帧 ModelMatrix，尚未实现。
- **Jitter 补偿被注释**：`StandardPBR.shader` 中 `motionVector -= (JitterOffset - PreviousJitterOffset)` 一行被注释掉，理论上速度场应减去抖动差异以保持稳定。
- **RT 精度**：Albedo/Normal/ORM 使用 `R16G16B16A16_FLOAT`，HDR 颜色与法线精度够用，但若引入高动态范围输出需评估是否升级 `R32G32B32A32_FLOAT`。
- `gbuffer.hlsl` 与 `standard_pbr.hlsl` 为遗留实现（前者甚至输出全 1 占位），内容与当前渲染结果无关，易误导读者，建议后续删除或标注 deprecated。
- 无 RT 压缩/合并（如将 ShadingModelID 并入深度或 ORM alpha 已实现但无其他合并优化），带宽开销为 4 张全屏纹理。

## 维护注意事项

- **新增 GBuffer 通道**：需要同步修改 4 处——`CreateOffscreenRTs` 的格式表、`CreateScenePSO` 的 `RTVFormats`、`StandardPBR.shader` 的 `GBuffer` 输出结构、Screen.shader 的采样代码。
- **描述符顺序**：`m_rtvHeap` 中 RTV 顺序必须与 `m_offscreenRTs` 索引一致（0=Albedo, 1=Normal, 2=ORM, 3=Motion），ScreenPass 的 SRV 堆 t0-t3 同样依赖该顺序。
- **状态转换约定**：BasePass 结束时深度缓冲已转 `PIXEL_SHADER_RESOURCE`，LightPass 依赖该状态直接采样；若修改 BasePass 输出，请同步检查 `main.cpp` 中 GtaoPass/SSGI/ScreenPass 的注释与状态假设。
- **Actor CB 必须每帧更新**：`actor->UpdateConstantBuffer(...)` 在 `Scene::Update` 与 `Scene::Render` 内各调用一次，新增矩阵类参数时需在两个调用点同时传入，否则会出现阴影/速度场不同步。

## 关键实现要点（续）

### Scene::Render 的矩阵准备

在遍历 Actor 之前，`Scene::Render` 会重新计算一组矩阵（与 `Scene::Update` 中的逻辑保持一致）：

```
viewMatrix   = m_camera.GetViewMatrix()
projMatrix   = m_camera.GetProjectionMatrix()
if (m_jitterOffset != 0) {
    projMatrix.r[2][0] += jitter.x * 2.0 / viewportWidth    // 行主序投影矩阵 [2,0]
    projMatrix.r[2][1] += jitter.y * 2.0 / viewportHeight   // [2,1] 控制 NDC 偏移
}
invProjMatrix / invViewMatrix = 逆矩阵
lightViewProjMatrix = CalculateLiSPSMMatrix(...)            // 阴影矩阵
currentViewProjMatrix = viewMatrix * projMatrix(无Jitter)   // Motion Vector 专用
```

每个 Actor 的 `UpdateConstantBuffer` 接收同一套参数，但内部使用各自的 `modelMatrix`（Actor Transform），因此每个 Actor 拥有独立的 CB 资源与 GPU 地址。

### 旧式渲染路径

当 `m_actors` 为空时（如早期场景或调试），`Scene::Render` 回退到：

```cpp
commandList->SetPipelineState(pso);       // 传入的默认 gbufferPso
m_staticMesh.Render(commandList, rootSignature);
```

该路径不绑定材质 CB，也不写 Motion Vector 之外的其他差异，仅用于兼容旧代码。

### 清除策略

每帧 BasePass 对 4 张 RT 与深度缓冲做全量清除：

- RT0-3 清除为 `(0,0,0,1)`；
- 深度清除为 `1.0`（标准深度范围，`LESS_EQUAL` 比较）。

由于所有 RT 每帧都会被 BasePass 全覆盖写入，清除主要保证天空像素与未覆盖区域（RT3 速度场）具有确定值，避免 TAA/SSGI 采样到脏数据。

## 与相邻 Pass 的衔接

| 衔接点 | 说明 |
| --- | --- |
| BasePass → LightPass | 深度缓冲已转 `PIXEL_SHADER_RESOURCE`，LightPass 直接采样重建世界坐标；阴影图与 G-Buffer 无直接依赖 |
| BasePass → GtaoPass | GtaoPass 采样深度（t0）与 RT1 法线（t1），两者状态均已是 SRV |
| BasePass → SsgiPass | SsgiPass 采样 RT0 颜色、RT1 法线、RT3 速度与深度 |
| BasePass → ScreenPass | ScreenPass 采样 RT0/1/2 与深度，做最终光照合成 |
| BasePass → TaaPass | TAA 依赖 RT3 速度场与深度做历史重投影 |

## 常见问题排查

- **画面全黑**：检查 RT 状态是否卡在 `RENDER_TARGET`（后续 Pass 采样会读到未定义数据），或 Actor 的材质 PSO 是否为 `nullptr`（`Shader::GetPSO(0)` 未创建）。
- **阴影与画面不同步**：确认 `lightViewProjMatrix` 在 `Scene::Update` 与 `Scene::Render` 中计算一致，且 Actor CB 在 BasePass 前已更新。
- **TAA 重影**：先确认 RT3 速度场是否正常（TAA.hlsl `DEBUG_MODE 3` 可视化），再检查 `UpdatePreviousViewProjectionMatrix` 是否在帧末被调用（TAA 开启时主循环才调用）。
- **材质不生效**：确认 `material->Bind(commandList, rootSignature, 2)` 的第三个参数（根参数索引）为 2，与根签名 Slot 2（b1）一致。

## 性能提示

- BasePass 的 DrawCall 数量 = Actor 数量；大量静态网格可考虑后续合批或 Instance 化。
- 4 张 `R16` 全屏 RT 的写入带宽是主要开销之一；若关闭 TAA 与 SSGI，RT3 速度场可考虑延迟到需要时再渲染。
- Bindless 纹理采样依赖 TextureManager 的全局 SRV 堆，尽量保持材质纹理在同一堆内，减少堆切换。