# SkyPass - 天空球渲染

> SkyPass 在 SSGI 之后、ScreenPass 之前执行，绘制一个包裹相机的大球体，用立方体贴图（SkyCube）填充天空背景。天空先渲染到中间 RT（TAA 开启）或视口颜色缓冲（TAA 关闭），随后 ScreenPass 用 alpha 混合叠加场景光照结果，天空像素输出透明（alpha=0）以露出天空。

## 概述

```
SkyPass::Initialize(commandList, sphereRadius = 500.0)
  ├─ CreateSRVHeap()                       // 1 个 SRV（SkyCube）
  ├─ GenerateSkySphere(500, 32, 16)        // 球体网格（法线朝内）
  └─ 上传顶点/索引缓冲（UPLOAD 堆，持久映射）
        │
        ▼ 每帧
SkyPass::Render(cmd, skyPso, rootSig, skyTexture)
  ├─ CreateSkyCubeSRV(skyTexture)          // TEXTURECUBE SRV
  ├─ 绑定 b0 SceneCB（相机位置/矩阵）+ SRV 表（slot 1）
  ├─ 顶点/索引缓冲 → DrawIndexedInstanced
  └─ 写入中间 RT（TAA 开）或视口颜色缓冲（TAA 关）
```

- 天空球半径 500，远大于场景；顶点着色器把 `pos + CameraPositionWS` 变换到裁剪空间，并把深度钉到 `w * 0.99999` 确保在最远处；
- 采样方向使用**顶点原始位置（球心到顶点的方向）**，因此天空始终跟随相机（无限远效果）；
- PSO 关闭背面剔除（从球体内部观看）并禁用深度测试（先画天空，ScreenPass 用 alpha 混合覆盖）。

## 核心文件清单

| 文件 | 作用 |
| --- | --- |
| `Engine/public/SkyPass.h` | SkyPass 类声明（`SkyVertex` 只有 position[3]） |
| `Engine/private/SkyPass.cpp` | 球体生成、VB/IB 上传、SRV 创建、Render、PSO |
| `Engine/Shader/SkyShader/Sky.shader` | 实际使用的天空 shader（Pass `SkyBox`，立方体贴图采样） |
| `Engine/Shader/skybox.hlsl` | 遗留的等距柱状投影（equirectangular）天空 shader（未被当前管线使用） |
| `Engine/main.cpp` | `skyPso` 创建与每帧调度（TAA 分支切换渲染目标） |

## 架构与数据流

### 网格生成（GenerateSkySphere）

```
参数: radius=500.0, slices=32, stacks=16
顶点: stack 在 [0,16] 循环 phi = π*stack/16
      slice 在 [0,32] 循环 theta = 2π*slice/32
      position = (r*sinφ*cosθ, r*cosφ, r*sinφ*sinθ)
索引: 每个四边形两个三角形，顺序反转（从内部观看）
顶点缓冲: SkyVertex = float position[3]（stride 12 字节）
索引缓冲: UINT32，DXGI_FORMAT_R32_UINT
```

VB/IB 均为 `D3D12_HEAP_TYPE_UPLOAD` + `GENERIC_READ`，初始化时 `Map/memcpy/Unmap` 一次性上传。

### 每帧数据流

```
main.cpp (TAA 开启):
  中间 RT: ClearRenderTargetView(black) → OMSetRenderTargets(intermediateRTV)
  viewport = taaPass->GetViewportWidth/Height
  skyPass->Render(...)                       // 天空写入中间 RT
  → ScreenPass 不清空、直接叠加（alpha 混合）

main.cpp (TAA 关闭):
  视口颜色缓冲: ClearRenderTargetView(black) → OMSetRenderTargets(viewportColorRTV)
  同样先做一次 PIXEL_SHADER_RESOURCE → RENDER_TARGET 转换
  skyPass->Render(...)
```

### Sky.shader 顶点/像素着色器

```hlsl
PSInput VS(VSInput input) {
    float3 sampleDir = normalize(input.pos);           // 采样方向 = 顶点方向
    float3 worldPos = input.pos + CameraPositionWS;    // 跟随相机
    float4 viewPos = mul(ViewMatrix, float4(worldPos, 1.0f));
    output.pos = mul(ProjectionMatrix, viewPos);
    output.pos.z = output.pos.w * 0.99999f;            // 深度钉到最远
    output.worldDir = sampleDir;
    return output;
}
float4 PS(PSInput input) : SV_TARGET {
    float3 skyColor = SkyCube.Sample(gSamLinearClamp, normalize(input.worldDir)).rgb;
    return float4(skyColor, 1.0f);
}
```

## 关键实现要点

### SRV 创建

`CreateSkyCubeSRV(skyTexture)` 使用 `D3D12_SRV_DIMENSION_TEXTURECUBE`，格式与 mip 级别直接取资源描述（`skyTexture->GetDesc()`），每次 Render 都重建描述符（低成本，且能自动适配换图后的格式）。

### PSO（SkyPass::CreatePSO）

| 属性 | 值 | 原因 |
| --- | --- | --- |
| 输入布局 | 仅 `POSITION`（`R32G32B32_FLOAT`） | 天空球顶点只有位置 |
| CullMode | `CULL_MODE_NONE` | 球体内部可见 |
| DepthEnable | `FALSE` | 天空最先画，深度交由后续合成 |
| RTVFormat | `R8G8B8A8_UNORM` | 与交换链/视口颜色缓冲一致 |
| PrimitiveTopology | `TRIANGLE` | 三角形列表 |

### 与 ScreenPass 的 alpha 协议

Screen.shader 的合成像素着色器对天空像素输出：

```hlsl
if (depth < 1.0) {
    finalColor = directLighting * shadow * 6.0 + ambient;
    alpha = 1.0;
} else {
    finalColor = float3(0, 0, 0);   // 天空像素
    alpha = 0.0;                    // 透明，露出 SkyPass 画好的天空
}
```

`ScreenPass::CreatePSO` 通过 `CreateFullscreenPSO(..., true)` 开启 alpha 混合（`SRC_ALPHA / INV_SRC_ALPHA`），因此天空区域的合成结果 = SkyPass 的天空颜色。若 ScreenPass 混合被意外关闭，天空会变黑。

### 遗留 skybox.hlsl

`skybox.hlsl` 使用 `Texture2D skyboxTexture` + 等距柱状投影（`atan2/asin` 生成 UV），是早期实现；当前管线通过 `MaterialManager::LoadShader(Sky.shader)` 加载 `Sky.shader`，失败时仅打印警告并禁用天空（`skyPso == nullptr` 时 SkyPass 整段跳过）。

## 对外接口

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `SkyPass::Initialize(commandList, sphereRadius = 500.0f)` | `SkyPass.h` | 创建 SRV 堆与球体几何体 |
| `SkyPass::Render(cmd, pso, rootSig, skyTexture)` | `SkyPass.h` | 每帧绘制天空球 |
| `SkyPass::CreatePSO(rootSig, vs, ps)` | `SkyPass.h` | 天空球专用 PSO（float3 位置输入） |
| `SkyPass::SetSceneConstantBuffer(sceneCB)` | `SkyPass.h` | 绑定 SceneCB（相机位置与矩阵） |

## 配置与调参

| 参数 | 默认 | 入口 | 说明 |
| --- | --- | --- | --- |
| 球体半径 | 500.0 | `SkyPass::Initialize` 参数 | 需远大于场景包围盒 |
| 细分 | slices=32, stacks=16 | `GenerateSkySphere` 参数 | 高细分只在球体边缘明显，32×16 足够 |
| 天空纹理 | 场景加载 | `g_scene->ReturnSkyCube()` / `Scene::LoadLevel` | 立方体贴图，支持 mip（IBL 复用） |
| Sky shader | `Sky.shader` | main.cpp 加载逻辑 | 加载失败则天空不渲染 |
| Skylight 强度/颜色 | 1.0 / 白 | `Scene::SetSkylightIntensity / SetSkylightColor` | 影响 ScreenPass 的 IBL 环境光（不直接影响 SkyPass 输出） |

## 已知限制与 TODO

- **天空不被光照/雾化调制**：SkyPass 输出原始贴图颜色，ScreenPass 对天空像素输出透明，因此场景雾效、曝光调整不会作用于天空；
- **深度写入禁用**：天空不写深度，若后续引入需要深度剪裁的半透明对象（如云层），需要单独处理；
- **无相机矩阵同步问题**：`CameraPositionWS` 来自 SceneCB，天空球随相机平移；若相机远裁剪面小于 500，天空球会被裁剪（当前 far plane 配置下无此问题）；
- **skybox.hlsl 遗留文件**：内容与当前实现无关，易误导读者，建议删除或标注 deprecated；
- **单纹理天空**：不支持程序化天空（渐变/大气散射），需要预烘焙 HDR 环境贴图。

## 维护注意事项

- **修改天空 shader 时**：必须用 `SkyPass::CreatePSO` 创建 PSO（输入布局为 float3 位置），不要走 `CreateScenePSO`（float4 位置布局）——main.cpp 中有明确注释说明该坑；
- **渲染目标切换**：TAA 开启/关闭时天空的渲染目标不同（中间 RT vs 视口颜色缓冲），两处代码块结构相似，修改一处必须同步另一处；
- **天空纹理来源**：`g_scene->ReturnSkyCube()` 返回 `ComPtr<ID3D12Resource>`，ScreenPass 也会为 IBL 创建同纹理的 TEXTURECUBE SRV；替换天空资源时确保 `Scene::LoadAndUploadTexture` 的 cubemap 分支被使用（`isCubemap=true`）；
- **状态假设**：进入 SkyPass 前，目标 RT 已由主循环完成 `PIXEL_SHADER_RESOURCE → RENDER_TARGET` 转换；SkyPass 本身不改变 RT 状态（保持 RENDER_TARGET 交给 ScreenPass 叠加）；
- **调试建议**：临时把 `skyPso` 设为 nullptr 可快速验证“无天空”路径；用 PIX 抓帧可确认天空像素 alpha=0 是否被 ScreenPass 正确保留。

## 与 IBL 的关系

天空立方体贴图同时承担两个角色：

1. **背景显示**：SkyPass 直接采样（`Sky.shader` PS）；
2. **IBL 环境光**：Screen.shader 用同一纹理做预过滤近似——`SkyCube.SampleLevel(sampler, R, roughness * 8)` 模拟预过滤镜面反射、`SkyCube.SampleLevel(sampler, N, 8)` 模拟漫反射辐照度（最高 mip 近似）。

引擎同时存在 `IBLResources`（`BRDFIntegration.hlsl / IrradianceConvolution.hlsl / PrefilterEnvMap.hlsl`，设计输出 512 BRDF LUT / 32 辐照度 / 128 预过滤 5 mip），但该类**从未被主循环实例化**，当前 Screen.shader 的合成路径直接对 SkyCube 按 mip 采样，未使用预计算资源——两套方案并存，是后续 IBL 升级的切入点（详见 ScreenPass.md 的已知限制）。

## 常见问题排查

- **天空全黑**：检查 `skyPso` 是否创建成功（加载 `Sky.shader` 失败时 main.cpp 只打警告）；检查 ScreenPass 是否开启 alpha 混合（`CreateFullscreenPSO(..., true)`）。
- **天空随场景移动/错位**：确认顶点着色器使用 `input.pos + CameraPositionWS` 且采样方向用 `normalize(input.pos)`；两者写反会导致方向偏移。
- **天空在物体前/后错乱**：确认 SkyPass 在 ScreenPass 之前执行，且 Screen.shader 对 `depth >= 1.0` 输出 alpha=0。
- **改半径后天空消失**：半径必须大于相机 far plane；同时确认细分后的球体不被相机的近平面穿过。
- **换天空贴图不生效**：确认新纹理以 cubemap 方式加载（`LoadAndUploadTexture(..., true)`），且格式能被 `CreateSkyCubeSRV` 接受（SRV 格式直接取资源格式）。

## 性能提示

- 天空球 32×16 细分 = 1057 顶点 / 3072 索引，DrawIndexedInstanced 开销可忽略；
- 每帧 `CreateSkyCubeSRV` 只重建一个描述符，CPU 成本极低；
- 若开启高分辨率 mip 采样（IBL 路径），天空贴图的 mip 链质量会影响反射表现，建议导入时保留完整 mip 链。

## 资源与状态速查

| 项目 | 值 |
| --- | --- |
| 顶点结构 | `struct SkyVertex { float position[3]; }`，stride 12 字节 |
| 顶点缓冲堆 | `D3D12_HEAP_TYPE_UPLOAD`，状态 `GENERIC_READ` |
| 索引格式 | `R32_UINT` |
| SRV 堆 | 1 个 `SHADER_VISIBLE` 描述符 |
| SRV 维度 | `TEXTURECUBE`，mip 数取资源描述 |
| 采样器 | `gSamLinearClamp`（s3） |
| 深度钳制 | `pos.z = pos.w * 0.99999f` |
| 渲染目标 | 中间 RT（TAA 开）/ 视口颜色缓冲（TAA 关） |

## 启动加载链路

```
main.cpp:
  skyShader = MaterialManager::LoadShader(Engine/Shader/SkyShader/Sky.shader)
  skyShader->CompileShaders(device)                       // 失败 → 警告，天空禁用
  skyVS = skyShader->GetVertexShaderBytecode(0)           // Pass 0 = SkyBox
  skyPS = skyShader->GetPixelShaderBytecode(0)
  skyPso = skyPass->CreatePSO(rootSignature, skyVS, skyPS)
```

`Sky.shader` 的 Pass 名为 `SkyBox`，`#pragma vertex VS / #pragma fragment PS`；shader 依赖的 `SkyCube` 纹理与 `CameraPositionWS` 等变量由场景常量缓冲/编译器注入提供。