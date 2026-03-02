## FEngine

FEngine 是一个基于 DirectX 12 从零构建的实时渲染引擎，采用 C++ 开发，面向学习和实验现代图形渲染技术。

### 渲染架构

引擎采用多 Pass 延迟渲染架构，渲染管线包含以下阶段：

- **BasePass**：G-Buffer 输出，生成 Albedo、Normal、ORM（遮蔽/粗糙度/金属度）三张渲染目标。
- **ShadowPass**：阴影深度渲染，生成阴影贴图供后续光照使用。
- **LightPass**：延迟光照计算，结合 G-Buffer 和阴影贴图完成直接光照与阴影评估。
- **GtaoPass**：Ground Truth Ambient Occlusion（GTAO），屏幕空间环境遮蔽。从深度重建视空间位置，在法线半球内进行视线方向积分计算 AO，配合 Cross-Bilateral Blur 进行边缘保持降噪。支持可调节的 AO 半径、强度、切片数和步进数，并内置屏幕边缘淡化消除伪影。GTAO 关闭时自动回退到白色纹理（AO=1，无遮蔽）。
- **SsgiPass**：屏幕空间全局光照（Screen Space Global Illumination），基于蒙特卡洛屏幕空间光线追踪实现间接漫反射照明。在法线半球内进行余弦加权采样，将视图空间射线投影到屏幕空间并以透视校正深度插值逐像素步进，命中表面后采样 Albedo 并计算距离衰减。渲染管线由多个子 Pass 构成：SSGI Raymarch → Cross-Bilateral Blur（横向 + 纵向分离模糊）。输出 RGBA16F 纹理，RGB 为间接光颜色，A 通道为射线命中率权重。合成阶段利用命中率在 SSGI 与 IBL 之间 lerp 混合，未命中的射线由 IBL 环境光补充。支持可调节的方向数、步进数、追踪半径和强度参数。关闭时自动回退到黑色纹理（无间接光贡献）。
- **ScreenPass**：全屏后处理合成，将直接光照、阴影、IBL 间接光、SSGI 间接光、GTAO 遮蔽（含多次反弹近似）、天空球等合并为最终画面。当 SSGI 开启时，使用命中率权重在 IBL 和 SSGI 之间进行 lerp 混合。
- **SkyPass**：天空球渲染，支持 Cubemap 采样。
- **TaaPass**：时域抗锯齿（TAA），基于 Halton 序列抖动采样，结合 Motion Vector 实现帧间混合与历史重投影。

### 材质系统

材质系统参考 Unity ShaderLab 风格，支持自定义 `.shader` 文件格式。引擎自动解析 Shader 文件，生成 HLSL 代码、编译着色器并创建 PSO。内置两种着色模型：

- **StandardPBR**：标准基于物理的渲染着色模型。
- **ToonPBR**：卡通风格着色模型。

支持多 Pass 渲染和材质实例化（MaterialInstance）。材质编辑器基于 ImGui 实现，可实时调整参数并即时预览效果。材质资产格式为 `.material`。

### PBR 与 IBL 光照

引擎实现了完整的 IBL（Image-Based Lighting）管线：

- 通过 Compute Shader 预计算 **BRDF LUT**（双向反射分布函数积分查找表）。
- **Irradiance Map**：漫反射辐照度卷积贴图。
- **Prefiltered Environment Map**：不同粗糙度等级的预滤波环境贴图，用于镜面反射。
- 集成 **球谐光照（Spherical Harmonics, SH）**，用于 Skylight 漫反射间接光照。

### 纹理系统

纹理系统集成了 NVIDIA Texture Tools，支持运行时将 PNG/JPG/HDR 等格式压缩为 BC3/BC7 DDS，带纹理缓存机制。支持 Cubemap、2D 纹理，资产格式为 `.texture.ast`。编辑器提供纹理预览面板。

### 场景管理

场景管理支持 Actor-Component 模式：

- 每个 **Actor** 拥有独立的 Transform、StaticMeshComponent 和 MaterialInstance。
- 支持 `.mesh` 资产描述文件，引用 FBX 模型源文件。
- 支持 `.level` 关卡序列化与加载。
- 支持 FBX 模型导入（基于 FBX SDK 2020.3.7）。

### 编辑器

编辑器基于 ImGui，提供以下功能面板：

- **资源浏览器**：文件树结构浏览项目资产。
- **材质编辑器**：实时调整着色参数、切换着色模型。
- **纹理预览**：查看纹理资产详情与压缩格式。
- **场景层级面板**：管理场景中的 Actor 层级关系。
- **GTAO 调试面板**：实时调节 AO 半径、强度、切片数、步进数等参数。
- **SSGI 调试面板**：支持开关 GI 模式（Off / SSGI），实时调节方向数、步进数、追踪半径、强度等参数。

### 项目构建

项目已实现路径可移植化，通过 exe 位置动态推算项目根目录，无硬编码绝对路径，可直接 clone 后编译运行。

- 开发环境：Visual Studio 2019/2022
- 图形 API：DirectX 12
- 第三方依赖：ImGui、DirectXTex、FBX SDK、NVIDIA Texture Tools、stb_image