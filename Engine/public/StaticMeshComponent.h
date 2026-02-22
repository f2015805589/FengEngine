// Engine/public/StaticMeshComponent.h
#pragma once
#include <d3d12.h>
#include <unordered_map>
#include <string>
#include <fbxsdk.h>

// 前向声明
class MaterialInstance;

struct StaticMeshComponentVertexData {
    float mPosition[4];
    float mTexcoord[4];
    float mNormal[4];
    float mTangent[4];

    bool operator==(const StaticMeshComponentVertexData& other) const {
        return memcmp(mPosition, other.mPosition, sizeof(mPosition)) == 0 &&
            memcmp(mTexcoord, other.mTexcoord, sizeof(mTexcoord)) == 0 &&
            memcmp(mNormal, other.mNormal, sizeof(mNormal)) == 0 &&
            memcmp(mTangent, other.mTangent, sizeof(mTangent)) == 0;
    }
};
struct SubMesh {
    ID3D12Resource* mIBO;
    D3D12_INDEX_BUFFER_VIEW mIBView;
    int mIndexCount;
    ~SubMesh() { if (mIBO) mIBO->Release(); }
};

class StaticMeshComponent {
public:
    ID3D12Resource* mVBO = nullptr;
    D3D12_VERTEX_BUFFER_VIEW mVBOView = {};
    StaticMeshComponentVertexData* mVertexData = nullptr;
    int mVertexCount = 0;
    std::unordered_map<std::string, SubMesh*> mSubMeshes;

    ~StaticMeshComponent() {
        if (mVBO) mVBO->Release();
        delete[] mVertexData;
        for (auto& pair : mSubMeshes) {
            delete pair.second;
        }
        mSubMeshes.clear();
    }

    void SetVertexCount(int inVertexCount);
    void SetVertexPosition(int inIndex, float inX, float inY, float inZ, float inW = 1.0f);
    void SetVertexTexcoord(int inIndex, float inX, float inY, float inZ = 0.0f, float inW = 1.0f);
    void SetVertexNormal(int inIndex, float inX, float inY, float inZ, float inW = 0.0f);
    void SetVertexTangent(int inIndex, float inX, float inY, float inZ, float inW = 1.0f);

    void InitFromFile(ID3D12GraphicsCommandList* inCommandList, const char* inFilePath);
    void Render(ID3D12GraphicsCommandList* inCommandList, ID3D12RootSignature* rootSignature);

    // 材质相关方法
    void SetMaterial(MaterialInstance* material) { m_material = material; }
    MaterialInstance* GetMaterial() const { return m_material; }

private:
    bool ParseFBXScene(FbxScene* pScene, ID3D12GraphicsCommandList* inCommandList);
    void ProcessFBXNode(FbxNode* pNode, ID3D12GraphicsCommandList* inCommandList);
    void ProcessFBXMesh(FbxMesh* pMesh, const std::string& nodeName, ID3D12GraphicsCommandList* inCommandList);

    // 材质成员
    MaterialInstance* m_material = nullptr;
};