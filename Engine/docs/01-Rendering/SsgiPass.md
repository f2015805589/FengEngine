# SsgiPass - 屏幕空间全局光照

> SsgiPass 在 GtaoPass 之后、SkyPass 之前执行，用屏幕空间光线步进（ray marching）估算间接漫反射光照，并通过低分辨率渲染、时序累积与双向模糊降低开销和噪点。SSGI 结果以 `(RGB 间接光, A 命中率权重)` 形式输出，ScreenPass 依据命中率在 IBL 与 SSGI 之间做 lerp 混合。

## 概述

```
输入: 深度 / BaseColor(RT0) / Normal(RT1) / Velocity(RT3) / 历史帧
        │
        ▼
1. SSGI Raw Pass（低分辨率，默认 1/4）
   SSGI.hlsl: 余弦加权半球采样 → 屏幕空间射线 → 深度比较命中
              → 蒙特卡洛积分 + 时序累积（重投影 + 颜色裁剪）
        │
        ▼
2. History 保存（CopyResource: Raw → History，ping-pong）
        │
        ▼
3. Upsample Pass（SSGIUpsample.hlsl: 低分辨率 → 全分辨率，双线性）
        │
        ▼
4. Blur H → Blur V（SSGIBlur.hlsl: 5-tap 高斯 × 深度边缘权重）
        │
        ▼
输出: m_ssgiFinalRT（全分辨率 R16G16B16A16_FLOAT）→ ScreenPass（t7）
```

- 关闭时 `GetSSGITexture()` 返回 1×1 黑色纹理（RGB=0 且 A=0，表示无命中、无间接光）；
- 支持 Full / Half / Quarter 三档内部渲染分辨率（`SSGIResolutionScale`）；
- 时序累积要求场景提供 Motion Vector（RT3），与 TAA 共享速度场。

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/public/SsgiPass.h` | SsgiPass 类、`GIType` / `SSGIResolutionScale` 枚举、`SsgiConstants` |
| `Engine/private/SsgiPass.cpp` | 7 张 RT、32 槽 SRV 堆、噪声/默认纹理、5 个子 Pass 调度 |
| `Engine/Shader/SSGI.hlsl` | 射线构建（`CreateSSRay`）、追踪（`TraceSSRay`）、时序累积、`PSMain` |
| `Engine/Shader/SSGIUpsample.hlsl` | 升采样（`PSMain`，LinearClamp 双线性） |
| `Engine/Shader/SSGIBlur.hlsl` | 双向模糊（`PSMainH / PSMainV`，5-tap + 深度边缘） |
| `Engine/Shader/SSGIDepthMax.hlsl` | 深度最大值 Pass（已实现但当前未被调度，见已知限制） |
| `Engine/main.cpp` | `ssgiPso / ssgiDepthPso / ssgiUpsamplePso / ssgiBlurHPso / ssgiBlurVPso` 编译与调度 |

## 架构与数据流

### 渲染目标（7 张）

| RTV 索引 | 资源 | 分辨率 | 格式 | 用途 |
| --- | --- | --- | --- | --- |
| 0 | `m_depthMaxPingRT` | SSGI 分辨率 | `R32_FLOAT` | 深度最大值（未使用） |
| 1 | `m_depthMaxPongRT` | SSGI 分辨率 | `R32_FLOAT` | 深度最大值（未使用） |
| 2 | `m_ssgiRawRT` | SSGI 分辨率 | `R16G16B16A16_FLOAT` | 光线步进输出 |
| 3 | `m_ssgiBlurTempRT` | 全分辨率 | `R16G16B16A16_FLOAT` | Upsample 输出 / 纵向模糊输出 |
| 4 | `m_ssgiFinalRT` | 全分辨率 | `R16G16B16A16_FLOAT` | 最终输出（由 BlurTemp 拷贝而来） |
| 5 | `m_historyRT1` | SSGI 分辨率 | `R16G16B16A16_FLOAT` | 历史帧 ping |
| 6 | `m_historyRT2` | SSGI 分辨率 | `R16G16B16A16_FLOAT` | 历史帧 pong |

`m_ssgiWidth/Height = viewport / (1 << resolutionScale)`：Full=1/1、Half=1/2、Quarter=1/4。

### SRV 布局（m_srvHeap，32 个描述符）

| 槽位 | 阶段 | 内容 |
| --- | --- | --- |
| 0-6 | Raymarch（`kRaymarchSrvStart=0`） | t0 DepthMax、t1 BaseColor、t2 Normal、t3 Noise、t4 深度、t5 History、t6 Velocity |
| 7-8 | Blur H（`kBlurHSrvStart=7`） | t0 输入纹理、t1 深度 |
| 9-10 | Blur V（`kBlurVSrvStart=9`） | t0 输入纹理、t1 深度 |
| 11-12 | Upsample（`kUpsampleSrvStart=11`） | t0 低分辨率 SSGI、t1 深度 |

### SsgiConstants

```cpp
struct SsgiConstants {
    XMFLOAT2 resolution;         // SSGI 渲染分辨率（低分辨率）
    XMFLOAT2 inverseResolution;
    float radius;                // 默认 6.0
    float intensity;             // 默认 1.0
    int stepCount;               // 默认 128
    int directionCount;          // 默认 64
    int frameCounter;            // 帧计数（时域抖动与累积）
    int depthPyramidPasses;      // 默认 3（当前未使用）
    float depthThickness;        // 默认 0.02
    float temporalBlend;         // 首帧 0.0，之后 0.95
    XMFLOAT2 padding;
};
```

`UpdateConstants()` 中 `temporalBlend = (m_frameCounter == 0) ? 0.0f : 0.95f`，`m_frameCounter` 在 Render 末尾递增、Resize/SetResolutionScale 时清零。

## 关键实现要点

### 屏幕空间射线（SSGI.hlsl）

`CreateSSRay(originV, dirV, tmax)` 把视图空间射线两端点投影到像素空间：

- 像素坐标第三分量存 **`1/viewZ`（倒数深度）**，保证沿屏幕空间直线插值时深度透视正确；
- 射线方向归一化为“每步约 1 个 SSGI 像素”；屏幕投影长度 < 0.5 像素视为无效射线；
- 若射线朝向近平面（`dirV.z < 0`），裁剪 `tmax` 防止穿越近平面。

### 光线步进（TraceSSRay）

```
marchStep = 1.01（像素）
maxSteps = min(SSGIStepCount, 256)
for t in [marchStep .. ray.tmax]:
    p = ray.o + ray.d * t
    pixel = int2(p.xy)；重复像素跳过
    采样场景 NDC 深度；天空（>=1.0）跳过
    sceneLinearDepth = GetLinearDepth(sceneDepthNdc)
    rayLinearDepth   = 1.0 / p.z
    depthDiff = rayLinearDepth - sceneLinearDepth
    adaptiveThickness = DepthThickness + |sceneLinearDepth| * 0.02
    命中条件: depthDiff > 0 && depthDiff < adaptiveThickness
```

命中后累加 `hitAlbedo * distAtt`（`distAtt = saturate(1 - dist/adjustedRadius)`），未命中射线不贡献光照（留给 IBL 通过 alpha 权重补充）。

### 半球采样与降噪

- 方向采样：`CosineWeightedHemisphere(normalV, rand2)` + R2 准随机序列（`R2Sequence`）；
- 抖动：`noise1`（视图空间位置哈希）+ 黄金比例帧噪声（`frac(frameCounter * 0.618)`），帧间抖动幅度加大以配合时域累积；
- 分辨率补偿：Half 分辨率强度 ×1.15、Quarter ×1.3（`intensityScale`）；半径相应缩小（Half: ÷2.6、Quarter: ÷3）；
- 输出 alpha = 命中率 `hitCount / directions`。

### 时序累积（Temporal Accumulation）

```
velocity = VelocityTexture.Sample(uv).xy
historyUV = uv - velocity
motionAmount = length(velocity * SSGIResolution)
fastMotion = motionAmount > 5.0            // 快速运动放弃历史
validHistory = historyUV 在 [0,1] 且 !fastMotion 且 frameCounter > 0

if validHistory:
    当前帧 3×3 邻域 → colorMin/colorMax/colorAvg（颜色裁剪盒）
    历史帧 3×3 搜索（深度差评分 depthScore = |Δdepth| * 1000，bestScore > 3 拒绝历史）
    history = clamp(bestHistory, colorMin, colorMax)
    blend = temporalBlend * 0.95 * saturate(1 - motionAmount * 0.1)
    result = lerp(indirect, history, blend)
```

### 升采样与模糊

- `SSGIUpsample.hlsl`：LinearClamp 双线性采样低分辨率 SSGI；天空像素输出 0；
- `SSGIBlur.hlsl`：5-tap 权重 `{0.204164, 0.304005, 0.193783, 0.07208, 0.016}`，深度边缘权重 `exp(-|Δdepth| * 80)`；模糊强度按分辨率放大（Half ×1.5、Quarter ×2.5）；
- 横向模糊写 `m_ssgiFinalRT`，纵向模糊写回 `m_ssgiBlurTempRT`，最后 `CopyResource` 把结果拷到 `m_ssgiFinalRT`（因此最终输出恒为 `m_ssgiFinalRT`）。

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `SsgiPass::Initialize(w, h)` | `SsgiPass.h` | 创建 RT/SRV/CB/默认纹理/噪声纹理 |
| `SsgiPass::Render(cmd, depthMaxPso, ssgiPso, upsamplePso, blurHPso, blurVPso, rootSig, depthBuffer, baseColorRT, normalRT, velocityRT)` | `SsgiPass.h` | 每帧入口 |
| `SsgiPass::CreateDepthPSO / CreateColorPSO` | `SsgiPass.h` | PSO 工厂（`R32_FLOAT` / `R16G16B16A16_FLOAT`） |
| `SsgiPass::GetSSGITexture()` | `SsgiPass.h` | 最终 GI 纹理（关闭时返回 1×1 黑色） |
| `SsgiPass::SetGIType / SetRadius / SetIntensity / SetStepCount / SetDirectionCount / SetResolutionScale / SetDepthPyramidPasses` | `SsgiPass.h` | 运行时参数 |
| `SsgiPass::Resize(w, h)` | `SsgiPass.h` | 分辨率变更（重建 RT，重置帧计数与历史） |

## 配置与调参

| 参数 | 默认 | UI 入口 | 说明 |
| --- | --- | --- | --- |
| GI 开关 | Off（`GIType::Off`） | Setting 窗口 `Combo("GI")`：Close/SSGI | 开启时同时调用 `g_scene->SetGIType(1)` |
| SSGI 分辨率 | Quarter（默认 `SSGIResolutionScale::Quarter`） | `Combo("SSGI Resolution")`：Full/Half/Quarter | 注意头文件注释写“默认1/2分辨率”，实际默认 1/4 |
| Directions | 64 | `SliderInt("SSGI Directions", 8~64)` | 半球方向数 |
| Steps | 128 | `SliderInt("SSGI Steps", 4~256)` | 每射线步数（shader 上限 256） |
| Radius | 6.0 | `SliderFloat("SSGI Radius", 0.1~6.0)` | 间接光搜索半径 |
| Intensity | 1.0 | `SliderFloat("SSGI Intensity", 0.1~5.0)` | 间接光强度 |
| DepthThickness | 0.02 | 代码常量 | 命中厚度基数 |
| TemporalBlend | 0.95 | 代码常量 | 历史帧混合权重 |

## 已知限制与 TODO

- **深度金字塔未启用**：`Render` 中 `(void)depthMaxPso` 表明 DepthMax Pass 未执行，`SSGIDepthMax.hlsl` 与 `m_depthMaxPingRT/PongRT` 处于“已实现但未接入”状态，`depthPyramidPasses` 参数也未被 shader 使用；
- **Upsample 非深度感知**：`SSGIUpsample.hlsl` 注释称“深度感知双线性”，实际代码只是普通双线性 + 天空剔除，跨深度边缘会渗色；
- **噪声纹理用 `rand()`**：`CreateNoiseTexture` 的 4×4 噪声用 `rand()` 生成，未做种子控制，且 `rand()` 的分布质量一般；
- **快速运动拒绝阈值偏大**：`motionAmount > 5` 像素才放弃历史，旋转相机时可能出现残影；
- **历史帧无过期保护**：场景大幅变化（加载关卡、移动物体）时历史帧不会主动失效，只能依赖颜色裁剪兜底；
- **全分辨率模糊成本高**：Upsample + Blur H + Blur V 三个全屏 Pass 是性能大头，Quarter 分辨率下尤其明显。

## 维护注意事项

- **SRV 槽位宏**：`kRaymarchSrvStart / kUpsampleSrvStart / kBlurHSrvStart / kBlurVSrvStart` 分散在 `Render` 函数内，新增阶段时保持 32 槽总量与各起点不重叠；
- **历史 ping-pong**：`m_useHistory2` 决定读写目标；`RenderToSwapChain` 读取历史、`CopyResource` 写另一张；Resize 或切换分辨率时必须 `m_frameCounter = 0; m_useHistory2 = false` 重置，否则首帧会读到脏历史；
- **与 TAA 的共享**：SSGI 复用 RT3 速度场做历史重投影，若未来把速度场改为半分辨率或更改编码（如除以宽高），需同步更新 `SSGI.hlsl` 的 `motionAmount` 计算；
- **关闭时行为**：`GetSSGITexture()` 返回 1×1 黑色纹理，ScreenPass 用 `ssgiWeight=0` 走纯 IBL，因此“关闭 GI”与“开启但全未命中”在合成上等价；
- **调试建议**：用 `GetSSGITexture()` 直接可视化（ScreenPass t7 是最终输出）；要查看 Raw 结果可临时把 `m_ssgiFinalRT` 的拷贝目标改为 `m_ssgiRawRT` 的直出。

## 子 Pass 状态转换时序

以 Quarter 分辨率（scale=4）为例，`Render` 内所有转换由 lambda `transition(res, before, after)` 完成（状态相同则跳过）：

```
[Raymarch]        m_ssgiRawRT   PS_RESOURCE → RENDER_TARGET → 绘制 → → PS_RESOURCE
[History 保存]     outputHistoryRT: PS_RESOURCE → COPY_DEST
                  m_ssgiRawRT:    PS_RESOURCE → COPY_SOURCE → CopyResource → 各自转回
[Upsample]        m_ssgiBlurTempRT: PS_RESOURCE → RENDER_TARGET → 绘制 → → PS_RESOURCE
[Blur H]          m_ssgiFinalRT:   PS_RESOURCE → RENDER_TARGET → 绘制 → → PS_RESOURCE
[Blur V]          m_ssgiBlurTempRT: PS_RESOURCE → RENDER_TARGET → 绘制 → → PS_RESOURCE
[最终拷贝]         m_ssgiFinalRT:   PS_RESOURCE → COPY_DEST
                  m_ssgiBlurTempRT: PS_RESOURCE → COPY_SOURCE → CopyResource → 各自转回
```

所有 RT 初始化状态为 `PIXEL_SHADER_RESOURCE`；每帧以该状态进入、以该状态结束，符合主循环对其他 Pass 的假设。

## 与 ScreenPass 的混合协议

SSGI 输出纹理的 alpha 通道是命中率权重，Screen.shader 中的使用方式：

```hlsl
float4 ssgiData = SSGITexture.Sample(gSamPointClamp, input.uv);
float3 ssgi = ssgiData.rgb;
float ssgiWeight = ssgiData.a;

// giType > 0.5（SSGI 模式）时：按命中率在 IBL 与 SSGI 之间 lerp
ambient = (giType > 0.5) ? lerp(ambient, ssgi, ssgiWeight) : ambient;
```

- 命中率高（如封闭室内）→ 间接光几乎全部来自 SSGI；
- 命中率低（开阔场景、射线出屏）→ 回退到 IBL（天空环境光）；
- 关闭 SSGI 时黑色 1×1 纹理的 alpha=0 → 等效于 `lerp(ambient, 0, 0) = ambient`，行为正确。

## 常见问题排查

- **SSGI 全黑**：检查 `giType` 是否同时设置了 Scene（`g_scene->SetGIType`）与 Pass（`ssgiPass->SetGIType`）；主循环中两者必须一致。
- **画面闪烁/噪点**：确认 `m_frameCounter` 持续递增（首帧 `temporalBlend=0` 是预期行为）；提高 `Directions` 或降低 `Steps` 对噪声影响最明显。
- **间接光过曝**：降低 `SSGI Intensity`；Quarter 分辨率自带 ×1.3 强度补偿，切换到 Full 后亮度会下降属正常现象。
- **残影（相机移动后）**：调小 `motionAmount` 阈值（当前 5.0 像素）或降低 `TemporalBlend`。
- **升级分辨率后画面突变**：`SetResolutionScale` 会重置帧计数与历史缓冲，首帧用当前帧结果，第二帧起逐渐累积，属预期行为。