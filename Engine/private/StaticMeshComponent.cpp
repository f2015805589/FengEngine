// Engine/private/StaticMeshComponent.cpp
#include "public/StaticMeshComponent.h"
#include "public/BattleFireDirect.h"
#include "public/Material/MaterialInstance.h"
#include <assert.h>
#include <iostream>
#include <fbxsdk.h>
#include <windows.h>

std::string GetModulePath() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string::size_type pos = std::string(path).find_last_of("\\/");
    return std::string(path).substr(0, pos);
}

void StaticMeshComponent::SetVertexCount(int inVertexCount) {
    mVertexCount = inVertexCount;
    mVertexData = new StaticMeshComponentVertexData[inVertexCount];
    memset(mVertexData, 0, sizeof(StaticMeshComponentVertexData) * inVertexCount);
}

void StaticMeshComponent::SetVertexPosition(int inIndex, float inX, float inY, float inZ, float inW) {
    if (inIndex >= 0 && inIndex < mVertexCount) {
        mVertexData[inIndex].mPosition[0] = inX;
        mVertexData[inIndex].mPosition[1] = inY;
        mVertexData[inIndex].mPosition[2] = inZ;
        mVertexData[inIndex].mPosition[3] = inW;
    }
}

void StaticMeshComponent::SetVertexTexcoord(int inIndex, float inX, float inY, float inZ, float inW) {
    if (inIndex >= 0 && inIndex < mVertexCount) {
        mVertexData[inIndex].mTexcoord[0] = inX;
        mVertexData[inIndex].mTexcoord[1] = inY;
        mVertexData[inIndex].mTexcoord[2] = inZ;
        mVertexData[inIndex].mTexcoord[3] = inW;
    }
}

void StaticMeshComponent::SetVertexNormal(int inIndex, float inX, float inY, float inZ, float inW) {
    if (inIndex >= 0 && inIndex < mVertexCount) {
        mVertexData[inIndex].mNormal[0] = inX;
        mVertexData[inIndex].mNormal[1] = inY;
        mVertexData[inIndex].mNormal[2] = inZ;
        mVertexData[inIndex].mNormal[3] = inW;
    }
}

void StaticMeshComponent::SetVertexTangent(int inIndex, float inX, float inY, float inZ, float inW) {
    if (inIndex >= 0 && inIndex < mVertexCount) {
        mVertexData[inIndex].mTangent[0] = inX;
        mVertexData[inIndex].mTangent[1] = inY;
        mVertexData[inIndex].mTangent[2] = inZ;
        mVertexData[inIndex].mTangent[3] = inW;
    }
}

void StaticMeshComponent::InitFromFile(ID3D12GraphicsCommandList* inCommandList, const char* inFilePath) {
    if (GetFileAttributesA(inFilePath) == INVALID_FILE_ATTRIBUTES) {
        std::string errorMsg = "FBX File Not Found: " + std::string(inFilePath);
        MessageBoxA(NULL, errorMsg.c_str(), "File Error", MB_OK | MB_ICONERROR);
        return;
    }

    FbxManager* fbxManager = FbxManager::Create();
    if (!fbxManager) {
        MessageBoxA(NULL, "Failed to create FBX Manager", "FBX Error", MB_OK | MB_ICONERROR);
        return;
    }

    FbxIOSettings* ioSettings = FbxIOSettings::Create(fbxManager, IOSROOT);
    fbxManager->SetIOSettings(ioSettings);

    std::string modulePath = GetModulePath();
    std::string pluginsPath = modulePath + "/plugins";

    bool pluginsLoaded = false;
    if (fbxManager->LoadPluginsDirectory(FbxGetApplicationDirectory())) {
        pluginsLoaded = true;
    }
    else if (fbxManager->LoadPluginsDirectory(pluginsPath.c_str())) {
        pluginsLoaded = true;
    }

    if (!pluginsLoaded) {
        std::string errorMsg = "Failed to load FBX plugins. Tried paths:\n"
            + std::string(FbxGetApplicationDirectory()) + "\n"
            + pluginsPath;
        MessageBoxA(NULL, errorMsg.c_str(), "FBX Plugin Error", MB_OK | MB_ICONWARNING);
    }

    FbxImporter* importer = FbxImporter::Create(fbxManager, "");
    if (!importer->Initialize(inFilePath, -1, fbxManager->GetIOSettings())) {
        std::string errorMsg = "FBX Import Failed: " + std::string(importer->GetStatus().GetErrorString())
            + "\nFile: " + std::string(inFilePath);
        MessageBoxA(NULL, errorMsg.c_str(), "FBX Error", MB_OK | MB_ICONERROR);
        importer->Destroy();
        fbxManager->Destroy();
        return;
    }

    FbxScene* scene = FbxScene::Create(fbxManager, "Scene");
    if (!scene) {
        MessageBoxA(NULL, "Failed to create FBX Scene", "FBX Error", MB_OK | MB_ICONERROR);
        importer->Destroy();
        fbxManager->Destroy();
        return;
    }

    if (!importer->Import(scene)) {
        std::string errorMsg = "FBX Import Scene Failed: " + std::string(importer->GetStatus().GetErrorString());
        MessageBoxA(NULL, errorMsg.c_str(), "FBX Error", MB_OK | MB_ICONERROR);
        scene->Destroy();
        importer->Destroy();
        fbxManager->Destroy();
        return;
    }

    if (!ParseFBXScene(scene, inCommandList)) {
        MessageBoxA(NULL, "Failed to parse FBX Scene", "FBX Error", MB_OK | MB_ICONERROR);
    }

    importer->Destroy();
    scene->Destroy();
    fbxManager->Destroy();

    if (mVertexCount > 0 && mVertexData) {
        mVBO = CreateBufferObject(inCommandList, mVertexData,
            sizeof(StaticMeshComponentVertexData) * mVertexCount,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

        mVBOView.BufferLocation = mVBO->GetGPUVirtualAddress();
        mVBOView.StrideInBytes = sizeof(StaticMeshComponentVertexData);
        mVBOView.SizeInBytes = sizeof(StaticMeshComponentVertexData) * mVertexCount;
    }
}

bool StaticMeshComponent::ParseFBXScene(FbxScene* pScene, ID3D12GraphicsCommandList* inCommandList) {
    if (!pScene) return false;

    FbxNode* rootNode = pScene->GetRootNode();
    if (rootNode) {
        ProcessFBXNode(rootNode, inCommandList);
    }
    return true;
}

void StaticMeshComponent::ProcessFBXNode(FbxNode* pNode, ID3D12GraphicsCommandList* inCommandList) {
    if (!pNode) return;

    for (int i = 0; i < pNode->GetNodeAttributeCount(); ++i) {
        FbxNodeAttribute* attr = pNode->GetNodeAttributeByIndex(i);
        if (attr && attr->GetAttributeType() == FbxNodeAttribute::eMesh) {
            ProcessFBXMesh(static_cast<FbxMesh*>(attr), pNode->GetName(), inCommandList);
        }
    }

    for (int i = 0; i < pNode->GetChildCount(); ++i) {
        ProcessFBXNode(pNode->GetChild(i), inCommandList);
    }
}

void StaticMeshComponent::ProcessFBXMesh(FbxMesh* pMesh, const std::string& nodeName, ID3D12GraphicsCommandList* inCommandList) {
    if (!pMesh) return;

    std::vector<StaticMeshComponentVertexData> vertices;
    std::vector<unsigned int> indices;

    int polygonCount = pMesh->GetPolygonCount();

    for (int polyIndex = 0; polyIndex < polygonCount; ++polyIndex) {
        int polygonSize = pMesh->GetPolygonSize(polyIndex);

        // �ռ���ǰ����ζ�������
        std::vector<unsigned int> polyVertIndices;

        for (int vertIndex = 0; vertIndex < polygonSize; ++vertIndex) {
            int controlPointIndex = pMesh->GetPolygonVertex(polyIndex, vertIndex);

            StaticMeshComponentVertexData vtx = {};
            // === ����λ�� ===
            FbxVector4 pos = pMesh->GetControlPointAt(controlPointIndex);
            vtx.mPosition[0] = (float)pos[0];
            vtx.mPosition[1] = (float)pos[2]; // YZ ����
            vtx.mPosition[2] = (float)pos[1];
            vtx.mPosition[3] = 1.0f;

            // === UV ===
            FbxVector2 uv(0, 0);
            bool unmapped = false;
            if (pMesh->GetLayer(0) && pMesh->GetLayer(0)->GetUVs()) {
                const char* uvSetName = pMesh->GetLayer(0)->GetUVs()->GetName();
                pMesh->GetPolygonVertexUV(polyIndex, vertIndex, uvSetName, uv, unmapped);
            }
            vtx.mTexcoord[0] = (float)uv[0];
            vtx.mTexcoord[1] = 1.0f - (float)uv[1]; // V ��ת
            vtx.mTexcoord[2] = 0.0f;
            vtx.mTexcoord[3] = 1.0f;

            // === ���� ===
            FbxVector4 normal;
            if (pMesh->GetPolygonVertexNormal(polyIndex, vertIndex, normal)) {
                normal.Normalize();
                vtx.mNormal[0] = (float)normal[0];
                vtx.mNormal[1] = (float)normal[2]; // YZ ����
                vtx.mNormal[2] = (float)normal[1];
                vtx.mNormal[3] = 0.0f;
            }

            // === ���ߣ���ѡ�� ===
            if (pMesh->GetElementTangentCount() > 0) {
                FbxGeometryElementTangent* tangentElement = pMesh->GetElementTangent(0);
                if (tangentElement && tangentElement->GetMappingMode() == FbxGeometryElement::eByPolygonVertex) {
                    int tangentIndex = polyIndex * polygonSize + vertIndex;
                    if (tangentElement->GetReferenceMode() == FbxGeometryElement::eIndexToDirect) {
                        tangentIndex = tangentElement->GetIndexArray().GetAt(tangentIndex);
                    }
                    FbxVector4 tangent = tangentElement->GetDirectArray().GetAt(tangentIndex);
                    vtx.mTangent[0] = (float)tangent[0];
                    vtx.mTangent[1] = (float)tangent[2];
                    vtx.mTangent[2] = (float)tangent[1];
                    vtx.mTangent[3] = (float)tangent[3];
                }
            }

            // === Generate default tangent if FBX has no tangent data ===
            if (vtx.mTangent[0] == 0.0f && vtx.mTangent[1] == 0.0f &&
                vtx.mTangent[2] == 0.0f && vtx.mTangent[3] == 0.0f) {
                // Generate tangent from normal using Gram-Schmidt orthogonalization
                float nx = vtx.mNormal[0];
                float ny = vtx.mNormal[1];
                float nz = vtx.mNormal[2];

                // Choose reference vector (avoid parallel to normal)
                float rx, ry, rz;
                if (fabs(ny) > 0.999f) {
                    rx = 1.0f; ry = 0.0f; rz = 0.0f;  // Use X-axis
                } else {
                    rx = 0.0f; ry = 1.0f; rz = 0.0f;  // Use Y-axis
                }

                // Gram-Schmidt: tangent = normalize(ref - dot(ref, normal) * normal)
                float dot = rx * nx + ry * ny + rz * nz;
                float tx = rx - dot * nx;
                float ty = ry - dot * ny;
                float tz = rz - dot * nz;

                // Normalize
                float len = sqrtf(tx * tx + ty * ty + tz * tz);
                if (len > 0.0001f) {
                    tx /= len; ty /= len; tz /= len;
                } else {
                    tx = 1.0f; ty = 0.0f; tz = 0.0f;
                }

                vtx.mTangent[0] = tx;
                vtx.mTangent[1] = ty;
                vtx.mTangent[2] = tz;
                vtx.mTangent[3] = 1.0f;  // w component (bitangent sign)
            }

            // === ���浱ǰ���� ===
            vertices.push_back(vtx);
            polyVertIndices.push_back((unsigned int)vertices.size() - 1);
        }

        // === ���ǻ� (fan triangulation) ===
        for (int j = 1; j < polygonSize - 1; ++j) {
            indices.push_back(polyVertIndices[0]);
            indices.push_back(polyVertIndices[j]);
            indices.push_back(polyVertIndices[j + 1]);
        }
    }

    // === ���õ���� ===
    SetVertexCount((int)vertices.size());
    memcpy(mVertexData, vertices.data(), sizeof(StaticMeshComponentVertexData) * vertices.size());

    // === �������������� ===
    SubMesh* subMesh = new SubMesh();
    subMesh->mIndexCount = (int)indices.size();
    subMesh->mIBO = CreateBufferObject(inCommandList, indices.data(),
        sizeof(unsigned int) * (int)indices.size(),
        D3D12_RESOURCE_STATE_INDEX_BUFFER);

    subMesh->mIBView.BufferLocation = subMesh->mIBO->GetGPUVirtualAddress();
    subMesh->mIBView.Format = DXGI_FORMAT_R32_UINT;
    subMesh->mIBView.SizeInBytes = sizeof(unsigned int) * (UINT)indices.size();
    mSubMeshes[nodeName] = subMesh;
}



void StaticMeshComponent::Render(ID3D12GraphicsCommandList* inCommandList, ID3D12RootSignature* rootSignature) {
    // ���ö��㻺����
    inCommandList->IASetVertexBuffers(0, 1, &mVBOView);

    // ��������ʵײ�(��������)
    if (m_material && rootSignature) {
        // 首先检查是否有待加载的纹理
        if (m_material->HasPendingTextures()) {
            m_material->LoadTexturesFromPaths(inCommandList);
        }
        m_material->Bind(inCommandList, rootSignature, 2);  // Slot 2 ��Ӧ b1
    }

    // ��Ⱦ����������
    for (auto& pair : mSubMeshes) {
        SubMesh* subMesh = pair.second;
        inCommandList->IASetIndexBuffer(&subMesh->mIBView);
        inCommandList->DrawIndexedInstanced(subMesh->mIndexCount, 1, 0, 0, 0);
    }
}