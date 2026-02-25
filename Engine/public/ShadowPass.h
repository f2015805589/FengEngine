// ShadowPass.h
// Shadow Map 深度渲染Pass - 从光源视角渲染场景深度
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include "public/BattleFireDirect.h"

using Microsoft::WRL::ComPtr;

class Scene;

class ShadowPass {
public:
    ShadowPass(int shadowMapSize = 2048);
    ~ShadowPass();

    // 初始化
    bool Initialize();

    // 设置场景常量缓冲区（包含LightViewProjectionMatrix）
    void SetSceneConstantBuffer(ID3D12Resource* sceneCB) { m_sceneConstantBuffer = sceneCB; }

    // 渲染Shadow Map（从光源视角渲染场景深度）
    void Render(ID3D12GraphicsCommandList* commandList,
                ID3D12PipelineState* pso,
                ID3D12RootSignature* rootSignature,
                Scene* scene);

    // 创建Shadow Map PSO（带深度偏移防止Shadow Acne）
    ID3D12PipelineState* CreateShadowPSO(ID3D12RootSignature* rootSignature,
                                          D3D12_SHADER_BYTECODE vertexShader,
                                          D3D12_SHADER_BYTECODE pixelShader);

    // 获取Shadow Map资源（用于后续Pass采样）
    ID3D12Resource* GetShadowMap() const { return m_shadowMap.Get(); }

    // 获取Shadow Map的SRV描述符堆
    ID3D12DescriptorHeap* GetSRVHeap() const { return m_srvHeap.Get(); }

    // 获取Shadow Map的SRV GPU句柄
    D3D12_GPU_DESCRIPTOR_HANDLE GetShadowMapSRV() const;

    // 获取Shadow Map尺寸
    int GetShadowMapSize() const { return m_shadowMapSize; }

    // 分辨率变更
    bool Resize(int newSize);

private:
    void CreateShadowMapResource();
    void CreateDescriptorHeaps();

    int m_shadowMapSize;  // Shadow Map分辨率（正方形）

    // Shadow Map深度资源
    ComPtr<ID3D12Resource> m_shadowMap;

    // 描述符堆
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;  // DSV堆（用于深度写入）
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;  // SRV堆（用于后续采样）
    UINT m_dsvDescriptorSize;
    UINT m_srvDescriptorSize;

    // 场景常量缓冲区引用
    ID3D12Resource* m_sceneConstantBuffer = nullptr;
};
