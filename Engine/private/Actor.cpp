// Actor.cpp
#include "public/Actor.h"
#include "public/StaticMeshComponent.h"
#include <fstream>
#include <sstream>

using namespace DirectX;

// ============ Transform实现 ============
XMMATRIX Transform::GetModelMatrix() const {
    // 缩放 * 旋转 * 平移
    XMMATRIX scaleMatrix = XMMatrixScaling(scale.x, scale.y, scale.z);

    // 欧拉角转旋转矩阵（ZXY顺序）
    XMMATRIX rotationMatrix = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(rotation.x),  // Pitch (X轴)
        XMConvertToRadians(rotation.y),  // Yaw (Y轴)
        XMConvertToRadians(rotation.z)   // Roll (Z轴)
    );

    XMMATRIX translationMatrix = XMMatrixTranslation(position.x, position.y, position.z);

    return scaleMatrix * rotationMatrix * translationMatrix;
}

// ============ MeshAssetInfo实现 ============
bool MeshAssetInfo::SaveToFile(const std::wstring& filepath) const {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    // Text format for mesh asset descriptor
    file << "[MeshAsset]\n";
    file << "Name=" << meshName << "\n";
    file << "FBXPath=" << fbxPath << "\n";
    file << "DefaultMaterial=" << defaultMaterial << "\n";

    file.close();
    return true;
}

bool MeshAssetInfo::LoadFromFile(const std::wstring& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        OutputDebugStringA("MeshAssetInfo::LoadFromFile - Failed to open file\n");
        char msg[256];
        sprintf_s(msg, "File path: %S\n", filepath.c_str());
        OutputDebugStringA(msg);
        return false;
    }

    std::string line;
    std::string currentSection;

    while (std::getline(file, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == '#') {
            continue;  // Skip empty lines and comments
        }

        // Check for section headers
        if (line[0] == '[' && line[line.length() - 1] == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }

        // Parse key-value pairs
        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, equalPos);
        std::string value = line.substr(equalPos + 1);

        // Parse based on current section
        if (currentSection == "MeshAsset") {
            if (key == "Name") {
                meshName = value;
            } else if (key == "FBXPath") {
                fbxPath = value;
            } else if (key == "DefaultMaterial") {
                defaultMaterial = value;
            }
        }
    }

    file.close();

    // Debug output
    OutputDebugStringA("MeshAssetInfo loaded successfully:\n");
    char msg[512];
    sprintf_s(msg, "  Name: %s\n  FBXPath: %s\n  DefaultMaterial: %s\n",
              meshName.c_str(), fbxPath.c_str(), defaultMaterial.c_str());
    OutputDebugStringA(msg);

    return true;
}

// ============ Actor实现 ============
Actor::Actor(const std::string& name)
    : m_name(name),
      m_mesh(nullptr),
      m_material(nullptr),
      m_constantBuffer(nullptr),
      m_mappedConstantBuffer(nullptr),
      m_isSelected(false) {
    memset(&m_cbData, 0, sizeof(m_cbData));
}

Actor::~Actor() {
    // 注意：mesh和material通常由外部管理，这里不删除

    // 清理Constant Buffer
    if (m_constantBuffer) {
        if (m_mappedConstantBuffer) {
            m_constantBuffer->Unmap(0, nullptr);
            m_mappedConstantBuffer = nullptr;
        }
        m_constantBuffer->Release();
        m_constantBuffer = nullptr;
    }
}

bool Actor::LoadFromMeshFile(const std::wstring& meshFilePath) {
    if (!m_meshAssetInfo.LoadFromFile(meshFilePath)) {
        return false;
    }

    // Note: Transform is NOT loaded from .mesh file
    // Transform will be set from the .level file
    // Mesh loading logic is handled in Scene

    return true;
}

void Actor::SetTransform(const Transform& transform) {
    m_transform = transform;
}

void Actor::SetPosition(const XMFLOAT3& pos) {
    m_transform.position = pos;
}

void Actor::SetRotation(const XMFLOAT3& rot) {
    m_transform.rotation = rot;
}

void Actor::SetScale(const XMFLOAT3& scale) {
    m_transform.scale = scale;
}

void Actor::UpdateModelMatrix() {
    // 在渲染时动态计算，无需额外存储
}

XMMATRIX Actor::GetModelMatrix() const {
    return m_transform.GetModelMatrix();
}

void Actor::CreateConstantBuffer(ID3D12Device* device) {
    if (!device) return;

    // 使用已有工具函数创建CB
    m_constantBuffer = CreateConstantBufferObject(sizeof(SceneCBData));
    if (!m_constantBuffer) {
        OutputDebugStringA("Actor::CreateConstantBuffer - Failed to create CB\n");
        return;
    }

    // 持久映射
    HRESULT hr = m_constantBuffer->Map(0, nullptr, &m_mappedConstantBuffer);
    if (FAILED(hr)) {
        OutputDebugStringA("Actor::CreateConstantBuffer - Failed to map CB\n");
        m_constantBuffer->Release();
        m_constantBuffer = nullptr;
        return;
    }

    // 初始化CB：用单位矩阵填充关键矩阵字段
    memset(&m_cbData, 0, sizeof(m_cbData));
    DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
    DirectX::XMStoreFloat4x4(&m_cbData.projMatrix, identity);
    DirectX::XMStoreFloat4x4(&m_cbData.viewMatrix, identity);
    DirectX::XMStoreFloat4x4(&m_cbData.modelMatrix, identity);
    DirectX::XMStoreFloat4x4(&m_cbData.normalMatrix, identity);

    memcpy(m_mappedConstantBuffer, &m_cbData, sizeof(SceneCBData));
}

void Actor::UpdateConstantBuffer(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                  const DirectX::XMFLOAT3& lightDir, const DirectX::XMFLOAT3& cameraPos,
                                  float skylightIntensity, const DirectX::XMFLOAT3& skylightColor,
                                  const DirectX::XMMATRIX& invProjMatrix, const DirectX::XMMATRIX& invViewMatrix,
                                  const DirectX::XMMATRIX& lightViewProjMatrix,
                                  const DirectX::XMMATRIX& previousViewProjMatrix,
                                  const DirectX::XMFLOAT2& jitterOffset, const DirectX::XMFLOAT2& previousJitterOffset,
                                  int viewportWidth, int viewportHeight,
                                  float nearPlane, float farPlane,
                                  const DirectX::XMMATRIX& currentViewProjMatrix,
                                  int shadowMode) {
    if (!m_mappedConstantBuffer) return;

    // 使用共享函数填充CB（Actor使用自己的ModelMatrix）
    FillSceneCBData(m_cbData,
        viewMatrix, projMatrix, GetModelMatrix(),
        lightDir, cameraPos,
        skylightIntensity, skylightColor,
        invProjMatrix, invViewMatrix,
        lightViewProjMatrix, previousViewProjMatrix,
        jitterOffset, previousJitterOffset,
        viewportWidth, viewportHeight,
        nearPlane, farPlane,
        currentViewProjMatrix, shadowMode);

    memcpy(m_mappedConstantBuffer, &m_cbData, sizeof(SceneCBData));
}
