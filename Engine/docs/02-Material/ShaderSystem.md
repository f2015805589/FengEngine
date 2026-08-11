# Shader 系统（ShaderSystem）

> 本文依据 `Engine/private/Material/ShaderParser.cpp`、`Engine/private/Material/Shader.cpp` 编写，说明 `.shader` 资产（Unity ShaderLab 风格）的解析、HLSL 自动生成、编译（D3DCompile）、PSO 创建与 RenderQueue 机制。与材质实例的关系见 `02-Material/MaterialSystem.md`。

## 概述

ShaderSystem 是材质系统的「编译前端」：把引擎自定义的 Unity ShaderLab 风格 `.shader` 文本，通过 `ShaderParser` 解析出 Properties（材质参数）、ShadingModel（着色模型）与 Pass（VS/PS 入口与 HLSL 代码），再由 `Shader` 类按 Pass 注入引擎约定的常量缓冲区/纹理声明，生成完整 HLSL 源码，用 `D3DCompile`（vs_5_1 / ps_5_1）编译，最后创建 PSO。

关键特性：

- **Properties → 常量缓冲区**：`//#` 注释行声明参数，自动生成 `cbuffer MaterialConstants : register(b1)`，非纹理参数 16 字节对齐，纹理参数生成 `uint <Name>Index`（Bindless 索引）。
- **Bindless 纹理槽**：自动生成 `Texture2D g_BindlessTextures[1024] : register(t10)` 与 `SAMPLE_TEXTURE` 宏，材质只需往 CB 写 SRV 索引。
- **多 Pass / 延迟渲染**：Deferred shader 若只有一个 Pass，会自动把 `Engine/Shader/Shadingmodel/Screen.shader` 的第一个 Pass 挂为 Pass 1（延迟光照）。
- **ShadingModel 注入**：带 `ShadingModel{}` 块的 shader 会把 BRDF 函数与 `case` 注入到 Screen shader 的 `BRDF()` switch 中（详见 ShadingModels.md）。
- **RenderQueue**：Shader 级 `RenderQueue "Deferred"/"Forward"` 决定代码生成前缀与 PSO 形态。

## 核心文件清单

| 文件 | 职责 |
| --- | --- |
| `Engine/public/Material/ShaderParser.h` / `Engine/private/Material/ShaderParser.cpp` | `.shader` 文本解析、HLSL 前缀生成、参数列表生成 |
| `Engine/public/Material/Shader.h` / `Engine/private/Material/Shader.cpp` | Shader 生命周期：解析结果落地、HLSL 生成、编译、PSO、ShadingModel 注册与 Screen 注入 |
| `Engine/public/Material/ShaderParameter.h` | 参数类型与布局结构（byteOffset/byteSize/registerSlot/uiWidget） |
| `Engine/public/Material/MaterialManager.h` / `.cpp` | Shader 缓存与两阶段编译调度（CompileAndCreateAllShadersPSO） |
| `Engine/Shader/StandardPBR.shader` / `ToonPBR.shader` / `Shadingmodel/Screen.shader` / `SkyShader/Sky.shader` | 资产样例 |
| `Engine/Shader/Shader_Cache/*.hlsl` | 运行时生成的完整 HLSL（调试产物，不入库） |
| `Engine/private/BattleFireDirect.cpp` | 根签名（b0 场景 CB / Bindless SRV 表 / b1 材质 CB）与 `CreateScenePSO` |

## 架构与数据流

### .shader 资产结构（Unity ShaderLab 风格）

```
Shader "StandardPBR"
{
    RenderQueue "Deferred"              // 顶层队列（可选，默认 Forward）
    Properties {                        // 材质参数（//# 注释行）
        //# float4 BaseColor {default(1.0, 1.0, 1.0, 1.0), ui(ColorPicker)};
        //# float Roughness {default(0.5), min(0.0), max(1.0), ui(Slider)};
        //# Texture2D BaseColorTex;
    }
    ShadingModel {                      // 可选：着色模型（ToonPBR 有）
        shadingmodel=2;
        BRDF=ToonBRDF(N, V, L, albedo, metallic, roughness);
        BRDF { /* 函数定义 */ }
    }
    Pass {
        Name "GBufferPass"
        HLSLPROGRAM
        #pragma vertex MainVS
        #pragma fragment MainPS
        /* 纯 HLSL 代码 */
        ENDHLSL
    }
}
```

### 解析流程（ShaderParser::ParseShaderFile）

1. 正则 `Shader\s+"([^"]+)"` 提取名称（失败即返回 false）。
2. 正则 `RenderQueue\s+"([^"]+)"` 提取队列，缺省 `"Forward"`。
3. `content.find("Properties")` → `ParseProperties`：在块内逐行找 `//#`，`ParsePropertyLine` 解析 `<type> <name> {default(...), min(...), max(...), ui(...)}`；纹理无 `{}` 时默认 `ui(TexturePicker)`。
4. `content.find("ShadingModel")` → `ParseShadingModel`：解析 `shadingmodel=ID`、`BRDF=调用;` 与 `BRDF{函数体}`；失败仅告警不影响加载。
5. 循环 `content.find("Pass")` → `ParsePass`：解析 `Name`、Pass 级 `RenderQueue`、`#pragma vertex/fragment` 入口点，移除 `#pragma` 行得到纯 HLSL。
6. Deferred 特判：若 `m_renderQueue == "Deferred"` 且只有 1 个 Pass，自动 `ParseShaderFile(L"Engine/Shader/Shadingmodel/Screen.shader")` 并把其 Pass 0 追加为 Pass 1；若有 2 个 Pass 则保留；否则报错。

### HLSL 生成（ShaderParser::GenerateHLSLCode）

按 shader 名与队列注入不同前缀：

| 场景 | 注入内容 |
| --- | --- |
| `Sky`（特判） | `cbuffer DefaultVertexCB : register(b0)`（含 `ReservedMemory[1020]`）、`TextureCube SkyCube : register(t0)`、采样器 s0-s5 |
| Deferred Pass 0（GBuffer） | 完整场景 CB（含 `LightViewProjectionMatrix`、`PreviousViewProjectionMatrix`、`JitterOffset`、`ScreenSize`、`NearPlane/FarPlane`、`CurrentViewProjectionMatrix`）+ `GenerateMaterialCB()` + 场景纹理 `g_Cubemap(t0)/g_Color(t1)/g_Normal(t2)/g_Orm(t3)` + Bindless 纹理声明 + 采样器 |
| Deferred Pass 1（Screen） | 场景 CB（`ReservedMemory[1020]`，与旧 screen.hlsl 布局兼容）+ `BaseColor(t0)/Normal(t1)/Orm(t2)/DepthTexture(t3)/SkyCube(t4)/ShadowMap(t5)/GTAOTexture(t6)/SSGITexture(t7)` + 采样器 |
| Forward | `SceneConstants : register(b0)`（g_matWorld/g_matView/g_matProj/g_matWorldViewProj/g_CameraPos/g_time/g_LightDir/g_LightColor/g_LightIntensity）+ `g_SkyboxTex(t0)` + 材质 CB + 纹理声明 |

`GenerateMaterialCB()` 生成：

- 非纹理参数按 16 字节对齐（`offset % 16 + size > 16` 时对齐到下一个 16 字节）；Bool 转 `int`。
- 纹理参数生成 `uint <Name>Index;`（Bindless，4 字节）。
- 若总长不足 256 字节，补 `float4 _Padding[N]` 到 256。

`GenerateTextureDeclarations()` 生成：

```hlsl
Texture2D g_BindlessTextures[1024] : register(t10);
#define SAMPLE_TEXTURE(texIndex, sampler, uv) g_BindlessTextures[texIndex].Sample(sampler, uv)
#define SAMPLE_TEXTURE_LOD(texIndex, sampler, uv, lod) g_BindlessTextures[texIndex].SampleLevel(sampler, uv, lod)
#define Sample<Name>(sampler, uv) SAMPLE_TEXTURE(<Name>Index, sampler, uv)
```

### Shader::LoadFromShaderFile 与 Screen 注入

1. `GenerateShaderParameters()` 生成 `m_parameters`（非纹理参数记录 `byteOffset`；纹理参数 `registerSlot` 从 10 起、`byteSize=4`、CB 内偏移紧随其后）。
2. `CalculateConstantBufferSize()` 按最大 offset+size 向上 256 对齐。
3. 若带 ShadingModel 且 ID 未注册：写入全局 `g_shadingModels[ID]`，置 `g_screenNeedsRecompile = true`，并删除 `Engine/Shader/Shader_Cache/Screen.hlsl` 缓存。
4. 每个 Pass 生成 `generatedHLSL` 并写入 `Engine/Shader/Shader_Cache/`（Sky → `Sky.hlsl`；Screen → `Screen.hlsl`；其他 → `<Shader>_<Pass>.hlsl`）。Screen.hlsl 会做两处字符串注入：
   - 在 `float3 BRDF(int shadingModelID` 前插入所有已注册的 `brdfFunctionCode`；
   - 在 `default: return float3(0, 0, 0);` 前插入 `case <ID>: return <BRDF调用>;`。
5. `m_useGeneratedHLSL = true`，后续 `CompileShaders` 走生成代码路径。

### 编译与 PSO

- `CompileShaders`：对每个 Pass 用 `D3DCompile` 编译 `generatedHLSL`，target 为 `vs_5_1` / `ps_5_1`（Bindless 需要），flag `D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION`；失败时输出 `Engine/Shader/<Pass>_VS_Error.txt` / `_PS_Error.txt` 并 `MessageBoxA`。
- `CreatePSO(device, rootSig, passIndex)`：
  - Deferred Pass 0 / Forward：`CreateScenePSO`（场景 mesh 的 input layout）。
  - Deferred Pass 1：全屏 quad input layout（`POSITION` float3 + `TEXCOORD` float2），`CullMode=NONE`、Alpha 混合（天空盒透明）、关闭深度、单 RT `R8G8B8A8_UNORM`。
- `MaterialManager::CompileAndCreateAllShadersPSO`：阶段 1 编译非 Screen（注册 ShadingModel），阶段 2 编译 Screen（此时全部 BRDF 已注入）。
- `CheckAndRecompileScreen`（全局函数）：置位后清除 Screen 缓存并 `LoadShader` 重新编译，带 `g_isRecompilingScreen` 防递归。

## 关键实现要点

- **对齐规则**：生成 HLSL 与 `GenerateShaderParameters` 使用同一套 16 字节对齐算法，保证 CPU 打包偏移与 HLSL 声明一致。
- **纹理索引语义**：CB 里存的是「相对索引」= 全局 Bindless 槽位 − 10（`MaterialInstance::LoadTexturesFromPaths` 中 `relativeIndex = srvIndex - 10`）。
- **兼容旧格式**：`LoadFromXML`（MSXML 解析 `.shader.ast`）与文件编译路径（vs_5_0/ps_5_0 + `CreateShaderFromFile`）仍保留，`MaterialManager::LoadShader` 按扩展名分发。
- **渲染状态**：`m_cullMode/m_depthTest/m_depthWrite` 成员存在，但新 `.shader` 解析未消费（旧 XML 有 `<RenderState>` 节点）。
- **多 Pass 访问**：`GetPassCount()/GetPassName(i)/GetPSO(i)/GetVertexShaderBytecode(i)/GetPixelShaderBytecode(i)`，向后兼容接口默认取 Pass 0。

## 对外接口

| 接口 | 说明 |
| --- | --- |
| `Shader::LoadFromXML(path)` | 旧 XML 格式加载（保留兼容） |
| `Shader::LoadFromShaderFile(path)` | 新 Unity 风格加载：解析 + 注册 ShadingModel + 生成 HLSL 缓存 |
| `Shader::CompileShaders(device)` | 编译全部 Pass 的 VS/PS 字节码 |
| `Shader::CreatePSO(device, rootSig, passIndex=0)` | 为指定 Pass 创建 PSO（生命周期由外部/析构管理） |
| `Shader::GetParameters()/GetParameter(name)` | 参数布局（byteOffset/byteSize/registerSlot） |
| `Shader::GetConstantBufferSize()` | CB 大小（256 对齐） |
| `Shader::GetRenderQueue()/IsDeferredShader()` | 队列类型判断 |
| `ShaderParser::ParseShaderFile/GenerateHLSLCode/GenerateShaderParameters` | 解析与生成接口 |
| `MaterialManager::LoadShader/GetShader/ClearShaderCache/CompileAndCreateAllShadersPSO` | 缓存与编译调度 |
| `CheckAndRecompileScreen(device, rootSig, matMgr)` | Screen shader 热重编译（新 ShadingModel 注册后） |

## 配置与调参

- **Properties 写法**：`//# <type> <name> {default(...), min(...), max(...), ui(...)};`，类型支持 `float/float2/float3/float4/int/bool/Texture2D/TextureCube/float4x4`；`default` 支持逗号分隔的向量。
- **RenderQueue 取值**：`"Deferred"`（GBuffer+自动挂 Screen Pass）或 `"Forward"`（注入前向常量）。`"Deferred"` 时 Pass 数必须为 1 或 2。
- **编译目标**：固定 `vs_5_1`/`ps_5_1`，Debug 模式无优化；想发布优化需移除 `D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION`。
- **生成产物**：`Engine/Shader/Shader_Cache/` 下按 `Shader_Pass.hlsl` 命名；`Screen.hlsl` 为全局共享（含所有 ShadingModel 注入）。
- **根签名约束**：材质 CB 固定 b1（slot 2）、Bindless 表从 t10 起；修改需同步 `InitRootSignature()` 与 `ShaderParser` 生成代码。

## 已知限制与TODO

- `ParseProperties` 只识别 `//#` 注释行，Unity 原生 `[Header]/[Toggle]` 属性语法不支持。
- `Pass` 关键字出现在注释/字符串中会被误判（`content.find("Pass")` 朴素搜索）。
- Screen 注入依赖精确字符串匹配（`"float3 BRDF(int shadingModelID"`、`"default: return float3(0, 0, 0);"`），改 Screen.shader 函数签名需同步改注入锚点。
- `screenPassGenerated` 是文件内 `static` 状态，跨多次加载可能跳过重新生成；`g_screenNeedsRecompile` 标志位依赖 `CheckAndRecompileScreen` 正确复位。
- Deferred Pass 1 的 PSO 固定单 RT `R8G8B8A8_UNORM`（交换链格式），若输出目标变更需同步。
- `GenerateMaterialCB` 是简化对齐（未处理 HLSL packoffset 的复杂嵌套结构），Matrix4x4 只按 64 字节连续排布。
- 旧的 `LoadFromXML` 与文件编译路径（vs_5_0）已废弃但未删除，维护时注意双路径分叉。

## 维护注意事项

- 新增 shader：放到 `Engine/Shader/` 下即可被 `ResourceManager` 扫描（`screen.shader`/`Sky.shader` 会被排除，需显式加载）。
- 修改 Screen.shader 或新增 ShadingModel 后，先删 `Engine/Shader/Shader_Cache/Screen.hlsl` 再启动，避免陈旧缓存。
- 编译错误定位顺序：控制台 `Shader compilation error` → `Engine/Shader/<Pass>_*_Error.txt` → 生成的 `Shader_Cache/*.hlsl`。
- 参数顺序调整会改变 CB 偏移，必须同时保证 `GenerateHLSLCode` 与 `GenerateShaderParameters` 的对齐逻辑一致，否则 CPU 打包与 GPU 布局错位。
- ShadingModel ID 全局唯一（`g_shadingModels` 以 ID 为键），重复 ID 会静默跳过注册。
*** End Patch