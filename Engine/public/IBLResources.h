// IBLResources.h
// IBL 资源管理类 - 管理预计算的 IBL 纹理

#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include "BattleFireDirect.h"

using Microsoft::WRL::ComPtr;

class IBLResources {
public:
    IBLResources() = default;
    ~IBLResources();

    // 初始化 IBL 资源（预计算所有纹理）
    bool Initialize(ID3D12GraphicsCommandList* commandList,
                    ID3D12Resource* environmentCubemap,
                    ID3D12RootSignature* rootSignature);

    // 获取资源
    ID3D12Resource* GetBRDFLUT() const { return m_brdfLUT.Get(); }
    ID3D12Resource* GetIrradianceMap() const { return m_irradianceMap.Get(); }
    ID3D12Resource* GetPrefilteredMap() const { return m_prefilteredMap.Get(); }

    // 获取 SRV 堆
    ID3D12DescriptorHeap* GetSRVHeap() const { return m_srvHeap.Get(); }

    // BRDF LUT 尺寸
    static const UINT BRDF_LUT_SIZE = 512;
    // 辐照度贴图尺寸
    static const UINT IRRADIANCE_SIZE = 32;
    // 预过滤环境贴图尺寸和 mip 级别
    static const UINT PREFILTER_SIZE = 128;
    static const UINT PREFILTER_MIP_LEVELS = 5;

private:
    // 创建 BRDF LUT
    bool CreateBRDFLUT(ID3D12GraphicsCommandList* commandList);

    // 创建辐照度立方体贴图
    bool CreateIrradianceMap(ID3D12GraphicsCommandList* commandList,
                              ID3D12Resource* environmentCubemap);

    // 创建预过滤环境贴图
    bool CreatePrefilteredMap(ID3D12GraphicsCommandList* commandList,
                               ID3D12Resource* environmentCubemap);

    // 创建 SRV 堆
    void CreateSRVHeap();

    // 创建用于计算着色器的根签名
    bool CreateComputeRootSignature();

    // 编译计算着色器
    bool CompileComputeShaders();

    // 资源
    ComPtr<ID3D12Resource> m_brdfLUT;           // BRDF 积分 LUT (2D)
    ComPtr<ID3D12Resource> m_irradianceMap;     // 辐照度立方体贴图
    ComPtr<ID3D12Resource> m_prefilteredMap;    // 预过滤环境贴图 (带 mipmap)

    // SRV 描述符堆
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    ComPtr<ID3D12DescriptorHeap> m_uavHeap;     // 用于计算着色器输出
    UINT m_srvDescriptorSize = 0;

    // 计算着色器 PSO
    ComPtr<ID3D12RootSignature> m_computeRootSignature;
    ComPtr<ID3D12PipelineState> m_brdfPSO;
    ComPtr<ID3D12PipelineState> m_irradiancePSO;
    ComPtr<ID3D12PipelineState> m_prefilterPSO;

    // 着色器字节码
    ComPtr<ID3DBlob> m_brdfShaderBlob;
    ComPtr<ID3DBlob> m_irradianceShaderBlob;
    ComPtr<ID3DBlob> m_prefilterShaderBlob;

    // 预过滤参数常量缓冲
    ComPtr<ID3D12Resource> m_prefilterParamsCB;
};
