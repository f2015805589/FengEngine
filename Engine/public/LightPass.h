// lightpass.h
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include "public/BattleFireDirect.h"

using Microsoft::WRL::ComPtr;

class LightPass {
public:
    LightPass(int width, int height);
    ~LightPass();

    // 设置场景的常量缓冲区
    void SetSceneConstantBuffer(ID3D12Resource* sceneCB) { m_sceneConstantBuffer = sceneCB; }

    bool Initialize(ID3D12GraphicsCommandList* commandList);
    void Render(ID3D12GraphicsCommandList* commandList,
        ID3D12PipelineState* pso,
        ID3D12RootSignature* rootSignature,
        ID3D12Resource* rt0,
        ID3D12Resource* rt1,
        ID3D12Resource* rt2);

    ID3D12Resource* GetShadowMap() const { return m_shadowMap.Get(); }
    ID3D12DescriptorHeap* GetSRVHeap() const { return m_srvHeap.Get(); }


    ID3D12PipelineState* CreateLightPSO(ID3D12RootSignature* inID3D12RootSignature,
        D3D12_SHADER_BYTECODE inVertexShader, D3D12_SHADER_BYTECODE inPixelShader);

    // 分辨率变更时重新创建资源
    bool Resize(int newWidth, int newHeight);
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
private:
    void CreateSRVHeap();
    void CreateShadowMap(ID3D12GraphicsCommandList* commandList);
    void CreateInputSRVs(ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* rt0,
        ID3D12Resource* rt1,
        ID3D12Resource* rt2);

    int m_width;
    int m_height;

    // 阴影贴图资源
    ComPtr<ID3D12Resource> m_shadowMap;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvDescriptorSize;

    // 输入RT的SRV描述符堆
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize;

    // 引用Scene的常量缓冲区，不拥有所有权
    ID3D12Resource* m_sceneConstantBuffer = nullptr;
};
