// Actor.h
#pragma once
#include <string>
#include <DirectXMath.h>
#include <d3d12.h>
#include "BattleFireDirect.h"

// Forward declarations
class StaticMeshComponent;
class MaterialInstance;

// Actor Transform information
struct Transform {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 rotation;  // Euler angles (degrees)
    DirectX::XMFLOAT3 scale;

    Transform()
        : position(0.0f, 0.0f, 0.0f),
          rotation(0.0f, 0.0f, 0.0f),
          scale(1.0f, 1.0f, 1.0f) {}

    // Get model matrix
    DirectX::XMMATRIX GetModelMatrix() const;
};

// .mesh file format information (asset descriptor)
struct MeshAssetInfo {
    std::string meshName;
    std::string fbxPath;              // Relative path, e.g. "Content/Actor/FBX/sphere.fbx"
    std::string defaultMaterial;      // Default material name, e.g. "DefaultPBR"

    // Serialization/Deserialization
    bool SaveToFile(const std::wstring& filepath) const;
    bool LoadFromFile(const std::wstring& filepath);
};

// Actor class: Entity object in the scene
class Actor {
public:
    Actor(const std::string& name);
    ~Actor();

    // Load from .mesh file
    bool LoadFromMeshFile(const std::wstring& meshFilePath);

    // Transform operations
    void SetTransform(const Transform& transform);
    Transform& GetTransform() { return m_transform; }
    const Transform& GetTransform() const { return m_transform; }

    void SetPosition(const DirectX::XMFLOAT3& pos);
    void SetRotation(const DirectX::XMFLOAT3& rot);
    void SetScale(const DirectX::XMFLOAT3& scale);

    DirectX::XMFLOAT3 GetPosition() const { return m_transform.position; }
    DirectX::XMFLOAT3 GetRotation() const { return m_transform.rotation; }
    DirectX::XMFLOAT3 GetScale() const { return m_transform.scale; }

    // Update model matrix
    void UpdateModelMatrix();
    DirectX::XMMATRIX GetModelMatrix() const;

    // Getter
    const std::string& GetName() const { return m_name; }
    StaticMeshComponent* GetMesh() const { return m_mesh; }
    MaterialInstance* GetMaterial() const { return m_material; }
    const MeshAssetInfo& GetMeshAssetInfo() const { return m_meshAssetInfo; }

    // Setter
    void SetMesh(StaticMeshComponent* mesh) { m_mesh = mesh; }
    void SetMaterial(MaterialInstance* material) { m_material = material; }

    // Constant Buffer管理
    ID3D12Resource* GetConstantBuffer() const { return m_constantBuffer; }
    void CreateConstantBuffer(ID3D12Device* device);
    void UpdateConstantBuffer(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                             const DirectX::XMFLOAT3& lightDir, const DirectX::XMFLOAT3& cameraPos,
                             float skylightIntensity, const DirectX::XMFLOAT3& skylightColor,
                             const DirectX::XMMATRIX& invProjMatrix, const DirectX::XMMATRIX& invViewMatrix,
                             const DirectX::XMMATRIX& lightViewProjMatrix,
                             const DirectX::XMMATRIX& previousViewProjMatrix,
                             const DirectX::XMFLOAT2& jitterOffset, const DirectX::XMFLOAT2& previousJitterOffset,
                             int viewportWidth, int viewportHeight,
                             float nearPlane, float farPlane,
                             const DirectX::XMMATRIX& currentViewProjMatrix);

    // Is selected (for editor)
    bool IsSelected() const { return m_isSelected; }
    void SetSelected(bool selected) { m_isSelected = selected; }

private:
    std::string m_name;
    Transform m_transform;
    MeshAssetInfo m_meshAssetInfo;

    // Rendering components
    StaticMeshComponent* m_mesh;
    MaterialInstance* m_material;

    // Constant Buffer（每个Actor独立）
    ID3D12Resource* m_constantBuffer;
    void* m_mappedConstantBuffer;
    SceneCBData m_cbData;  // 使用共享CB结构体

    // Editor state
    bool m_isSelected;
};
