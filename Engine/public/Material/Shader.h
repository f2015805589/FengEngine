#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <string>
#include <vector>
#include <map>
#include "ShaderParameter.h"
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Forward declaration
class MaterialManager;

// 全局函数：检查是否需要重新编译Screen shader
bool CheckAndRecompileScreen(ID3D12Device* device, ID3D12RootSignature* rootSig, MaterialManager* matMgr);

class Shader {
public:
    Shader(const std::string& name);
    ~Shader();

    // 从XML文件加载shader定义（旧方式，保留兼容）
    bool LoadFromXML(const std::wstring& filePath);

    // 从Unity风格shader文件加载（新方式）
    bool LoadFromShaderFile(const std::wstring& filePath);

    // 编译shader（VS和PS）- 编译所有Pass
    bool CompileShaders(ID3D12Device* device);

    // 创建PSO（为指定Pass创建PSO）
    ID3D12PipelineState* CreatePSO(ID3D12Device* device, ID3D12RootSignature* rootSig, int passIndex = 0);

    // Getter方法
    const std::string& GetName() const { return m_name; }
    const std::vector<ShaderParameter>& GetParameters() const { return m_parameters; }
    const ShaderParameter* GetParameter(const std::string& name) const;
    int GetConstantBufferSize() const { return m_constantBufferSize; }
    ID3D12PipelineState* GetPSO(int passIndex = 0) const;  // 获取指定Pass的PSO
    int GetPassCount() const { return static_cast<int>(m_passes.size()); }  // 获取Pass数量
    std::string GetPassName(int passIndex) const {
        if (passIndex >= 0 && passIndex < static_cast<int>(m_passes.size())) {
            return m_passes[passIndex].name;
        }
        return "";
    }

    // 获取渲染队列类型
    const std::string& GetRenderQueue() const { return m_renderQueue; }
    bool IsDeferredShader() const { return m_renderQueue == "Deferred"; }

    // 为了向后兼容，这些方法返回Pass 0的字节码
    const D3D12_SHADER_BYTECODE& GetVertexShaderBytecode(int passIndex = 0) const;
    const D3D12_SHADER_BYTECODE& GetPixelShaderBytecode(int passIndex = 0) const;

private:
    // Pass信息结构
    struct PassInfo {
        std::string name;
        std::string vsEntry;
        std::string psEntry;
        std::string generatedHLSL;
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        D3D12_SHADER_BYTECODE vsBytecode;
        D3D12_SHADER_BYTECODE psBytecode;
        ID3D12PipelineState* pso;  // PSO（由外部管理生命周期）

        PassInfo() : pso(nullptr) {
            vsBytecode = { nullptr, 0 };
            psBytecode = { nullptr, 0 };
        }
    };

    std::string m_name;
    std::wstring m_vsPath;           // VS shader路径（旧方式）
    std::wstring m_psPath;           // PS shader路径（旧方式）
    std::string m_vsEntryPoint;      // VS入口点（旧方式）
    std::string m_psEntryPoint;      // PS入口点（旧方式）

    std::vector<ShaderParameter> m_parameters;  // 所有参数列表
    int m_constantBufferSize;                   // 材质常量缓冲区大小（字节）

    // 新增：多Pass支持
    std::vector<PassInfo> m_passes;  // 所有Pass信息
    bool m_useGeneratedHLSL = false;  // 标记是否使用生成的HLSL

    // 渲染队列类型
    std::string m_renderQueue = "Forward";  // "Deferred" 或 "Forward"

    // 渲染状态
    D3D12_CULL_MODE m_cullMode;
    bool m_depthTest;
    bool m_depthWrite;

    // 旧方式的shader资源（向后兼容）
    ComPtr<ID3DBlob> m_vsBlob;
    ComPtr<ID3DBlob> m_psBlob;
    D3D12_SHADER_BYTECODE m_vsBytecode;
    D3D12_SHADER_BYTECODE m_psBytecode;
    ID3D12PipelineState* m_pso;

    // XML解析辅助函数（旧方式）
    bool ParseXMLFile(const std::wstring& filePath);

    // 计算常量缓冲区总大小（256字节对齐）
    int CalculateConstantBufferSize();

    // 编译HLSL字符串（新增）
    bool CompileHLSLString(const std::string& hlslCode, const std::string& entryPoint,
                          const std::string& target, D3D12_SHADER_BYTECODE* outBytecode);
};
