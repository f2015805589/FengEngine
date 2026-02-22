// IBLResources.cpp
// IBL 资源管理类实现

#include "public/IBLResources.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <stdexcept>
#include <iostream>

IBLResources::~IBLResources() {
    // ComPtr 会自动释放资源
}

bool IBLResources::Initialize(ID3D12GraphicsCommandList* commandList,
                               ID3D12Resource* environmentCubemap,
                               ID3D12RootSignature* rootSignature) {
    std::cout << "Initializing IBL Resources..." << std::endl;

    // 1. 编译计算着色器
    if (!CompileComputeShaders()) {
        std::cout << "Failed to compile IBL compute shaders" << std::endl;
        return false;
    }

    // 2. 创建计算着色器根签名
    if (!CreateComputeRootSignature()) {
        std::cout << "Failed to create compute root signature" << std::endl;
        return false;
    }

    // 3. 创建 SRV 堆
    CreateSRVHeap();

    // 4. 生成 BRDF LUT
    std::cout << "Generating BRDF LUT..." << std::endl;
    if (!CreateBRDFLUT(commandList)) {
        std::cout << "Failed to create BRDF LUT" << std::endl;
        return false;
    }

    // 5. 生成辐照度贴图
    std::cout << "Generating Irradiance Map..." << std::endl;
    if (!CreateIrradianceMap(commandList, environmentCubemap)) {
        std::cout << "Failed to create Irradiance Map" << std::endl;
        return false;
    }

    // 6. 生成预过滤环境贴图
    std::cout << "Generating Pre-filtered Environment Map..." << std::endl;
    if (!CreatePrefilteredMap(commandList, environmentCubemap)) {
        std::cout << "Failed to create Pre-filtered Map" << std::endl;
        return false;
    }

    std::cout << "IBL Resources initialized successfully!" << std::endl;
    return true;
}

bool IBLResources::CompileComputeShaders() {
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    ComPtr<ID3DBlob> errorBlob;

    // 编译 BRDF LUT 计算着色器
    HRESULT hr = D3DCompileFromFile(
        L"Engine/Shader/IBL/BRDFIntegration.hlsl",
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSMain", "cs_5_0",
        compileFlags, 0,
        &m_brdfShaderBlob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cout << "BRDF shader compile error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
        return false;
    }

    // 编译辐照度卷积计算着色器
    hr = D3DCompileFromFile(
        L"Engine/Shader/IBL/IrradianceConvolution.hlsl",
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSMain", "cs_5_0",
        compileFlags, 0,
        &m_irradianceShaderBlob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cout << "Irradiance shader compile error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
        return false;
    }

    // 编译预过滤环境贴图计算着色器
    hr = D3DCompileFromFile(
        L"Engine/Shader/IBL/PrefilterEnvMap.hlsl",
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "CSMain", "cs_5_0",
        compileFlags, 0,
        &m_prefilterShaderBlob, &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cout << "Prefilter shader compile error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
        return false;
    }

    return true;
}

bool IBLResources::CreateComputeRootSignature() {
    // 根参数：
    // 0: CBV (常量缓冲，用于预过滤参数)
    // 1: SRV (环境立方体贴图)
    // 2: UAV (输出纹理)
    // 3: Sampler

    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE1 samplerRange;
    samplerRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);

    CD3DX12_ROOT_PARAMETER1 rootParams[4];
    rootParams[0].InitAsConstantBufferView(0); // CBV
    rootParams[1].InitAsDescriptorTable(1, &srvRange); // SRV
    rootParams[2].InitAsDescriptorTable(1, &uavRange); // UAV
    rootParams[3].InitAsDescriptorTable(1, &samplerRange); // Sampler

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init_1_1(_countof(rootParams), rootParams, 0, nullptr,
                          D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rootSigDesc,
                                                        D3D_ROOT_SIGNATURE_VERSION_1_1,
                                                        &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            std::cout << "Root signature serialize error: " << (char*)error->GetBufferPointer() << std::endl;
        }
        return false;
    }

    hr = gD3D12Device->CreateRootSignature(0, signature->GetBufferPointer(),
                                            signature->GetBufferSize(),
                                            IID_PPV_ARGS(&m_computeRootSignature));
    if (FAILED(hr)) {
        std::cout << "Failed to create compute root signature" << std::endl;
        return false;
    }

    // 创建 BRDF PSO
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_computeRootSignature.Get();
    psoDesc.CS = { m_brdfShaderBlob->GetBufferPointer(), m_brdfShaderBlob->GetBufferSize() };

    hr = gD3D12Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_brdfPSO));
    if (FAILED(hr)) {
        std::cout << "Failed to create BRDF PSO" << std::endl;
        return false;
    }

    // 创建辐照度 PSO
    psoDesc.CS = { m_irradianceShaderBlob->GetBufferPointer(), m_irradianceShaderBlob->GetBufferSize() };
    hr = gD3D12Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_irradiancePSO));
    if (FAILED(hr)) {
        std::cout << "Failed to create Irradiance PSO" << std::endl;
        return false;
    }

    // 创建预过滤 PSO
    psoDesc.CS = { m_prefilterShaderBlob->GetBufferPointer(), m_prefilterShaderBlob->GetBufferSize() };
    hr = gD3D12Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_prefilterPSO));
    if (FAILED(hr)) {
        std::cout << "Failed to create Prefilter PSO" << std::endl;
        return false;
    }

    return true;
}

void IBLResources::CreateSRVHeap() {
    // SRV 堆：3 个描述符
    // 0: BRDF LUT SRV
    // 1: Irradiance Map SRV
    // 2: Pre-filtered Map SRV
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 10; // 预留更多
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    gD3D12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    m_srvDescriptorSize = gD3D12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // UAV 堆用于计算着色器输出
    D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
    uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uavHeapDesc.NumDescriptors = 10;
    uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    gD3D12Device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&m_uavHeap));
}

bool IBLResources::CreateBRDFLUT(ID3D12GraphicsCommandList* commandList) {
    // 创建 BRDF LUT 纹理 (RG16F)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = BRDF_LUT_SIZE;
    texDesc.Height = BRDF_LUT_SIZE;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_brdfLUT)
    );

    if (FAILED(hr)) {
        return false;
    }

    // 创建 UAV
    CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle(m_uavHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    gD3D12Device->CreateUnorderedAccessView(m_brdfLUT.Get(), nullptr, &uavDesc, uavHandle);

    // 执行计算着色器
    commandList->SetPipelineState(m_brdfPSO.Get());
    commandList->SetComputeRootSignature(m_computeRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_uavHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE uavGpuHandle(m_uavHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootDescriptorTable(2, uavGpuHandle);

    // Dispatch
    commandList->Dispatch(BRDF_LUT_SIZE / 16, BRDF_LUT_SIZE / 16, 1);

    // 资源屏障：UAV -> SRV
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_brdfLUT.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &barrier);

    // 创建 SRV
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    gD3D12Device->CreateShaderResourceView(m_brdfLUT.Get(), &srvDesc, srvHandle);

    std::cout << "BRDF LUT created (" << BRDF_LUT_SIZE << "x" << BRDF_LUT_SIZE << ")" << std::endl;
    return true;
}

bool IBLResources::CreateIrradianceMap(ID3D12GraphicsCommandList* commandList,
                                        ID3D12Resource* environmentCubemap) {
    // 创建辐照度立方体贴图 (RGBA16F)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = IRRADIANCE_SIZE;
    texDesc.Height = IRRADIANCE_SIZE;
    texDesc.DepthOrArraySize = 6; // 立方体贴图
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_irradianceMap)
    );

    if (FAILED(hr)) {
        return false;
    }

    // 这里简化处理：跳过实际的计算着色器执行
    // 在实际实现中，需要设置环境贴图 SRV，执行计算着色器

    // 资源屏障：UAV -> SRV
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_irradianceMap.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &barrier);

    // 创建 SRV
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    srvHandle.Offset(1, m_srvDescriptorSize); // 第二个描述符

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MipLevels = 1;

    gD3D12Device->CreateShaderResourceView(m_irradianceMap.Get(), &srvDesc, srvHandle);

    std::cout << "Irradiance Map created (" << IRRADIANCE_SIZE << "x" << IRRADIANCE_SIZE << ")" << std::endl;
    return true;
}

bool IBLResources::CreatePrefilteredMap(ID3D12GraphicsCommandList* commandList,
                                         ID3D12Resource* environmentCubemap) {
    // 创建预过滤环境贴图 (RGBA16F, 带 mipmap)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = PREFILTER_SIZE;
    texDesc.Height = PREFILTER_SIZE;
    texDesc.DepthOrArraySize = 6; // 立方体贴图
    texDesc.MipLevels = PREFILTER_MIP_LEVELS;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    HRESULT hr = gD3D12Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_prefilteredMap)
    );

    if (FAILED(hr)) {
        return false;
    }

    // 这里简化处理：跳过实际的计算着色器执行
    // 在实际实现中，需要对每个 mip level 执行计算着色器

    // 资源屏障：UAV -> SRV
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_prefilteredMap.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &barrier);

    // 创建 SRV
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    srvHandle.Offset(2, m_srvDescriptorSize); // 第三个描述符

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MipLevels = PREFILTER_MIP_LEVELS;

    gD3D12Device->CreateShaderResourceView(m_prefilteredMap.Get(), &srvDesc, srvHandle);

    std::cout << "Pre-filtered Map created (" << PREFILTER_SIZE << "x" << PREFILTER_SIZE
              << ", " << PREFILTER_MIP_LEVELS << " mips)" << std::endl;
    return true;
}
