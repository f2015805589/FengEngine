#pragma once
#include <string>
#include <DirectXMath.h>
#include <vector>

enum class ShaderParameterType {
    Float,
    Vector2,
    Vector3,
    Vector4,
    Int,
    Bool,
    Texture2D,
    TextureCube,
    Matrix4x4
};

struct ShaderParameter {
    std::string name;              // 参数名称
    ShaderParameterType type;      // 参数类型
    int registerSlot;              // 寄存器槽位（纹理用t#，CB用offset）
    int byteOffset;                // 在常量缓冲区中的偏移（字节）
    int byteSize;                  // 字节大小

    // UI提示信息
    std::string uiWidget;          // "Slider", "ColorPicker", "Checkbox", "TexturePicker", "Dropdown"
    float minValue;                // 范围最小值（用于slider）
    float maxValue;                // 范围最大值（用于slider）
    std::vector<std::string> options;  // 下拉选项（用于dropdown）

    // 默认值（union可以存储不同类型）
    union DefaultValue {
        float floatVal;
        int intVal;
        bool boolVal;
        DirectX::XMFLOAT2 vector2Val;
        DirectX::XMFLOAT3 vector3Val;
        DirectX::XMFLOAT4 vector4Val;
        DirectX::XMFLOAT4X4 matrixVal;

        // 构造函数（初始化为0）
        DefaultValue() { memset(this, 0, sizeof(DefaultValue)); }
    } defaultValue;

    std::wstring defaultTexturePath;  // 纹理的默认路径

    // 构造函数初始化
    ShaderParameter()
        : registerSlot(0)
        , byteOffset(0)
        , byteSize(0)
        , minValue(0.0f)
        , maxValue(1.0f)
    {
    }

    // 辅助函数：获取参数类型的字节大小
    static int GetTypeSizeInBytes(ShaderParameterType type) {
        switch (type) {
            case ShaderParameterType::Float: return 4;
            case ShaderParameterType::Vector2: return 8;
            case ShaderParameterType::Vector3: return 12;
            case ShaderParameterType::Vector4: return 16;
            case ShaderParameterType::Int: return 4;
            case ShaderParameterType::Bool: return 4;
            case ShaderParameterType::Texture2D: return 0;  // 纹理不占用CB空间
            case ShaderParameterType::TextureCube: return 0;
            case ShaderParameterType::Matrix4x4: return 64;
            default: return 0;
        }
    }

    // 辅助函数：将字符串转换为参数类型
    static ShaderParameterType StringToType(const std::string& typeStr) {
        if (typeStr == "Float") return ShaderParameterType::Float;
        if (typeStr == "Vector2") return ShaderParameterType::Vector2;
        if (typeStr == "Vector3") return ShaderParameterType::Vector3;
        if (typeStr == "Vector4") return ShaderParameterType::Vector4;
        if (typeStr == "Int") return ShaderParameterType::Int;
        if (typeStr == "Bool") return ShaderParameterType::Bool;
        if (typeStr == "Texture2D") return ShaderParameterType::Texture2D;
        if (typeStr == "TextureCube") return ShaderParameterType::TextureCube;
        if (typeStr == "Matrix4x4") return ShaderParameterType::Matrix4x4;
        return ShaderParameterType::Float;  // 默认
    }

    // 辅助函数：将参数类型转换为字符串
    static std::string TypeToString(ShaderParameterType type) {
        switch (type) {
            case ShaderParameterType::Float: return "Float";
            case ShaderParameterType::Vector2: return "Vector2";
            case ShaderParameterType::Vector3: return "Vector3";
            case ShaderParameterType::Vector4: return "Vector4";
            case ShaderParameterType::Int: return "Int";
            case ShaderParameterType::Bool: return "Bool";
            case ShaderParameterType::Texture2D: return "Texture2D";
            case ShaderParameterType::TextureCube: return "TextureCube";
            case ShaderParameterType::Matrix4x4: return "Matrix4x4";
            default: return "Unknown";
        }
    }
};
