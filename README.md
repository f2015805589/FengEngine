FEngine 是一个基于 DirectX 12 从零构建的实时渲染引擎，采用 C++ 开发，面向学习和实验现代图形渲染技术。

引擎采用多 Pass 延迟渲染架构，渲染管线包含 BasePass（G-Buffer 输出 Albedo、Normal、ORM）、LightPass（光照计算与阴影）、ScreenPass（全屏后处理合成）、SkyPass（天空球渲染）以及 TaaPass（时域抗锯齿）。TAA 基于 Halton 序列抖动采样，结合 Motion Vector 实现帧间混合。

材质系统参考 Unity ShaderLab 风格，支持自定义 .shader 文件格式，引擎自动解析并生成 HLSL 代码、编译、创建 PSO。内置 StandardPBR 和 ToonPBR 两种着色模型，支持多 Pass 渲染和材质实例化。材质编辑器基于 ImGui 实现，可实时调整参数。

PBR 光照方面，引擎实现了完整的 IBL 管线：通过 Compute Shader 预计算 BRDF LUT、Irradiance Map 和 Prefiltered Environment Map，并集成球谐光照（SH）用于 Skylight 漫反射。

纹理系统集成了 NVIDIA Texture Tools，支持运行时将 PNG/JPG/HDR 等格式压缩为 BC3/BC7 DDS，带纹理缓存和预览面板。支持 Cubemap、2D 纹理、.texture.ast 资产格式。

场景管理支持 Actor-Component 模式，每个 Actor 拥有独立的 Transform、StaticMeshComponent 和 MaterialInstance。支持 .mesh 资产描述文件、.level 关卡序列化、FBX 模型导入。

编辑器基于 ImGui，提供资源浏览器（文件树）、材质编辑器、纹理预览、场景层级面板等功能。

项目已实现路径可移植化，通过 exe 位置动态推算项目根目录，无硬编码绝对路径，可直接 clone 后编译运行。
