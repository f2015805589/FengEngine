// 新增方法 - 添加到Shader.cpp末尾
// 注意：LoadFromShaderFile的实现已经移到Shader.cpp中，这里不再需要

/*
// 旧版本已废弃 - 现在使用Shader.cpp中的完整实现
// 从Unity风格shader文件加载
bool Shader::LoadFromShaderFile(const std::wstring& filePath) {
    ShaderParser parser;

    // 解析shader文件
    if (!parser.ParseShaderFile(filePath)) {
        std::wcout << L"Failed to parse shader file: " << filePath << std::endl;
        return false;
    }

    // 获取shader名称
    m_name = parser.GetShaderName();

    // 获取参数列表
    m_parameters = parser.GenerateShaderParameters();

    // 计算CB大小
    m_constantBufferSize = CalculateConstantBufferSize();

    // 获取第一个Pass的信息
    const auto& passes = parser.GetPasses();
    if (passes.empty()) {
        std::cout << "No passes found in shader" << std::endl;
        return false;
    }

    const auto& firstPass = passes[0];
    m_vsEntryPoint = firstPass.vsEntry;
    m_psEntryPoint = firstPass.psEntry;

    // 生成完整的HLSL代码（包含自动生成的CB和纹理声明）
    m_generatedHLSL = parser.GenerateHLSLCode(0);
    m_useGeneratedHLSL = true;

    return true;
}
*/

// 编译HLSL字符串
bool Shader::CompileHLSLString(const std::string& hlslCode, const std::string& entryPoint,
                               const std::string& target, D3D12_SHADER_BYTECODE* outBytecode) {
    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(
        hlslCode.c_str(),
        hlslCode.size(),
        nullptr,  // source name
        nullptr,  // defines
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint.c_str(),
        target.c_str(),
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cout << "Shader compilation error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            errorBlob->Release();
        }
        return false;
    }

    outBytecode->pShaderBytecode = shaderBlob->GetBufferPointer();
    outBytecode->BytecodeLength = shaderBlob->GetBufferSize();

    // 注意：shaderBlob需要保持，不能立即Release
    // 存储到ComPtr中
    if (target.find("vs") != std::string::npos) {
        m_vsBlob.Attach(shaderBlob);
    } else {
        m_psBlob.Attach(shaderBlob);
    }

    return true;
}
