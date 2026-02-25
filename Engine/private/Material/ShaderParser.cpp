#include "public/Material/ShaderParser.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <iostream>

#ifdef _WIN32
#include <Windows.h>
#endif

ShaderParser::ShaderParser() {
}

bool ShaderParser::ParseShaderFile(const std::wstring& filePath) {
    // 读取文件
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::wcout << L"Failed to open shader file: " << filePath << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // 1. 解析Shader名称
    std::regex shaderNameRegex("Shader\\s+\"([^\"]+)\"");
    std::smatch match;
    if (std::regex_search(content, match, shaderNameRegex)) {
        m_shaderName = match[1].str();
    } else {
        std::cout << "Failed to find Shader name" << std::endl;
        return false;
    }

    // 2. 解析顶层RenderQueue（可选）
    std::regex renderQueueRegex("RenderQueue\\s+\"([^\"]+)\"");
    if (std::regex_search(content, match, renderQueueRegex)) {
        m_renderQueue = match[1].str();
    } else {
        m_renderQueue = "Forward";  // 默认为Forward
    }

    // 3. 解析Properties块
    size_t pos = content.find("Properties");
    if (pos != std::string::npos) {
        if (!ParseProperties(content, pos)) {
            std::cout << "Failed to parse Properties block" << std::endl;
            return false;
        }
    }

    // 4. 尝试解析ShadingModel块（可选，不存在也不报错）
    pos = content.find("ShadingModel");
    if (pos != std::string::npos) {
        // 只有找到ShadingModel块时才解析
        if (!ParseShadingModel(content, pos)) {
            // 解析失败只警告，不影响整个shader加载
            std::cout << "WARNING: Failed to parse ShadingModel block, skipping..." << std::endl;
            m_hasShadingModel = false;
        }
    }

    // 5. 解析Pass块
    pos = 0;
    while ((pos = content.find("Pass", pos)) != std::string::npos) {
        if (!ParsePass(content, pos)) {
            std::cout << "Failed to parse Pass block" << std::endl;
            return false;
        }
        pos++;
    }

    if (m_passes.empty()) {
        std::cout << "No Pass found in shader" << std::endl;
        return false;
    }

    // 对于Deferred shader，自动加载Screen.shader作为第二个Pass
    if (m_renderQueue == "Deferred") {
        if (m_passes.size() == 1) {
            // 只有一个Pass（GBuffer填充），自动从Screen.shader加载第二个Pass
            ShaderParser screenParser;
            if (!screenParser.ParseShaderFile(L"Engine/Shader/Shadingmodel/Screen.shader")) {
                std::cerr << "ERROR: Failed to load Screen.shader for deferred rendering!" << std::endl;
                return false;
            }

            // 获取Screen.shader的第一个Pass（也就是延迟光照Pass）
            const auto& screenPasses = screenParser.GetPasses();
            if (screenPasses.empty()) {
                std::cerr << "ERROR: Screen.shader has no passes!" << std::endl;
                return false;
            }

            // 添加Screen.shader的Pass作为第二个Pass
            m_passes.push_back(screenPasses[0]);
            std::cout << "Auto-loaded Screen.shader as Pass 1 for deferred shader: " << m_shaderName << std::endl;
        }
        else if (m_passes.size() != 2) {
            std::cerr << "ERROR: Deferred shader '" << m_shaderName
                      << "' should have 1 or 2 passes, but found " << m_passes.size()
                      << " passes!" << std::endl;
            return false;
        }
    }

    return true;
}

bool ShaderParser::ParseProperties(const std::string& content, size_t& pos) {
    // 找到Properties块的开始和结束
    size_t blockStart = content.find('{', pos);
    if (blockStart == std::string::npos) return false;

    size_t blockEnd = FindBlockEnd(content, blockStart);
    if (blockEnd == std::string::npos) return false;

    // 提取Properties块内容
    std::string propertiesBlock = content.substr(blockStart + 1, blockEnd - blockStart - 1);

    // 逐行解析
    std::istringstream stream(propertiesBlock);
    std::string line;
    while (std::getline(stream, line)) {
        // 查找 //# 标记的属性定义
        if (line.find("//#") != std::string::npos) {
            PropertyDefinition prop;
            if (ParsePropertyLine(line, prop)) {
                m_properties.push_back(prop);
                std::cout << "ShaderParser: Parsed property '" << prop.name << "' type=" << (int)prop.type << std::endl;
            }
        }
    }

    std::cout << "ShaderParser: Total properties parsed: " << m_properties.size() << std::endl;
    return true;
}

bool ShaderParser::ParsePass(const std::string& content, size_t& pos) {
    PassDefinition pass;

    // 找到Pass块的开始
    size_t blockStart = content.find('{', pos);
    if (blockStart == std::string::npos) return false;

    size_t blockEnd = FindBlockEnd(content, blockStart);
    if (blockEnd == std::string::npos) return false;

    std::string passBlock = content.substr(blockStart + 1, blockEnd - blockStart - 1);

    // 解析Pass Name（可选）
    std::regex nameRegex("Name\\s+\"([^\"]+)\"");
    std::smatch match;
    if (std::regex_search(passBlock, match, nameRegex)) {
        pass.name = match[1].str();
    } else {
        pass.name = "Pass" + std::to_string(m_passes.size());
    }

    // 解析RenderQueue（可选，默认为Forward）
    std::regex queueRegex("RenderQueue\\s+\"([^\"]+)\"");
    if (std::regex_search(passBlock, match, queueRegex)) {
        pass.renderQueue = match[1].str();
    } else {
        pass.renderQueue = "Forward";  // 默认前向渲染
    }

    // 查找HLSLPROGRAM...ENDHLSL块
    size_t hlslStart = passBlock.find("HLSLPROGRAM");
    size_t hlslEnd = passBlock.find("ENDHLSL");

    if (hlslStart == std::string::npos || hlslEnd == std::string::npos) {
        std::cout << "HLSLPROGRAM block not found in Pass" << std::endl;
        return false;
    }

    std::string hlslBlock = passBlock.substr(hlslStart + 11, hlslEnd - hlslStart - 11);

    // 解析#pragma指令
    std::regex vertexRegex("#pragma\\s+vertex\\s+(\\w+)");
    std::regex fragmentRegex("#pragma\\s+fragment\\s+(\\w+)");

    if (std::regex_search(hlslBlock, match, vertexRegex)) {
        pass.vsEntry = match[1].str();
    }

    if (std::regex_search(hlslBlock, match, fragmentRegex)) {
        pass.psEntry = match[1].str();
    }

    // 移除#pragma行，保留纯HLSL代码
    std::regex pragmaRegex("#pragma[^\\n]*\\n");
    pass.hlslCode = std::regex_replace(hlslBlock, pragmaRegex, "");

    m_passes.push_back(pass);
    pos = blockEnd;

    return true;
}

bool ShaderParser::ParsePropertyLine(const std::string& line, PropertyDefinition& outProp) {
    // 格式: //# <type> <name> {default(...), min(...), max(...), ui(...)};

    // 移除 //# 前缀和分号
    std::string trimmed = line;
    size_t start = trimmed.find("//#");
    if (start == std::string::npos) return false;
    trimmed = trimmed.substr(start + 3);

    // 移除分号
    size_t semiPos = trimmed.find(';');
    if (semiPos != std::string::npos) {
        trimmed = trimmed.substr(0, semiPos);
    }

    // 解析类型和名称
    std::regex typeNameRegex("\\s*(\\w+)\\s+(\\w+)");
    std::smatch match;
    if (!std::regex_search(trimmed, match, typeNameRegex)) {
        return false;
    }

    std::string typeStr = match[1].str();
    outProp.name = match[2].str();
    outProp.type = ParseType(typeStr);

    // 解析属性块 {...}
    size_t braceStart = trimmed.find('{');
    size_t braceEnd = trimmed.find('}');

    if (braceStart != std::string::npos && braceEnd != std::string::npos) {
        std::string attrs = trimmed.substr(braceStart + 1, braceEnd - braceStart - 1);

        // 解析default
        std::regex defaultRegex("default\\s*\\(\\s*([^)]+)\\s*\\)");
        if (std::regex_search(attrs, match, defaultRegex)) {
            outProp.defaultValue = match[1].str();
        }

        // 解析min
        std::regex minRegex("min\\s*\\(\\s*([\\d.]+)\\s*\\)");
        if (std::regex_search(attrs, match, minRegex)) {
            outProp.minValue = std::stof(match[1].str());
        }

        // 解析max
        std::regex maxRegex("max\\s*\\(\\s*([\\d.]+)\\s*\\)");
        if (std::regex_search(attrs, match, maxRegex)) {
            outProp.maxValue = std::stof(match[1].str());
        }

        // 解析ui
        std::regex uiRegex("ui\\s*\\(\\s*(\\w+)\\s*\\)");
        if (std::regex_search(attrs, match, uiRegex)) {
            outProp.uiWidget = match[1].str();
        }
    } else {
        // 对于纹理，没有{...}块
        if (outProp.type == ShaderParameterType::Texture2D ||
            outProp.type == ShaderParameterType::TextureCube) {
            outProp.uiWidget = "TexturePicker";
        }
    }

    return true;
}

size_t ShaderParser::FindBlockStart(const std::string& content, size_t pos, const std::string& blockName) {
    size_t namePos = content.find(blockName, pos);
    if (namePos == std::string::npos) return std::string::npos;
    return content.find('{', namePos);
}

size_t ShaderParser::FindBlockEnd(const std::string& content, size_t pos) {
    int braceCount = 1;
    size_t i = pos + 1;

    while (i < content.length() && braceCount > 0) {
        if (content[i] == '{') braceCount++;
        else if (content[i] == '}') braceCount--;
        i++;
    }

    return (braceCount == 0) ? i - 1 : std::string::npos;
}

std::string ShaderParser::GenerateHLSLCode(int passIndex) const {
    if (passIndex < 0 || passIndex >= m_passes.size()) {
        return "";
    }

    const auto& pass = m_passes[passIndex];
    std::ostringstream code;

    // 特殊处理：Sky shader
    if (m_shaderName == "Sky") {
        // Sky shader 使用与 Screen shader 相同的常量缓冲区布局（因为共享 Scene CB）
        code << "cbuffer DefaultVertexCB : register(b0)\n";
        code << "{\n";
        code << "    float4x4 ProjectionMatrix;\n";
        code << "    float4x4 ViewMatrix;\n";
        code << "    float4x4 ModelMatrix;\n";
        code << "    float4x4 IT_ModelMatrix;\n";
        code << "    float3 LightDirection;\n";
        code << "    float _LightPadding;\n";
        code << "    float3 CameraPositionWS;\n";
        code << "    float _CameraPadding;\n";
        code << "    float Skylight;\n";
        code << "    float3 _Padding0;\n";
        code << "    float4x4 InverseProjectionMatrix;\n";
        code << "    float4x4 InverseViewMatrix;\n";
        code << "    float3 SkylightColor;\n";
        code << "    float _Padding1;\n";
        code << "    float4x4 ReservedMemory[1020];\n";
        code << "};\n\n";

        // Sky shader 只需要 SkyCube 纹理（t0）
        code << "TextureCube SkyCube : register(t0);\n\n";

        // 采样器
        code << "SamplerState gSamPointWrap : register(s0);\n";
        code << "SamplerState gSamPointClamp : register(s1);\n";
        code << "SamplerState gSamLinearWarp : register(s2);\n";
        code << "SamplerState gSamLinearClamp : register(s3);\n";
        code << "SamplerState gSamAnisotropicWarp : register(s4);\n";
        code << "SamplerState gSamAnisotropicClamp : register(s5);\n\n";

        // 添加原始 HLSL 代码
        code << m_passes[passIndex].hlslCode;
        return code.str();
    }

    // 根据Shader级别的RenderQueue和Pass索引决定注入什么
    if (m_renderQueue == "Deferred") {
        if (passIndex == 0) {
            // Pass 0: GBuffer填充 - 需要scene CB和材质CB
            code << "cbuffer DefaultVertexCB : register(b0)\n";
            code << "{\n";
            code << "    float4x4 ProjectionMatrix;\n";
            code << "    float4x4 ViewMatrix;\n";
            code << "    float4x4 ModelMatrix;\n";
            code << "    float4x4 IT_ModelMatrix;\n";
            code << "    float3 LightDirection;\n";
            code << "    float _LightPadding;\n";
            code << "    float3 CameraPositionWS;\n";
            code << "    float _CameraPadding;\n";
            code << "    float Skylight;\n";  // 场景级Skylight参数
            code << "    float3 _Padding0;\n";  // 保持16字节对齐
            code << "    float4x4 InverseProjectionMatrix;\n";
            code << "    float4x4 InverseViewMatrix;\n";
            code << "    float3 SkylightColor;\n";
            code << "    float _Padding1;\n";
            code << "    float4x4 LightViewProjectionMatrix;\n";  // 阴影矩阵
            code << "    float4x4 PreviousViewProjectionMatrix;\n";  // TAA: 上一帧VP矩阵
            code << "    float2 JitterOffset;\n";  // TAA: 当前帧Jitter
            code << "    float2 PreviousJitterOffset;\n";  // TAA: 上一帧Jitter
            code << "    float2 ScreenSize;\n";  // 屏幕尺寸
            code << "    float2 InverseScreenSize;\n";  // 屏幕尺寸倒数
            code << "    float NearPlane;\n";  // 近裁剪面
            code << "    float FarPlane;\n";  // 远裁剪面
            code << "    float2 _Padding2;\n";  // 保持16字节对齐
            code << "    float4x4 CurrentViewProjectionMatrix;\n";  // TAA: 当前帧VP矩阵（不带Jitter，用于Motion Vector）
            code << "};\n\n";

            // 注入材质常量缓冲区（b1）
            code << GenerateMaterialCB() << "\n\n";

            // 使用ndctriangle.hlsl的纹理布局（t0-t3 - scene纹理）
            code << "TextureCube g_Cubemap : register(t0);\n";
            code << "Texture2D g_Color : register(t1);\n";
            code << "Texture2D g_Normal : register(t2);\n";
            code << "Texture2D g_Orm : register(t3);\n\n";

            // 材质纹理（t10+ - 从GenerateTextureDeclarations生成）
            code << GenerateTextureDeclarations() << "\n\n";

            // 使用相同的采样器
            code << "SamplerState gSamPointWrap : register(s0);\n";
            code << "SamplerState gSamPointClamp : register(s1);\n";
            code << "SamplerState gSamLinearWarp : register(s2);\n";
            code << "SamplerState gSamLinearClamp : register(s3);\n";
            code << "SamplerState gSamAnisotropicWarp : register(s4);\n";
            code << "SamplerState gSamAnisotropicClamp : register(s5);\n\n";
        }
        else if (passIndex == 1) {
            // Pass 1: 延迟光照 - 使用深度重构位置
            code << "cbuffer DefaultVertexCB : register(b0)\n";
            code << "{\n";
            code << "    float4x4 ProjectionMatrix;\n";
            code << "    float4x4 ViewMatrix;\n";
            code << "    float4x4 ModelMatrix;\n";
            code << "    float4x4 IT_ModelMatrix;\n";
            code << "    float3 LightDirection;\n";
            code << "    float _LightPadding;\n";  // 显式padding，强制对齐
            code << "    float3 CameraPositionWS;\n";
            code << "    float _CameraPadding;\n";  // 显式padding，强制对齐
            code << "    float Skylight;\n";  // 场景级Skylight强度
            code << "    float3 _Padding0;\n";  // 保持16字节对齐
            code << "    float4x4 InverseProjectionMatrix;\n";  // 用于深度重构
            code << "    float4x4 InverseViewMatrix;\n";  // 用于深度重构
            code << "    float3 SkylightColor;\n";  // Skylight颜色（UE风格Tint）
            code << "    float _Padding1;\n";  // 保持16字节对齐
            code << "    float4x4 ReservedMemory[1020];\n";  // 恢复原来的大小
            code << "};\n\n";

            // 使用深度纹理代替位置纹理（t0-t5）
            code << "Texture2D BaseColor : register(t0);\n";
            code << "Texture2D Normal : register(t1);\n";
            code << "Texture2D Orm : register(t2);\n";
            code << "Texture2D DepthTexture : register(t3);\n";  // 深度纹理替代position
            code << "TextureCube SkyCube : register(t4);\n";
            code << "Texture2D ShadowMap : register(t5);\n\n";  // LightPass输出的阴影图

            // 使用screen.hlsl的采样器
            code << "SamplerState gSamPointWrap : register(s0);\n";
            code << "SamplerState gSamPointClamp : register(s1);\n";
            code << "SamplerState gSamLinearWarp : register(s2);\n";
            code << "SamplerState gSamLinearClamp : register(s3);\n";
            code << "SamplerState gSamAnisotropicWarp : register(s4);\n";
            code << "SamplerState gSamAnisotropicClamp : register(s5);\n\n";
        }
    }
    else {
        // Forward渲染：注入Scene常量、材质常量和材质纹理
        code << "// Auto-generated Scene Constants\n";
        code << "cbuffer SceneConstants : register(b0) {\n";
        code << "    float4x4 g_matWorld;\n";
        code << "    float4x4 g_matView;\n";
        code << "    float4x4 g_matProj;\n";
        code << "    float4x4 g_matWorldViewProj;\n";
        code << "    float3 g_CameraPos;\n";
        code << "    float g_time;\n";
        code << "    float3 g_LightDir;\n";
        code << "    float g_padding;\n";
        code << "    float3 g_LightColor;\n";
        code << "    float g_LightIntensity;\n";
        code << "};\n\n";

        code << "// Auto-generated Global Resources\n";
        code << "TextureCube g_SkyboxTex : register(t0);\n";
        code << "SamplerState g_sampler : register(s0);\n\n";

        code << GenerateMaterialCB() << "\n\n";
        code << GenerateTextureDeclarations() << "\n\n";
    }

    // 添加原始HLSL代码
    code << m_passes[passIndex].hlslCode;

    return code.str();
}

std::string ShaderParser::GenerateMaterialCB() const {
    if (m_properties.empty()) {
        return "";
    }

    std::ostringstream cb;
    cb << "// Auto-generated Material Constants (Bindless)\n";
    cb << "cbuffer MaterialConstants : register(b1) {\n";

    int offset = 0;

    // 首先添加非纹理参数
    for (const auto& prop : m_properties) {
        // 跳过纹理
        if (prop.type == ShaderParameterType::Texture2D ||
            prop.type == ShaderParameterType::TextureCube) {
            continue;
        }

        // 对齐到16字节边界（简化版，实际需要更复杂的对齐逻辑）
        int size = GetTypeSize(prop.type);
        if (offset % 16 + size > 16) {
            offset = ((offset + 15) / 16) * 16;  // 对齐到下一个16字节边界
        }

        std::string typeStr;
        switch (prop.type) {
            case ShaderParameterType::Float: typeStr = "float"; break;
            case ShaderParameterType::Vector2: typeStr = "float2"; break;
            case ShaderParameterType::Vector3: typeStr = "float3"; break;
            case ShaderParameterType::Vector4: typeStr = "float4"; break;
            case ShaderParameterType::Int: typeStr = "int"; break;
            case ShaderParameterType::Bool: typeStr = "int"; break;
            case ShaderParameterType::Matrix4x4: typeStr = "float4x4"; break;
            default: typeStr = "float4"; break;
        }

        cb << "    " << typeStr << " " << prop.name << ";  // Offset: " << offset << "\n";
        offset += size;
    }

    // 然后添加纹理索引（Bindless）
    // 对齐到4字节边界（uint是4字节）
    if (offset % 4 != 0) {
        offset = ((offset + 3) / 4) * 4;
    }

    for (const auto& prop : m_properties) {
        if (prop.type == ShaderParameterType::Texture2D ||
            prop.type == ShaderParameterType::TextureCube) {
            cb << "    uint " << prop.name << "Index;  // Bindless texture index, Offset: " << offset << "\n";
            offset += 4;  // uint是4字节
        }
    }

    // 填充到256字节
    if (offset > 0 && offset < 256) {
        int padding = 256 - offset;
        int paddingFloat4s = padding / 16;
        if (paddingFloat4s > 0) {
            cb << "    float4 _Padding[" << paddingFloat4s << "];  // Padding to 256 bytes\n";
        }
    }

    cb << "};\n";

    return cb.str();
}

std::string ShaderParser::GenerateTextureDeclarations() const {
    std::ostringstream texDecl;

    // 统计纹理数量
    int textureCount = 0;
    for (const auto& prop : m_properties) {
        if (prop.type == ShaderParameterType::Texture2D ||
            prop.type == ShaderParameterType::TextureCube) {
            textureCount++;
        }
    }

    if (textureCount == 0) {
        return "";
    }

    // Bindless Texture System - 使用无界纹理数组 (SM 5.1+)
    // 注意：SM 5.1中无界数组需要使用 ResourceDescriptorHeap 或指定大小
    // 这里使用一个足够大的固定大小数组来模拟无界数组
    texDecl << "// Bindless Texture System (SM 5.1)" << std::endl;
    texDecl << "// Global texture array - all textures accessed via index" << std::endl;
    texDecl << "Texture2D g_BindlessTextures[1024] : register(t10);" << std::endl;
    texDecl << std::endl;

    // 为每个纹理属性生成索引常量的注释（实际索引在CB中）
    texDecl << "// Texture indices defined in MaterialConstants CB:" << std::endl;
    for (const auto& prop : m_properties) {
        if (prop.type == ShaderParameterType::Texture2D) {
            texDecl << "// - " << prop.name << "Index : uint" << std::endl;
        } else if (prop.type == ShaderParameterType::TextureCube) {
            texDecl << "// - " << prop.name << "Index : uint (TextureCube)" << std::endl;
        }
    }
    texDecl << std::endl;

    // 生成纹理采样辅助宏
    texDecl << "// Texture sampling helper macros" << std::endl;
    texDecl << "#define SAMPLE_TEXTURE(texIndex, sampler, uv) g_BindlessTextures[texIndex].Sample(sampler, uv)" << std::endl;
    texDecl << "#define SAMPLE_TEXTURE_LOD(texIndex, sampler, uv, lod) g_BindlessTextures[texIndex].SampleLevel(sampler, uv, lod)" << std::endl;
    texDecl << std::endl;

    // 为每个纹理生成便捷的采样宏
    for (const auto& prop : m_properties) {
        if (prop.type == ShaderParameterType::Texture2D) {
            texDecl << "#define Sample" << prop.name << "(sampler, uv) SAMPLE_TEXTURE(" << prop.name << "Index, sampler, uv)" << std::endl;
        }
    }

    return texDecl.str();
}

std::vector<ShaderParameter> ShaderParser::GenerateShaderParameters() const {
    std::vector<ShaderParameter> params;
    int offset = 0;
    int textureSlot = 10;  // Bindless纹理从t10开始

    std::cout << "GenerateShaderParameters: m_properties.size() = " << m_properties.size() << std::endl;

    // 首先处理非纹理参数
    for (const auto& prop : m_properties) {
        if (prop.type == ShaderParameterType::Texture2D ||
            prop.type == ShaderParameterType::TextureCube) {
            continue;  // 纹理参数稍后处理
        }

        ShaderParameter param;
        param.name = prop.name;
        param.type = prop.type;
        param.uiWidget = prop.uiWidget;
        param.minValue = prop.minValue;
        param.maxValue = prop.maxValue;

        int size = GetTypeSize(prop.type);
        // 对齐
        if (offset % 16 + size > 16) {
            offset = ((offset + 15) / 16) * 16;
        }
        param.registerSlot = 1;  // b1
        param.byteOffset = offset;
        param.byteSize = size;
        offset += size;

        // 解析默认值
        if (!prop.defaultValue.empty()) {
            std::istringstream iss(prop.defaultValue);
            switch (prop.type) {
                case ShaderParameterType::Float:
                    iss >> param.defaultValue.floatVal;
                    break;
                case ShaderParameterType::Vector4: {
                    char comma;
                    iss >> param.defaultValue.vector4Val.x >> comma
                        >> param.defaultValue.vector4Val.y >> comma
                        >> param.defaultValue.vector4Val.z >> comma
                        >> param.defaultValue.vector4Val.w;
                    break;
                }
                case ShaderParameterType::Vector3: {
                    char comma;
                    iss >> param.defaultValue.vector3Val.x >> comma
                        >> param.defaultValue.vector3Val.y >> comma
                        >> param.defaultValue.vector3Val.z;
                    break;
                }
                case ShaderParameterType::Int:
                    iss >> param.defaultValue.intVal;
                    break;
                case ShaderParameterType::Bool:
                    iss >> param.defaultValue.boolVal;
                    break;
                default:
                    break;
            }
        }

        params.push_back(param);
        std::cout << "GenerateShaderParameters: Added non-texture param '" << param.name << "'" << std::endl;
    }

    // 然后处理纹理参数（Bindless模式：纹理索引存储在CB中）
    // 对齐到4字节边界
    if (offset % 4 != 0) {
        offset = ((offset + 3) / 4) * 4;
    }

    for (const auto& prop : m_properties) {
        if (prop.type != ShaderParameterType::Texture2D &&
            prop.type != ShaderParameterType::TextureCube) {
            continue;
        }

        ShaderParameter param;
        param.name = prop.name;
        param.type = prop.type;
        param.uiWidget = prop.uiWidget.empty() ? "TexturePicker" : prop.uiWidget;
        param.minValue = prop.minValue;
        param.maxValue = prop.maxValue;

        // Bindless模式：纹理索引存储在CB中
        param.registerSlot = textureSlot++;  // 保留用于兼容
        param.byteOffset = offset;  // 纹理索引在CB中的偏移
        param.byteSize = 4;  // uint是4字节
        offset += 4;

        params.push_back(param);
        std::cout << "GenerateShaderParameters: Added TEXTURE param '" << param.name << "' slot=" << param.registerSlot << std::endl;
    }

    std::cout << "GenerateShaderParameters: Total params = " << params.size() << std::endl;
    return params;
}

ShaderParameterType ShaderParser::ParseType(const std::string& typeStr) const {
    if (typeStr == "float") return ShaderParameterType::Float;
    if (typeStr == "float2") return ShaderParameterType::Vector2;
    if (typeStr == "float3") return ShaderParameterType::Vector3;
    if (typeStr == "float4") return ShaderParameterType::Vector4;
    if (typeStr == "int") return ShaderParameterType::Int;
    if (typeStr == "bool") return ShaderParameterType::Bool;
    if (typeStr == "Texture2D") return ShaderParameterType::Texture2D;
    if (typeStr == "TextureCube") return ShaderParameterType::TextureCube;
    if (typeStr == "float4x4") return ShaderParameterType::Matrix4x4;
    return ShaderParameterType::Float;
}

int ShaderParser::GetTypeSize(ShaderParameterType type) const {
    switch (type) {
        case ShaderParameterType::Float: return 4;
        case ShaderParameterType::Vector2: return 8;
        case ShaderParameterType::Vector3: return 12;
        case ShaderParameterType::Vector4: return 16;
        case ShaderParameterType::Int: return 4;
        case ShaderParameterType::Bool: return 4;
        case ShaderParameterType::Matrix4x4: return 64;
        default: return 0;
    }
}

bool ShaderParser::ParseShadingModel(const std::string& content, size_t& pos) {
    // 找到ShadingModel块的开始
    size_t blockStart = content.find('{', pos);
    if (blockStart == std::string::npos) {
        return false;
    }

    size_t blockEnd = FindBlockEnd(content, blockStart);
    if (blockEnd == std::string::npos) {
        return false;
    }

    std::string modelBlock = content.substr(blockStart + 1, blockEnd - blockStart - 1);

    // 解析 shadingmodel=ID
    std::regex idRegex("shadingmodel\\s*=\\s*(\\d+)");
    std::smatch match;
    if (!std::regex_search(modelBlock, match, idRegex)) {
        return false;
    }
    m_shadingModel.shadingModelID = std::stoi(match[1].str());

    // 解析 BRDF=调用
    std::regex brdfCallRegex("BRDF\\s*=\\s*([^;]+);");
    if (!std::regex_search(modelBlock, match, brdfCallRegex)) {
        return false;
    }
    m_shadingModel.brdfCall = match[1].str();

    // 去除空白
    auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \t\n\r"));
        s.erase(s.find_last_not_of(" \t\n\r") + 1);
    };
    trim(m_shadingModel.brdfCall);

    // 解析 BRDF{函数定义}
    size_t brdfBlockPos = modelBlock.find("BRDF");
    brdfBlockPos = modelBlock.find("BRDF", brdfBlockPos + 4); // 跳过第一个BRDF=
    if (brdfBlockPos != std::string::npos) {
        size_t brdfStart = modelBlock.find('{', brdfBlockPos);
        if (brdfStart != std::string::npos) {
            size_t brdfEnd = FindBlockEnd(modelBlock, brdfStart);
            if (brdfEnd != std::string::npos) {
                m_shadingModel.brdfFunctionCode = modelBlock.substr(brdfStart + 1, brdfEnd - brdfStart - 1);
                trim(m_shadingModel.brdfFunctionCode);
            }
        }
    }

    m_hasShadingModel = true;
    std::cout << "Parsed ShadingModel: ID=" << m_shadingModel.shadingModelID << std::endl;
    return true;
}
