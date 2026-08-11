# GtaoPass - GTAO / SSAO 环境光遮蔽

> GtaoPass 在 LightPass 之后、SkyPass 之前执行，从深度缓冲与 G-Buffer 法线计算屏幕空间环境光遮蔽（AO），输出经过 Cross-Bilateral Blur 降噪的 AO 纹理，供 ScreenPass 在光照合成阶段调制环境光。该 Pass 支持三种模式：Off / SSAO / GTAO。

## 概述

```
输入: gDSRT 深度（R24_UNORM_X8_TYPELESS）+ G-Buffer RT1 法线（R16G16B16A16_FLOAT）
        │
        ▼
Pass 1: GTAO/SSAO 计算（GTAO.hlsl PSMain）
        │  → m_aoRawRT（R8G8B8A8_UNORM，AO 存 RGB，1.0=无遮蔽）
        ▼
Pass 2: Cross-Bilateral Blur（GTAOBlur.hlsl PSMain）
        │  3×3 高斯核 × 深度边缘权重
        ▼
输出: m_aoBlurredRT → ScreenPass（t6）
```

- GTAO 模式：XeGTAO 风格的地平线追踪积分（slice × steps，余弦空间地平线）；
- SSAO 模式：Crytek 风格法线半球随机采样（16 样本）；
- 关闭时 `GetAOTexture()` 返回 1×1 白色纹理（AO=1，无遮蔽），下游 Pass 无需分支。

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/public/GtaoPass.h` | GtaoPass 类声明、`AOType` 枚举、`GtaoConstants` 结构 |
| `Engine/private/GtaoPass.cpp` | RT/SRV/CB 创建、两 Pass 渲染、PSO 工厂、Resize |
| `Engine/Shader/GTAO.hlsl` | `ComputeSSAO / ComputeGTAO`、视图空间重建、噪声函数 |
| `Engine/Shader/GTAOBlur.hlsl` | Cross-Bilateral Blur（3×3 高斯 + 相对深度差权重） |
| `Engine/main.cpp` | `gtaoPso / gtaoBlurPso` 编译与每帧调度 |

## 架构与数据流

### 资源

| 资源 | 格式 | 说明 |
| --- | --- | --- |
| `m_aoRawRT` | `R8G8B8A8_UNORM`，视口尺寸 | 模糊前 AO（调试可用 `GetRawAOTexture()`） |
| `m_aoBlurredRT` | `R8G8B8A8_UNORM`，视口尺寸 | 最终 AO 输出 |
| `m_aoSrvHeap` | 2 个描述符 | t0=深度，t1=法线（AO 计算 Pass） |
| `m_blurSrvHeap` | 2 个描述符 | t0=Raw AO，t1=深度（模糊 Pass） |
| `m_rtvHeap` | 2 个描述符 | RTV0=Raw，RTV1=Blurred |
| `m_gtaoConstantBuffer` | `GtaoConstants`（256 对齐） | 每帧更新 |
| `m_defaultWhiteTexture` | 1×1 `R8G8B8A8_UNORM` | AO 关闭时替代输出 |

### GtaoConstants

```cpp
struct GtaoConstants {
    XMFLOAT2 resolution;          // 视口分辨率
    XMFLOAT2 inverseResolution;
    float aoRadius;               // SSAO/GTAO 分别维护 m_ssaoRadius / m_gtaoRadius
    float aoIntensity;
    int sliceCount;               // GTAO 切片数（默认 2）
    int stepsPerSlice;            // 每切片步数（默认 4）
    int frameCounter;             // 帧计数（噪声轮换预留）
    int aoType;                   // 0=Off, 1=SSAO, 2=GTAO
    float falloffStart;           // = aoRadius * 0.6
    float falloffEnd;             // = aoRadius
};
```

`UpdateConstants()` 每帧写入：`falloffStart = aoRadius * 0.6f`、`falloffEnd = aoRadius`，并递增 `m_frameCounter`。

### 渲染流程（GtaoPass::Render）

```
if (m_aoType == AOType::Off) return;      // 关闭时直接跳过

UpdateConstants(); m_frameCounter++;

// ---- Pass 1: AO 计算 ----
CreateAOInputSRVs(depthBuffer, normalRT);  // t0 深度, t1 法线
m_aoRawRT: PIXEL_SHADER_RESOURCE → RENDER_TARGET
Clear(1,1,1,1) + OMSetRenderTargets(RTV0)
SetRootSignature / SetPSO(gtaoPso)
b0 = SceneCB；slot2(b1) = m_gtaoConstantBuffer；slot1 = m_aoSrvHeap 表
共享全屏四边形 → DrawInstanced(6,1,0,0)
m_aoRawRT: RENDER_TARGET → PIXEL_SHADER_RESOURCE

// ---- Pass 2: Cross-Bilateral Blur ----
CreateBlurInputSRVs(depthBuffer);          // t0 Raw AO, t1 深度
m_aoBlurredRT: PIXEL_SHADER_RESOURCE → RENDER_TARGET
Clear(1,1,1,1) + OMSetRenderTargets(RTV1)
SetPSO(blurPso)；b0 = SceneCB；slot1 = m_blurSrvHeap 表
DrawInstanced(6,1,0,0)
m_aoBlurredRT: RENDER_TARGET → PIXEL_SHADER_RESOURCE
```

注意：GTAO 常量缓冲绑定在根参数 **slot 2**（`SetGraphicsRootConstantBufferView(2, ...)`），即根签名中 b1 的位置，与材质 CB 复用同一槽位——GTAO Pass 不使用材质，因此无冲突。

## 关键实现要点

### 视图空间重建与法线转换（GTAO.hlsl）

- `ReconstructViewPosition(uv, depth)`：UV → NDC（Y 翻转）→ `mul(InverseProjectionMatrix, clipPos)` → 透视除法；
- `ViewToScreenUV(viewPos)`：正向投影（AO 采样点投影回屏幕）；
- `GetViewNormal(uv)`：从 RT1 读取世界法线（`R16G16B16A16_FLOAT` 直接存 `[-1,1]`，无需解码），乘以 `(float3x3)ViewMatrix` 转到视图空间；
- `InterleavedGradientNoise(position)`：空间哈希噪声，用于切片旋转与步进抖动。

### GTAO 核心（ComputeGTAO）

```
for slice in 0..AOSliceCount:
    sliceK = (slice + noiseSlice) / AOSliceCount          // 噪声旋转切片角
    omega  = (cos, sin) 切片屏幕空间方向
    构造切片平面轴 axisVec、投影法线 projectedNormalVec、n = signNorm * acos(cosNorm)
    初始化地平线余弦 lowHorizonCos0/1 = cos(n ± π/2)
    for step in 0..AOStepsPerSlice:
        stepNoise = frac(noise + (slice + step*steps) * 0.6180339)   // R1 序列
        s = pow((step + stepNoise)/steps, 2.0) + minS               // 非线性分布
        sampleOffset = round(s * omega) * InverseScreenSize          // 像素对齐
        正/负方向采样深度 → 重建视图位置 → 计算 horizon cos（shc）
        weight = 距离衰减（falloffBase = |(Δxy, Δz*(1+thinOccluderCompensation))|）
        shc = lerp(lowHorizonCos, shc, weight); horizonCos = max(...)
    积分: iarc = (cosNorm + 2*h*sin(n) - cos(2h-n)) / 4
    visibility += projectedNormalVecLength * (iarc0 + iarc1)
ao = 1 - pow(saturate(1 - visibility/AOSliceCount), AOIntensity)
```

关键常量：`sampleDistributionPower = 2.0`、`thinOccluderCompensation = 0.0`、`minS`（跳过中心像素）、投影法线长度向 1.0 lerp 5%（减少高坡度过暗）。

### SSAO 核心（ComputeSSAO）

- 16 个样本，Fibonacci 球面分布（黄金角 `2.399963`）+ 噪声旋转；
- 采样距离 `AORadius * (0.1 + 0.9 * t²)` 非线性分布；
- 遮挡判断：`depthDiff > 0.01 && depthDiff < AORadius`（防自遮挡 + 范围检查）；
- 权重 = 距离衰减 × 深度衰减，累加后归一化输出 AO。

### Cross-Bilateral Blur（GTAOBlur.hlsl）

```
中心像素 AO/深度；天空像素（depth >= 1.0）直接返回 1.0
3×3 高斯权重: [1 2 1; 2 4 2; 1 2 1]（总和 16）
对每个邻域:
  越界 → 用中心 AO 补足（避免边缘权重失衡黑边）
  天空 → AO=1 参与，权重 × 0.01（极低）
  深度权重 = (relDiff < 0.02) ? 1.0 : exp(-(relDiff/0.02)^2 * 10)
  最终权重 = 高斯权重 × 深度权重
输出 = Σ(AO × 权重) / Σ权重
```

`depthThreshold = 0.02`（相对深度差 2%），深度权重用指数衰减保持边缘。

### 天空像素处理

`PSMain` 中 `depth >= 1.0` 直接输出白色（AO=1）；模糊 Pass 中天空像素权重压到 0.01，避免 AO 从天空边缘渗入场景。

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `GtaoPass::Initialize(w, h)` | `GtaoPass.h` | 创建 RT/SRV/CB |
| `GtaoPass::Render(cmd, gtaoPso, blurPso, rootSig, depthBuffer, normalRT)` | `GtaoPass.h` | 每帧入口（两个子 Pass） |
| `GtaoPass::CreateGtaoPSO / CreateBlurPSO` | `GtaoPass.h` | 均基于 `CreateFullscreenPSO(..., R8G8B8A8_UNORM)` |
| `GtaoPass::GetAOTexture()` | `GtaoPass.h` | 最终 AO 纹理（关闭时返回白色 1×1 纹理） |
| `GtaoPass::GetRawAOTexture()` | `GtaoPass.h` | 模糊前 AO（调试） |
| `GtaoPass::SetAOType / SetRadius / SetIntensity / SetSliceCount / SetStepsPerSlice` | `GtaoPass.h` | 运行时参数 |
| `GtaoPass::Resize(w, h)` | `GtaoPass.h` | 分辨率变更（释放并重建 RT 与 RTV 堆） |

## 配置与调参

| 参数 | 默认 | UI 入口 | 说明 |
| --- | --- | --- | --- |
| AO 模式 | GTAO（`m_aoType`） | Setting 窗口 `Combo("AO")`：Close/SSAO/GTAO | SSAO 与 GTAO 各自维护半径/强度 |
| AO Radius | SSAO/GTAO 各自默认 | `SliderFloat("AO Radius", 0.1~5.0)` | 采样半径 |
| AO Intensity | 默认 | `SliderFloat("AO Intensity", 0.1~5.0)` | `ao = 1 - pow(ao, intensity)` |
| AO Slices | 2 | `SliderInt("AO Slices", 1~8)` | 仅 GTAO；越多质量越好，开销线性 |
| AO Steps/Slice | 4 | `SliderInt("AO Steps/Slice", 1~8)` | 仅 GTAO |
| 模糊阈值 | 0.02 | 代码常量 | GTAOBlur.hlsl `depthThreshold` |

## 已知限制与 TODO

- **无时域降噪**：GTAO 只有空间模糊（3×3），`frameCounter` 虽传入 CB 但 shader 未使用时序累积，低采样数下仍有噪点；
- **多反弹 AO 不在本 Pass**：`MultiBounceAO(ao, albedo)`（Frostbite 近似）实现在 `Screen.shader` 中，于合成阶段对最终 AO 做多反弹补偿；本 Pass 输出的仍是单次遮蔽值，也不输出 Bent Normal；
- **无 Bent Normal**：Screen.shader 的 IBL 使用法线直接采样辐照度，未利用 AO 方向信息；
- **GTAO 半分辨率**：AO 全分辨率计算，未做降分辨率 + 升采样优化；
- `thinOccluderCompensation` 硬编码为 0.0，薄物体过度遮蔽的开关没有暴露到 UI；
- AO 纹理使用 `R8G8B8A8_UNORM`（注释说明实际 R8 即可），存在 4 倍带宽浪费。

## 维护注意事项

- **常量绑定槽位**：GTAO 私有 CB 绑定在根参数 2（b1），与材质 CB 共享槽位；新增需要在 AO Pass 中采样纹理的代码时，不要占用 slot 2 之外的根参数。
- **SRV 堆顺序**：`m_aoSrvHeap`（t0 深度/t1 法线）与 `m_blurSrvHeap`（t0 RawAO/t1 深度）顺序不同，修改 shader 的 `register(tN)` 时必须同步；
- **默认白色纹理**：`CreateDefaultWhiteTexture()` 使用临时 command list + fence 上传，属于启动期一次性工作；若改为惰性创建，注意不要占用主循环的 allocator；
- **Resize 行为**：`Resize` 释放 `m_aoRawRT/m_aoBlurredRT/m_rtvHeap` 后重建，**不释放** SRV 堆与常量缓冲（每帧重新创建 SRV 描述符），改动堆结构时需注意；
- **调试**：用 `GetRawAOTexture()` 对比模糊前后，可快速定位是计算问题还是降噪问题；`depth >= 1.0` 的判定依赖深度清除值为 1.0，若修改深度约定需同步。

## 算法参考与术语表

| 术语 | 含义 | 本引擎位置 |
| --- | --- | --- |
| Slice | 切片平面，沿视线方向旋转的 2D 采样平面 | `ComputeGTAO` 外层循环 |
| Horizon（地平线） | 切片内法线两侧能看到的最高仰角方向 | `horizonCos0/1` 余弦空间追踪 |
| 余弦空间 | 用 `cos(仰角)` 代替角度做 max/lerp，减少三角函数调用 | `shc = dot(sampleHorizonVec, viewDir)` |
| Projected Normal | 法线在切片平面上的投影，用于可见性加权 | `projectedNormalVec` |
| Cross-Bilateral | 用深度差异调制高斯权重的边缘保持滤波 | `GTAOBlur.hlsl` |
| R1 序列 | 黄金比例低差异序列，用于步进抖动 | `stepBaseNoise * 0.6180339887498948482` |
| Interleaved Gradient Noise | 空间哈希噪声，时间/空间上分布均匀 | `InterleavedGradientNoise()` |

## 典型调参路径

- **AO 偏噪**：先提高 `Steps/Slice`（如 4 → 6），再考虑增加 `Slices`；带宽允许时把模糊核升级为 5×5（需修改 `GTAOBlur.hlsl` 的权重表与偏移表）。
- **AO 过强/过弱**：调整 `AO Intensity`（>1 变暗程度增强，<1 减弱）与 `AO Radius`（半径小则只遮蔽近距离）。
- **边缘发灰/渗色**：降低 `depthThreshold`（0.02 → 0.01）使深度边缘更锐利；提高天空像素权重抑制值（当前 0.01）。
- **性能预算**：AO 两 Pass 均为全屏像素着色器；`Slices × Steps` 即每像素的深度采样次数（默认 2×4=8 次 × 正负方向），可据此估算开销。