# ShaderAssets — Shader 资产组织与运行时编译

> 本文介绍 FEngine 的 Shader 资产组织：`Engine/Shader/` 目录布局（IBL / SkyShader / Shadingmodel / Utility / Compression 等子目录）、`.shader` / `.material` 资产格式、`Shader_Cache` 运行时缓存/调试转储机制，以及运行时编译与日志输出。

## 概述

FEngine 的 Shader 资产分为两类：

1. **手写 .hlsl**：直接由各 Pass 通过 `CreateShaderFromFile` 编译（如 `ndctriangle.hlsl`、`lighting.hlsl`、`GTAO.hlsl` 等），放在 `Engine/Shader/` 根目录或 `IBL/`、`Utility/`、`Compression/` 子目录；
2. **Unity 风格 .shader 资产**：由 `ShaderParser` 解析、`Shader` 类加载并**动态生成完整 HLSL**，支持多 Pass 与自定义 ShadingModel（`StandardPBR.shader`、`ToonPBR.shader`、`SkyShader/Sky.shader`、`Shadingmodel/Screen.shader`）。

运行时，`MaterialManager` 会把解析/生成的 HLSL 转储到 `Engine/Shader/Shader_Cache/`（调试文件），并在 `Shader_Cache/log/` 下写日志；每次启动 shader 都会重新编译（无二进制缓存）。

## 核心文件清单

| 文件/目录 | 职责 |
| --- | --- |
| `Engine/Shader/` | 手写 HLSL + Pass 专用着色器（见目录布局） |
| `Engine/Shader/StandardPBR.shader` / `ToonPBR.shader` | Unity 风格 shader 资产（多 Pass、ShadingModel） |
| `Engine/Shader/StandardPBR.material` | 旧式 XML 材质定义（兼容路径） |
| `Engine/Shader/SkyShader/Sky.shader` | 天空盒 shader（SkyPass 专用） |
| `Engine/Shader/Shadingmodel/Screen.shader` | 延迟光照聚合 shader（运行时注入 ShadingModel） |
| `Engine/public/Material/Shader.h` | `Shader` 类（多 Pass、PSO 管理） |
| `Engine/public/Material/ShaderParser.h` | Unity 风格解析器（Properties/Pass/ShadingModel） |
| `Engine/private/Material/Shader.cpp` | 加载/生成/编译/PSO 创建、`Shader_Cache` 转储 |
| `Engine/private/Material/ShaderParser.cpp` | 解析与 HLSL 代码生成（CB/纹理声明注入） |
| `Engine/private/Material/MaterialManager.cpp` | 缓存管理、日志（`Shader_Cache/log/*`） |
| `Engine/Shader/Shader_Cache/` | 运行时生成的 HLSL 与日志目录（自动创建） |

## 架构与数据流

### Shader 目录布局

```text
Engine/Shader/
├── 根目录（Pass 专用手写 HLSL）
│   ├── ndctriangle.hlsl          # BasePSO / UiPSO 全屏三角形
│   ├── gbuffer.hlsl              # GBuffer 填充（旧式）
│   ├── standard_pbr.hlsl         # 旧式 PBR 顶点/像素着色器
│   ├── lighting.hlsl             # LightPass 直接光照
│   ├── shadowdepth.hlsl          # ShadowPass 阴影深度
│   ├── GTAO.hlsl / GTAOBlur.hlsl # 环境光遮蔽 + 模糊
│   ├── SSGI.hlsl / SSGIBlur.hlsl / SSGIDepthMax.hlsl / SSGIUpsample.hlsl
│   ├── TAA.hlsl / TaaCopy.hlsl   # 时序抗锯齿 + 拷贝
│   ├── screen.hlsl               # ScreenPass（旧式延迟光照）
│   ├── skybox.hlsl               # 天空盒（旧式）
│   ├── SHCalculation.hlsl        # 球谐系数计算 CS（未接入，见 05-Lighting/IBL.md）
│   ├── Generated_GBufferPass.hlsl / Generated_DeferredLighting.hlsl  # 生成代码快照
│   ├── StandardPBR.shader / StandardPBR.material / ToonPBR.shader
│   └── ......
├── IBL/            # BRDFIntegration.hlsl / IrradianceConvolution.hlsl / PrefilterEnvMap.hlsl
├── SkyShader/      # Sky.shader（SkyPass 加载）
├── Shadingmodel/   # Screen.shader（延迟光照聚合，运行时注入 ShadingModel）
├── Utility/        # TexturePreview.hlsl（纹理预览面板）
└── Compression/    # BC1Compress.hlsl / BC3Compress.hlsl / BC5Compress.hlsl（纹理压缩 CS）
```

`ResourceManager::ScanShaders` 会扫描 `Engine/Shader/`（Engine 层）与 `Content/Shaders/`（Content 层）中的 `.shader`，但**排除** `screen.shader` / `sky.shader`（这两个由特殊 Pass 直接加载）。

### 资产格式

#### .shader（Unity 风格，`ShaderParser`）

```text
Shader "StandardPBR"
{
    RenderQueue "Deferred"        // 渲染队列：Deferred / Forward
    ShadingModel                  // 可选：自定义 BRDF（ToonPBR 示例）
    {
        shadingmodel=2;
        BRDF=ToonBRDF(N, V, L, albedo, metallic, roughness);
        BRDF { float3 ToonBRDF(...) { ... } }
    }
    Properties
    {
        //# float4 BaseColor {default(1.0,1.0,1.0,1.0), ui(ColorPicker)};
        //# float Roughness {default(0.5), min(0.0), max(1.0), ui(Slider)};
        //# Texture2D BaseColorTex;
        ...
    }
    Pass
    {
        Name "GBufferPass"
        HLSLPROGRAM
        #pragma vertex MainVS
        #pragma fragment MainPS
        ... 原始 HLSL ...
        ENDHLSL
    }
}
```

- `Properties` 行以 `//#` 注释形式书写，解析器用正则提取 `default(...)`、`min(...)`、`max(...)`、`ui(...)`；
- `Pass` 内 `HLSLPROGRAM ... ENDHLSL` 是纯净 HLSL，最终 HLSL 由 `ShaderParser::GenerateHLSLCode(passIndex)` 自动注入 CB / 纹理 / 采样器声明后生成；
- 每个 Pass 有入口点（`#pragma vertex/fragment`）与 `Name`；`RenderQueue "Deferred"` 的 shader，Pass 0 走 `CreateScenePSO`（GBuffer），Pass 1 走全屏 PSO（延迟光照，Screen.hlsl）。

#### .material（旧式 XML，兼容）

`StandardPBR.material` 是 XML 格式：`<Shader name="StandardPBR">` 内声明 VS/PS 文件、入口点、`<Parameters>`（类型/register/offset/默认值/UIWidget）、`<ConstantBufferLayout>`（b1，256 字节）、`<RenderState>`（CullMode/BlendState/DepthTest/DepthWrite）。新系统优先使用 `.shader` + 代码创建材质（`MaterialManager::CreateMaterial`）。

### 生成与编译流程

```text
LoadShader(path)（MaterialManager，带名称缓存）
└── Shader::LoadFromShaderFile
    ├── ShaderParser::ParseShaderFile（名称/RenderQueue/Properties/Passes/ShadingModel）
    ├── 注册自定义 ShadingModel → g_shadingModels（全局收集器）
    │     └── 新 ID 注册 → g_screenNeedsRecompile = true
    │           └── DeleteFileW(Shader_Cache/Screen.hlsl)（强制重新生成）
    ├── GenerateHLSLCode(passIndex)：注入 CB（DefaultVertexCB，含 ReservedMemory[1020] 占位）
    │     / 纹理声明（SkyCube t0 或 t4）/ 6 个采样器 / Bindless 宏
    │     （SAMPLE_TEXTURE_LOD → g_BindlessTextures[texIndex].SampleLevel）
    ├── 转储生成代码到 Shader_Cache/<Shader>_<Pass>.hlsl
    │     （特殊：Sky → Sky.hlsl；Screen → Screen.hlsl，并注入所有 ShadingModel 的 BRDF 函数与 switch case）
    └── 保存 PassInfo（vsEntry/psEntry/generatedHLSL）
└── Shader::CompileShaders(device)
    ├── 对每个 Pass：CompileHLSLString（D3DCompile，D3DCOMPILE_DEBUG | SKIP_OPTIMIZATION）
    ├── 失败 → 打印 error blob 到 stdout
    └── CreatePSO(passIndex)：Deferred Pass0 → CreateScenePSO；Deferred Pass1 → 全屏 PSO（CullNone、alpha 混合、无深度）
```

### ShadingModel 注入机制

- `g_shadingModels`（`std::map<int, ShaderParser::ShadingModelDefinition>`）收集所有 shader 声明的自定义 BRDF；`ShadingModelDefinition` 包含 `shadingModelID`、`brdfCall`、`brdfFunctionCode`；
- 生成 `Screen.hlsl` 时做两处字符串注入：在 `float3 BRDF(int shadingModelID ...` 函数前插入 BRDF 函数体；在 `default: return float3(0,0,0);` 前插入 `case N: return BRDF(...)`；
- `CheckAndRecompileScreen`（`Shader.cpp` 全局函数）：当 `g_screenNeedsRecompile` 时清除 Screen 缓存并重新 `LoadShader(L"Engine/Shader/Shadingmodel/Screen.shader")`，带 `g_isRecompilingScreen` 递归保护。

### Shader_Cache 机制

`Shader_Cache` 位于 `Engine/Shader/Shader_Cache/`，运行时自动创建（`MaterialManager.cpp` 的日志辅助函数与 `Shader.cpp` 的 `CreateDirectoryRecursive` 都会确保目录存在），包含：

| 内容 | 说明 |
| --- | --- |
| `<ShaderName>_<PassName>.hlsl` | 每个材质 shader 生成后的完整 HLSL 转储（调试用） |
| `Sky.hlsl` | Sky shader 的生成代码（统一文件名） |
| `Screen.hlsl` | Screen shader 的生成代码（含注入的 ShadingModel） |
| `log/cache_debug.txt` | Shader 缓存命中/重编译调试日志（`[CheckAndRecompileScreen]` 等） |
| `log/material_load.txt` | 材质加载信息日志（时间戳 + 消息，`LogMaterialInfo`） |
| `log/<material>_error.txt` | 材质加载错误日志（`LogMaterialError`，时间戳） |
| `log/ui_debug.txt` | 材质编辑器 UI 操作日志（`MaterialEditorPanel.cpp`） |

注意：**这不是二进制 PSO/字节码缓存**——每次启动都会重新解析、生成与编译；`Shader_Cache/*.hlsl` 只是“生成代码的调试转储”，`Screen.hlsl` 被删除是触发重编译的标志（通过 `g_screenNeedsRecompile`）。

### 运行时编译与日志

- 手写 .hlsl：`CreateShaderFromFile`（`BattleFireDirect.cpp`）编译失败时 `printf` 错误 blob 到控制台；
- 生成 HLSL：`CompileHLSLString` 失败时 `std::cout << "Shader compilation error: ..."`；`Shader.cpp` 全程有 `[DEBUG]` / `[ERROR]` 前缀的 `std::cout` 输出（Screen 重编译、ShadingModel 注册、生成文件路径等）；
- 材质系统：`LogMaterialInfo / LogMaterialError / LogMaterialSuccess` 写 `Shader_Cache/log/` 下带时间戳的追加日志；
- 编译目标：生成材质 shader 与手写 shader 均为 `vs_5_0 / ps_5_0`；IBL 计算着色器为 `cs_5_0`（见 `05-Lighting/IBL.md`）；
- 编译标志：一律 `D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION`（所有入口，无 Release 分支）。

## 关键实现要点

- **注入模板的确定性**：`ShaderParser::GenerateHLSLCode` 按 shader 名分支 —— `Sky` 只注入场景 CB（`ReservedMemory[1020]` 占位）与 `SkyCube t0`；`Deferred` 的 Pass 0 注入场景 CB + 材质 CB（b1，256 字节对齐）+ 纹理声明（t10-t12）；Pass 1（延迟光照）注入 `SkyCube t4` 与阴影图等；`Forward` 注入场景 CB 即可。所有分支都会追加 6 个静态采样器声明（s0-s5）以匹配根签名。
- **Bindless 采样宏**：生成的 HLSL 使用 `SAMPLE_TEXTURE_LOD(texIndex, sampler, uv, lod)` → `g_BindlessTextures[texIndex].SampleLevel(...)`，纹理索引通过材质 CB 传入（与全局 `srvHeap` 的 t10 起槽位分配对应，见 `Scene::AllocateBindlessSRVSlot`）。
- **CB 大小计算**：`Shader::CalculateConstantBufferSize` 按参数 byteOffset+byteSize 求最大结束位置，再向上取 256 字节对齐；`StandardPBR.material` 的布局为 BaseColor(0) / Roughness(16) / Metallic(20) / 纹理索引(24/28/32)，CB 总大小 256。
- **多 Pass 生成去重**：`Screen.hlsl` 只生成一次（静态标志 `screenPassGenerated`），新 ShadingModel 注册时置位 `g_screenNeedsRecompile` 强制重新生成；生成前会 `DeleteFileW` 旧文件。
- **天空盒特殊路径**：`Sky.shader` 的 PSO 不走 `CreateScenePSO`（其 input layout 是 float4 分量），`main.cpp` 用 `SkyPass::CreatePSO` 以 float3 POSITION 布局创建，并保留“sky shader 加载失败则禁用天空渲染”的回退。

## 对外接口

- `Shader`（`Engine/public/Material/Shader.h`）：`LoadFromXML`（旧）、`LoadFromShaderFile`、`CompileShaders(device)`、`CreatePSO(device, rootSig, passIndex)`、`GetPSO(passIndex)`、`GetPassCount/GetPassName`、`GetParameters/GetParameter`、`GetConstantBufferSize`、`GetVertexShaderBytecode/GetPixelShaderBytecode`、`GetRenderQueue`、`IsDeferredShader`；
- `ShaderParser`（`Engine/public/Material/ShaderParser.h`）：`ParseShaderFile`、`GetShaderName/GetRenderQueue/GetProperties/GetPasses/HasShadingModel/GetShadingModel`、`GenerateHLSLCode(passIndex)`、`GenerateShaderParameters`；
- 全局：`CheckAndRecompileScreen(ID3D12Device*, ID3D12RootSignature*, MaterialManager*)`、`g_shadingModels`（跨 TU 的全局收集器）。

## 配置与调参

| 项 | 值 | 位置 |
| --- | --- | --- |
| 材质 CB 大小 | 256 字节对齐（`CalculateConstantBufferSize`） | `Shader.cpp` |
| 材质 CB 寄存器 | b1（根签名 slot 2） | `InitRootSignature` / `GenerateMaterialCB` |
| 场景 CB | b0（176 floats，含 `ReservedMemory[1020]` 占位） | `BattleFireDirect.h` / `GenerateHLSLCode` |
| SkyCube 绑定 | Sky: t0；Screen/光照: t4 | `ShaderParser.cpp` |
| 纹理槽 | t10-t12（材质 BaseColor/Normal/ORM） | `Scene::CreateTextureSRV` |
| 编译目标 | vs/ps/cs_5_0 | 各编译入口 |
| 编译标志 | DEBUG + SKIP_OPTIMIZATION | 各编译入口 |
| 缓存目录 | `Engine/Shader/Shader_Cache/`（+ log/） | `MaterialManager.cpp` / `Shader.cpp` |
| ShadingModel ID | 1（StandardPBR 硬编码）、2（ToonPBR） | `StandardPBR.shader` / `ToonPBR.shader` |

## 已知限制与 TODO

- **无二进制着色器缓存**：每次启动全量解析 + 生成 + 编译（Debug 标志），启动耗时随 shader 数量线性增长。
- `Shader_Cache` 是调试转储目录，却被 `ResourceManager` 扫描（`Engine/Shader/` 递归），生成的 `.hlsl` 与 `log/` 文件会出现在资源浏览器里。
- Screen.hlsl 的 ShadingModel 注入依赖字符串查找（`float3 BRDF(int shadingModelID` 与 `default: return float3(0, 0, 0);`），模板变更会导致注入静默失败（有 `[ERROR]` 日志但不会中断）。
- `g_shadingModels` 是全局状态，`ClearShaderCache` 不会清除已注册的 ShadingModel；重新加载同名 shader 时靠 ID 去重，ID 冲突会静默跳过。
- `Screen.hlsl` 的删除/重编译逻辑耦合了静态标志（`screenPassGenerated`），多 shader 交错加载时行为依赖加载顺序。
- 生成 HLSL 中 `ReservedMemory[1020]` 占位使 CB 体积膨胀到 4096+ 字节（对齐需要），GPU 带宽开销大于实际数据量。
- `.material`（XML）与 `.shader`（Unity 风格）两套格式并存，新代码优先 `.shader`，旧资产兼容路径未删除。
- `Generated_GBufferPass.hlsl` / `Generated_DeferredLighting.hlsl` 是生成代码的仓库快照，与运行时 `Shader_Cache` 产物可能不同步。

## 维护注意事项

- 修改 `ShaderParser::GenerateHLSLCode` 的 CB 布局时，必须同步 `SceneCBData`（`BattleFireDirect.h`）与 `Shader_Cache` 中已生成的调试文件内容，否则运行时静默错位（CB 布局错误通常表现为光照/UV 错乱）。
- 新增自定义 ShadingModel 时：在 `.shader` 的 `ShadingModel` 块中声明 `shadingmodel=N` + `BRDF=函数名(...)` + `BRDF{...}` 函数体；ID 与 `GBuffer.ORM.w`（`shadingModelID / 255.0f`）的编码保持一致。
- Screen shader 模板的插入锚点（BRDF 函数声明、default case）不要随意改名；新增注入点时同步更新 `Shader.cpp` 两处注入逻辑（`LoadFromShaderFile` 内与 `Shader_Cache` 生成路径）。
- 调试着色器问题时优先看 `Engine/Shader/Shader_Cache/<Shader>_<Pass>.hlsl`（最终编译的就是这份代码）与 `log/` 下的时间戳日志；控制台 `[DEBUG]` 输出同样有效。
- `Shader_Cache` 是运行产物，建议加入忽略列表；清理缓存 = 删除 `Shader_Cache` 下生成的 `.hlsl` 与 `log/`（目录会在下次运行自动重建）。
- 修改编译目标（如升级到 `vs_6_0`/`cs_6_x`）时，检查所有 `D3DCompile`/`D3DCompileFromFile` 调用点与 IBL 计算着色器（`IBLResources.cpp`），并验证驱动支持度（当前 Feature Level 11.0）。