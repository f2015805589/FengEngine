# TaaPass - 时域抗锯齿

> TaaPass 在 ScreenPass 之后、UIPass 之前执行，通过亚像素抖动采样与历史帧混合实现时域抗锯齿。它同时管理：Halton 序列抖动（驱动整个管线的投影矩阵偏移）、双历史缓冲（ping-pong）、TAA resolve、以及把结果拷贝到视口颜色缓冲的 TaaCopy。

## 概述

```
帧首: taaPass->UpdateJitter() → Halton(2,3) 抖动 → g_scene->SetJitterOffset()
      Scene::Update 把抖动应用到投影矩阵（r[2] 行）→ BasePass 亚像素偏移采样
        │
        ▼
ScreenPass 输出到 TAA 中间 RT（m_intermediateRT，不清空，叠加 SkyPass 结果）
        │
        ▼
TaaPass::RenderToSwapChain(cmd, taaPso, rootSig, motionVectorRT, depthBuffer, viewportColorRTV)
  ├─ 输入: t0 中间RT（当前帧）、t1 历史帧、t2 MotionVector、t3 深度
  ├─ 输出: 写入 History（ping-pong 的另一张）
  └─ 结果即“TAA 后的当前帧”= 下一帧的历史
        │
        ▼
TaaPass::CopyToSwapChain(cmd, taaCopyPso, rootSig, viewportColorRTV)
  └─ History → 视口颜色缓冲（ImGui 显示目标）
```

- 关闭 TAA 时：SkyPass/ScreenPass 直接渲染到视口颜色缓冲，TAA 相关代码整体跳过；
- TAA 开启时：深度缓冲在 TAA 前后各做一次状态转换（`DEPTH_WRITE → PIXEL_SHADER_RESOURCE → DEPTH_WRITE`）；
- 帧末：`g_scene->UpdatePreviousViewProjectionMatrix()` + `taaPass->SwapHistoryBuffers()`。

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/public/TaaPass.h` | TaaPass 类、`TaaConstants` 结构 |
| `Engine/private/TaaPass.cpp` | RT/SRV/CB、Halton 抖动、`RenderToSwapChain`、`CopyToSwapChain`、历史切换 |
| `Engine/Shader/TAA.hlsl` | TAA resolve 像素着色器（YCoCg、Variance Clipping、亮度加权混合） |
| `Engine/Shader/TaaCopy.hlsl` | 简单纹理拷贝着色器（History → 视口颜色缓冲） |
| `Engine/main.cpp` | Jitter 调度、中间 RT 目标切换、帧末历史矩阵更新 |
| `Engine/public/Settings.h` | `GetTaaJitterScale()` 抖动强度缩放 |

## 架构与数据流

### 资源（4 张 RT + 1 个 SRV 堆）

| RTV 索引 | 资源 | 初始状态 | 用途 |
| --- | --- | --- | --- |
| 0 | `m_outputRT` | `PIXEL_SHADER_RESOURCE` | 输出 RT（当前未被主循环直接使用，预留） |
| 1 | `m_intermediateRT` | `RENDER_TARGET` | SkyPass + ScreenPass 的合成目标 |
| 2 | `m_historyRT` | `PIXEL_SHADER_RESOURCE` | 历史缓冲 1 |
| 3 | `m_historyRT2` | `PIXEL_SHADER_RESOURCE` | 历史缓冲 2 |

格式统一为 `R16G16B16A16_FLOAT`。SRV 堆 5 个描述符：0-3 供 `RenderToSwapChain`（当前帧/历史/速度/深度），4 供 `CopyToSwapChain` 专用（避免覆盖 0-3）。

### TaaConstants

```cpp
struct TaaConstants {
    XMFLOAT2 jitterOffset;      // 当前帧 Jitter
    XMFLOAT2 previousJitter;    // 上一帧 Jitter
    XMFLOAT2 resolution;        // 视口分辨率
    float blendFactor;          // 首帧 0.0，之后 m_blendFactor（默认 0.9）
    float padding;
};
```

### Halton 抖动（UpdateJitter）

```
m_previousJitter = m_currentJitter
m_jitterIndex = (m_jitterIndex + 1) % 16          // JITTER_SAMPLE_COUNT = 16
jitterX = (HaltonSequence(index+1, 2) - 0.5) * jitterScale
jitterY = (HaltonSequence(index+1, 3) - 0.5) * jitterScale
m_currentJitter = (jitterX, jitterY)
```

- `HaltonSequence(index, base)`：经典数字反转实现；
- `jitterScale` 来自 `Settings::GetTaaJitterScale()`（默认 1.0，即标准 0.5 像素抖动）；
- `GetJitteredProjectionMatrix(proj)`：`r[2][0/1] += jitter * 2 / viewportWH`，主循环实际通过 `Scene::SetJitterOffset` + `Scene::Update` 应用，两者公式一致。

## 关键实现要点

### TAA resolve（TAA.hlsl PSMain）

```
1. 采样当前帧颜色（中间 RT）
2. GetClosestFragment(uv): 4 角深度取“最近”片元（注意：注释声称 reversed-z，引擎实际为标准深度，见已知限制）
3. 在最近片元处采样 Motion Vector → historyUV = uv - motionVector
4. 采样历史帧；有效性检查（亮度 > 0.001 或 alpha > 0.01；UV 越界无效）
5. 当前帧 3×3 邻域:
   样本 → RGBToYCoCg(ToneMap(c))，计算 mean(m1)/stdDev(m2)
   同时取 neighborMin/neighborMax（AABB 硬边界）
6. 历史颜色裁剪:
   ClipHistoryVariance(historyYCoCg, m1, stdDev, gamma=1.0)   // 方差裁剪
   再 clamp 到 [neighborMin, neighborMax]                      // AABB 兜底
   YCoCg → RGB，ToneMapInverse 回到线性空间
7. 混合因子:
   blendFactor = 0.1（当前帧权重）
   → lerp(0.1, 0.3, saturate(motionLength * 0.5))             // 运动加快
   → lerp(blendFactor, 0.5, saturate(clipAmount * 2.0))        // 裁剪多则更信任当前帧
8. 亮度加权混合（Reinhard 空间）:
   weightC = 1/(1+lumaC)，weightH = 1/(1+lumaH)
   result = (C*wC*b + H*wH*(1-b)) / (wC*b + wH*(1-b))
9. clamp 到 [0, 65504]（half 上限），输出 alpha=1
```

`DEBUG_MODE` 宏：0=正常、1=当前帧、2=历史帧、3=Motion Vector（×50 放大显示）。

### RenderToSwapChain 与历史写入

```
writeHistoryRT = m_useHistory2 ? m_historyRT : m_historyRT2   // 写另一张
barriers: 中间RT RENDER_TARGET → PS_RESOURCE
          writeHistoryRT PS_RESOURCE → RENDER_TARGET
OMSetRenderTargets(writeHistoryRTV) → DrawInstanced(6)
转换回:   writeHistoryRT → PS_RESOURCE；中间RT → RENDER_TARGET
m_firstFrame = false
```

### CopyToSwapChain（TaaCopy）

- 在 SRV 槽 4 创建 History 的 SRV；
- 绑定 `viewportColorRTV`（ViewportManager 颜色缓冲），全屏四边形绘制，`TaaCopy.hlsl` 直接 `SourceTexture.Sample(LinearSampler, uv)`；
- 之所以需要独立 Copy 而不是 `CopyResource`：TAA 结果在 History 中，且历史格式与视口颜色缓冲格式不同（`R16G16B16A16_FLOAT` vs `R8G8B8A8_UNORM`），需要经 shader 转换。

### 首帧处理

- `UpdateTaaConstants`：`blendFactor = m_firstFrame ? 0.0f : m_blendFactor`；
- `TAA.hlsl` 侧还有历史有效性检查（空历史 → 直接输出当前帧）；
- `Resize` 时 `m_firstFrame = true`，避免尺寸变化后历史 UV 失配。

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `TaaPass::Initialize(w, h)` | `TaaPass.h` | 创建 4 RT + SRV 堆 + CB |
| `TaaPass::RenderToSwapChain(cmd, pso, rootSig, motionVectorRT, depthBuffer, swapChainRTV)` | `TaaPass.h` | TAA resolve（写入 History） |
| `TaaPass::CopyToSwapChain(cmd, copyPso, rootSig, swapChainRTV)` | `TaaPass.h` | History → 视口颜色缓冲 |
| `TaaPass::UpdateJitter() / GetJitterOffset() / GetJitteredProjectionMatrix()` | `TaaPass.h` | 抖动生成与投影应用 |
| `TaaPass::SwapHistoryBuffers()` | `TaaPass.h` | 帧末切换历史读写目标 |
| `TaaPass::GetIntermediateRT() / GetIntermediateRTV()` | `TaaPass.h` | 供 SkyPass/ScreenPass 写入 |
| `TaaPass::GetOutputTexture() / GetHistoryTexture()` | `TaaPass.h` | 资源访问 |
| `TaaPass::SetBlendFactor / SetEnabled` | `TaaPass.h` | 混合强度与开关 |
| `TaaPass::CreatePSO / CreateCopyPSO` | `TaaPass.h` | PSO 工厂（`R16G16B16A16_FLOAT` / `R8G8B8A8_UNORM`） |

## 配置与调参

| 参数 | 默认 | 入口 | 说明 |
| --- | --- | --- | --- |
| TAA 开关 | true | Setting 窗口 `Checkbox("Enable TAA")` | 关闭时走无 TAA 路径 |
| Jitter 强度 | 1.0 | `Settings::SetTaaJitterScale()` | 1.0 = 标准 0.5 像素 |
| 混合因子 | 0.9（90% 历史） | `TaaPass::SetBlendFactor()` | 越大越稳定，残影风险越高 |
| Halton 采样数 | 16 | `JITTER_SAMPLE_COUNT` 常量 | 16 帧循环一次抖动周期 |
| 裁剪 gamma | 1.0 | TAA.hlsl `gamma` 常量 | 方差裁剪范围 |

## 已知限制与 TODO

- **深度约定注释矛盾**：`TAA.hlsl` 的 `GetClosestFragment` 注释写“D3D12 使用 reversed-z，深度值越大越近”，但引擎实际为标准深度（清除 1.0、`LESS_EQUAL`），`COMPARE_DEPTH(a,b)=step(b,a)` 实际取的是**最远**片元——请验证该处是否为 bug 或有意的保守选择；
- **静态物体假设**：Motion Vector 基于上一帧 VP 矩阵与当前 ModelMatrix（与 BasePass 相同限制），动态物体重投影错误会产生鬼影；
- **m_outputRT 未使用**：TAA 输出直接写 History，`m_outputRT` 为预留资源；
- **无运动向量剔除**：遮挡/去遮挡区域（disocclusion）仅靠颜色裁剪兜底，无深度重投影验证（`GetClosestFragment` 是一种近似）；
- **调试宏需重新编译**：`DEBUG_MODE` 是编译期宏，改完需重新编译 shader 才能看到调试视图。

## 维护注意事项

- **Jitter 应用点有两处**：`Scene::Update`（SceneCB）与 `Scene::Render`（Actor CB）都手动把 Jitter 加到投影矩阵；`TaaPass::GetJitteredProjectionMatrix` 是另一份等价实现，改动抖动公式时必须三处同步；
- **Motion Vector 必须无 Jitter**：`currentViewProjMatrix` 使用原始投影矩阵；若未来开启 Jitter 补偿（StandardPBR.shader 中被注释的 `motionVector -= (JitterOffset - PreviousJitterOffset)`），需同步修改 TAA 与 SSGI 的速度场消费逻辑；
- **中间 RT 生命周期**：TAA 开启时 SkyPass 先清空中间 RT 再画天空，ScreenPass 不清空直接叠加（alpha 混合）；任何在两者之间插入的新 Pass 都要遵守“天空先画、合成后画”的顺序；
- **历史缓冲与分辨率**：`Resize` 会重建 RT 并重置 `m_firstFrame`；主循环的 `ResizeViewportRenderTargets` 中 TAA 必须在其他 Pass 之后 Resize（顺序敏感）；
- **帧末收尾**：`UpdatePreviousViewProjectionMatrix()` 与 `SwapHistoryBuffers()` 只在 TAA 开启分支内执行，关闭 TAA 时上一帧 VP 矩阵不会更新——若未来 TAA 关闭也启用 Motion Vector 相关功能，需要移出该分支。

## 渲染路径对比（TAA 开/关）

| 阶段 | TAA 开启 | TAA 关闭 |
| --- | --- | --- |
| Jitter | 每帧更新并应用 | `g_scene->SetJitterOffset(0, 0)` |
| SkyPass 目标 | 中间 RT（清黑） | 视口颜色缓冲（清黑） |
| ScreenPass 目标 | 中间 RT（不清空，叠加） | 视口颜色缓冲（叠加） |
| 深度状态 | DEPTH_WRITE → SRV → DEPTH_WRITE（TAA 采样需要） | ScreenPass 后直接转回 DEPTH_WRITE |
| 最终输出 | TAA resolve → History → TaaCopy → 视口颜色缓冲 | ScreenPass 直接写入视口颜色缓冲 |
| 帧末收尾 | `UpdatePreviousViewProjectionMatrix()` + `SwapHistoryBuffers()` | 无 |

主循环中这两条路径的切换点集中在 `main.cpp` 的 SkyPass/ScreenPass/TaaPass 三段，改动渲染目标时需保持两套分支同步。

## TAA 与 SSGI 的协作

- 两者共享 RT3 Motion Vector：TAA 用于颜色重投影，SSGI 用于历史帧重投影；
- SSGI 历史缓冲（低分辨率）与 TAA 历史缓冲（全分辨率）相互独立，但都依赖同一个速度场；
- 两处“运动幅度”阈值不同：TAA 用像素长度动态调整混合，SSGI 以 5 像素为硬阈值；调整速度场编码时需评估对两者的影响。

## 常见问题排查

- **画面模糊/残影**：降低 `blendFactor`（当前 0.9）；检查 Jitter 是否正常轮换（`UpdateJitter` 每帧调用一次）；确认 `UpdatePreviousViewProjectionMatrix` 在 TAA 分支内执行。
- **边缘抖动/闪烁**：检查 Motion Vector 是否正确（DEBUG_MODE=3 可视化）；确认 BasePass 使用无 Jitter 的 `CurrentViewProjectionMatrix`。
- **首帧闪黑**：`m_firstFrame` 时 `blendFactor=0` 属预期；若持续闪黑，检查历史 RT 是否被误清除（历史 RT 不参与 Clear）。
- **切换 TAA 开关后画面错位**：关闭 TAA 时中间 RT 不被使用，但内容保留；重新开启后的第一帧会把它当作历史，属可接受的瞬态，也可在开关时强制 `m_firstFrame = true`。
- **分辨率切换后残影**：`Resize` 已重建 RT 并重置首帧标记；若仍复现，确认 `ResizeViewportRenderTargets` 中 TAA 的 Resize 参数来自新的视口尺寸。

## 性能提示

- TAA resolve 每像素成本：9 次邻域采样 + 历史 3×3 搜索（9 次）+ 若干次深度采样，约 20+ 次纹理采样；
- 可在 `DEBUG_MODE` 调试稳定后，把邻域从 3×3 降为 2×2（五样本）以省带宽；
- `CopyToSwapChain` 是全屏拷贝，与 `BeginRenderToSwapChain` 的清除可以合并（先清后画已实现，无需额外优化）；
- 若最终目标分辨率与视口颜色缓冲一致（当前默认一致），可考虑把 TAA 输出直接绑定到视口颜色缓冲的 RTV，省去 TaaCopy——但需处理 `R16G16B16A16_FLOAT → R8G8B8A8_UNORM` 的格式转换与 SRV 显示路径。