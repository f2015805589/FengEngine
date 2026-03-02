#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include "BattleFireDirect.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

enum class GIType {
    Off = 0,
    SSGI = 1
};

struct SsgiConstants {
    XMFLOAT2 resolution;
    XMFLOAT2 inverseResolution;
    float radius;
    float intensity;
    int stepCount;
    int directionCount;
    int frameCounter;
    int depthPyramidPasses;
    float depthThickness;
    float temporalBlend;
    XMFLOAT2 padding;
};

class SsgiPass {
public:
    SsgiPass() = default;
    ~SsgiPass();

    bool Initialize(int viewportWidth, int viewportHeight);
    void SetSceneConstantBuffer(ID3D12Resource* sceneCB) { m_sceneConstantBuffer = sceneCB; }

    void Render(ID3D12GraphicsCommandList* cmdList,
        ID3D12PipelineState* depthMaxPso,
        ID3D12PipelineState* ssgiPso,
        ID3D12PipelineState* taaPso,
        ID3D12PipelineState* blurHPso,
        ID3D12PipelineState* blurVPso,
        ID3D12RootSignature* rootSig,
        ID3D12Resource* depthBuffer,
        ID3D12Resource* baseColorRT,
        ID3D12Resource* normalRT);

    ID3D12PipelineState* CreateDepthPSO(ID3D12RootSignature* rootSig,
        D3D12_SHADER_BYTECODE vs,
        D3D12_SHADER_BYTECODE ps);

    ID3D12PipelineState* CreateColorPSO(ID3D12RootSignature* rootSig,
        D3D12_SHADER_BYTECODE vs,
        D3D12_SHADER_BYTECODE ps);

    void Resize(int newWidth, int newHeight);

    ID3D12Resource* GetSSGITexture() const {
        if (m_giType == GIType::SSGI) return m_ssgiFinalRT.Get();
        return m_defaultBlackTexture.Get();
    }

    void SetGIType(int type) { m_giType = static_cast<GIType>(type); }
    int GetGIType() const { return static_cast<int>(m_giType); }
    bool IsEnabled() const { return m_giType == GIType::SSGI; }

    void SetRadius(float value) { m_radius = value; }
    float GetRadius() const { return m_radius; }

    void SetIntensity(float value) { m_intensity = value; }
    float GetIntensity() const { return m_intensity; }

    void SetStepCount(int value) { m_stepCount = value; }
    int GetStepCount() const { return m_stepCount; }

    void SetDirectionCount(int value) { m_directionCount = value; }
    int GetDirectionCount() const { return m_directionCount; }

    void SetDepthPyramidPasses(int value) { m_depthPyramidPasses = value; }
    int GetDepthPyramidPasses() const { return m_depthPyramidPasses; }

private:
    void CreateRenderTargets();
    void CreateSRVHeap();
    void CreateConstantBuffer();
    void CreateDefaultBlackTexture();
    void CreateNoiseTexture();
    void UpdateConstants();
    void SetViewportAndScissor(ID3D12GraphicsCommandList* cmdList);

    void CreateDepthInputSRV(ID3D12Resource* sourceDepth, DXGI_FORMAT format, UINT descriptorIndex);
    void CreateRaymarchInputSRVs(ID3D12Resource* depthMaxTex, ID3D12Resource* baseColorRT, ID3D12Resource* normalRT, ID3D12Resource* sceneDepth, UINT descriptorStartIndex);
    void CreateTaaInputSRVs(ID3D12Resource* currentRT, ID3D12Resource* historyRT, ID3D12Resource* depthTex);
    void CreateBlurInputSRV(ID3D12Resource* sourceRT, ID3D12Resource* sceneDepth, UINT descriptorStartIndex);

private:
    int m_viewportWidth = 0;
    int m_viewportHeight = 0;

    ComPtr<ID3D12Resource> m_depthMaxPingRT;
    ComPtr<ID3D12Resource> m_depthMaxPongRT;
    ComPtr<ID3D12Resource> m_ssgiRawRT;
    ComPtr<ID3D12Resource> m_ssgiBlurTempRT;
    ComPtr<ID3D12Resource> m_ssgiFinalRT;
    ComPtr<ID3D12Resource> m_historyRT1;
    ComPtr<ID3D12Resource> m_historyRT2;

    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_rtvDescriptorSize = 0;
    UINT m_srvDescriptorSize = 0;

    ID3D12Resource* m_sceneConstantBuffer = nullptr;
    ComPtr<ID3D12Resource> m_ssgiConstantBuffer;

    ComPtr<ID3D12Resource> m_defaultBlackTexture;
    ComPtr<ID3D12Resource> m_defaultBlackTextureUpload;

    ComPtr<ID3D12Resource> m_noiseTexture;
    ComPtr<ID3D12Resource> m_noiseTextureUpload;

    GIType m_giType = GIType::Off;

float m_radius = 6.0f;
float m_intensity = 1.0f;
int m_stepCount = 24;
int m_directionCount = 64;
    int m_depthPyramidPasses = 3;
    int m_frameCounter = 0;
    bool m_useHistory2 = false;
};
