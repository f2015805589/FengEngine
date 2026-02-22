#pragma once
#include <map>
#include <string>
#include <memory>
#include <vector>
#include "Shader.h"
#include "MaterialInstance.h"

class MaterialManager {
public:
    // 获取单例实例
    static MaterialManager& GetInstance();

    // 初始化和关闭
    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // RootSignature管理（用于创建PSO）
    void SetRootSignature(ID3D12RootSignature* rootSig) { m_rootSignature = rootSig; }
    ID3D12RootSignature* GetRootSignature() const { return m_rootSignature; }

    // Shader管理
    Shader* LoadShader(const std::wstring& shaderFilePath);
    Shader* GetShader(const std::string& shaderName);
    const std::vector<std::string> GetAllShaderNames() const;
    void ClearShaderCache(const std::string& shaderName);  // 清除指定shader的缓存
    void CompileAndCreateAllShadersPSO();  // 编译所有shader并创建PSO（在所有shader加载完后调用）

    // Material管理
    MaterialInstance* LoadMaterial(const std::wstring& materialFilePath);
    MaterialInstance* CreateMaterial(const std::string& name, Shader* shader);
    MaterialInstance* GetMaterial(const std::string& name);
    bool SaveMaterial(MaterialInstance* material, const std::wstring& filePath);
    const std::vector<std::string> GetAllMaterialNames() const;

    // 重新加载材质（强制从文件加载，忽略缓存）
    MaterialInstance* ReloadMaterial(const std::wstring& materialFilePath);

    // Texture管理（暂时简化，后续可与Scene的纹理系统集成）
    ID3D12Resource* LoadTexture(const std::wstring& texturePath);

    // 获取设备
    ID3D12Device* GetDevice() const { return m_device; }

private:
    // 私有构造/析构（单例模式）
    MaterialManager();
    ~MaterialManager();
    MaterialManager(const MaterialManager&) = delete;
    MaterialManager& operator=(const MaterialManager&) = delete;

    ID3D12Device* m_device;
    ID3D12RootSignature* m_rootSignature;

    // Shader和Material存储
    std::map<std::string, std::unique_ptr<Shader>> m_shaders;
    std::map<std::string, std::unique_ptr<MaterialInstance>> m_materials;

    // 纹理缓存
    std::map<std::wstring, ID3D12Resource*> m_textures;
};
