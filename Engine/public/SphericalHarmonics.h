#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// 球谐光照系统（用于 Skylight 漫反射 IBL）
// CPU 端从 cubemap 计算 3 阶 SH 系数（9 个 float3），通过 SceneCBData 传递给 shader
class SphericalHarmonics {
public:
    SphericalHarmonics();
    ~SphericalHarmonics();

    // 初始化（无需 GPU 资源，CPU 端计算）
    bool Initialize(ID3D12Device* device);

    // 从 CubeMap 计算 SH 系数（CPU 端读取 cubemap 像素）
    void ComputeFromCubemap(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* cubemap,
        UINT cubemapSize);

    // 获取 9 个 SH 系数（传给 SceneCBData）
    const DirectX::XMFLOAT3* GetSHCoefficients() const { return m_shCoefficients; }

    // 评估 SH 光照（CPU 端，用于调试）
    DirectX::XMFLOAT3 EvaluateSH(const DirectX::XMFLOAT3& direction) const;

private:
    ComPtr<ID3D12Device> m_device;

    // SH 系数（CPU 端）
    DirectX::XMFLOAT3 m_shCoefficients[9];

    // readback 缓冲（用于读取 cubemap 像素）
    ComPtr<ID3D12Resource> m_readbackBuffer;
};
