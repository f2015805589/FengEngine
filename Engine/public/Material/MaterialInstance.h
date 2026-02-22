#pragma once
#include <d3d12.h>
#include <string>
#include <map>
#include <DirectXMath.h>
#include "Shader.h"
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

class MaterialInstance {
public:
    MaterialInstance(const std::string& name, Shader* shader);
    ~MaterialInstance();

    // 从XML文件加载材质实例
    bool LoadFromXML(const std::wstring& filePath);

    // 保存材质实例到XML文件
    bool SaveToXML(const std::wstring& filePath);

    // 参数设置方法
    void SetFloat(const std::string& name, float value);
    void SetVector(const std::string& name, const XMFLOAT4& value);
    void SetVector3(const std::string& name, const XMFLOAT3& value);
    void SetInt(const std::string& name, int value);
    void SetBool(const std::string& name, bool value);
    void SetTexture(const std::string& name, const std::wstring& texturePath);

    // 设置纹理GPU资源（用于动态绑定）
    void SetTextureResource(const std::string& name, ID3D12Resource* resource, int registerSlot);

    // Bindless纹理：设置纹理的SRV索引（在全局SRV堆中的索引）
    void SetTextureSRVIndex(const std::string& name, UINT srvIndex);
    UINT GetTextureSRVIndex(const std::string& name) const;
    const std::map<std::string, UINT>& GetTextureSRVIndices() const { return m_textureSRVIndices; }

    // 参数获取方法
    float GetFloat(const std::string& name) const;
    XMFLOAT4 GetVector(const std::string& name) const;
    XMFLOAT3 GetVector3(const std::string& name) const;
    int GetInt(const std::string& name) const;
    bool GetBool(const std::string& name) const;
    std::wstring GetTexture(const std::string& name) const;
    ID3D12Resource* GetTextureResource(const std::string& name) const;

    // GPU资源管理
    bool Initialize(ID3D12Device* device);
    void UpdateConstantBuffer();  // 打包参数到常量缓冲区
    void Bind(ID3D12GraphicsCommandList* commandList,
              ID3D12RootSignature* rootSig,
              int materialCBSlot);  // 绑定材质到渲染管线

    // 绑定材质纹理到SRV堆（在渲染前调用）
    void BindTextures(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT descriptorSize);

    // 获取材质纹理资源映射（用于渲染时绑定）
    const std::map<int, ID3D12Resource*>& GetTextureResources() const { return m_textureResources; }

    // Getter方法
    Shader* GetShader() const { return m_shader; }
    const std::string& GetName() const { return m_name; }
    ID3D12Resource* GetConstantBuffer() const { return m_constantBuffer; }

    // 标记材质为脏（需要更新CB）
    void MarkDirty() { m_isDirty = true; }
    bool IsDirty() const { return m_isDirty; }

    // 标记纹理为脏（需要重新绑定SRV）
    void MarkTexturesDirty() { m_texturesDirty = true; }
    bool IsTexturesDirty() const { return m_texturesDirty; }

    // 从已保存的纹理路径加载纹理到GPU（在commandList可用时调用）
    // 返回true如果有纹理被加载
    bool LoadTexturesFromPaths(ID3D12GraphicsCommandList* commandList);

    // 检查是否有未加载的纹理
    bool HasPendingTextures() const { return m_hasPendingTextures; }

private:
    std::string m_name;
    Shader* m_shader;  // 引用的shader（不拥有所有权）

    // CPU端参数存储
    std::map<std::string, float> m_floatParams;
    std::map<std::string, XMFLOAT4> m_vectorParams;
    std::map<std::string, XMFLOAT3> m_vector3Params;
    std::map<std::string, int> m_intParams;
    std::map<std::string, bool> m_boolParams;
    std::map<std::string, std::wstring> m_textureParams;  // 纹理路径

    // Bindless纹理：存储纹理名称到SRV索引的映射
    std::map<std::string, UINT> m_textureSRVIndices;  // textureName -> SRV index in global heap

    // 纹理GPU资源（按寄存器槽位索引）- 保留用于兼容
    std::map<int, ID3D12Resource*> m_textureResources;  // registerSlot -> Resource

    // GPU资源
    ID3D12Resource* m_constantBuffer;        // 材质常量缓冲区 (b1)
    unsigned char* m_constantBufferData;     // CPU端缓冲区数据（用于打包）
    void* m_mappedConstantBuffer;            // 映射的CB指针

    bool m_isDirty;  // 标记是否需要更新CB
    bool m_texturesDirty;  // 标记纹理是否需要重新绑定
    bool m_hasPendingTextures;  // 标记是否有待加载的纹理

    // 内部辅助函数
    void PackConstantBuffer();  // 将参数打包到CB
    void InitializeDefaultParameters();  // 从shader初始化默认值
};
