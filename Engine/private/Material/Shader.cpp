#include "public/Material/Shader.h"
#include "public/Material/ShaderParser.h"
#include "public/Material/MaterialManager.h"
#include "public/BattleFireDirect.h"
#include <d3dx12.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <comdef.h>
#include <msxml6.h>
#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "msxml6.lib")
#pragma comment(lib, "shlwapi.lib")

// 全局ShadingModel收集器
std::map<int, ShaderParser::ShadingModelDefinition> g_shadingModels;
static bool g_screenNeedsRecompile = false;  // 标记Screen.hlsl是否需要重新编译
static bool g_isRecompilingScreen = false;  // 防止递归：标记正在重新编译Screen shader

// 全局函数实现：检查并重新编译Screen shader
bool CheckAndRecompileScreen(ID3D12Device* device, ID3D12RootSignature* rootSig, MaterialManager* matMgr) {
    // 如果正在重新编译Screen shader，跳过（防止递归）
    if (g_isRecompilingScreen) {
        return false;
    }

    std::cout << "[DEBUG] CheckAndRecompileScreen called. g_screenNeedsRecompile = " << g_screenNeedsRecompile << std::endl;
    std::cout << "[DEBUG] g_shadingModels size = " << g_shadingModels.size() << std::endl;

    if (!g_screenNeedsRecompile) {
        std::cout << "[DEBUG] g_screenNeedsRecompile is false, skipping Screen recompile" << std::endl;
        return false;  // 不需要重新编译
    }

    if (!matMgr) {
        std::cout << "ERROR: MaterialManager is NULL, cannot update Screen shader" << std::endl;
        return false;
    }

    std::cout << "========== Recompiling Screen shader with " << g_shadingModels.size() << " ShadingModels... ==========" << std::endl;

    // 设置标志：正在重新编译Screen shader（防止递归）
    g_isRecompilingScreen = true;

    // 从MaterialManager的缓存中移除旧的Screen shader
    std::cout << "[DEBUG] Clearing Screen shader cache..." << std::endl;
    matMgr->ClearShaderCache("Screen");

    // 直接调用LoadShader来重新加载Screen
    // 这会触发编译并自动创建PSO，并存储到MaterialManager的缓存中
    std::cout << "[DEBUG] Reloading Screen shader from file..." << std::endl;
    Shader* newScreenShader = matMgr->LoadShader(L"Engine/Shader/Shadingmodel/Screen.shader");

    // DEBUG: 立即检查Screen shader是否被缓存
    std::ofstream debugLog("Engine/Shader/Shader_Cache/log/cache_debug.txt", std::ios::app);
    if (debugLog.is_open()) {
        debugLog << "[CheckAndRecompileScreen] After LoadShader, checking if Screen is cached..." << std::endl;
        Shader* cachedScreen = matMgr->GetShader("Screen");
        if (cachedScreen) {
            debugLog << "[CheckAndRecompileScreen] SUCCESS: Screen shader IS in cache!" << std::endl;
        } else {
            debugLog << "[CheckAndRecompileScreen] ERROR: Screen shader NOT in cache after LoadShader!" << std::endl;
        }
    }

    // 清除标志：Screen shader重新编译完成
    g_isRecompilingScreen = false;

    if (!newScreenShader) {
        std::cout << "ERROR: Failed to reload Screen shader" << std::endl;
        return false;
    }

    std::cout << "========== Screen shader recompiled successfully with " << g_shadingModels.size() << " ShadingModels ==========" << std::endl;
    g_screenNeedsRecompile = false;
    return true;
}

// 辅助函数：BSTR to std::string
std::string BSTRToString(BSTR bstr) {
    if (!bstr) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string str(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, bstr, -1, &str[0], len, nullptr, nullptr);
    return str;
}

// 辅助函数：BSTR to std::wstring
std::wstring BSTRToWString(BSTR bstr) {
    if (!bstr) return L"";
    return std::wstring(bstr);
}

// 辅助函数：递归创建目录
bool CreateDirectoryRecursive(const std::wstring& path) {
    // 检查目录是否已存在
    DWORD attribs = GetFileAttributesW(path.c_str());
    if (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;  // 目录已存在
    }

    // 查找最后一个路径分隔符
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        // 递归创建父目录
        std::wstring parentPath = path.substr(0, pos);
        if (!CreateDirectoryRecursive(parentPath)) {
            return false;
        }
    }

    // 创建当前目录
    return CreateDirectoryW(path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

Shader::Shader(const std::string& name)
    : m_name(name)
    , m_constantBufferSize(256)  // 默认256字节
    , m_pso(nullptr)
    , m_cullMode(D3D12_CULL_MODE_BACK)
    , m_depthTest(true)
    , m_depthWrite(true)
    , m_vsEntryPoint("MainVS")
    , m_psEntryPoint("MainPS")
{
    m_vsBytecode.pShaderBytecode = nullptr;
    m_vsBytecode.BytecodeLength = 0;
    m_psBytecode.pShaderBytecode = nullptr;
    m_psBytecode.BytecodeLength = 0;
}

Shader::~Shader() {
    // 清理所有Pass的PSO
    for (auto& pass : m_passes) {
        if (pass.pso) {
            pass.pso->Release();
            pass.pso = nullptr;
        }
    }

    // 清理旧的PSO（向后兼容）
    if (m_pso) {
        m_pso->Release();
        m_pso = nullptr;
    }
}

bool Shader::LoadFromXML(const std::wstring& filePath) {
    // 初始化COM
    HRESULT hr = CoInitialize(nullptr);
    bool comInitialized = SUCCEEDED(hr);

    // 创建XML文档对象
    IXMLDOMDocument2* pXMLDom = nullptr;
    hr = CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                          __uuidof(IXMLDOMDocument2), (void**)&pXMLDom);
    if (FAILED(hr)) {
        if (comInitialized) CoUninitialize();
        return false;
    }

    // 加载XML文件
    VARIANT_BOOL loadSuccess = VARIANT_FALSE;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BSTR;
    var.bstrVal = SysAllocString(filePath.c_str());
    hr = pXMLDom->load(var, &loadSuccess);
    VariantClear(&var);

    if (loadSuccess != VARIANT_TRUE) {
        pXMLDom->Release();
        if (comInitialized) CoUninitialize();
        return false;
    }

    // 解析Shader节点
    IXMLDOMElement* pRoot = nullptr;
    hr = pXMLDom->get_documentElement(&pRoot);
    if (FAILED(hr) || !pRoot) {
        pXMLDom->Release();
        if (comInitialized) CoUninitialize();
        return false;
    }

    // 获取shader name属性
    VARIANT varName;
    VariantInit(&varName);
    pRoot->getAttribute(_bstr_t("name"), &varName);
    if (varName.vt == VT_BSTR && varName.bstrVal) {
        m_name = BSTRToString(varName.bstrVal);
    }
    VariantClear(&varName);

    // 解析ShaderFiles
    IXMLDOMNodeList* pShaderFiles = nullptr;
    pRoot->getElementsByTagName(_bstr_t("ShaderFiles"), &pShaderFiles);
    if (pShaderFiles) {
        IXMLDOMNode* pShaderFilesNode = nullptr;
        pShaderFiles->get_item(0, &pShaderFilesNode);
        if (pShaderFilesNode) {
            IXMLDOMNodeList* pChildren = nullptr;
            pShaderFilesNode->get_childNodes(&pChildren);
            if (pChildren) {
                long childCount = 0;
                pChildren->get_length(&childCount);
                for (long i = 0; i < childCount; i++) {
                    IXMLDOMNode* pChild = nullptr;
                    pChildren->get_item(i, &pChild);
                    if (pChild) {
                        BSTR nodeName = nullptr;
                        pChild->get_nodeName(&nodeName);
                        BSTR nodeText = nullptr;
                        pChild->get_text(&nodeText);

                        std::string name = BSTRToString(nodeName);
                        if (name == "VertexShader") {
                            m_vsPath = BSTRToWString(nodeText);
                        } else if (name == "PixelShader") {
                            m_psPath = BSTRToWString(nodeText);
                        } else if (name == "VSEntryPoint") {
                            m_vsEntryPoint = BSTRToString(nodeText);
                        } else if (name == "PSEntryPoint") {
                            m_psEntryPoint = BSTRToString(nodeText);
                        }

                        if (nodeName) SysFreeString(nodeName);
                        if (nodeText) SysFreeString(nodeText);
                        pChild->Release();
                    }
                }
                pChildren->Release();
            }
            pShaderFilesNode->Release();
        }
        pShaderFiles->Release();
    }

    // 解析Parameters
    IXMLDOMNodeList* pParameters = nullptr;
    pRoot->getElementsByTagName(_bstr_t("Parameters"), &pParameters);
    if (pParameters) {
        IXMLDOMNode* pParametersNode = nullptr;
        pParameters->get_item(0, &pParametersNode);
        if (pParametersNode) {
            IXMLDOMNodeList* pParamList = nullptr;
            pParametersNode->get_childNodes(&pParamList);
            if (pParamList) {
                long paramCount = 0;
                pParamList->get_length(&paramCount);
                for (long i = 0; i < paramCount; i++) {
                    IXMLDOMNode* pParam = nullptr;
                    pParamList->get_item(i, &pParam);
                    if (pParam) {
                        BSTR nodeName = nullptr;
                        pParam->get_nodeName(&nodeName);
                        std::string nodeNameStr = BSTRToString(nodeName);

                        if (nodeNameStr == "Parameter") {
                            ShaderParameter param;

                            // 获取属性
                            IXMLDOMNamedNodeMap* pAttrs = nullptr;
                            pParam->get_attributes(&pAttrs);
                            if (pAttrs) {
                                // name
                                IXMLDOMNode* pNameAttr = nullptr;
                                pAttrs->getNamedItem(_bstr_t("name"), &pNameAttr);
                                if (pNameAttr) {
                                    BSTR val = nullptr;
                                    pNameAttr->get_text(&val);
                                    param.name = BSTRToString(val);
                                    SysFreeString(val);
                                    pNameAttr->Release();
                                }

                                // type
                                IXMLDOMNode* pTypeAttr = nullptr;
                                pAttrs->getNamedItem(_bstr_t("type"), &pTypeAttr);
                                if (pTypeAttr) {
                                    BSTR val = nullptr;
                                    pTypeAttr->get_text(&val);
                                    param.type = ShaderParameter::StringToType(BSTRToString(val));
                                    param.byteSize = ShaderParameter::GetTypeSizeInBytes(param.type);
                                    SysFreeString(val);
                                    pTypeAttr->Release();
                                }

                                // register
                                IXMLDOMNode* pRegAttr = nullptr;
                                pAttrs->getNamedItem(_bstr_t("register"), &pRegAttr);
                                if (pRegAttr) {
                                    BSTR val = nullptr;
                                    pRegAttr->get_text(&val);
                                    std::string regStr = BSTRToString(val);
                                    // 解析 "b1" 或 "t10"
                                    if (!regStr.empty() && (regStr[0] == 't' || regStr[0] == 'b')) {
                                        param.registerSlot = std::stoi(regStr.substr(1));
                                    }
                                    SysFreeString(val);
                                    pRegAttr->Release();
                                }

                                // offset
                                IXMLDOMNode* pOffsetAttr = nullptr;
                                pAttrs->getNamedItem(_bstr_t("offset"), &pOffsetAttr);
                                if (pOffsetAttr) {
                                    BSTR val = nullptr;
                                    pOffsetAttr->get_text(&val);
                                    param.byteOffset = std::stoi(BSTRToString(val));
                                    SysFreeString(val);
                                    pOffsetAttr->Release();
                                }

                                pAttrs->Release();
                            }

                            // 解析子节点（Default, Range, UIWidget等）
                            IXMLDOMNodeList* pChildNodes = nullptr;
                            pParam->get_childNodes(&pChildNodes);
                            if (pChildNodes) {
                                long childCount = 0;
                                pChildNodes->get_length(&childCount);
                                for (long j = 0; j < childCount; j++) {
                                    IXMLDOMNode* pChild = nullptr;
                                    pChildNodes->get_item(j, &pChild);
                                    if (pChild) {
                                        BSTR childNodeName = nullptr;
                                        pChild->get_nodeName(&childNodeName);
                                        BSTR childText = nullptr;
                                        pChild->get_text(&childText);

                                        std::string childName = BSTRToString(childNodeName);
                                        std::string childValue = BSTRToString(childText);

                                        if (childName == "Default") {
                                            // 解析默认值
                                            if (param.type == ShaderParameterType::Float) {
                                                param.defaultValue.floatVal = std::stof(childValue);
                                            } else if (param.type == ShaderParameterType::Int) {
                                                param.defaultValue.intVal = std::stoi(childValue);
                                            } else if (param.type == ShaderParameterType::Bool) {
                                                param.defaultValue.boolVal = (std::stoi(childValue) != 0);
                                            } else if (param.type == ShaderParameterType::Vector4) {
                                                std::istringstream iss(childValue);
                                                iss >> param.defaultValue.vector4Val.x
                                                    >> param.defaultValue.vector4Val.y
                                                    >> param.defaultValue.vector4Val.z
                                                    >> param.defaultValue.vector4Val.w;
                                            } else if (param.type == ShaderParameterType::Vector3) {
                                                std::istringstream iss(childValue);
                                                iss >> param.defaultValue.vector3Val.x
                                                    >> param.defaultValue.vector3Val.y
                                                    >> param.defaultValue.vector3Val.z;
                                            } else if (param.type == ShaderParameterType::Texture2D ||
                                                     param.type == ShaderParameterType::TextureCube) {
                                                // 将string转换为wstring
                                                int len = MultiByteToWideChar(CP_UTF8, 0, childValue.c_str(), -1, nullptr, 0);
                                                if (len > 0) {
                                                    param.defaultTexturePath.resize(len - 1);
                                                    MultiByteToWideChar(CP_UTF8, 0, childValue.c_str(), -1,
                                                                       &param.defaultTexturePath[0], len);
                                                }
                                            }
                                        } else if (childName == "UIWidget") {
                                            param.uiWidget = childValue;
                                        } else if (childName == "Range") {
                                            // 解析Range的min和max属性
                                            IXMLDOMNamedNodeMap* pRangeAttrs = nullptr;
                                            pChild->get_attributes(&pRangeAttrs);
                                            if (pRangeAttrs) {
                                                IXMLDOMNode* pMinAttr = nullptr;
                                                pRangeAttrs->getNamedItem(_bstr_t("min"), &pMinAttr);
                                                if (pMinAttr) {
                                                    BSTR minVal = nullptr;
                                                    pMinAttr->get_text(&minVal);
                                                    param.minValue = std::stof(BSTRToString(minVal));
                                                    SysFreeString(minVal);
                                                    pMinAttr->Release();
                                                }

                                                IXMLDOMNode* pMaxAttr = nullptr;
                                                pRangeAttrs->getNamedItem(_bstr_t("max"), &pMaxAttr);
                                                if (pMaxAttr) {
                                                    BSTR maxVal = nullptr;
                                                    pMaxAttr->get_text(&maxVal);
                                                    param.maxValue = std::stof(BSTRToString(maxVal));
                                                    SysFreeString(maxVal);
                                                    pMaxAttr->Release();
                                                }

                                                pRangeAttrs->Release();
                                            }
                                        }

                                        if (childNodeName) SysFreeString(childNodeName);
                                        if (childText) SysFreeString(childText);
                                        pChild->Release();
                                    }
                                }
                                pChildNodes->Release();
                            }

                            m_parameters.push_back(param);
                        }

                        if (nodeName) SysFreeString(nodeName);
                        pParam->Release();
                    }
                }
                pParamList->Release();
            }
            pParametersNode->Release();
        }
        pParameters->Release();
    }

    // 解析ConstantBufferLayout
    IXMLDOMNodeList* pCBLayout = nullptr;
    pRoot->getElementsByTagName(_bstr_t("ConstantBufferLayout"), &pCBLayout);
    if (pCBLayout) {
        IXMLDOMNode* pCBNode = nullptr;
        pCBLayout->get_item(0, &pCBNode);
        if (pCBNode) {
            IXMLDOMNodeList* pCBChildren = nullptr;
            pCBNode->get_childNodes(&pCBChildren);
            if (pCBChildren) {
                long childCount = 0;
                pCBChildren->get_length(&childCount);
                for (long i = 0; i < childCount; i++) {
                    IXMLDOMNode* pChild = nullptr;
                    pCBChildren->get_item(i, &pChild);
                    if (pChild) {
                        BSTR nodeName = nullptr;
                        pChild->get_nodeName(&nodeName);
                        std::string name = BSTRToString(nodeName);
                        if (name == "Size") {
                            BSTR nodeText = nullptr;
                            pChild->get_text(&nodeText);
                            m_constantBufferSize = std::stoi(BSTRToString(nodeText));
                            SysFreeString(nodeText);
                        }
                        SysFreeString(nodeName);
                        pChild->Release();
                    }
                }
                pCBChildren->Release();
            }
            pCBNode->Release();
        }
        pCBLayout->Release();
    }

    pRoot->Release();
    pXMLDom->Release();
    if (comInitialized) CoUninitialize();

    return true;
}

bool Shader::CompileShaders(ID3D12Device* device) {
    if (m_useGeneratedHLSL) {
        // 编译所有Pass
        for (size_t i = 0; i < m_passes.size(); ++i) {
            auto& pass = m_passes[i];

            std::cout << "Compiling Pass " << i << " (" << pass.name << ")..." << std::endl;

            // 编译VS
            ID3DBlob* vsBlob = nullptr;
            ID3DBlob* errorBlob = nullptr;

            HRESULT hr = D3DCompile(
                pass.generatedHLSL.c_str(),
                pass.generatedHLSL.size(),
                nullptr,
                nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE,
                pass.vsEntry.c_str(),
                "vs_5_1",  // 升级到5.1以支持Bindless纹理(space语法)
                D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
                0,
                &vsBlob,
                &errorBlob
            );

            if (FAILED(hr)) {
                if (errorBlob) {
                    std::string errorMsg = (char*)errorBlob->GetBufferPointer();
                    std::cout << "======================================" << std::endl;
                    std::cout << "Vertex shader compilation FAILED (Pass " << pass.name << "): " << std::endl;
                    std::cout << errorMsg << std::endl;
                    std::cout << "======================================" << std::endl;

                    // 输出错误到文件（使用Pass名称）
                    std::ofstream errFile("Engine/Shader/" + pass.name + "_VS_Error.txt");
                    if (errFile.is_open()) {
                        errFile << errorMsg;
                        errFile.close();
                    }

                    // 弹出MessageBox显示详细错误
                    std::string title = "VS Compilation Error - " + pass.name;
                    MessageBoxA(NULL, errorMsg.c_str(), title.c_str(), MB_OK | MB_ICONERROR);

                    errorBlob->Release();
                }
                return false;
            }

            pass.vsBlob.Attach(vsBlob);
            pass.vsBytecode.pShaderBytecode = vsBlob->GetBufferPointer();
            pass.vsBytecode.BytecodeLength = vsBlob->GetBufferSize();

            // 编译PS
            ID3DBlob* psBlob = nullptr;
            errorBlob = nullptr;

            hr = D3DCompile(
                pass.generatedHLSL.c_str(),
                pass.generatedHLSL.size(),
                nullptr,
                nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE,
                pass.psEntry.c_str(),
                "ps_5_1",  // 升级到5.1以支持Bindless纹理(space语法)
                D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
                0,
                &psBlob,
                &errorBlob
            );

            if (FAILED(hr)) {
                if (errorBlob) {
                    std::string errorMsg = (char*)errorBlob->GetBufferPointer();
                    std::cout << "======================================" << std::endl;
                    std::cout << "Pixel shader compilation FAILED (Pass " << pass.name << "): " << std::endl;
                    std::cout << errorMsg << std::endl;
                    std::cout << "======================================" << std::endl;

                    // 输出错误到文件（使用Pass名称）
                    std::ofstream errFile("Engine/Shader/" + pass.name + "_PS_Error.txt");
                    if (errFile.is_open()) {
                        errFile << errorMsg;
                        errFile.close();
                    }

                    // 弹出MessageBox显示详细错误
                    std::string title = "PS Compilation Error - " + pass.name;
                    MessageBoxA(NULL, errorMsg.c_str(), title.c_str(), MB_OK | MB_ICONERROR);

                    errorBlob->Release();
                }
                return false;
            }

            pass.psBlob.Attach(psBlob);
            pass.psBytecode.pShaderBytecode = psBlob->GetBufferPointer();
            pass.psBytecode.BytecodeLength = psBlob->GetBufferSize();

            std::cout << "Pass " << pass.name << " compiled successfully." << std::endl;
        }

        return true;
    } else {
        // 原有的文件编译方式（已废弃，保留向后兼容）
        // 编译VS
        D3D12_SHADER_BYTECODE vsBytecode;
        CreateShaderFromFile(m_vsPath.c_str(), m_vsEntryPoint.c_str(), "vs_5_0", &vsBytecode);

        // 检查VS编译是否成功
        if (vsBytecode.pShaderBytecode == nullptr || vsBytecode.BytecodeLength == 0) {
            return false;
        }

        // 编译PS
        D3D12_SHADER_BYTECODE psBytecode;
        CreateShaderFromFile(m_psPath.c_str(), m_psEntryPoint.c_str(), "ps_5_0", &psBytecode);

        // 检查PS编译是否成功
        if (psBytecode.pShaderBytecode == nullptr || psBytecode.BytecodeLength == 0) {
            return false;
        }

        // 保存bytecode（注意：这些bytecode由D3DCompileFromFile分配，需要保持）
        m_vsBytecode = vsBytecode;
        m_psBytecode = psBytecode;

        return true;
    }
}

ID3D12PipelineState* Shader::CreatePSO(ID3D12Device* device, ID3D12RootSignature* rootSig, int passIndex) {
    if (passIndex < 0 || passIndex >= m_passes.size()) {
        std::cout << "Invalid pass index: " << passIndex << std::endl;
        return nullptr;
    }

    auto& pass = m_passes[passIndex];

    // 根据RenderQueue和Pass索引决定使用哪种PSO创建方式
    if (m_renderQueue == "Deferred") {
        if (passIndex == 0) {
            // Pass 0: GBuffer填充 - 使用scene mesh的input layout
            pass.pso = CreateScenePSO(rootSig, pass.vsBytecode, pass.psBytecode);
        }
        else if (passIndex == 1) {
            // Pass 1: 延迟光照（ScreenPass）- 使用screen quad的input layout
            D3D12_INPUT_ELEMENT_DESC screenInputLayout[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
            psoDesc.InputLayout = { screenInputLayout, _countof(screenInputLayout) };
            psoDesc.pRootSignature = rootSig;
            psoDesc.VS = pass.vsBytecode;
            psoDesc.PS = pass.psBytecode;
            psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // 全屏quad不需要背面剔除

            // 启用 Alpha 混合以支持天空盒透明
            CD3DX12_BLEND_DESC blendDesc(D3D12_DEFAULT);
            blendDesc.RenderTarget[0].BlendEnable = TRUE;
            blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
            blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            psoDesc.BlendState = blendDesc;

            psoDesc.DepthStencilState.DepthEnable = FALSE;  // ScreenPass不需要深度测试
            psoDesc.DepthStencilState.StencilEnable = FALSE;
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;  // 交换链格式
            psoDesc.SampleDesc.Count = 1;

            device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pass.pso));
        }
    }
    else {
        // Forward渲染：使用scene mesh的input layout
        pass.pso = CreateScenePSO(rootSig, pass.vsBytecode, pass.psBytecode);
    }

    std::cout << "Created PSO for Pass " << passIndex << " (" << pass.name << ")" << std::endl;

    return pass.pso;
}

const ShaderParameter* Shader::GetParameter(const std::string& name) const {
    for (const auto& param : m_parameters) {
        if (param.name == name) {
            return &param;
        }
    }
    return nullptr;
}

int Shader::CalculateConstantBufferSize() {
    int maxOffset = 0;
    int maxSize = 0;
    for (const auto& param : m_parameters) {
        if (param.type != ShaderParameterType::Texture2D &&
            param.type != ShaderParameterType::TextureCube) {
            int endOffset = param.byteOffset + param.byteSize;
            if (endOffset > maxOffset + maxSize) {
                maxOffset = param.byteOffset;
                maxSize = param.byteSize;
            }
        }
    }

    int totalSize = maxOffset + maxSize;
    // 256字节对齐
    return ((totalSize + 255) / 256) * 256;
}

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

    // 获取渲染队列类型（Shader级别）
    m_renderQueue = parser.GetRenderQueue();

    // 如果定义了ShadingModel，注册到全局表
    if (parser.HasShadingModel()) {
        const auto& sm = parser.GetShadingModel();
        std::cout << "[DEBUG] Shader '" << m_name << "' has ShadingModel with ID " << sm.shadingModelID << std::endl;

        if (g_shadingModels.find(sm.shadingModelID) == g_shadingModels.end()) {
            g_shadingModels[sm.shadingModelID] = sm;
            g_screenNeedsRecompile = true;  // 标记Screen需要重新编译

            std::cout << "[DEBUG] NEW ShadingModel registered! Setting g_screenNeedsRecompile = true" << std::endl;
            std::cout << "[DEBUG] Total ShadingModels now: " << g_shadingModels.size() << std::endl;

            // 删除旧的Screen.hlsl缓存，强制重新生成
            DeleteFileW(L"Engine/Shader/Shader_Cache/Screen.hlsl");

            std::cout << "Registered ShadingModel ID " << sm.shadingModelID << " from " << m_name << std::endl;
            std::cout << "Screen.hlsl will be regenerated with new ShadingModel" << std::endl;
        } else {
            std::cout << "[DEBUG] ShadingModel ID " << sm.shadingModelID << " already registered, skipping" << std::endl;
        }
    } else {
        std::cout << "[DEBUG] Shader '" << m_name << "' has NO ShadingModel" << std::endl;
    }

    // 获取所有Pass信息并保存到m_passes
    const auto& parserPasses = parser.GetPasses();
    if (parserPasses.empty()) {
        std::cout << "No passes found in shader" << std::endl;
        return false;
    }

    // 清空旧的passes
    m_passes.clear();

    // 为每个Pass创建PassInfo
    for (size_t i = 0; i < parserPasses.size(); ++i) {
        const auto& parserPass = parserPasses[i];
        PassInfo passInfo;

        passInfo.name = parserPass.name;
        passInfo.vsEntry = parserPass.vsEntry;
        passInfo.psEntry = parserPass.psEntry;

        // 生成完整的HLSL代码（包含自动生成的CB和纹理声明）
        passInfo.generatedHLSL = parser.GenerateHLSLCode(static_cast<int>(i));

        // DEBUG: 输出生成的HLSL到文件以便检查
        std::wstring shaderNameW(m_name.begin(), m_name.end());
        std::wstring passNameW(passInfo.name.begin(), passInfo.name.end());

        std::wstring debugPath;
        bool shouldWriteFile = true;

        // 特殊处理：Sky shader 自己的 Pass 应该输出为 Sky.hlsl
        if (m_name == "Sky" && i == 0) {
            // Sky shader 的第一个Pass（也是唯一的Pass）：输出为统一的 Sky.hlsl
            debugPath = L"Engine/Shader/Shader_Cache/Sky.hlsl";
            std::cout << "[DEBUG] Processing Sky shader, generating Sky.hlsl" << std::endl;
        }
        // 特殊处理：Screen shader 自己的 Pass 应该输出为 Screen.hlsl
        else if (m_name == "Screen" && i == 0) {
            // Screen shader 自己的第一个Pass（也是唯一的Pass）：输出为统一的 Screen.hlsl
            debugPath = L"Engine/Shader/Shader_Cache/Screen.hlsl";

            std::cout << "[DEBUG] Processing Screen shader, g_shadingModels.size() = " << g_shadingModels.size() << std::endl;

            // 总是注入自定义ShadingModel到Screen.hlsl（移除 screenPassGenerated 检查）
            if (!g_shadingModels.empty()) {
                std::string& hlsl = passInfo.generatedHLSL;

                std::cout << "[DEBUG] Injecting " << g_shadingModels.size() << " custom ShadingModels into Screen shader..." << std::endl;

                // 1. 在 BRDF switch 函数前注入自定义BRDF函数
                std::string brdfFunctions;
                for (const auto& pair : g_shadingModels) {
                    const auto& sm = pair.second;
                    if (!sm.brdfFunctionCode.empty()) {
                        brdfFunctions += "\n        // ShadingModel " + std::to_string(sm.shadingModelID) + "\n";
                        brdfFunctions += "        " + sm.brdfFunctionCode + "\n";
                    }
                }

                size_t brdfPos = hlsl.find("float3 BRDF(int shadingModelID");
                if (brdfPos != std::string::npos) {
                    hlsl.insert(brdfPos, brdfFunctions);
                    std::cout << "[DEBUG] Injected BRDF functions at position " << brdfPos << std::endl;
                } else {
                    std::cout << "[ERROR] Could not find BRDF function insertion point!" << std::endl;
                }

                // 2. 在 switch 的 default 前添加 case
                std::string cases;
                for (const auto& pair : g_shadingModels) {
                    const auto& sm = pair.second;
                    cases += "case " + std::to_string(sm.shadingModelID) + ": return " + sm.brdfCall + ";\n                ";
                }

                size_t defaultPos = hlsl.find("default: return float3(0, 0, 0);");
                if (defaultPos != std::string::npos) {
                    hlsl.insert(defaultPos, cases);
                    std::cout << "[DEBUG] Injected case statements at position " << defaultPos << std::endl;
                } else {
                    std::cout << "[ERROR] Could not find default case insertion point!" << std::endl;
                }

                std::cout << "========== Injected " << g_shadingModels.size() << " custom ShadingModels into Screen.hlsl ==========" << std::endl;
            } else {
                std::cout << "[WARNING] g_shadingModels is empty, no custom ShadingModels to inject!" << std::endl;
            }
        } else if (i == 0) {
            // 第一个Pass（GBuffer填充）：每个材质一个文件，使用实际的Pass名称
            debugPath = L"Engine/Shader/Shader_Cache/" + shaderNameW + L"_" + passNameW + L".hlsl";
        } else if (i == 1 && m_renderQueue == "Deferred") {
            // 第二个Pass（延迟光照）：全局统一的Screen.hlsl
            debugPath = L"Engine/Shader/Shader_Cache/Screen.hlsl";

            // 只写一次，除非有新的ShadingModel注册
            static bool screenPassGenerated = false;
            std::cout << "[DEBUG] Screen Pass generation check: screenPassGenerated=" << screenPassGenerated
                      << ", g_screenNeedsRecompile=" << g_screenNeedsRecompile << std::endl;

            if (screenPassGenerated && !g_screenNeedsRecompile) {
                std::cout << "[DEBUG] Skipping Screen.hlsl regeneration (already generated and no new ShadingModels)" << std::endl;
                shouldWriteFile = false;
            } else {
                std::cout << "[DEBUG] REGENERATING Screen.hlsl!" << std::endl;
                screenPassGenerated = true;
                // 注意：不要在这里重置 g_screenNeedsRecompile！
                // 因为这里可能是在ToonPBR等shader首次加载时，Screen.hlsl被写入文件
                // 但真正的Screen shader还没有被重新加载和编译
                // 让 CheckAndRecompileScreen 在重新加载Screen shader后再重置flag

                std::cout << "========== Regenerating Screen.hlsl with " << g_shadingModels.size() << " ShadingModels... ==========" << std::endl;

                // 注入自定义ShadingModel到Screen.hlsl
                if (!g_shadingModels.empty()) {
                    std::string& hlsl = passInfo.generatedHLSL;

                    // 1. 在 BRDF switch 函数前注入自定义BRDF函数
                    std::string brdfFunctions;
                    for (const auto& pair : g_shadingModels) {
                        const auto& sm = pair.second;
                        if (!sm.brdfFunctionCode.empty()) {
                            brdfFunctions += "\n        // ShadingModel " + std::to_string(sm.shadingModelID) + "\n";
                            brdfFunctions += "        " + sm.brdfFunctionCode + "\n";
                        }
                    }

                    size_t brdfPos = hlsl.find("float3 BRDF(int shadingModelID");
                    if (brdfPos != std::string::npos) {
                        hlsl.insert(brdfPos, brdfFunctions);
                        std::cout << "[DEBUG] Injected BRDF functions at position " << brdfPos << std::endl;
                    } else {
                        std::cout << "[ERROR] Could not find BRDF function insertion point!" << std::endl;
                    }

                    // 2. 在 switch 的 default 前添加 case
                    std::string cases;
                    for (const auto& pair : g_shadingModels) {
                        const auto& sm = pair.second;
                        cases += "case " + std::to_string(sm.shadingModelID) + ": return " + sm.brdfCall + ";\n                ";
                    }

                    size_t defaultPos = hlsl.find("default: return float3(0, 0, 0);");
                    if (defaultPos != std::string::npos) {
                        hlsl.insert(defaultPos, cases);
                        std::cout << "[DEBUG] Injected case statements at position " << defaultPos << std::endl;
                    } else {
                        std::cout << "[ERROR] Could not find default case insertion point!" << std::endl;
                    }

                    std::cout << "========== Injected " << g_shadingModels.size() << " custom ShadingModels into Screen.hlsl ==========" << std::endl;
                } else {
                    std::cout << "[WARNING] g_shadingModels is empty, no custom ShadingModels to inject!" << std::endl;
                }
            }
        } else {
            // 其他Pass
            debugPath = L"Engine/Shader/Shader_Cache/" + shaderNameW + L"_" + passNameW + L".hlsl";
        }

        if (shouldWriteFile) {
            // 确保目录存在 - 提取目录路径
            size_t lastSlash = debugPath.find_last_of(L"\\/");
            if (lastSlash != std::wstring::npos) {
                std::wstring dirPath = debugPath.substr(0, lastSlash);
                if (!CreateDirectoryRecursive(dirPath)) {
                    std::wcout << L"WARNING: Failed to create directory: " << dirPath << std::endl;
                }
            }

            std::ofstream debugFile(debugPath);
            if (debugFile.is_open()) {
                debugFile << passInfo.generatedHLSL;
                debugFile.close();
                std::wcout << L"DEBUG: Generated HLSL written to " << debugPath << std::endl;
            } else {
                std::wcout << L"ERROR: Failed to create file: " << debugPath << std::endl;
            }
        }

        m_passes.push_back(passInfo);
    }

    m_useGeneratedHLSL = true;

    return true;
}

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

// 获取指定Pass的PSO
ID3D12PipelineState* Shader::GetPSO(int passIndex) const {
    if (passIndex < 0 || passIndex >= m_passes.size()) {
        return nullptr;
    }
    return m_passes[passIndex].pso;
}

// 获取指定Pass的VS字节码
const D3D12_SHADER_BYTECODE& Shader::GetVertexShaderBytecode(int passIndex) const {
    static D3D12_SHADER_BYTECODE empty = { nullptr, 0 };
    if (passIndex < 0 || passIndex >= m_passes.size()) {
        return empty;
    }
    return m_passes[passIndex].vsBytecode;
}

// 获取指定Pass的PS字节码
const D3D12_SHADER_BYTECODE& Shader::GetPixelShaderBytecode(int passIndex) const {
    static D3D12_SHADER_BYTECODE empty = { nullptr, 0 };
    if (passIndex < 0 || passIndex >= m_passes.size()) {
        return empty;
    }
    return m_passes[passIndex].psBytecode;
}
