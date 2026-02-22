// TextureManager.cpp
// 纹理管理器实现

#define NOMINMAX

#include "public/Texture/TextureManager.h"
#include "public/Texture/TextureCompressor.h"
#include "public/BattleFireDirect.h"
#include <d3dx12.h>
#include <iostream>
#include <algorithm>
#include <shlwapi.h>
#include <windows.h>

#pragma comment(lib, "shlwapi.lib")

namespace {
    // wstring转string
    std::string WStringToString(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
        return result;
    }

    // string转wstring
    std::wstring StringToWString(const std::string& str) {
        if (str.empty()) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        std::wstring result(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], len);
        return result;
    }
}

// ========== 单例实现 ==========

TextureManager& TextureManager::GetInstance() {
    static TextureManager instance;
    return instance;
}

TextureManager::TextureManager() {
}

TextureManager::~TextureManager() {
    Shutdown();
}

// ========== 初始化和清理 ==========

bool TextureManager::Initialize(ID3D12Device* device) {
    if (!device) {
        std::cout << "TextureManager::Initialize - Invalid device" << std::endl;
        return false;
    }

    m_device = device;

    // 创建SRV描述符堆
    CreateSRVHeap();

    // 初始化SRV槽位管理
    m_srvSlotUsed.resize(MAX_TEXTURES, false);
    m_nextFreeSlot = 0;

    // 获取可执行文件目录，创建绝对路径的缓存目录
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    m_cacheDir = std::wstring(exePath) + L"\\TextureCache\\";

    // 确保缓存目录存在
    if (!CreateDirectoryW(m_cacheDir.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            MessageBoxW(NULL, (L"Failed to create cache dir: " + m_cacheDir).c_str(), L"Warning", MB_OK);
        }
    }

    std::wcout << L"TextureManager cache dir: " << m_cacheDir << std::endl;
    std::cout << "TextureManager initialized with " << MAX_TEXTURES << " SRV slots" << std::endl;
    return true;
}

void TextureManager::Shutdown() {
    // 卸载所有纹理
    UnloadAllTextures();

    // 释放压缩器
    m_compressor.reset();

    // 释放SRV堆
    m_srvHeap.Reset();

    m_device = nullptr;
    std::cout << "TextureManager shutdown" << std::endl;
}

// ========== SRV堆管理 ==========

void TextureManager::CreateSRVHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = MAX_TEXTURES;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        std::cout << "Failed to create TextureManager SRV heap" << std::endl;
        return;
    }

    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_srvHeap->SetName(L"TextureManager_SRVHeap");
}

UINT TextureManager::AllocateSRVIndex() {
    std::lock_guard<std::mutex> lock(m_srvMutex);

    // 从上次分配位置开始查找
    for (UINT i = 0; i < MAX_TEXTURES; i++) {
        UINT index = (m_nextFreeSlot + i) % MAX_TEXTURES;
        if (!m_srvSlotUsed[index]) {
            m_srvSlotUsed[index] = true;
            m_nextFreeSlot = (index + 1) % MAX_TEXTURES;
            return index;
        }
    }

    std::cout << "TextureManager: No free SRV slots available!" << std::endl;
    return UINT_MAX;
}

void TextureManager::FreeSRVIndex(UINT index) {
    if (index >= MAX_TEXTURES) return;

    std::lock_guard<std::mutex> lock(m_srvMutex);
    m_srvSlotUsed[index] = false;
}

D3D12_CPU_DESCRIPTOR_HANDLE TextureManager::GetSRVCPUHandle(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += index * m_srvDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetSRVGPUHandle(UINT index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += index * m_srvDescriptorSize;
    return handle;
}

// ========== 纹理加载 ==========

TextureAsset* TextureManager::LoadTexture(const std::wstring& path) {
    std::cout << "TextureManager::LoadTexture - START: " << WStringToString(path) << std::endl;
    std::lock_guard<std::mutex> lock(m_textureMutex);
    std::cout << "TextureManager::LoadTexture - Got lock, calling LoadTextureInternal" << std::endl;
    TextureAsset* result = LoadTextureInternal(path);
    std::cout << "TextureManager::LoadTexture - END: " << (result ? "SUCCESS" : "FAILED") << std::endl;
    return result;
}

TextureAsset* TextureManager::LoadTextureInternal(const std::wstring& path) {
    // 检查是否已加载
    auto pathIt = m_pathToName.find(path);
    if (pathIt != m_pathToName.end()) {
        auto texIt = m_textures.find(pathIt->second);
        if (texIt != m_textures.end()) {
            TextureAsset* cachedTexture = texIt->second.get();
            // 如果纹理已缓存但未加载到GPU，尝试加载
            if (cachedTexture && !cachedTexture->IsLoaded() && m_commandList) {
                std::cout << "TextureManager: Loading cached texture to GPU: " << pathIt->second << std::endl;
                cachedTexture->LoadToGPU(m_device, m_commandList);
            }
            return cachedTexture;
        }
    }

    // 生成资产名称
    std::string name = GenerateNameFromPath(path);

    // 检查名称是否已存在
    auto texIt = m_textures.find(name);
    if (texIt != m_textures.end()) {
        TextureAsset* cachedTexture = texIt->second.get();
        // 如果纹理已缓存但未加载到GPU，尝试加载
        if (cachedTexture && !cachedTexture->IsLoaded() && m_commandList) {
            std::cout << "TextureManager: Loading cached texture to GPU: " << name << std::endl;
            cachedTexture->LoadToGPU(m_device, m_commandList);
        }
        return cachedTexture;
    }

    // 创建新的纹理资产
    auto texture = std::make_unique<TextureAsset>(name);

    bool loaded = false;

    if (IsAssetFile(path)) {
        // 从.texture.ast文件加载
        loaded = texture->LoadFromAssetFile(path);
    }
    else if (IsDDSFile(path)) {
        // 直接加载DDS（创建临时资产描述）
        TextureAssetDesc desc;
        desc.sourcePath = path;
        desc.format = TextureCompressionFormat::None;  // DDS已经压缩
        desc.generateMips = false;
        texture->ImportFromSource(path, desc);
        loaded = true;
    }
    else if (IsSourceFile(path)) {
        // 从源文件导入（使用默认设置）
        TextureAssetDesc desc;
        desc.sourcePath = path;
        desc.format = TextureCompressionFormat::BC3;
        desc.generateMips = true;
        desc.sRGB = true;
        texture->ImportFromSource(path, desc);
        loaded = true;
    }

    if (!loaded) {
        std::cout << "TextureManager: Failed to load texture: " << WStringToString(path) << std::endl;
        return nullptr;
    }

    // 加载到GPU
    if (!m_commandList) {
        std::cout << "TextureManager: WARNING - commandList is NULL, texture will not be loaded to GPU!" << std::endl;
        std::cout << "TextureManager: Call SetCommandList() before loading textures." << std::endl;
        // 仍然保存纹理资产，但标记为未加载
    }

    if (m_commandList) {
        if (!texture->LoadToGPU(m_device, m_commandList)) {
            std::cout << "TextureManager: Failed to load texture to GPU: " << name << std::endl;
            return nullptr;
        }
    }

    // 保存到缓存
    TextureAsset* result = texture.get();
    m_textures[name] = std::move(texture);
    m_pathToName[path] = name;

    std::cout << "TextureManager: Loaded texture '" << name << "'" << std::endl;
    return result;
}

std::future<TextureAsset*> TextureManager::LoadTextureAsync(const std::wstring& path) {
    return std::async(std::launch::async, [this, path]() {
        std::lock_guard<std::mutex> lock(m_textureMutex);
        return LoadTextureInternal(path);
    });
}

TextureAsset* TextureManager::ImportTexture(const std::wstring& sourcePath, const TextureAssetDesc& desc) {
    std::lock_guard<std::mutex> lock(m_textureMutex);

    // 生成资产名称
    std::string name = GenerateNameFromPath(sourcePath);

    // 检查是否已存在
    auto it = m_textures.find(name);
    if (it != m_textures.end()) {
        return it->second.get();
    }

    // 创建新的纹理资产（UE风格：导入时不压缩）
    auto texture = std::make_unique<TextureAsset>(name);

    // 设置描述但不立即压缩
    TextureAssetDesc importDesc = desc;
    importDesc.format = TextureCompressionFormat::None;  // 导入时不压缩
    texture->ImportFromSource(sourcePath, importDesc);

    // 检查m_device和m_commandList
    if (!m_device || !m_commandList) {
        std::cout << "TextureManager: device or commandList is NULL" << std::endl;
        return nullptr;
    }

    // 加载原图到GPU（不压缩）
    if (!texture->LoadSourceToGPU(m_device, m_commandList)) {
        std::cout << "TextureManager: Failed to load source texture: " << name << std::endl;
        return nullptr;
    }

    // 保存到缓存
    TextureAsset* result = texture.get();
    m_textures[name] = std::move(texture);
    m_pathToName[sourcePath] = name;

    std::cout << "TextureManager: Imported texture '" << name << "' (uncompressed)" << std::endl;
    return result;
}

TextureAsset* TextureManager::GetTexture(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_textureMutex);

    auto it = m_textures.find(name);
    if (it != m_textures.end()) {
        return it->second.get();
    }
    return nullptr;
}

TextureAsset* TextureManager::GetTextureByPath(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(m_textureMutex);

    auto pathIt = m_pathToName.find(path);
    if (pathIt != m_pathToName.end()) {
        auto texIt = m_textures.find(pathIt->second);
        if (texIt != m_textures.end()) {
            return texIt->second.get();
        }
    }
    return nullptr;
}

// ========== 纹理卸载 ==========

void TextureManager::UnloadTexture(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_textureMutex);

    auto it = m_textures.find(name);
    if (it != m_textures.end()) {
        // 从路径映射中移除
        for (auto pathIt = m_pathToName.begin(); pathIt != m_pathToName.end();) {
            if (pathIt->second == name) {
                pathIt = m_pathToName.erase(pathIt);
            }
            else {
                ++pathIt;
            }
        }

        m_textures.erase(it);
        std::cout << "TextureManager: Unloaded texture '" << name << "'" << std::endl;
    }
}

void TextureManager::UnloadAllTextures() {
    std::lock_guard<std::mutex> lock(m_textureMutex);

    m_textures.clear();
    m_pathToName.clear();

    // 重置SRV槽位
    std::fill(m_srvSlotUsed.begin(), m_srvSlotUsed.end(), false);
    m_nextFreeSlot = 0;

    std::cout << "TextureManager: Unloaded all textures" << std::endl;
}

// ========== 缓存管理 ==========

void TextureManager::ClearCache() {
    // 使用Windows API遍历目录
    std::wstring searchPath = m_cacheDir + L"*.dds";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring filePath = m_cacheDir + findData.cFileName;
            DeleteFileW(filePath.c_str());
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    std::cout << "TextureManager: Cache cleared" << std::endl;
}

void TextureManager::RebuildCache(ID3D12GraphicsCommandList* commandList) {
    std::lock_guard<std::mutex> lock(m_textureMutex);

    // 设置命令列表
    m_commandList = commandList;

    // 重新加载所有纹理
    for (auto& pair : m_textures) {
        pair.second->UnloadFromGPU();
        pair.second->LoadToGPU(m_device, commandList);
    }

    std::cout << "TextureManager: Cache rebuilt" << std::endl;
}

bool TextureManager::ValidateCache(const std::wstring& assetPath) {
    // 加载资产文件检查缓存有效性
    TextureAsset tempAsset("temp");
    if (tempAsset.LoadFromAssetFile(assetPath)) {
        return tempAsset.IsCacheValid();
    }
    return false;
}

// ========== 统计信息 ==========

size_t TextureManager::GetTotalMemoryUsage() const {
    size_t total = 0;
    for (const auto& pair : m_textures) {
        total += pair.second->GetMemorySize();
    }
    return total;
}

std::vector<std::string> TextureManager::GetAllTextureNames() const {
    std::vector<std::string> names;
    names.reserve(m_textures.size());
    for (const auto& pair : m_textures) {
        names.push_back(pair.first);
    }
    return names;
}

// ========== 辅助函数 ==========

std::string TextureManager::GenerateNameFromPath(const std::wstring& path) {
    wchar_t fileName[MAX_PATH];
    wcscpy_s(fileName, path.c_str());
    PathStripPathW(fileName);
    PathRemoveExtensionW(fileName);

    // 如果是.texture.ast文件，再次移除.texture后缀
    std::wstring name = fileName;
    size_t pos = name.rfind(L".texture");
    if (pos != std::wstring::npos) {
        name = name.substr(0, pos);
    }

    return WStringToString(name);
}

bool TextureManager::IsAssetFile(const std::wstring& path) {
    std::wstring ext = PathFindExtensionW(path.c_str());
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return (ext == L".ast" && path.find(L".texture.ast") != std::wstring::npos);
}

bool TextureManager::IsSourceFile(const std::wstring& path) {
    std::wstring ext = PathFindExtensionW(path.c_str());
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return (ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
            ext == L".bmp" || ext == L".tga" || ext == L".hdr");
}

bool TextureManager::IsDDSFile(const std::wstring& path) {
    std::wstring ext = PathFindExtensionW(path.c_str());
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return (ext == L".dds");
}
