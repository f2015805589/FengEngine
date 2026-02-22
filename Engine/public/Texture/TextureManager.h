#pragma once
#include "TextureAsset.h"
#include <map>
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <future>

class TextureCompressor;

class TextureManager {
public:
    static TextureManager& GetInstance();

    // 禁止拷贝
    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;

    // 初始化和清理
    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // ========== 纹理加载 ==========

    // 从资产文件或源文件加载纹理
    // 支持: .texture.ast, .png, .jpg, .dds, .hdr
    TextureAsset* LoadTexture(const std::wstring& path);

    // 异步加载纹理（返回future）
    std::future<TextureAsset*> LoadTextureAsync(const std::wstring& path);

    // 从源文件导入并创建新资产
    TextureAsset* ImportTexture(const std::wstring& sourcePath,
                                 const TextureAssetDesc& desc);

    // 获取已加载的纹理
    TextureAsset* GetTexture(const std::string& name);
    TextureAsset* GetTextureByPath(const std::wstring& path);

    // ========== 纹理卸载 ==========

    void UnloadTexture(const std::string& name);
    void UnloadAllTextures();

    // ========== SRV堆管理 ==========

    // 分配SRV槽位
    UINT AllocateSRVIndex();

    // 释放SRV槽位
    void FreeSRVIndex(UINT index);

    // 获取SRV堆
    ID3D12DescriptorHeap* GetSRVHeap() const { return m_srvHeap.Get(); }

    // 获取SRV描述符大小
    UINT GetSRVDescriptorSize() const { return m_srvDescriptorSize; }

    // 根据索引获取CPU/GPU句柄
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPUHandle(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVGPUHandle(UINT index) const;

    // ========== 压缩器 ==========

    TextureCompressor* GetCompressor() const { return m_compressor.get(); }

    // ========== 缓存管理 ==========

    // 获取缓存目录
    const std::wstring& GetCacheDirectory() const { return m_cacheDir; }

    // 清除所有缓存
    void ClearCache();

    // 重建所有纹理缓存
    void RebuildCache(ID3D12GraphicsCommandList* commandList);

    // 验证缓存有效性
    bool ValidateCache(const std::wstring& assetPath);

    // ========== 统计信息 ==========

    int GetLoadedTextureCount() const { return (int)m_textures.size(); }
    size_t GetTotalMemoryUsage() const;
    std::vector<std::string> GetAllTextureNames() const;

    // ========== 设备访问 ==========

    ID3D12Device* GetDevice() const { return m_device; }

    // ========== 命令列表（用于GPU操作）==========

    void SetCommandList(ID3D12GraphicsCommandList* commandList) { m_commandList = commandList; }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList; }

    // ========== 常量 ==========

    static const UINT MAX_TEXTURES = 1000;  // SRV堆最大纹理数

private:
    TextureManager();
    ~TextureManager();

    ID3D12Device* m_device = nullptr;
    ID3D12GraphicsCommandList* m_commandList = nullptr;

    // 纹理存储（按名称索引）
    std::map<std::string, std::unique_ptr<TextureAsset>> m_textures;

    // 路径到名称的映射（用于快速查找）
    std::map<std::wstring, std::string> m_pathToName;

    // SRV描述符堆
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;

    // SRV槽位管理
    std::vector<bool> m_srvSlotUsed;
    UINT m_nextFreeSlot = 0;
    std::mutex m_srvMutex;

    // GPU压缩器
    std::unique_ptr<TextureCompressor> m_compressor;

    // 缓存目录
    std::wstring m_cacheDir = L"Engine/TextureCache/";

    // 异步加载相关
    std::mutex m_textureMutex;

    // ========== 内部方法 ==========

    // 创建SRV描述符堆
    void CreateSRVHeap();

    // 从路径生成资产名称
    std::string GenerateNameFromPath(const std::wstring& path);

    // 检查文件扩展名
    bool IsAssetFile(const std::wstring& path);
    bool IsSourceFile(const std::wstring& path);
    bool IsDDSFile(const std::wstring& path);

    // 加载纹理的内部实现
    TextureAsset* LoadTextureInternal(const std::wstring& path);
};
