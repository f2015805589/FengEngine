#pragma once
#include <string>
#include <vector>
#include <map>
#include "ShaderParameter.h"

// Unity风格Shader解析器
class ShaderParser {
public:
    struct PropertyDefinition {
        std::string name;
        ShaderParameterType type;
        std::string defaultValue;
        float minValue = 0.0f;
        float maxValue = 1.0f;
        std::string uiWidget;  // "Slider", "ColorPicker", "TexturePicker"
    };

    struct PassDefinition {
        std::string name;
        std::string vsEntry;  // vertex shader入口点
        std::string psEntry;  // pixel shader入口点
        std::string hlslCode; // HLSL代码（去掉标记后的纯代码）
        std::string renderQueue;  // "Deferred" 或 "Forward"
    };

    struct ShadingModelDefinition {
        int shadingModelID = 0;
        std::string brdfCall;
        std::string brdfFunctionCode;
    };

    ShaderParser();
    ~ShaderParser() = default;

    // 解析Unity风格的shader文件
    bool ParseShaderFile(const std::wstring& filePath);

    // Getters
    const std::string& GetShaderName() const { return m_shaderName; }
    const std::string& GetRenderQueue() const { return m_renderQueue; }
    const std::vector<PropertyDefinition>& GetProperties() const { return m_properties; }
    const std::vector<PassDefinition>& GetPasses() const { return m_passes; }
    bool HasShadingModel() const { return m_hasShadingModel; }
    const ShadingModelDefinition& GetShadingModel() const { return m_shadingModel; }

    // 生成完整的HLSL代码（注入CB和纹理声明）
    std::string GenerateHLSLCode(int passIndex) const;

    // 转换为ShaderParameter列表（兼容现有系统）
    std::vector<ShaderParameter> GenerateShaderParameters() const;

private:
    std::string m_shaderName;
    std::string m_renderQueue;  // Shader级别的RenderQueue
    std::vector<PropertyDefinition> m_properties;
    std::vector<PassDefinition> m_passes;

    bool m_hasShadingModel = false;
    ShadingModelDefinition m_shadingModel;

    // 解析辅助函数
    bool ParseProperties(const std::string& content, size_t& pos);
    bool ParsePass(const std::string& content, size_t& pos);
    bool ParsePropertyLine(const std::string& line, PropertyDefinition& outProp);
    bool ParseShadingModel(const std::string& content, size_t& pos);

    // 查找标记位置
    size_t FindBlockStart(const std::string& content, size_t pos, const std::string& blockName);
    size_t FindBlockEnd(const std::string& content, size_t pos);

    // 生成代码辅助
    std::string GenerateMaterialCB() const;
    std::string GenerateTextureDeclarations() const;

    // 类型解析
    ShaderParameterType ParseType(const std::string& typeStr) const;
    int GetTypeSize(ShaderParameterType type) const;
};
