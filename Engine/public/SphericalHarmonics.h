#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// 球谐光照系统（用于Skylight漫反射）
class SphericalHarmonics {
public:
    SphericalHarmonics();
    ~SphericalHarmonics();

    // 初始化（创建CS和资源）
    bool Initialize(ID3D12Device* device);

    // 从CubeMap计算SH系数
    void ComputeFromCubemap(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* cubemap,
        UINT cubemapSize);

    // 获取SH系数缓冲区（用于传递给shader）
    ID3D12Resource* GetSHBuffer() const { return m_shCoefficientsBuffer.Get(); }

    // 获取9个SH系数（CPU端读取，用于调试）
    const DirectX::XMFLOAT3* GetSHCoefficients() const { return m_shCoefficients; }

    // 评估SH光照（CPU端，用于调试）
    DirectX::XMFLOAT3 EvaluateSH(const DirectX::XMFLOAT3& direction) const;

private:
    // 编译Compute Shader
    bool CompileComputeShaders(ID3D12Device* device);

    // 创建PSO
    bool CreatePipelineStates(ID3D12Device* device);

    ComPtr<ID3D12Device> m_device;

    // Compute Shader相关
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_computePSO;
    ComPtr<ID3D12PipelineState> m_normalizePSO;

    // SH系数缓冲区（GPU端）
    ComPtr<ID3D12Resource> m_shCoefficientsBuffer;  // StructuredBuffer<float3>[9]
    ComPtr<ID3D12Resource> m_shReadbackBuffer;      // CPU读回用

    // SH系数（CPU端副本）
    DirectX::XMFLOAT3 m_shCoefficients[9];

    // 描述符堆
    ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
    UINT m_descriptorSize;
};
