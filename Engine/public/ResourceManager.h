#pragma once
#include <string>
#include <vector>
#include <map>
#include <d3d12.h>

class Shader;
class MaterialInstance;

// 资源层级枚举
enum class ResourceLayer {
    Content,  // Content 层级（优先级高，项目资源）
    Engine    // Engine 层级（优先级低，引擎资源）
};

// 资源信息结构
struct ResourceInfo {
    std::string name;           // 资源名称
    std::wstring filePath;      // 完整文件路径
    ResourceLayer layer;        // 所属层级
    std::string type;           // 资源类型（shader, material, texture等）
    bool isLoaded;              // 是否已加载

    ResourceInfo() : layer(ResourceLayer::Engine), isLoaded(false) {}
};

// 文件树节点结构
struct FileTreeNode {
    std::string name;                      // 文件/文件夹名称
    std::wstring fullPath;                 // 完整路径
    bool isDirectory;                      // 是否是目录
    std::string extension;                 // 文件扩展名（仅文件）
    std::vector<FileTreeNode*> children;   // 子节点

    FileTreeNode() : isDirectory(false) {}
    ~FileTreeNode() {
        for (auto* child : children) {
            delete child;
        }
    }
};

class ResourceManager {
public:
    static ResourceManager& GetInstance();

    // 初始化资源管理器
    void Initialize(ID3D12Device* device, ID3D12RootSignature* rootSig);

    // 扫描并加载所有资源
    void ScanAndLoadAllResources();

    // 扫描指定目录的 shader 文件
    void ScanShaders(const std::wstring& directory, ResourceLayer layer);

    // 扫描指定目录的 material 文件
    void ScanMaterials(const std::wstring& directory, ResourceLayer layer);

    // 扫描完整目录树（所有文件和文件夹）
    void ScanCompleteDirectory(const std::wstring& directory, FileTreeNode* parentNode);

    // 获取所有 shader 资源信息
    const std::vector<ResourceInfo>& GetAllShaderResources() const { return m_shaderResources; }

    // 获取所有 material 资源信息
    const std::vector<ResourceInfo>& GetAllMaterialResources() const { return m_materialResources; }

    // 获取文件树根节点
    FileTreeNode* GetContentRoot() const { return m_contentRoot; }
    FileTreeNode* GetEngineRoot() const { return m_engineRoot; }

    // 获取资源统计信息
    int GetTotalShaderCount() const { return static_cast<int>(m_shaderResources.size()); }
    int GetLoadedShaderCount() const;
    int GetTotalMaterialCount() const { return static_cast<int>(m_materialResources.size()); }
    int GetLoadedMaterialCount() const;

    // UI 窗口控制
    void ShowResourceWindow(bool* open);

private:
    ResourceManager() : m_contentRoot(nullptr), m_engineRoot(nullptr) {}
    ~ResourceManager() {
        delete m_contentRoot;
        delete m_engineRoot;
    }
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // 扫描目录中的文件（递归）
    void ScanDirectory(const std::wstring& directory, const std::wstring& extension, ResourceLayer layer, std::vector<ResourceInfo>& outResources);

    // 从文件路径提取资源名称
    std::string ExtractResourceName(const std::wstring& filePath);

    // 渲染文件树节点（递归）
    void RenderFileTreeNode(FileTreeNode* node);

    ID3D12Device* m_device = nullptr;
    ID3D12RootSignature* m_rootSignature = nullptr;

    // 资源列表
    std::vector<ResourceInfo> m_shaderResources;
    std::vector<ResourceInfo> m_materialResources;

    // 文件树根节点
    FileTreeNode* m_contentRoot;
    FileTreeNode* m_engineRoot;

    // 路径配置（在Initialize中动态设置）
    std::wstring m_contentPath;
    std::wstring m_enginePath;
};
