#include "public/Material/MaterialInstance.h"
#include "public/BattleFireDirect.h"
#include "public/Scene.h"
#include "public/Texture/TextureManager.h"
#include "public/Texture/TextureAsset.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <comdef.h>
#include <msxml6.h>

#pragma comment(lib, "msxml6.lib")

// 辅助函数声明（与Shader.cpp中相同）
extern std::string BSTRToString(BSTR bstr);
extern std::wstring BSTRToWString(BSTR bstr);

MaterialInstance::MaterialInstance(const std::string& name, Shader* shader)
    : m_name(name)
    , m_shader(shader)
    , m_constantBuffer(nullptr)
    , m_constantBufferData(nullptr)
    , m_mappedConstantBuffer(nullptr)
    , m_isDirty(true)
    , m_texturesDirty(false)
    , m_hasPendingTextures(false)
{
    if (m_shader) {
        // 分配CPU端缓冲区
        int bufferSize = m_shader->GetConstantBufferSize();
        if (bufferSize > 0) {
            m_constantBufferData = new unsigned char[bufferSize];
            memset(m_constantBufferData, 0, bufferSize);
        }

        // 初始化默认参数
        InitializeDefaultParameters();
    }
}

MaterialInstance::~MaterialInstance() {
    if (m_constantBufferData) {
        delete[] m_constantBufferData;
        m_constantBufferData = nullptr;
    }

    if (m_constantBuffer) {
        m_constantBuffer->Release();
        m_constantBuffer = nullptr;
    }
}

void MaterialInstance::InitializeDefaultParameters() {
    if (!m_shader) return;

    const auto& parameters = m_shader->GetParameters();
    for (const auto& param : parameters) {
        switch (param.type) {
            case ShaderParameterType::Float:
                m_floatParams[param.name] = param.defaultValue.floatVal;
                break;
            case ShaderParameterType::Vector4:
                m_vectorParams[param.name] = param.defaultValue.vector4Val;
                break;
            case ShaderParameterType::Vector3:
                m_vector3Params[param.name] = param.defaultValue.vector3Val;
                break;
            case ShaderParameterType::Int:
                m_intParams[param.name] = param.defaultValue.intVal;
                break;
            case ShaderParameterType::Bool:
                m_boolParams[param.name] = param.defaultValue.boolVal;
                break;
            case ShaderParameterType::Texture2D:
            case ShaderParameterType::TextureCube:
                m_textureParams[param.name] = param.defaultTexturePath;
                // Bindless模式：初始化默认SRV索引为0（指向默认纹理）
                m_textureSRVIndices[param.name] = 0;
                break;
            default:
                break;
        }
    }

    m_isDirty = true;
}

bool MaterialInstance::Initialize(ID3D12Device* device) {
    if (!m_shader || !device) return false;

    int bufferSize = m_shader->GetConstantBufferSize();
    if (bufferSize == 0) return true;  // 没有CB需求

    // 创建常量缓冲区（upload heap，可持续映射）
    m_constantBuffer = CreateConstantBufferObject(bufferSize);
    if (!m_constantBuffer) {
        return false;
    }

    // 映射常量缓冲区
    D3D12_RANGE readRange = { 0, 0 };  // 不需要读取
    HRESULT hr = m_constantBuffer->Map(0, &readRange, &m_mappedConstantBuffer);
    if (FAILED(hr)) {
        return false;
    }

    // 初始更新
    UpdateConstantBuffer();

    return true;
}

void MaterialInstance::SetFloat(const std::string& name, float value) {
    m_floatParams[name] = value;
    m_isDirty = true;
}

void MaterialInstance::SetVector(const std::string& name, const XMFLOAT4& value) {
    m_vectorParams[name] = value;
    m_isDirty = true;
}

void MaterialInstance::SetVector3(const std::string& name, const XMFLOAT3& value) {
    m_vector3Params[name] = value;
    m_isDirty = true;
}

void MaterialInstance::SetInt(const std::string& name, int value) {
    m_intParams[name] = value;
    m_isDirty = true;
}

void MaterialInstance::SetBool(const std::string& name, bool value) {
    m_boolParams[name] = value;
    m_isDirty = true;
}

void MaterialInstance::SetTexture(const std::string& name, const std::wstring& texturePath) {
    m_textureParams[name] = texturePath;
    m_isDirty = true;
}

float MaterialInstance::GetFloat(const std::string& name) const {
    auto it = m_floatParams.find(name);
    return (it != m_floatParams.end()) ? it->second : 0.0f;
}

XMFLOAT4 MaterialInstance::GetVector(const std::string& name) const {
    auto it = m_vectorParams.find(name);
    return (it != m_vectorParams.end()) ? it->second : XMFLOAT4(0, 0, 0, 0);
}

XMFLOAT3 MaterialInstance::GetVector3(const std::string& name) const {
    auto it = m_vector3Params.find(name);
    return (it != m_vector3Params.end()) ? it->second : XMFLOAT3(0, 0, 0);
}

int MaterialInstance::GetInt(const std::string& name) const {
    auto it = m_intParams.find(name);
    return (it != m_intParams.end()) ? it->second : 0;
}

bool MaterialInstance::GetBool(const std::string& name) const {
    auto it = m_boolParams.find(name);
    return (it != m_boolParams.end()) ? it->second : false;
}

std::wstring MaterialInstance::GetTexture(const std::string& name) const {
    auto it = m_textureParams.find(name);
    return (it != m_textureParams.end()) ? it->second : L"";
}

void MaterialInstance::SetTextureResource(const std::string& name, ID3D12Resource* resource, int registerSlot) {
    if (resource) {
        m_textureResources[registerSlot] = resource;
        m_texturesDirty = true;
        std::cout << "MaterialInstance '" << m_name << "': Set texture resource for '"
                  << name << "' at slot t" << registerSlot << std::endl;
    }
}

void MaterialInstance::SetTextureSRVIndex(const std::string& name, UINT srvIndex) {
    m_textureSRVIndices[name] = srvIndex;
    m_isDirty = true;  // 需要更新CB，因为纹理索引存储在CB中
    std::cout << "MaterialInstance '" << m_name << "': Set texture '" << name
              << "' SRV index = " << srvIndex << " (Bindless)" << std::endl;
}

UINT MaterialInstance::GetTextureSRVIndex(const std::string& name) const {
    auto it = m_textureSRVIndices.find(name);
    if (it != m_textureSRVIndices.end()) {
        return it->second;
    }
    return UINT_MAX;  // 无效索引
}

ID3D12Resource* MaterialInstance::GetTextureResource(const std::string& name) const {
    // 查找参数对应的寄存器槽位
    if (!m_shader) return nullptr;

    const auto& parameters = m_shader->GetParameters();
    for (const auto& param : parameters) {
        if (param.name == name &&
            (param.type == ShaderParameterType::Texture2D ||
             param.type == ShaderParameterType::TextureCube)) {
            auto it = m_textureResources.find(param.registerSlot);
            if (it != m_textureResources.end()) {
                return it->second;
            }
            break;
        }
    }
    return nullptr;
}

void MaterialInstance::BindTextures(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT descriptorSize) {
    if (!device || !srvHeap || m_textureResources.empty()) return;

    // 遍历所有纹理资源，更新对应的SRV槽位
    for (const auto& pair : m_textureResources) {
        int slotIndex = pair.first;
        ID3D12Resource* resource = pair.second;

        if (!resource) continue;

        // 计算目标槽位的CPU句柄
        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += slotIndex * descriptorSize;

        // 获取纹理描述
        D3D12_RESOURCE_DESC texDesc = resource->GetDesc();

        // 创建SRV描述
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        // 创建SRV
        device->CreateShaderResourceView(resource, &srvDesc, handle);
    }

    m_texturesDirty = false;
}

void MaterialInstance::PackConstantBuffer() {
    if (!m_shader || !m_constantBufferData) return;

    const auto& parameters = m_shader->GetParameters();
    for (const auto& param : parameters) {
        // 根据类型写入对应的值到指定偏移
        unsigned char* dest = m_constantBufferData + param.byteOffset;

        switch (param.type) {
            case ShaderParameterType::Float: {
                auto it = m_floatParams.find(param.name);
                if (it != m_floatParams.end()) {
                    *reinterpret_cast<float*>(dest) = it->second;
                }
                break;
            }
            case ShaderParameterType::Vector4: {
                auto it = m_vectorParams.find(param.name);
                if (it != m_vectorParams.end()) {
                    *reinterpret_cast<XMFLOAT4*>(dest) = it->second;
                }
                break;
            }
            case ShaderParameterType::Vector3: {
                auto it = m_vector3Params.find(param.name);
                if (it != m_vector3Params.end()) {
                    *reinterpret_cast<XMFLOAT3*>(dest) = it->second;
                }
                break;
            }
            case ShaderParameterType::Int: {
                auto it = m_intParams.find(param.name);
                if (it != m_intParams.end()) {
                    *reinterpret_cast<int*>(dest) = it->second;
                }
                break;
            }
            case ShaderParameterType::Bool: {
                auto it = m_boolParams.find(param.name);
                if (it != m_boolParams.end()) {
                    *reinterpret_cast<int*>(dest) = it->second ? 1 : 0;  // bool转int
                }
                break;
            }
            // Bindless纹理：将SRV索引写入CB
            case ShaderParameterType::Texture2D:
            case ShaderParameterType::TextureCube: {
                auto it = m_textureSRVIndices.find(param.name);
                if (it != m_textureSRVIndices.end()) {
                    *reinterpret_cast<UINT*>(dest) = it->second;
                } else {
                    // 默认索引0（应该指向一个默认纹理）
                    *reinterpret_cast<UINT*>(dest) = 0;
                }
                break;
            }
            default:
                break;
        }
    }
}

void MaterialInstance::UpdateConstantBuffer() {
    if (!m_constantBuffer || !m_mappedConstantBuffer || !m_constantBufferData) {
        return;
    }

    // 打包参数到CPU端缓冲区
    PackConstantBuffer();

    // 复制到GPU映射内存
    int bufferSize = m_shader ? m_shader->GetConstantBufferSize() : 256;
    memcpy(m_mappedConstantBuffer, m_constantBufferData, bufferSize);

    m_isDirty = false;
}

void MaterialInstance::Bind(ID3D12GraphicsCommandList* commandList,
                            ID3D12RootSignature* rootSig,
                            int materialCBSlot) {
    if (!commandList || !m_shader) return;

    // 如果有脏数据，更新常量缓冲区
    if (m_isDirty) {
        UpdateConstantBuffer();
    }

    // 绑定材质常量缓冲区到指定槽位（默认为槽位2，对应b1）
    if (m_constantBuffer) {
        commandList->SetGraphicsRootConstantBufferView(
            materialCBSlot,
            m_constantBuffer->GetGPUVirtualAddress()
        );
    }

    // 纹理绑定将在MaterialManager中处理（需要访问descriptor heap）
}

bool MaterialInstance::LoadFromXML(const std::wstring& filePath) {
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

    // 获取根节点
    IXMLDOMElement* pRoot = nullptr;
    hr = pXMLDom->get_documentElement(&pRoot);
    if (FAILED(hr) || !pRoot) {
        pXMLDom->Release();
        if (comInitialized) CoUninitialize();
        return false;
    }

    // 获取材质名称
    VARIANT varName;
    VariantInit(&varName);
    pRoot->getAttribute(_bstr_t("name"), &varName);
    if (varName.vt == VT_BSTR && varName.bstrVal) {
        m_name = BSTRToString(varName.bstrVal);
    }
    VariantClear(&varName);

    // 解析Shader节点（材质引用的shader名称）
    // 注意：这里只读取shader名称，实际的shader指针应该由MaterialManager设置
    std::string shaderName;
    IXMLDOMNodeList* pShaderNodes = nullptr;
    pRoot->getElementsByTagName(_bstr_t("Shader"), &pShaderNodes);
    if (pShaderNodes) {
        IXMLDOMNode* pShaderNode = nullptr;
        pShaderNodes->get_item(0, &pShaderNode);
        if (pShaderNode) {
            BSTR shaderText = nullptr;
            pShaderNode->get_text(&shaderText);
            if (shaderText) {
                shaderName = BSTRToString(shaderText);
                std::cout << "Material references shader: " << shaderName << std::endl;
                SysFreeString(shaderText);
            }
            pShaderNode->Release();
        }
        pShaderNodes->Release();
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
                            std::string paramName;
                            std::string paramType;
                            std::string paramValue;

                            // 获取属性
                            IXMLDOMNamedNodeMap* pAttrs = nullptr;
                            pParam->get_attributes(&pAttrs);
                            if (pAttrs) {
                                IXMLDOMNode* pNameAttr = nullptr;
                                pAttrs->getNamedItem(_bstr_t("name"), &pNameAttr);
                                if (pNameAttr) {
                                    BSTR val = nullptr;
                                    pNameAttr->get_text(&val);
                                    paramName = BSTRToString(val);
                                    SysFreeString(val);
                                    pNameAttr->Release();
                                }

                                IXMLDOMNode* pTypeAttr = nullptr;
                                pAttrs->getNamedItem(_bstr_t("type"), &pTypeAttr);
                                if (pTypeAttr) {
                                    BSTR val = nullptr;
                                    pTypeAttr->get_text(&val);
                                    paramType = BSTRToString(val);
                                    SysFreeString(val);
                                    pTypeAttr->Release();
                                }

                                pAttrs->Release();
                            }

                            // 获取值
                            BSTR nodeText = nullptr;
                            pParam->get_text(&nodeText);
                            paramValue = BSTRToString(nodeText);
                            SysFreeString(nodeText);

                            // 根据类型设置参数
                            if (paramType == "Float") {
                                SetFloat(paramName, std::stof(paramValue));
                            } else if (paramType == "Int") {
                                SetInt(paramName, std::stoi(paramValue));
                            } else if (paramType == "Bool") {
                                SetBool(paramName, std::stoi(paramValue) != 0);
                            } else if (paramType == "Vector4") {
                                std::istringstream iss(paramValue);
                                XMFLOAT4 vec;
                                iss >> vec.x >> vec.y >> vec.z >> vec.w;
                                SetVector(paramName, vec);
                            } else if (paramType == "Vector3") {
                                std::istringstream iss(paramValue);
                                XMFLOAT3 vec;
                                iss >> vec.x >> vec.y >> vec.z;
                                SetVector3(paramName, vec);
                            }
                        }

                        SysFreeString(nodeName);
                        pParam->Release();
                    }
                }
                pParamList->Release();
            }
            pParametersNode->Release();
        }
        pParameters->Release();
    }

    // 解析Textures
    IXMLDOMNodeList* pTextures = nullptr;
    pRoot->getElementsByTagName(_bstr_t("Textures"), &pTextures);
    if (pTextures) {
        IXMLDOMNode* pTexturesNode = nullptr;
        pTextures->get_item(0, &pTexturesNode);
        if (pTexturesNode) {
            IXMLDOMNodeList* pTexList = nullptr;
            pTexturesNode->get_childNodes(&pTexList);
            if (pTexList) {
                long texCount = 0;
                pTexList->get_length(&texCount);
                for (long i = 0; i < texCount; i++) {
                    IXMLDOMNode* pTex = nullptr;
                    pTexList->get_item(i, &pTex);
                    if (pTex) {
                        BSTR nodeName = nullptr;
                        pTex->get_nodeName(&nodeName);
                        std::string nodeNameStr = BSTRToString(nodeName);

                        if (nodeNameStr == "Texture") {
                            std::string texName;

                            // 获取name属性
                            IXMLDOMNamedNodeMap* pAttrs = nullptr;
                            pTex->get_attributes(&pAttrs);
                            if (pAttrs) {
                                IXMLDOMNode* pNameAttr = nullptr;
                                pAttrs->getNamedItem(_bstr_t("name"), &pNameAttr);
                                if (pNameAttr) {
                                    BSTR val = nullptr;
                                    pNameAttr->get_text(&val);
                                    texName = BSTRToString(val);
                                    SysFreeString(val);
                                    pNameAttr->Release();
                                }
                                pAttrs->Release();
                            }

                            // 获取路径值
                            BSTR nodeText = nullptr;
                            pTex->get_text(&nodeText);
                            std::string pathStr = BSTRToString(nodeText);

                            // 转换为wstring
                            int len = MultiByteToWideChar(CP_UTF8, 0, pathStr.c_str(), -1, nullptr, 0);
                            std::wstring texPath;
                            if (len > 0) {
                                texPath.resize(len - 1);
                                MultiByteToWideChar(CP_UTF8, 0, pathStr.c_str(), -1, &texPath[0], len);
                            }

                            SetTexture(texName, texPath);
                            SysFreeString(nodeText);
                        }

                        SysFreeString(nodeName);
                        pTex->Release();
                    }
                }
                pTexList->Release();
            }
            pTexturesNode->Release();
        }
        pTextures->Release();
    }

    pRoot->Release();
    pXMLDom->Release();
    if (comInitialized) CoUninitialize();

    // 如果有纹理路径，标记为待加载
    if (!m_textureParams.empty()) {
        m_hasPendingTextures = true;
    }

    m_isDirty = true;
    return true;
}

bool MaterialInstance::SaveToXML(const std::wstring& filePath) {
    std::ofstream file(filePath);
    if (!file.is_open()) return false;

    // 写入XML头
    file << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    file << "<Material name=\"" << m_name << "\">\n";

    // 写入Shader名称
    if (m_shader) {
        file << "  <Shader>" << m_shader->GetName() << "</Shader>\n";
    }

    // 写入Parameters
    file << "  <Parameters>\n";

    // Float参数
    for (const auto& pair : m_floatParams) {
        file << "    <Parameter name=\"" << pair.first << "\" type=\"Float\">"
             << pair.second << "</Parameter>\n";
    }

    // Vector4参数
    for (const auto& pair : m_vectorParams) {
        file << "    <Parameter name=\"" << pair.first << "\" type=\"Vector4\">"
             << pair.second.x << " " << pair.second.y << " "
             << pair.second.z << " " << pair.second.w << "</Parameter>\n";
    }

    // Vector3参数
    for (const auto& pair : m_vector3Params) {
        file << "    <Parameter name=\"" << pair.first << "\" type=\"Vector3\">"
             << pair.second.x << " " << pair.second.y << " "
             << pair.second.z << "</Parameter>\n";
    }

    // Int参数
    for (const auto& pair : m_intParams) {
        file << "    <Parameter name=\"" << pair.first << "\" type=\"Int\">"
             << pair.second << "</Parameter>\n";
    }

    // Bool参数
    for (const auto& pair : m_boolParams) {
        file << "    <Parameter name=\"" << pair.first << "\" type=\"Bool\">"
             << (pair.second ? "1" : "0") << "</Parameter>\n";
    }

    file << "  </Parameters>\n";

    // 写入Textures
    file << "  <Textures>\n";
    for (const auto& pair : m_textureParams) {
        // wstring转string
        int len = WideCharToMultiByte(CP_UTF8, 0, pair.second.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string pathStr;
        if (len > 0) {
            pathStr.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, pair.second.c_str(), -1, &pathStr[0], len, nullptr, nullptr);
        }

        // 获取register slot（从shader参数中查找）
        std::string slotStr = "t0";
        if (m_shader) {
            const ShaderParameter* param = m_shader->GetParameter(pair.first);
            if (param) {
                slotStr = "t" + std::to_string(param->registerSlot);
            }
        }

        file << "    <Texture name=\"" << pair.first << "\" slot=\"" << slotStr << "\">"
             << pathStr << "</Texture>\n";
    }
    file << "  </Textures>\n";

    file << "</Material>\n";
    file.close();

    return true;
}

bool MaterialInstance::LoadTexturesFromPaths(ID3D12GraphicsCommandList* commandList) {
    if (!m_shader || !commandList) return false;

    bool anyLoaded = false;
    TextureManager::GetInstance().SetCommandList(commandList);

    const auto& parameters = m_shader->GetParameters();
    for (const auto& param : parameters) {
        if (param.type != ShaderParameterType::Texture2D &&
            param.type != ShaderParameterType::TextureCube) {
            continue;
        }

        // 检查是否已经有有效的SRV索引（不是默认的0或UINT_MAX）
        UINT currentSRVIndex = GetTextureSRVIndex(param.name);
        if (currentSRVIndex != 0 && currentSRVIndex != UINT_MAX) {
            // 已经加载过了，跳过
            continue;
        }

        // 获取纹理路径
        std::wstring texPath = GetTexture(param.name);
        if (texPath.empty()) {
            continue;
        }

        std::cout << "MaterialInstance::LoadTexturesFromPaths - Loading '" << param.name << "'" << std::endl;

        // 加载纹理
        TextureAsset* textureAsset = TextureManager::GetInstance().LoadTexture(texPath);
        if (!textureAsset || !textureAsset->IsLoaded()) {
            std::cout << "MaterialInstance::LoadTexturesFromPaths - Failed to load texture: " << param.name << std::endl;
            continue;
        }

        // 分配Bindless SRV槽位
        UINT srvIndex = Scene::AllocateBindlessSRVSlot();
        if (srvIndex == UINT_MAX) {
            std::cout << "MaterialInstance::LoadTexturesFromPaths - Failed to allocate SRV slot for: " << param.name << std::endl;
            continue;
        }

        // 创建SRV
        Scene::CreateBindlessTextureSRV(srvIndex, textureAsset->GetResource());

        // 存储相对索引（shader中g_BindlessTextures从t10开始）
        UINT relativeIndex = srvIndex - 10;
        SetTextureSRVIndex(param.name, relativeIndex);

        // 保留旧的资源引用（用于兼容）
        SetTextureResource(param.name, textureAsset->GetResource(), param.registerSlot);

        std::cout << "MaterialInstance::LoadTexturesFromPaths - Loaded '" << param.name
                  << "' at SRV slot " << srvIndex << " (relative=" << relativeIndex << ")" << std::endl;

        anyLoaded = true;
    }

    if (anyLoaded) {
        m_hasPendingTextures = false;
        UpdateConstantBuffer();
    }

    return anyLoaded;
}

