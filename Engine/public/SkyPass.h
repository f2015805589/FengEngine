// SkyPass.h
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include "BattleFireDirect.h"

using Microsoft::WRL::ComPtr;

class SkyPass {
public:
    SkyPass() = default;
    ~SkyPass();

    // 设置场景常量缓冲区（包含相机位置、矩阵等）
    void SetSceneConstantBuffer(ID3D12Resource* sceneCB) { m_sceneConstantBuffer = sceneCB; }

    // 初始化：创建天空球几何体和SRV堆
    bool Initialize(ID3D12GraphicsCommandList* commandList, float sphereRadius = 500.0f);

    // 渲染天空球
    void Render(ID3D12GraphicsCommandList* cmdList,
        ID3D12PipelineState* pso,
        ID3D12RootSignature* rootSig,
        ComPtr<ID3D12Resource> skyTexture);

    // 创建PSO
    ID3D12PipelineState* CreatePSO(ID3D12RootSignature* rootSig,
        D3D12_SHADER_BYTECODE vs,
        D3D12_SHADER_BYTECODE ps);

private:
    // 创建SRV堆（用于SkyCube纹理）
    void CreateSRVHeap();

    // 为天空立方体贴图创建SRV
    void CreateSkyCubeSRV(ComPtr<ID3D12Resource> skyTexture);

    // 生成天空球顶点数据
    void GenerateSkySphere(float radius, int slices, int stacks);

    // 天空球顶点缓冲
    ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vbv{};

    // 天空球索引缓冲
    ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_INDEX_BUFFER_VIEW m_ibv{};
    UINT m_indexCount = 0;

    // SRV描述符堆（用于SkyCube纹理）
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;

    // 场景常量缓冲区引用（不拥有所有权）
    ID3D12Resource* m_sceneConstantBuffer = nullptr;

    // 顶点结构
    struct SkyVertex {
        float position[3];
    };

    std::vector<SkyVertex> m_vertices;
    std::vector<UINT> m_indices;
};
