#include "public/Material/MaterialManager.h"
#include "public/Texture/TextureManager.h"
#include "public/Texture/TextureAsset.h"
#include "public/Scene.h"
#include "public/BattleFireDirect.h"
#include <iostream>
#include <comdef.h>
#include <msxml6.h>
#include <fstream>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

#pragma comment(lib, "msxml6.lib")

// 日志辅助函数
static void LogMaterialError(const std::string& materialName, const std::string& message) {
    // 创建日志目录
    CreateDirectoryW(L"Engine/Shader/Shader_Cache", NULL);
    CreateDirectoryW(L"Engine/Shader/Shader_Cache/log", NULL);

    // 生成日志文件名
    std::string logFileName = "Engine/Shader/Shader_Cache/log/" + materialName + "_error.txt";

    // 获取当前时间
    time_t now = time(0);
    tm timeinfo;
    localtime_s(&timeinfo, &now);
    char timeStr[100];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // 写入日志
    std::ofstream logFile(logFileName, std::ios::app);
    if (logFile.is_open()) {
        logFile << "[" << timeStr << "] " << message << std::endl;
        logFile.close();
    }
}

static void LogMaterialInfo(const std::string& message) {
    // 创建日志目录
    CreateDirectoryW(L"Engine/Shader/Shader_Cache", NULL);
    CreateDirectoryW(L"Engine/Shader/Shader_Cache/log", NULL);

    // 生成日志文件名
    std::string logFileName = "Engine/Shader/Shader_Cache/log/material_load.txt";

    // 获取当前时间
    time_t now = time(0);
    tm timeinfo;
    localtime_s(&timeinfo, &now);
    char timeStr[100];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // 写入日志
    std::ofstream logFile(logFileName, std::ios::app);
    if (logFile.is_open()) {
        logFile << "[" << timeStr << "] " << message << std::endl;
        logFile.close();
    }
}

static void LogMaterialSuccess(const std::string& materialName, const std::string& shaderName) {
    LogMaterialInfo("SUCCESS: Loaded material '" + materialName + "' with shader '" + shaderName + "'");
}

MaterialManager::MaterialManager()
    : m_device(nullptr)
    , m_rootSignature(nullptr)
{
}

MaterialManager::~MaterialManager() {
    Shutdown();
}

MaterialManager& MaterialManager::GetInstance() {
    static MaterialManager instance;
    return instance;
}

bool MaterialManager::Initialize(ID3D12Device* device) {
    if (!device) {
        LogMaterialError("CRITICAL", "ERROR: Cannot initialize MaterialManager with NULL device!");
        return false;
    }
    m_device = device;
    return true;
}

void MaterialManager::Shutdown() {
    // 清理所有材质
    m_materials.clear();

    // 清理所有shader
    m_shaders.clear();

    // 清理纹理资源
    for (auto& pair : m_textures) {
        if (pair.second) {
            pair.second->Release();
        }
    }
    m_textures.clear();

    m_device = nullptr;
}

Shader* MaterialManager::LoadShader(const std::wstring& shaderFilePath) {
    if (!m_device) return nullptr;

    // 提取shader名称（从文件路径）
    std::wstring fileName = shaderFilePath;
    size_t lastSlash = fileName.find_last_of(L"/\\");
    if (lastSlash != std::wstring::npos) {
        fileName = fileName.substr(lastSlash + 1);
    }
    size_t lastDot = fileName.find_last_of(L".");
    if (lastDot != std::wstring::npos) {
        fileName = fileName.substr(0, lastDot);
    }

    // 转换为string
    int len = WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string shaderName;
    if (len > 0) {
        shaderName.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, &shaderName[0], len, nullptr, nullptr);
    }

    // 检查是否已加载
    auto it = m_shaders.find(shaderName);
    if (it != m_shaders.end()) {
        return it->second.get();
    }

    // 创建新shader
    auto shader = std::make_unique<Shader>(shaderName);

    // 检测文件扩展名，选择加载方式
    bool loadSuccess = false;
    if (shaderFilePath.find(L".shader.ast") != std::wstring::npos) {
        // 旧的XML格式
        loadSuccess = shader->LoadFromXML(shaderFilePath);
    } else if (shaderFilePath.find(L".shader") != std::wstring::npos) {
        // 新的Unity风格格式
        loadSuccess = shader->LoadFromShaderFile(shaderFilePath);
    } else {
        std::wcout << L"Unknown shader file format: " << shaderFilePath << std::endl;
        return nullptr;
    }

    if (!loadSuccess) {
        std::wcout << L"Failed to load shader: " << shaderFilePath << std::endl;
        return nullptr;
    }

    // 注意：不在这里编译shader，由外部调用 CompileShaders() 和 CreatePSO()

    Shader* shaderPtr = shader.get();
    m_shaders[shaderName] = std::move(shader);

    return shaderPtr;
}

Shader* MaterialManager::GetShader(const std::string& shaderName) {
    // 详细日志：显示当前缓存内容
    std::ofstream debugLog("Engine/Shader/Shader_Cache/log/cache_debug.txt", std::ios::app);
    if (debugLog.is_open()) {
        debugLog << "[GetShader] Looking for: '" << shaderName << "'" << std::endl;
        debugLog << "[GetShader] Current cache contains " << m_shaders.size() << " shaders:" << std::endl;
        for (const auto& pair : m_shaders) {
            debugLog << "  - '" << pair.first << "'" << std::endl;
        }
    }

    auto it = m_shaders.find(shaderName);
    if (it != m_shaders.end()) {
        if (debugLog.is_open()) {
            debugLog << "[GetShader] FOUND shader '" << shaderName << "'" << std::endl;
        }
        return it->second.get();
    } else {
        if (debugLog.is_open()) {
            debugLog << "[GetShader] NOT FOUND shader '" << shaderName << "'" << std::endl;
        }
        return nullptr;
    }
}

void MaterialManager::ClearShaderCache(const std::string& shaderName) {
    auto it = m_shaders.find(shaderName);
    if (it != m_shaders.end()) {
        LogMaterialInfo("Clearing shader cache for: " + shaderName);
        m_shaders.erase(it);
    }
}

void MaterialManager::CompileAndCreateAllShadersPSO() {
    std::cout << "\n========== Compiling All Shaders ==========" << std::endl;

    // 阶段1：先编译所有非Screen shader（注册ShadingModel）
    std::cout << "\n[Phase 1] Compiling non-Screen shaders (registering ShadingModels)..." << std::endl;
    for (auto& pair : m_shaders) {
        Shader* shader = pair.second.get();
        if (!shader) continue;

        // 跳过 Screen shader
        if (pair.first == "Screen") {
            std::cout << "Skipping Screen shader (will compile in Phase 2)" << std::endl;
            continue;
        }

        std::cout << "Compiling: " << pair.first << std::endl;

        // 编译shader（会注册ShadingModel）
        if (shader->CompileShaders(m_device)) {
            // 为每个Pass创建PSO
            if (m_rootSignature) {
                for (int i = 0; i < shader->GetPassCount(); i++) {
                    if (!shader->CreatePSO(m_device, m_rootSignature, i)) {
                        std::cout << "  Failed to create PSO for pass " << i << std::endl;
                    } else {
                        std::cout << "  PSO created for pass " << i << std::endl;
                    }
                }
            } else {
                std::cout << "  WARNING: RootSignature not set, PSO creation skipped" << std::endl;
            }
        } else {
            std::cout << "  Failed to compile shader" << std::endl;
        }
    }

    // 阶段2：编译Screen shader（这时所有ShadingModel都已注册）
    std::cout << "\n[Phase 2] Compiling Screen shader (with all ShadingModels registered)..." << std::endl;
    auto screenIt = m_shaders.find("Screen");
    if (screenIt != m_shaders.end()) {
        Shader* screenShader = screenIt->second.get();
        if (screenShader) {
            std::cout << "Compiling: Screen" << std::endl;

            if (screenShader->CompileShaders(m_device)) {
                if (m_rootSignature) {
                    for (int i = 0; i < screenShader->GetPassCount(); i++) {
                        if (!screenShader->CreatePSO(m_device, m_rootSignature, i)) {
                            std::cout << "  Failed to create PSO for pass " << i << std::endl;
                        } else {
                            std::cout << "  PSO created for pass " << i << std::endl;
                        }
                    }
                } else {
                    std::cout << "  WARNING: RootSignature not set, PSO creation skipped" << std::endl;
                }
            } else {
                std::cout << "  Failed to compile Screen shader" << std::endl;
            }
        }
    } else {
        std::cout << "  Screen shader not found in cache" << std::endl;
    }

    std::cout << "\n========== Compilation Complete ==========" << std::endl;
}

const std::vector<std::string> MaterialManager::GetAllShaderNames() const {
    std::vector<std::string> names;
    for (const auto& pair : m_shaders) {
        names.push_back(pair.first);
    }
    return names;
}

MaterialInstance* MaterialManager::LoadMaterial(const std::wstring& materialFilePath) {
    LogMaterialInfo("========== LoadMaterial START ==========");

    // 转换路径为string用于日志
    int pathLen = WideCharToMultiByte(CP_UTF8, 0, materialFilePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string pathStr;
    if (pathLen > 0) {
        pathStr.resize(pathLen - 1);
        WideCharToMultiByte(CP_UTF8, 0, materialFilePath.c_str(), -1, &pathStr[0], pathLen, nullptr, nullptr);
    }
    LogMaterialInfo("Material file path: " + pathStr);

    if (!m_device) {
        LogMaterialError("CRITICAL", "ERROR: MaterialManager device is NULL! Call Initialize() first.");
        return nullptr;
    }

    LogMaterialInfo("Device is valid, parsing XML to get material name and shader reference...");

    // 第一步：预解析XML以获取材质名和shader引用
    std::string materialName;
    std::string shaderName;

    // 使用MSXML解析
    HRESULT hr = CoInitialize(nullptr);
    bool comInitialized = SUCCEEDED(hr);

    IXMLDOMDocument2* pXMLDom = nullptr;
    hr = CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                          __uuidof(IXMLDOMDocument2), (void**)&pXMLDom);
    if (SUCCEEDED(hr)) {
        VARIANT_BOOL loadSuccess = VARIANT_FALSE;
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_BSTR;
        var.bstrVal = SysAllocString(materialFilePath.c_str());
        hr = pXMLDom->load(var, &loadSuccess);
        VariantClear(&var);

        if (loadSuccess == VARIANT_TRUE) {
            IXMLDOMElement* pRoot = nullptr;
            pXMLDom->get_documentElement(&pRoot);
            if (pRoot) {
                // 获取材质名
                VARIANT varName;
                VariantInit(&varName);
                pRoot->getAttribute(_bstr_t("name"), &varName);
                if (varName.vt == VT_BSTR && varName.bstrVal) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, varName.bstrVal, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0) {
                        materialName.resize(len - 1);
                        WideCharToMultiByte(CP_UTF8, 0, varName.bstrVal, -1, &materialName[0], len, nullptr, nullptr);
                    }
                }
                VariantClear(&varName);

                // 获取shader引用
                IXMLDOMNodeList* pShaderNodes = nullptr;
                pRoot->getElementsByTagName(_bstr_t("Shader"), &pShaderNodes);
                if (pShaderNodes) {
                    IXMLDOMNode* pShaderNode = nullptr;
                    pShaderNodes->get_item(0, &pShaderNode);
                    if (pShaderNode) {
                        BSTR shaderText = nullptr;
                        pShaderNode->get_text(&shaderText);
                        if (shaderText) {
                            int len = WideCharToMultiByte(CP_UTF8, 0, shaderText, -1, nullptr, 0, nullptr, nullptr);
                            if (len > 0) {
                                shaderName.resize(len - 1);
                                WideCharToMultiByte(CP_UTF8, 0, shaderText, -1, &shaderName[0], len, nullptr, nullptr);
                            }
                            SysFreeString(shaderText);
                        }
                        pShaderNode->Release();
                    }
                    pShaderNodes->Release();
                }

                pRoot->Release();
            }
        }
        pXMLDom->Release();
    }
    if (comInitialized) CoUninitialize();

    LogMaterialInfo("XML parsing completed. Material name: '" + materialName + "', Shader name: '" + shaderName + "'");

    // 如果解析失败，使用文件名作为材质名
    if (materialName.empty()) {
        std::wstring fileName = materialFilePath;
        size_t lastSlash = fileName.find_last_of(L"/\\");
        if (lastSlash != std::wstring::npos) {
            fileName = fileName.substr(lastSlash + 1);
        }
        size_t lastDot = fileName.find_last_of(L".");
        if (lastDot != std::wstring::npos) {
            fileName = fileName.substr(0, lastDot);
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            materialName.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, &materialName[0], len, nullptr, nullptr);
        }
    }

    LogMaterialInfo("Final material name: '" + materialName + "'");

    // 检查是否已加载
    auto it = m_materials.find(materialName);
    if (it != m_materials.end()) {
        LogMaterialInfo("Material '" + materialName + "' found in cache, returning cached version");
        return it->second.get();
    }

    LogMaterialInfo("Material not in cache, loading shader '" + shaderName + "'...");

    // 第二步：加载shader
    Shader* shader = nullptr;
    if (!shaderName.empty()) {
        shader = GetShader(shaderName);
        if (!shader) {
            LogMaterialInfo("Shader '" + shaderName + "' not found in cache, loading from file...");
            // Shader未加载，尝试加载
            std::wstring shaderPath = L"Engine/Shader/" + std::wstring(shaderName.begin(), shaderName.end()) + L".shader";
            shader = LoadShader(shaderPath);
        } else {
            LogMaterialInfo("Shader '" + shaderName + "' found in cache");
        }
    }

    if (!shader) {
        LogMaterialError(materialName, "ERROR: Shader '" + shaderName + "' not found");
        return nullptr;
    }

    LogMaterialInfo("Shader loaded successfully, creating material instance...");

    // 第三步：创建材质实例
    auto material = std::make_unique<MaterialInstance>(materialName, shader);

    // 从XML加载参数
    if (!material->LoadFromXML(materialFilePath)) {
        LogMaterialError(materialName, "ERROR: Failed to load material parameters from XML");
        return nullptr;
    }

    LogMaterialInfo("Material parameters loaded from XML, initializing GPU resources...");

    // 初始化GPU资源
    if (!material->Initialize(m_device)) {
        LogMaterialError(materialName, "ERROR: Failed to initialize GPU resources");
        return nullptr;
    }

    LogMaterialInfo("GPU resources initialized successfully");

    // 尝试加载材质中指定的纹理（如果commandList可用）
    extern ID3D12GraphicsCommandList* gCommandList;
    if (gCommandList) {
        LogMaterialInfo("gCommandList is VALID, loading textures...");
        if (material->LoadTexturesFromPaths(gCommandList)) {
            LogMaterialInfo("Textures loaded successfully");
        } else {
            LogMaterialInfo("No textures loaded (may have no texture paths or already loaded)");
        }
    } else {
        LogMaterialInfo("WARNING: gCommandList is NULL, textures will be loaded later when available");
    }

    MaterialInstance* materialPtr = material.get();
    m_materials[materialName] = std::move(material);

    // 记录成功日志
    LogMaterialSuccess(materialName, shaderName);
    LogMaterialInfo("========== LoadMaterial SUCCESS ==========");

    return materialPtr;
}

MaterialInstance* MaterialManager::ReloadMaterial(const std::wstring& materialFilePath) {
    // 提取材质名称
    std::wstring fileName = materialFilePath;
    size_t lastSlash = fileName.find_last_of(L"/\\");
    if (lastSlash != std::wstring::npos) {
        fileName = fileName.substr(lastSlash + 1);
    }
    size_t lastDot = fileName.find_last_of(L".");
    if (lastDot != std::wstring::npos) {
        fileName = fileName.substr(0, lastDot);
    }

    std::string materialName;
    int len = WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        materialName.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, &materialName[0], len, nullptr, nullptr);
    }

    // 从缓存中移除旧材质
    auto it = m_materials.find(materialName);
    if (it != m_materials.end()) {
        m_materials.erase(it);
    }

    // 重新加载
    return LoadMaterial(materialFilePath);
}

MaterialInstance* MaterialManager::CreateMaterial(const std::string& name, Shader* shader) {
    if (!m_device || !shader) return nullptr;

    // 检查是否已存在
    auto it = m_materials.find(name);
    if (it != m_materials.end()) {
        return it->second.get();
    }

    // 创建新材质
    auto material = std::make_unique<MaterialInstance>(name, shader);

    // 初始化GPU资源
    if (!material->Initialize(m_device)) {
        return nullptr;
    }

    MaterialInstance* materialPtr = material.get();
    m_materials[name] = std::move(material);

    return materialPtr;
}

MaterialInstance* MaterialManager::GetMaterial(const std::string& name) {
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? it->second.get() : nullptr;
}

bool MaterialManager::SaveMaterial(MaterialInstance* material, const std::wstring& filePath) {
    if (!material) return false;
    return material->SaveToXML(filePath);
}

const std::vector<std::string> MaterialManager::GetAllMaterialNames() const {
    std::vector<std::string> names;
    for (const auto& pair : m_materials) {
        names.push_back(pair.first);
    }
    return names;
}

ID3D12Resource* MaterialManager::LoadTexture(const std::wstring& texturePath) {
    // 检查缓存
    auto it = m_textures.find(texturePath);
    if (it != m_textures.end()) {
        return it->second;
    }

    // 检查是否是 .texture.ast 文件
    std::wstring ext = PathFindExtensionW(texturePath.c_str());
    std::wstring lowerPath = texturePath;

    // 检查是否以 .texture.ast 结尾
    bool isTextureAsset = (lowerPath.length() > 12 &&
        lowerPath.substr(lowerPath.length() - 12) == L".texture.ast");

    if (isTextureAsset) {
        // 通过 TextureManager 加载 .texture.ast 文件
        TextureAsset* textureAsset = TextureManager::GetInstance().LoadTexture(texturePath);
        if (textureAsset && textureAsset->IsLoaded()) {
            ID3D12Resource* resource = textureAsset->GetResource();
            if (resource) {
                // 缓存资源（注意：这里不增加引用计数，由TextureManager管理生命周期）
                m_textures[texturePath] = resource;
                std::wcout << L"MaterialManager: Loaded texture from .texture.ast: " << texturePath << std::endl;
                return resource;
            }
        }
        std::wcout << L"MaterialManager: Failed to load .texture.ast: " << texturePath << std::endl;
        return nullptr;
    }

    // 对于其他格式（.png, .dds, .jpg等），尝试通过TextureManager加载
    TextureAsset* textureAsset = TextureManager::GetInstance().LoadTexture(texturePath);
    if (textureAsset && textureAsset->IsLoaded()) {
        ID3D12Resource* resource = textureAsset->GetResource();
        if (resource) {
            m_textures[texturePath] = resource;
            return resource;
        }
    }

    return nullptr;
}
