// lightpass.h
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include "public/BattleFireDirect.h"

using Microsoft::WRL::ComPtr;

class Scene;

class LightPass {
public:
    LightPass(int width, int height, int shadowMapSize = 2048);
    ~LightPass();

    // 设置场景的常量缓冲区
    void SetSceneConstantBuffer(ID3D12Resource* sceneCB) { m_sceneConstantBuffer = sceneCB; }

    bool Initialize(ID3D12GraphicsCommandList* commandList);

    // 渲染平行光（包含shadow map生成和光照计算）
    void RenderDirectLight(ID3D12GraphicsCommandList* commandList,
        ID3D12PipelineState* shadowPso,
        ID3D12PipelineState* lightPso,
        ID3D12RootSignature* rootSignature,
        Scene* scene,
        ID3D12Resource* depthBuffer);

    // 创建Shadow Map PSO
    ID3D12PipelineState* CreateShadowPSO(ID3D12RootSignature* rootSignature,
        D3D12_SHADER_BYTECODE vertexShader,
        D3D12_SHADER_BYTECODE pixelShader);

    // 创建Light PSO
    ID3D12PipelineState* CreateLightPSO(ID3D12RootSignature* rootSignature,
        D3D12_SHADER_BYTECODE vertexShader,
        D3D12_SHADER_BYTECODE pixelShader);

    ID3D12Resource* GetLightRT() const { return m_lightRT.Get(); }
    ID3D12Resource* GetShadowMap() const { return m_shadowMap.Get(); }

    // 分辨率变更
    bool Resize(int newWidth, int newHeight);
    bool ResizeShadowMap(int newSize);

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    int GetShadowMapSize() const { return m_shadowMapSize; }

private:
    void CreateSRVHeap();
    void CreateLightRT(ID3D12GraphicsCommandList* commandList);
    void CreateShadowMapResource();
    void CreateInputSRVs(ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* depthBuffer);

    // 子pass: 渲染shadow map
    void RenderShadowMap(ID3D12GraphicsCommandList* commandList,
        ID3D12PipelineState* pso,
        ID3D12RootSignature* rootSignature,
        Scene* scene);

    // 子pass: 计算光照
    void RenderLighting(ID3D12GraphicsCommandList* commandList,
        ID3D12PipelineState* pso,
        ID3D12RootSignature* rootSignature,
        ID3D12Resource* depthBuffer);

    int m_width;
    int m_height;
    int m_shadowMapSize;

    // Light Pass输出RT（光照结果）
    ComPtr<ID3D12Resource> m_lightRT;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvDescriptorSize;

    // Shadow Map资源
    ComPtr<ID3D12Resource> m_shadowMap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;  // DSV堆（用于深度写入）
    UINT m_dsvDescriptorSize;

    // 输入RT的SRV描述符堆（2个：Depth + ShadowMap）
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize;

    // 引用Scene的常量缓冲区，不拥有所有权
    ID3D12Resource* m_sceneConstantBuffer = nullptr;
};
